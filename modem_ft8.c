#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <time.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <pthread.h>
#include <unistd.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "modem_ft8.h"

#include "ft8_lib/common/common.h"
#include "ft8_lib/common/wave.h"
#include "ft8_lib/common/debug.h"
#include "ft8_lib/ft8/pack.h"
#include "ft8_lib/ft8/decode.h"
#include "ft8_lib/ft8/encode.h"
#include "ft8_lib/ft8/constants.h"
#include "ft8_lib/fft/kiss_fftr.h"

static int32_t ft8_rx_buff[FT8_MAX_BUFF];
static float ft8_rx_buffer[FT8_MAX_BUFF];
static float ft8_tx_buff[FT8_MAX_BUFF];
static char ft8_tx_text[128];
static int ft8_rx_buff_index = 0;
static int ft8_tx_buff_index = 0;
static int	ft8_tx_nsamples = 0;
static int ft8_do_decode = 0;
static int	ft8_do_tx = 0;
static int	ft8_pitch = 0;
static int	ft8_mode = FT8_SEMI;
static pthread_t ft8_thread;
static int ft8_tx1st = 1;
void ft8_tx(char *message, int freq);
void ft8_process(char *message, int operation);

// BANDTAG: label every FT8 RX/TX console line with the band it happened on,
// and print a banner line whenever the dial moves (band/freq history in the log)
extern int freq_hdr;   // current tuned freq in Hz (sbitx.c)
static int bandtag_last = 0;
static int ft8_slot_freq = 0;   //freq when the current rx slot started
static int ft8_decode_freq = 0; //freq of the slot being decoded/printed
static const char *band_tag_of(int f){
	int k = f / 1000;
	if (k >= 1800  && k <= 2000)  return "160m";
	if (k >= 3500  && k <= 4000)  return "80m";
	if (k >= 5250  && k <= 5410)  return "60m";
	if (k >= 7000  && k <= 7300)  return "40m";
	if (k >= 10100 && k <= 10150) return "30m";
	if (k >= 14000 && k <= 14350) return "20m";
	if (k >= 18068 && k <= 18168) return "17m";
	if (k >= 21000 && k <= 21450) return "15m";
	if (k >= 24890 && k <= 24990) return "12m";
	if (k >= 28000 && k <= 29700) return "10m";
	return "??m";
}
static void bandtag_banner(){
	if (freq_hdr > 0 && freq_hdr != bandtag_last){
		char bb[64];
		sprintf(bb, "--- %s dial %d.%03d MHz ---\n", band_tag_of(freq_hdr),
			freq_hdr/1000000, (freq_hdr%1000000)/1000);
		write_console(FONT_LOG, bb);
		bandtag_last = freq_hdr;
	}
}
static void ft8_log_csv(const char *dir, int fq, int score, int snr, int pitch, int pw10, int vswr10, const char *text){
	FILE *lf = fopen("/home/pi/sbitx/data/ft8_decodes.csv", "a");
	if (!lf)
		return;
	time_t rt = time_sbitx();
	struct tm *tt = gmtime(&rt);
	fprintf(lf, "%04d-%02d-%02d,%02d:%02d:%02d,%s,%s,%d,%d,%d,%d,%d,%d,\"%s\"\n",
		tt->tm_year+1900, tt->tm_mon+1, tt->tm_mday,
		tt->tm_hour, tt->tm_min, tt->tm_sec,
		dir, band_tag_of(fq), fq, score, snr, pitch, pw10, vswr10, text);
	fclose(lf);
}

#ifndef FT8_START_QSO
#define FT8_START_QSO 1
#endif
// ---- HUNT: auto-answer CQs; ROBO adds auto band-hopping (see robo_tick in sbitx_gtk.c) ----
int ft8_hunt_active = 0;              // cached: FT8_AUTO is HUNT or ROBO
#define HUNT_MAX 512
static struct { char call[14]; int tries; time_t next_ok;
	int last_snr, prev_snr, heard; time_t last_heard; char line[80]; } hunt_tab[HUNT_MAX];
static int hunt_tab_n = 0;
static int hunt_tab_loaded = 0;
static char hunt_target[14];          // station we are working right now
static int hunt_target_seen = 0;      // target decoded in this batch
static int hunt_target_busy = 0;      // target seen working someone else
static int hunt_target_miss = 0;      // consecutive batches without hearing target
static int hunt_target_snr = -99;
static int hunt_qso_slots = 0;        // watchdog: 15s slots since target last heard

// ---- band preselect + SWR blacklist for auto modes ----
static time_t swr_bad_until[8];        // per band: no auto TX until this time
static int band_idx_of(int f){
	int k = f / 1000;
	if (k >= 3500 && k <= 4000) return 0;
	if (k >= 7000 && k <= 7300) return 1;
	if (k >= 10100 && k <= 10150) return 2;
	if (k >= 14000 && k <= 14350) return 3;
	if (k >= 18068 && k <= 18168) return 4;
	if (k >= 21000 && k <= 21450) return 5;
	if (k >= 24890 && k <= 24990) return 6;
	if (k >= 28000 && k <= 29700) return 7;
	return -1;
}
void hunt_swr_bad_set(int f){
	int i = band_idx_of(f);
	if (i >= 0)
		swr_bad_until[i] = time(NULL) + 3600;
}
static int hunt_band_listed(int f){
	static char list[100];
	static time_t loaded = 0;
	if (time(NULL) - loaded > 60){
		loaded = time(NULL);
		list[0] = 0;
		FILE *bf = fopen("/home/pi/sbitx/data/robo_bands.txt", "r");
		if (bf){
			if (!fgets(list, sizeof(list) - 1, bf))
				list[0] = 0;
			fclose(bf);
		}
	}
	if (!list[0] || strstr(list, "all"))
		return 1;
	return strstr(list, band_tag_of(f)) != NULL;
}
int hunt_band_ok(int f){               // also used by robo_apply
	int i = band_idx_of(f);
	if (i < 0)
		return 0;
	if (time(NULL) < swr_bad_until[i])
		return 0;
	return hunt_band_listed(f);
}

// ---- pick the clearest TX audio offset from the last decode batch ----
static int busy_hz[80];
static int busy_n = 0;
static time_t busy_ts = 0;
static void txbest_note(int hz){
	if (time(NULL) - busy_ts > 20){ busy_n = 0; busy_ts = time(NULL); }
	if (busy_n < 80)
		busy_hz[busy_n++] = hz;
}
int txbest_pick(){
	int list[84], n = 0;
	list[n++] = 250;
	for (int i = 0; i < busy_n; i++)
		if (busy_hz[i] > 200 && busy_hz[i] < 3000)
			list[n++] = busy_hz[i];
	list[n++] = 2950;
	for (int i = 1; i < n; i++){
		int v = list[i], j = i - 1;
		while (j >= 0 && list[j] > v){ list[j+1] = list[j]; j--; }
		list[j+1] = v;
	}
	int bi = 1, bg = 0;
	for (int i = 1; i < n; i++)
		if (list[i] - list[i-1] > bg){ bg = list[i] - list[i-1]; bi = i; }
	int pick = (list[bi] + list[bi-1]) / 2;
	if (pick < 300) pick = 300;
	if (pick > 2900) pick = 2900;
	return pick;
}

// console color by message type
static int hunt_cq_caller(const char *text, char *out);
static void hunt_note_heard(const char *text);
int hunt_worked_before(const char *call);
int hunt_skipped(const char *call);
static int ft8_line_font(const char *text){
	if (!strncmp(text, "CQ ", 3)){
		char hc[16];
		if (hunt_cq_caller(text, hc) && (hunt_worked_before(hc) || hunt_skipped(hc)))
			return FONT_FT8_QUEUED; // dim: already worked or skip-listed
		return FONT_FT8_CQ;
	}
	const char *sp = strrchr(text, ' ');
	const char *last = sp ? sp + 1 : text;
	if (!strcmp(last, "73") || !strcmp(last, "RR73") || !strcmp(last, "RRR"))
		return FONT_FT8_73;
	if (last[0] == '-' || last[0] == '+' ||
		(last[0] == 'R' && (last[1] == '-' || last[1] == '+')))
		return FONT_FT8_REPORT;
	return FONT_FT8_RX;
}

// ---- TX metering + per-QSO logging ----
extern int fwdpower, vswr;            // sbitx.c: deciwatts / vswr*10, live during TX
static int tx_peak_pw10 = 0, tx_peak_vswr10 = 0;
static int last_tx_pw10 = 0, last_tx_vswr10 = 0;

// longest-prefix country lookup from data/prefixes.csv
static char pfx_tab[420][2][24];
static int pfx_n = -1;
static const char *country_of(const char *call){
	if (pfx_n < 0){
		char ln[80];
		pfx_n = 0;
		FILE *cf = fopen("/home/pi/sbitx/data/prefixes.csv", "r");
		if (cf){
			fgets(ln, sizeof(ln), cf); // header
			while (pfx_n < 420 && fgets(ln, sizeof(ln), cf)){
				char *c = strchr(ln, ',');
				if (!c) continue;
				*c = 0;
				char *e = c + 1;
				e[strcspn(e, "\r\n")] = 0;
				strncpy(pfx_tab[pfx_n][0], ln, 23);
				strncpy(pfx_tab[pfx_n][1], e, 23);
				pfx_n++;
			}
			fclose(cf);
		}
	}
	int bi = -1, bl = 0;
	for (int i = 0; i < pfx_n; i++){
		int l = strlen(pfx_tab[i][0]);
		if (l > bl && !strncmp(call, pfx_tab[i][0], l)){ bl = l; bi = i; }
	}
	return bi >= 0 ? pfx_tab[bi][1] : "?";
}

// one row per completed QSO: date,utc,band,dial,call,grid,sent,recv,country,pw10,vswr10
void ft8_qso_csv(){
	FILE *qf = fopen("/home/pi/sbitx/data/qso_log.csv", "a");
	if (!qf)
		return;
	time_t rt = time_sbitx();
	struct tm *tt = gmtime(&rt);
	const char *call = field_str("CALL");
	if (strlen(call) < 3){
		fclose(qf);
		return;
	}
	fprintf(qf, "%04d-%02d-%02d,%02d:%02d:%02d,%s,%d,\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",%d,%d\n",
		tt->tm_year+1900, tt->tm_mon+1, tt->tm_mday, tt->tm_hour, tt->tm_min, tt->tm_sec,
		band_tag_of(freq_hdr), freq_hdr, call, field_str("EXCH"), field_str("SENT"),
		field_str("RECV"), country_of(call), last_tx_pw10, last_tx_vswr10);
	fclose(qf);
}

static int hunt_tab_find(const char *call){
	for (int i = 0; i < hunt_tab_n; i++)
		if (!strcmp(hunt_tab[i].call, call))
			return i;
	return -1;
}

static int hunt_tab_addslot(const char *call){
	int i = hunt_tab_find(call);
	if (i >= 0)
		return i;
	if (!call[0] || strlen(call) > 12)
		return -1;
	if (hunt_tab_n >= HUNT_MAX){ // forget the oldest half
		memmove(&hunt_tab[0], &hunt_tab[HUNT_MAX/2], (HUNT_MAX/2) * sizeof(hunt_tab[0]));
		hunt_tab_n = HUNT_MAX/2;
	}
	i = hunt_tab_n++;
	strcpy(hunt_tab[i].call, call);
	hunt_tab[i].tries = 0;
	hunt_tab[i].next_ok = 0;
	hunt_tab[i].last_snr = hunt_tab[i].prev_snr = -99;
	hunt_tab[i].heard = 0;
	hunt_tab[i].last_heard = 0;
	hunt_tab[i].line[0] = 0;
	return i;
}

static void hunt_mark_worked(const char *call){
	int i = hunt_tab_addslot(call);
	if (i >= 0)
		hunt_tab[i].tries = 99; // never call again
}

// pre-TX check: load stations already in the QSO log so we never call them again
static void hunt_load_log(){
	if (hunt_tab_loaded)
		return;
	hunt_tab_loaded = 1;
	FILE *qf = fopen("/home/pi/sbitx/data/qso_log.csv", "r");
	if (!qf)
		return;
	char ln[256];
	while (fgets(ln, sizeof(ln), qf)){
		char *p = strchr(ln, '"');       // first quoted field is the callsign
		if (!p) continue;
		char *q = strchr(p + 1, '"');
		if (!q || q - p - 1 > 12 || q == p + 1) continue;
		*q = 0;
		hunt_mark_worked(p + 1);
	}
	fclose(qf);
}

// extract the calling station from "CQ [DX/POTA/...] CALL [GRID]": first token with a digit
static int hunt_cq_caller(const char *text, char *out){
	char t[64], *tok, *sp;
	if (strncmp(text, "CQ ", 3) || strchr(text, '<'))
		return 0;
	strncpy(t, text, 63); t[63] = 0;
	tok = strtok_r(t, " ", &sp); // "CQ"
	while ((tok = strtok_r(NULL, " ", &sp))){
		int L = strlen(tok);
		if (strpbrk(tok, "0123456789") && L >= 3 && L <= 12){
			strcpy(out, tok);
			return 1;
		}
	}
	return 0;
}

// hunter response modes: 0 NORMAL, 1 MEDIUM, 2 HYPER (persisted in data/hunt_mode.txt)
static int hunt_mode_level = -1;
static void hunt_mode_load(){
	if (hunt_mode_level >= 0)
		return;
	hunt_mode_level = 0;
	FILE *f = fopen("/home/pi/sbitx/data/hunt_mode.txt", "r");
	if (f){
		if (fscanf(f, "%d", &hunt_mode_level) != 1)
			hunt_mode_level = 0;
		fclose(f);
	}
	if (hunt_mode_level < 0 || hunt_mode_level > 2)
		hunt_mode_level = 0;
}
void hunt_mode_set(int m){
	hunt_mode_level = (m < 0 || m > 2) ? 0 : m;
	FILE *f = fopen("/home/pi/sbitx/data/hunt_mode.txt", "w");
	if (f){ fprintf(f, "%d\n", hunt_mode_level); fclose(f); }
}
static int hm_snr_min(){ return hunt_mode_level == 2 ? -24 : hunt_mode_level == 1 ? -21 : -18; }
static int hm_wd_slots(){ return hunt_mode_level == 2 ? 4 : hunt_mode_level == 1 ? 6 : 10; }
static int hm_tries(){ return hunt_mode_level == 2 ? 2 : 3; }
static int hm_backoff(int tries){
	if (hunt_mode_level == 2)
		return 30;
	return (hunt_mode_level == 1 ? 60 : 120) * (1 << (tries - 1));
}
int hunt_worked_before(const char *call){
	hunt_load_log();
	int i = hunt_tab_find(call);
	return i >= 0 && hunt_tab[i].tries >= 99;
}

// prefix skip list (data/hunt_skip.txt, e.g. "VU"): the hunter ignores CQs from
// these prefixes. Stations calling us directly are still answered (separate path).
static char hunt_skip[100];
static time_t hunt_skip_loaded = 0;
int hunt_skipped(const char *call){
	if (time(NULL) - hunt_skip_loaded > 60){
		hunt_skip_loaded = time(NULL);
		hunt_skip[0] = 0;
		FILE *f = fopen("/home/pi/sbitx/data/hunt_skip.txt", "r");
		if (f){
			if (!fgets(hunt_skip, sizeof(hunt_skip) - 1, f))
				hunt_skip[0] = 0;
			fclose(f);
		}
	}
	if (!hunt_skip[0])
		return 0;
	char tmp[100], *tok, *sp;
	strcpy(tmp, hunt_skip);
	tok = strtok_r(tmp, " \r\n", &sp);
	while (tok){
		if (strcmp(tok, "none") && !strncmp(call, tok, strlen(tok)))
			return 1;
		tok = strtok_r(NULL, " \r\n", &sp);
	}
	return 0;
}

static int hunt_mode_on(){
	const char *s = field_str("FT8_AUTO");
	return s && (!strcmp(s, "HUNT") || !strcmp(s, "ROBO"));
}

// every decoded line: watch our target (fading? gone with someone else?)
static void hunt_note_heard(const char *text){
	char t[64], *to, *from, *sp;
	if (!hunt_target[0])
		return;
	strncpy(t, text, 63); t[63] = 0;
	to = strtok_r(t, " ", &sp);
	from = strtok_r(NULL, " ", &sp);
	if (!to || !from)
		return;
	if (!strcmp(from, hunt_target)){
		hunt_target_seen = 1;
		if (strcmp(to, "CQ") && strcmp(to, field_str("MYCALLSIGN")))
			hunt_target_busy = 1;
	}
}

// every fresh CQ goes into the queue with its stats
static void hunt_consider(const char *text, int snr, const char *line, const char *mycall_up){
	char hc[16];
	if (!ft8_hunt_active || snr < hm_snr_min())
		return;
	if (!hunt_cq_caller(text, hc))
		return;
	if (!strcmp(hc, mycall_up) || hunt_skipped(hc))
		return;
	hunt_load_log();
	int hi = hunt_tab_addslot(hc);
	if (hi < 0 || hunt_tab[hi].tries >= 90) // worked already
		return;
	hunt_tab[hi].prev_snr = hunt_tab[hi].heard ? hunt_tab[hi].last_snr : snr;
	hunt_tab[hi].last_snr = snr;
	hunt_tab[hi].heard++;
	hunt_tab[hi].last_heard = time(NULL);
	strncpy(hunt_tab[hi].line, line, 79);
	hunt_tab[hi].line[79] = 0;
}

// best station to call right now: strong, rising, recently heard, not backing off
static int hunt_queue_best(){
	time_t now = time(NULL);
	int best = -1, best_score = -999;
	for (int i = 0; i < hunt_tab_n; i++){
		if (hunt_tab[i].tries >= hm_tries() || hunt_tab[i].tries >= 90)
			continue;
		if (!hunt_tab[i].line[0] || now < hunt_tab[i].next_ok)
			continue;
		if (now - hunt_tab[i].last_heard > 90) // faded out / gone
			continue;
		int trend = hunt_tab[i].last_snr - hunt_tab[i].prev_snr;
		int score = hunt_tab[i].last_snr + (trend > 0 ? 3 : trend < -3 ? -5 : 0);
		if (score > best_score){ best_score = score; best = i; }
	}
	return best;
}

int hunt_queue_depth(){
	time_t now = time(NULL);
	int n = 0;
	for (int i = 0; i < hunt_tab_n; i++)
		if (hunt_tab[i].tries < hm_tries() && hunt_tab[i].tries < 90 && hunt_tab[i].line[0]
			&& now >= hunt_tab[i].next_ok && now - hunt_tab[i].last_heard <= 90)
			n++;
	return n;
}

void hunt_queue_report(){
	char ln[90];
	time_t now = time(NULL);
	int shown = 0;
	write_console(FONT_LOG, "queue: call snr trend heard tries age\n");
	for (int i = 0; i < hunt_tab_n && shown < 8; i++){
		if (!hunt_tab[i].line[0] || hunt_tab[i].tries >= 90)
			continue;
		if (now - hunt_tab[i].last_heard > 300)
			continue;
		sprintf(ln, "%c%-10s %3d %+3d x%d t%d %lds\n",
			!strcmp(hunt_tab[i].call, hunt_target) ? '>' : ' ',
			hunt_tab[i].call, hunt_tab[i].last_snr,
			hunt_tab[i].last_snr - hunt_tab[i].prev_snr, hunt_tab[i].heard,
			hunt_tab[i].tries, (long)(now - hunt_tab[i].last_heard));
		write_console(FONT_LOG, ln);
		shown++;
	}
	if (!shown)
		write_console(FONT_LOG, "(empty)\n");
}

static void hunt_start(int hi){
	char note[110], line[256];
	hunt_tab[hi].tries++;
	hunt_tab[hi].next_ok = time(NULL) + hm_backoff(hunt_tab[hi].tries);
	hunt_qso_slots = 0;
	hunt_target_seen = hunt_target_busy = hunt_target_miss = 0;
	strcpy(hunt_target, hunt_tab[hi].call);
	hunt_target_snr = hunt_tab[hi].last_snr;
	{	char ps[12]; //answer on the clearest audio offset
		sprintf(ps, "%d", txbest_pick());
		field_set("TX_PITCH", ps);
	}
	sprintf(note, "HUNT: answering %s (%d dB, try %d/%d, %d more in queue)\n",
		hunt_tab[hi].call, hunt_tab[hi].last_snr, hunt_tab[hi].tries, hm_tries(),
		hunt_queue_depth() - 1);
	write_console(FONT_LOG, note);
	strcpy(line, hunt_tab[hi].line);
	ft8_process(line, FT8_START_QSO);
}

// after each decode batch: never waste a TX slot on a dead target
static void hunt_pick(){
	ft8_hunt_active = hunt_mode_on();
	hunt_mode_load();
	if (!ft8_hunt_active)
		return;
	if (strlen(field_str("CALL"))){
		if (hunt_target[0]){
			if (hunt_target_busy){
				char note[80];
				sprintf(note, "HUNT: %s went with another station, next!\n", hunt_target);
				write_console(FONT_LOG, note);
				ft8_abort();
				call_wipe();
				hunt_target[0] = 0;
			}
			else if (!hunt_target_seen && ++hunt_target_miss >= 2){
				int nb = hunt_queue_best();
				if (nb >= 0 && hunt_tab[nb].last_snr >= hunt_target_snr - 3){
					char note[100];
					sprintf(note, "HUNT: %s fading (2 slots quiet), trying %s\n",
						hunt_target, hunt_tab[nb].call);
					write_console(FONT_LOG, note);
					ft8_abort();
					call_wipe();
					hunt_target[0] = 0;
				}
			}
		}
		hunt_target_seen = hunt_target_busy = 0;
		if (strlen(field_str("CALL")))
			return;
	}
	if (!hunt_band_ok(freq_hdr))
		return;
	hunt_load_log();
	int hi = hunt_queue_best();
	if (hi >= 0)
		hunt_start(hi);
}
void ft8_interpret(char *received, char *transmit);
extern void call_wipe();

// how to handle a command option
#define FT8_START_QSO 1
#define FT8_CONTINUE_QSO 0
static unsigned int wallclock =0;
static const int kMin_score = 10; // Minimum sync score threshold for candidates
static const int kMax_candidates = 120;
static const int kLDPC_iterations = 20;

static const int kMax_decoded_messages = 50;

static const int kFreq_osr = 2; // Frequency oversampling rate (bin subdivision)
static const int kTime_osr = 2; // Time oversampling rate (symbol subdivision)

#define LOG_LEVEL LOG_INFO

#define FT8_SYMBOL_BT 2.0f ///< symbol smoothing filter bandwidth factor (BT)
#define FT4_SYMBOL_BT 1.0f ///< symbol smoothing filter bandwidth factor (BT)

#define GFSK_CONST_K 5.336446f ///< == pi * sqrt(2 / log(2))

/// Computes a GFSK smoothing pulse.
/// The pulse is theoretically infinitely long, however, here it's truncated at 3 times the symbol length.
/// This means the pulse array has to have space for 3*n_spsym elements.
/// @param[in] n_spsym Number of samples per symbol
/// @param[in] b Shape parameter (values defined for FT8/FT4)
/// @param[out] pulse Output array of pulse samples
///
static void gfsk_pulse(int n_spsym, float symbol_bt, float* pulse)
{
    for (int i = 0; i < 3 * n_spsym; ++i)
    {
        float t = i / (float)n_spsym - 1.5f;
        float arg1 = GFSK_CONST_K * symbol_bt * (t + 0.5f);
        float arg2 = GFSK_CONST_K * symbol_bt * (t - 0.5f);
        pulse[i] = (erff(arg1) - erff(arg2)) / 2;
    }
}

/// Synthesize waveform data using GFSK phase shaping.
/// The output waveform will contain n_sym symbols.
/// @param[in] symbols Array of symbols (tones) (0-7 for FT8)
/// @param[in] n_sym Number of symbols in the symbol array
/// @param[in] f0 Audio frequency in Hertz for the symbol 0 (base frequency)
/// @param[in] symbol_bt Symbol smoothing filter bandwidth (2 for FT8, 1 for FT4)
/// @param[in] symbol_period Symbol period (duration), seconds
/// @param[in] signal_rate Sample rate of synthesized signal, Hertz
/// @param[out] signal Output array of signal waveform samples (should have space for n_sym*n_spsym samples)
///
static void synth_gfsk(const uint8_t* symbols, int n_sym, float f0, float symbol_bt, float symbol_period, int signal_rate, float* signal)
{
    int n_spsym = (int)(0.5f + signal_rate * symbol_period); // Samples per symbol
    int n_wave = n_sym * n_spsym;                            // Number of output samples
    float hmod = 1.0f;

    LOG(LOG_DEBUG, "n_spsym = %d\n", n_spsym);
    // Compute the smoothed frequency waveform.
    // Length = (nsym+2)*n_spsym samples, first and last symbols extended
    float dphi_peak = 2 * M_PI * hmod / n_spsym;
    float dphi[n_wave + 2 * n_spsym];

    // Shift frequency up by f0
    for (int i = 0; i < n_wave + 2 * n_spsym; ++i)
    {
        dphi[i] = 2 * M_PI * f0 / signal_rate;
    }

    float pulse[3 * n_spsym];
    gfsk_pulse(n_spsym, symbol_bt, pulse);

    for (int i = 0; i < n_sym; ++i)
    {
        int ib = i * n_spsym;
        for (int j = 0; j < 3 * n_spsym; ++j)
        {
            dphi[j + ib] += dphi_peak * symbols[i] * pulse[j];
        }
    }

    // Add dummy symbols at beginning and end with tone values equal to 1st and last symbol, respectively
    for (int j = 0; j < 2 * n_spsym; ++j)
    {
        dphi[j] += dphi_peak * pulse[j + n_spsym] * symbols[0];
        dphi[j + n_sym * n_spsym] += dphi_peak * pulse[j] * symbols[n_sym - 1];
    }

    // Calculate and insert the audio waveform
    float phi = 0;
    for (int k = 0; k < n_wave; ++k)
    { // Don't include dummy symbols
        signal[k] = sinf(phi);
        phi = fmodf(phi + dphi[k + n_spsym], 2 * M_PI);
    }

    // Apply envelope shaping to the first and last symbols
    int n_ramp = n_spsym / 8;
    for (int i = 0; i < n_ramp; ++i)
    {
        float env = (1 - cosf(2 * M_PI * i / (2 * n_ramp))) / 2;
        signal[i] *= env;
        signal[n_wave - 1 - i] *= env;
    }
}


int sbitx_ft8_encode(char *message, int32_t freq,  float *signal, bool is_ft4)
{
    float frequency = 1.0 * freq;

    // First, pack the text data into binary message
    uint8_t packed[FTX_LDPC_K_BYTES];
    int rc = pack77(message, packed);
    if (rc < 0)
    {
        printf("Cannot parse message!\n");
        printf("RC = %d\n", rc);
        return -1;
    }

    int num_tones = (is_ft4) ? FT4_NN : FT8_NN;
    float symbol_period = (is_ft4) ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
    float symbol_bt = (is_ft4) ? FT4_SYMBOL_BT : FT8_SYMBOL_BT;
    float slot_time = (is_ft4) ? FT4_SLOT_TIME : FT8_SLOT_TIME;

    // Second, encode the binary message as a sequence of FSK tones
    uint8_t tones[num_tones]; // Array of 79 tones (symbols)
    if (is_ft4)
        ft4_encode(packed, tones);
    else
        ft8_encode(packed, tones);

    // Third, convert the FSK tones into an audio signal
    int sample_rate = 12000;
    int num_samples = (int)(0.5f + num_tones * symbol_period * sample_rate); // samples in the data signal
    int num_silence = (slot_time * sample_rate - num_samples) / 2;           // Silence  to make 15 seconds
    int num_total_samples = num_silence + num_samples + num_silence;         // total Number samples 

    for (int i = 0; i < num_silence; i++) {
        signal[i] = 0;
        signal[i + num_samples + num_silence] = 0;
    }

    // Synthesize waveform data (signal) and save it as WAV file
    synth_gfsk(tones, num_tones, frequency, symbol_bt, symbol_period, sample_rate, signal + num_silence);
    return num_total_samples;
}

static float hann_i(int i, int N)
{
    float x = sinf((float)M_PI * i / N);
    return x * x;
}

static float hamming_i(int i, int N)
{
    const float a0 = (float)25 / 46;
    const float a1 = 1 - a0;

    float x1 = cosf(2 * (float)M_PI * i / N);
    return a0 - a1 * x1;
}

static float blackman_i(int i, int N)
{
    const float alpha = 0.16f; // or 2860/18608
    const float a0 = (1 - alpha) / 2;
    const float a1 = 1.0f / 2;
    const float a2 = alpha / 2;

    float x1 = cosf(2 * (float)M_PI * i / N);
    float x2 = 2 * x1 * x1 - 1; // Use double angle formula

    return a0 - a1 * x1 + a2 * x2;
}

void waterfall_init(waterfall_t* me, int max_blocks, int num_bins, int time_osr, int freq_osr)
{
    size_t mag_size = max_blocks * time_osr * freq_osr * num_bins * sizeof(me->mag[0]);
    me->max_blocks = max_blocks;
    me->num_blocks = 0;
    me->num_bins = num_bins;
    me->time_osr = time_osr;
    me->freq_osr = freq_osr;
    me->block_stride = (time_osr * freq_osr * num_bins);
    me->mag = (uint8_t  *)malloc(mag_size);
    LOG(LOG_DEBUG, "Waterfall size = %zu\n", mag_size);
}

void waterfall_free(waterfall_t* me)
{
    free(me->mag);
}

/// Configuration options for FT4/FT8 monitor
typedef struct
{
    float f_min;             ///< Lower frequency bound for analysis
    float f_max;             ///< Upper frequency bound for analysis
    int sample_rate;         ///< Sample rate in Hertz
    int time_osr;            ///< Number of time subdivisions
    int freq_osr;            ///< Number of frequency subdivisions
    ftx_protocol_t protocol; ///< Protocol: FT4 or FT8
} monitor_config_t;

/// FT4/FT8 monitor object that manages DSP processing of incoming audio data
/// and prepares a waterfall object
typedef struct
{
    float symbol_period; ///< FT4/FT8 symbol period in seconds
    int block_size;      ///< Number of samples per symbol (block)
    int subblock_size;   ///< Analysis shift size (number of samples)
    int nfft;            ///< FFT size
    float fft_norm;      ///< FFT normalization factor
    float* window;       ///< Window function for STFT analysis (nfft samples)
    float* last_frame;   ///< Current STFT analysis frame (nfft samples)
    waterfall_t wf;      ///< Waterfall object
    float max_mag;       ///< Maximum detected magnitude (debug stats)

    // KISS FFT housekeeping variables
    void* fft_work;        ///< Work area required by Kiss FFT
    kiss_fftr_cfg fft_cfg; ///< Kiss FFT housekeeping object
} monitor_t;

static void monitor_init(monitor_t* me, const monitor_config_t* cfg)
{
    float slot_time = (cfg->protocol == PROTO_FT4) ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    float symbol_period = (cfg->protocol == PROTO_FT4) ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
    // Compute DSP parameters that depend on the sample rate
    me->block_size = (int)(cfg->sample_rate * symbol_period); // samples corresponding to one FSK symbol
    me->subblock_size = me->block_size / cfg->time_osr;
    me->nfft = me->block_size * cfg->freq_osr;
    me->fft_norm = 2.0f / me->nfft;
    // const int len_window = 1.8f * me->block_size; // hand-picked and optimized

    me->window = (float *)malloc(me->nfft * sizeof(me->window[0]));
    for (int i = 0; i < me->nfft; ++i)
    {
        // window[i] = 1;
        me->window[i] = hann_i(i, me->nfft);
        // me->window[i] = blackman_i(i, me->nfft);
        // me->window[i] = hamming_i(i, me->nfft);
        // me->window[i] = (i < len_window) ? hann_i(i, len_window) : 0;
    }
    me->last_frame = (float *)malloc(me->nfft * sizeof(me->last_frame[0]));

    size_t fft_work_size;
    kiss_fftr_alloc(me->nfft, 0, 0, &fft_work_size);

    //LOG(LOG_INFO, "Block size = %d\n", me->block_size);
    //LOG(LOG_INFO, "Subblock size = %d\n", me->subblock_size);
    //LOG(LOG_INFO, "N_FFT = %d\n", me->nfft);
    LOG(LOG_DEBUG, "FFT work area = %zu\n", fft_work_size);

    me->fft_work = malloc(fft_work_size);
    me->fft_cfg = kiss_fftr_alloc(me->nfft, 0, me->fft_work, &fft_work_size);

    const int max_blocks = (int)(slot_time / symbol_period);
    const int num_bins = (int)(cfg->sample_rate * symbol_period / 2);
    waterfall_init(&me->wf, max_blocks, num_bins, cfg->time_osr, cfg->freq_osr);
    me->wf.protocol = cfg->protocol;
    me->symbol_period = symbol_period;

    me->max_mag = -120.0f;
}

static void monitor_free(monitor_t* me)
{
    waterfall_free(&me->wf);
    free(me->fft_work);
    free(me->last_frame);
    free(me->window);
}

// Compute FFT magnitudes (log wf) for a frame in the signal and update waterfall data
static void monitor_process(monitor_t* me, const float* frame)
{
    // Check if we can still store more waterfall data
    if (me->wf.num_blocks >= me->wf.max_blocks)
        return;

    int offset = me->wf.num_blocks * me->wf.block_stride;
    int frame_pos = 0;

    // Loop over block subdivisions
    for (int time_sub = 0; time_sub < me->wf.time_osr; ++time_sub)
    {
        kiss_fft_scalar timedata[me->nfft];
        kiss_fft_cpx freqdata[me->nfft / 2 + 1];

        // Shift the new data into analysis frame
        for (int pos = 0; pos < me->nfft - me->subblock_size; ++pos)
        {
            me->last_frame[pos] = me->last_frame[pos + me->subblock_size];
        }
        for (int pos = me->nfft - me->subblock_size; pos < me->nfft; ++pos)
        {
            me->last_frame[pos] = frame[frame_pos];
            ++frame_pos;
        }

        // Compute windowed analysis frame
        for (int pos = 0; pos < me->nfft; ++pos)
        {
            timedata[pos] = me->fft_norm * me->window[pos] * me->last_frame[pos];
        }

        kiss_fftr(me->fft_cfg, timedata, freqdata);

        // Loop over two possible frequency bin offsets (for averaging)
        for (int freq_sub = 0; freq_sub < me->wf.freq_osr; ++freq_sub)
        {
            for (int bin = 0; bin < me->wf.num_bins; ++bin)
            {
                int src_bin = (bin * me->wf.freq_osr) + freq_sub;
                float mag2 = (freqdata[src_bin].i * freqdata[src_bin].i) + (freqdata[src_bin].r * freqdata[src_bin].r);
                float db = 10.0f * log10f(1E-12f + mag2);
                // Scale decibels to unsigned 8-bit range and clamp the value
                // Range 0-240 covers -120..0 dB in 0.5 dB steps
                int scaled = (int)(2 * db + 240);

                me->wf.mag[offset] = (scaled < 0) ? 0 : ((scaled > 255) ? 255 : scaled);
                ++offset;

                if (db > me->max_mag)
                    me->max_mag = db;
            }
        }
    }

    ++me->wf.num_blocks;
}

static void monitor_reset(monitor_t* me)
{
    me->wf.num_blocks = 0;
    me->max_mag = 0;
}

static int sbitx_ft8_decode(float *signal, int num_samples, bool is_ft8)
{
    int sample_rate = 12000;

    LOG(LOG_DEBUG, "Sample rate %d Hz, %d samples, %.3f seconds\n", sample_rate, num_samples, (double)num_samples / sample_rate);

    // Compute FFT over the whole signal and store it
    monitor_t mon;
    monitor_config_t mon_cfg = {
        .f_min = 100,
        .f_max = 3000,
        .sample_rate = sample_rate,
        .time_osr = kTime_osr,
        .freq_osr = kFreq_osr,
        .protocol = is_ft8 ? PROTO_FT8 : PROTO_FT4
    };

		//timestamp the packets
		//the time is shifted back by the time it took to capture these sameples
		time_t	rawtime = (time_sbitx() / 15) * 15; //round to the earlier slot
		char time_str[20], response[100];
		struct tm *t = gmtime(&rawtime);
		sprintf(time_str, "%02d%02d%02d", t->tm_hour, t->tm_min, t->tm_sec);

		int i;
		char mycallsign_upper[20];
		char mycallsign[20];
		get_field_value("#mycallsign", mycallsign);
		for (i = 0; i < strlen(mycallsign); i++)
			mycallsign_upper[i] = toupper(mycallsign[i]);
		mycallsign_upper[i] = 0;	

    monitor_init(&mon, &mon_cfg);

    // Process the waveform data frame by frame - you could have a live loop here with data from an audio device
    for (int frame_pos = 0; frame_pos + mon.block_size <= num_samples; frame_pos += mon.block_size)
        monitor_process(&mon, signal + frame_pos);
    
//    LOG(LOG_DEBUG, "Waterfall accumulated %d symbols\n", mon.wf.num_blocks);
//    LOG(LOG_INFO, "Max magnitude: %.1f dB\n", mon.max_mag);

    // Find top candidates by Costas sync score and localize them in time and frequency
    candidate_t candidate_list[kMax_candidates];
    int num_candidates = ft8_find_sync(&mon.wf, kMax_candidates, candidate_list, kMin_score);

    // Hash table for decoded messages (to check for duplicates)
    int num_decoded = 0;
    message_t decoded[kMax_decoded_messages];
    message_t* decoded_hashtable[kMax_decoded_messages];

    // Initialize hash table pointers
    for (int i = 0; i < kMax_decoded_messages; ++i)
    {
        decoded_hashtable[i] = NULL;
    }

		int n_decodes = 0;
    // Go over candidates and attempt to decode messages
    for (int idx = 0; idx < num_candidates; ++idx)
    {
        const candidate_t* cand = &candidate_list[idx];
        if (cand->score < kMin_score)
            continue;

        float freq_hz = (cand->freq_offset + (float)cand->freq_sub / mon.wf.freq_osr) / mon.symbol_period;
        float time_sec = (cand->time_offset + (float)cand->time_sub / mon.wf.time_osr) * mon.symbol_period;

        message_t message;
        decode_status_t status;
        if (!ft8_decode(&mon.wf, cand, &message, kLDPC_iterations, &status)){
            // printf("000000 %3d %+4.2f %4.0f ~  ---\n", cand->score, time_sec, freq_hz);
            if (status.ldpc_errors > 0)
                LOG(LOG_DEBUG, "LDPC decode: %d errors\n", status.ldpc_errors);
            else if (status.crc_calculated != status.crc_extracted)
                LOG(LOG_DEBUG, "CRC mismatch!\n");
            else if (status.unpack_status != 0)
                LOG(LOG_DEBUG, "Error while unpacking!\n");
            continue;
        }

        LOG(LOG_DEBUG, "Checking hash table for %4.1fs / %4.1fHz [%d]...\n", time_sec, freq_hz, cand->score);
        int idx_hash = message.hash % kMax_decoded_messages;
        bool found_empty_slot = false;
        bool found_duplicate = false;
        do {
            if (decoded_hashtable[idx_hash] == NULL) {
                LOG(LOG_DEBUG, "Found an empty slot\n");
                found_empty_slot = true;
            }
            else if ((decoded_hashtable[idx_hash]->hash == message.hash) && (0 == strcmp(decoded_hashtable[idx_hash]->text, message.text))) {
                LOG(LOG_DEBUG, "Found a duplicate [%s]\n", message.text);
                found_duplicate = true;
            }
            else {
                LOG(LOG_DEBUG, "Hash table clash!\n");
                // Move on to check the next entry in hash table
                idx_hash = (idx_hash + 1) % kMax_decoded_messages;
            }
        } while (!found_empty_slot && !found_duplicate);

        if (found_empty_slot) {
           // Fill the empty hashtable slot
           memcpy(&decoded[idx_hash], &message, sizeof(message));
           decoded_hashtable[idx_hash] = &decoded[idx_hash];
           ++num_decoded;

					char buff[1000];
          bandtag_banner();
          sprintf(buff, "%s %s %3d %+03d %-4.0f ~ %s\n", time_str, band_tag_of(ft8_decode_freq ? ft8_decode_freq : freq_hdr), 
						cand->score, cand->snr, freq_hz, message.text);
					ft8_log_csv("RX", ft8_decode_freq ? ft8_decode_freq : freq_hdr, cand->score, cand->snr, (int)freq_hz, 0, 0, message.text);

					hunt_note_heard(message.text);
					txbest_note((int)freq_hz);
					if (strstr(buff, mycallsign_upper)){
						write_console(FONT_FT8_REPLY, buff);
						ft8_process(buff, FT8_CONTINUE_QSO);
					}
					else {
						write_console(ft8_line_font(message.text), buff);
						hunt_consider(message.text, cand->snr, buff, mycallsign_upper);
					}

	//				save_message('R', cand->score, cand-snr,freq_hz, message.text);
					n_decodes++;
        }
    }
    //LOG(LOG_INFO, "Decoded %d messages\n", num_decoded);

    monitor_free(&mon);

    return n_decodes;
}

//this variable is a count of number of repititions left for the 
//current message, it is not the user setting of the same number
static int ft8_repeat = 5;

int sbitx_ft8_encode(char *message, int32_t freq,  float *signal, bool is_ft4);

void ft8_setmode(int config){
	switch(config){
		case FT8_MANUAL:
			ft8_mode = FT8_MANUAL;
			write_console(FONT_LOG, "FT8 is manual now.\nSend messages through the keyboard\n");
			break;
		case FT8_SEMI:
			write_console(FONT_LOG, "FT8 is semi-automatic.\nClick on the callsign to start the QSO\n");
			ft8_mode = FT8_SEMI;
			break;
		case FT8_AUTO:
			write_console(FONT_LOG, "FT8 is automatic.\nIt will call CQ and QSO with the first reply.\n");
			ft8_mode = FT8_AUTO;
			break;
	}
}

static void ft8_start_tx(int offset_seconds){
	char buff[1000];
	//timestamp the packets for display log
	time_t	rawtime = time_sbitx();
	struct tm *t = gmtime(&rawtime);

  bandtag_banner();
  sprintf(buff, "%02d%02d%02d %s TX +00 %04d ~ %s\n", t->tm_hour, t->tm_min, t->tm_sec, band_tag_of(freq_hdr), ft8_pitch, ft8_tx_text);
	write_console(FONT_FT8_TX, buff);
	ft8_log_csv("TX", freq_hdr, 0, 0, ft8_pitch, 0, 0, ft8_tx_text);

	ft8_tx_nsamples = sbitx_ft8_encode(ft8_tx_text, ft8_pitch, ft8_tx_buff, false); 
	ft8_tx_buff_index = offset_seconds * 96000;
}

// the ft8_tx() only schedules the transmission
// it is picked up by ft8_poll to do the actuall transmission
void ft8_tx(char *message, int freq){
	char cmd[200], buff[1000];
	FILE	*pf;
	time_t	rawtime = time_sbitx();
	struct tm *t = gmtime(&rawtime);

	for (int i = 0; i < strlen(message); i++)
		message[i] = toupper(message[i]);
	strcpy(ft8_tx_text, message);

	ft8_pitch = freq;
  bandtag_banner();
  sprintf(buff, "%02d%02d%02d %s TX +00 %04d ~ %s\n", t->tm_hour, t->tm_min, t->tm_sec, band_tag_of(freq_hdr), ft8_pitch, ft8_tx_text);
	write_console(FONT_FT8_QUEUED, buff);

	//also set the times of transmission
	char str_tx1st[10], str_repeat[10];
	get_field_value_by_label("FT8_TX1ST", str_tx1st);
	get_field_value_by_label("FT8_REPEAT", str_repeat);
	int slot_second = time_sbitx() % 15;

	//the FT8_TX1ST setting is only to initiate a CQ call
	//if we are not transmitting CQ, then we follow
	//the slot selected earlier in ft8_process()

	if (!strncmp(message, "CQ", 2)){ 
		if(!strcmp(str_tx1st, "ON"))
			ft8_tx1st = 1;
		else
			ft8_tx1st = 0;
	}

	//no repeat for '73'
	int msg_length = strlen(message);
	if (msg_length > 3 && !strcmp(message + msg_length - 3, " 73")){
		ft8_repeat = 1;
	} 
	else
		ft8_repeat = atoi(str_repeat);

	// if it is a CQ message, then wait for the slot
	if (!strncmp(ft8_tx_text, "CQ ", 3))
		return;

	//figure out how many samples can be transmitted in this current slot
	int index = (slot_second % 15) * 96000;
}

void *ft8_thread_function(void *ptr){
	FILE *pf;
	char buff[1000], mycallsign_upper[20]; //there are many ways to crash sbitx, bufferoverflow of callsigns is 1

	//wake up every 100 msec to see if there is anything to decode
	while(1){
		usleep(1000);

		if (!ft8_do_decode)
			continue;

		ft8_do_decode = 0;
		sbitx_ft8_decode(ft8_rx_buffer, ft8_rx_buff_index, true);
		hunt_pick();
		//let the next batch begin
		ft8_rx_buff_index = 0;
	}
}

// the ft8 sampling is at 12000, the incoming samples are at
// 96000 samples/sec
void ft8_rx(int32_t *samples, int count){

	int decimation_ratio = 96000/12000;

	//if there is an overflow, then reset to the begining
	if (ft8_rx_buff_index + (count/decimation_ratio) >= FT8_MAX_BUFF){
		ft8_rx_buff_index = 0;		
		printf("Buffer Overflow\n");
	}

	//down convert to 12000 Hz sampling rate
	for (int i = 0; i < count; i += decimation_ratio)
		//ft8_rx_buff[ft8_rx_buff_index++] = samples[i];
		ft8_rx_buffer[ft8_rx_buff_index++] = samples[i] / 200000000.0f;

	int now = time_sbitx();
	if (now != wallclock)	
		wallclock = now;
	else 
		return;

	int slot_second = wallclock % 15;
	if (slot_second == 0){
		ft8_rx_buff_index = 0;
		ft8_slot_freq = freq_hdr; //the band this slot's audio is captured on
	}

//	printf("ft8 decoding trigger index %d, slot_second %d\n", ft8_rx_buff_index, slot_second);
	//we should have atleast 12 seconds of samples to decode
	if (ft8_rx_buff_index >= 13 * 12000 && slot_second > 13){
		ft8_decode_freq = ft8_slot_freq ? ft8_slot_freq : freq_hdr;
		ft8_do_decode = 1;
	}
}

void ft8_poll(int seconds, int tx_is_on){
	static int last_second = 0;
	static int last_wd = -1;
	ft8_hunt_active = hunt_mode_on();
	if (!tx_is_on && (seconds % 15) == 0 && seconds != last_wd){
		last_wd = seconds;
		if (ft8_hunt_active && strlen(field_str("CALL"))){
			if (++hunt_qso_slots > hm_wd_slots()){
				write_console(FONT_LOG, "HUNT: no reply, moving on\n");
				ft8_abort();
				call_wipe();
				hunt_qso_slots = 0;
			}
		}
	}

	//if we are already transmitting, we continue 
	//until we run out of ft8 sampels
	if (tx_is_on){
		static time_t swr_bad_since = 0;
		if (fwdpower > tx_peak_pw10){ tx_peak_pw10 = fwdpower; tx_peak_vswr10 = vswr; }
		// live SWR probe: the first seconds of every auto TX are the test.
		// sustained SWR > 1.9 kills the transmission NOW, not at its end.
		if (ft8_hunt_active && fwdpower > 20){ // >2W out: bridge reading is valid
			if (vswr > 19){
				if (!swr_bad_since)
					swr_bad_since = time(NULL);
				else if (time(NULL) - swr_bad_since >= 2){
					char sn[90];
					sprintf(sn, "SWR %d.%d high on %s - TX ABORTED, band blocked 1h\n",
						vswr/10, vswr%10, band_tag_of(freq_hdr));
					write_console(FONT_LOG, sn);
					hunt_swr_bad_set(freq_hdr);
					{ extern int robo_swr_flee; robo_swr_flee = 1; }
					swr_bad_since = 0;
					tx_peak_pw10 = 0;
					ft8_abort();
					tx_off();
					call_wipe();
					return;
				}
			}
			else
				swr_bad_since = 0;
		}
		else if (!vswr || vswr <= 19)
			swr_bad_since = 0;
		//tx_off should not abort repeats from modem_poll, when called from here
		int ft8_repeat_save = ft8_repeat;
		if (ft8_tx_nsamples == 0){
			tx_off();
			ft8_repeat = ft8_repeat_save;
			if (tx_peak_pw10 > 0){ //log the measured peak of this transmission
				last_tx_pw10 = tx_peak_pw10;
				last_tx_vswr10 = tx_peak_vswr10;
				ft8_log_csv("TXEND", freq_hdr, 0, 0, ft8_pitch, last_tx_pw10, last_tx_vswr10, ft8_tx_text);
				tx_peak_pw10 = 0;
				if (ft8_hunt_active && last_tx_vswr10 > 19){ //SWR safety for auto modes
					char sn[80];
					sprintf(sn, "SWR %d.%d high! auto TX stopped on %s for 1 hour\n",
						last_tx_vswr10/10, last_tx_vswr10%10, band_tag_of(freq_hdr));
					write_console(FONT_LOG, sn);
					hunt_swr_bad_set(freq_hdr);
					ft8_abort();
					call_wipe();
				}
			}
		}
		return;
	}
	
	if (!ft8_repeat || seconds == last_second)
		return;

	//we poll for this only once every second
	//we are here only if we are rx-ing and we have a pending transmission 
	last_second = seconds = seconds % 60;

	if (
		(ft8_tx1st == 1 && ((seconds >= 0  && seconds < 15) ||
			(seconds >=30 && seconds < 45))) ||
		(ft8_tx1st == 0 && ((seconds >= 15 && seconds < 30)|| 
			(seconds >= 45 && seconds < 59)))){
		tx_on(TX_SOFT);
		ft8_start_tx(seconds % 15);
		ft8_repeat--;
	} 
}

float ft8_next_sample(){
		float sample = 0;
		if (ft8_tx_buff_index/8 < ft8_tx_nsamples){
			sample = ft8_tx_buff[ft8_tx_buff_index/8]/7;
			ft8_tx_buff_index++;
		}
		else //stop transmitting ft8 
			ft8_tx_nsamples = 0;
		return sample;
}

/* these are used to process the current message */
static char m1[32], m2[32], m3[32], m4[32], signal_strength[10], mygrid[10],
	reply_message[100];
static int rx_pitch, tx_pitch, confidence_score, msg_time; 
static const char *call, *exchange, *report_send, *report_received, *mycall;

int ft8_message_tokenize(char *message){
	char *p;

	//tokenize the message
	p = strtok(message, " \r\n");
	if (!p) return -1;
	msg_time = atoi(p);

	p = strtok(NULL, " \r\n");
	if (!p) return -1;
	//skip the BANDTAG column ("40m"/"160m"/"??m") so old positional parsing still works
	{	int L = strlen(p);
		if (L >= 2 && L <= 4 && p[L-1] == 'm' && (isdigit((unsigned char)p[0]) || p[0] == '?')){
			p = strtok(NULL, " \r\n");
			if (!p) return -1;
		}
	}
	confidence_score = atoi(p);

	p = strtok(NULL, " \r\n");
	if (!p) return -1;
	strcpy(signal_strength, p);

	p = strtok(NULL, " \r\n");
	if (!p) return -1;
	rx_pitch = atoi(p);

	//santiy check, we should get a tilde '~' now
	p = strtok(NULL, " \r\n");
	if (!p)
		return -1;
	if (strcmp(p, "~"))
		return -1;

	p = strtok(NULL, " \r\n");
	if (!p) return -1;
	strcpy(m1, p);

	p = strtok(NULL, " \r\n");
	if (!p) return -1;
	strcpy(m2, p);

	p = strtok(NULL, " \r\n");
	if (p){
		strcpy(m3, p);

		p = strtok(NULL, " \r\n");
		if (p){
			strcpy(m4, p);
		}
		else 
			m4[0] = 0;
	}
	else
		m3[0];

	return 0;
}

// this kicks stars a new qso either as a CQ message or
// as a reply to someone's cq or as a 'break' with signal report to
// a concluding qso
void ft8_on_start_qso(char *message){
	modem_abort();
	tx_off();
	call_wipe();

	//for cq message that started on 0 or 30th second, use the 15 or 45 and
	//vice versa
	int msg_second = msg_time % 100; 	
	if (msg_second < 15 || (msg_second >= 30 && msg_second < 45))
		ft8_tx1st = 0; //we tx on 2nd and 4ht slots for msgs on 1st and 3rd
	else
		ft8_tx1st = 1;

	if (!strcmp(m1, "CQ")){
		if (m4[0]){
			field_set("CALL", m3);
			field_set("EXCH", m4);
			field_set("SENT", signal_strength);
		}
		else {
			field_set("CALL", m2);
			field_set("EXCH", m3);
			field_set("SENT", signal_strength);
		}
		sprintf(reply_message, "%s %s %s", call, mycall, mygrid);
	}
	//whoa, someone cold called us
	else if (!strcmp(m1, mycall)){
		field_set("CALL", m2);
		field_set("SENT", signal_strength);
		//they might have directly sent us a signal report
		if (isalpha(m3[0])){
			field_set("EXCH", m3);
			sprintf(reply_message, "%s %s %s", call, mycall, signal_strength);
		}
		else {
			field_set("RECV", m3);
			sprintf(reply_message, "%s %s R%s", call, mycall, signal_strength);
		}
	}
	else { //we are breaking into someone else's qso
		field_set("CALL", m2);
		field_set("EXCH", "");
		field_set("SENT", signal_strength);
		sprintf(reply_message, "%s %s %s", call, mycall, signal_strength);
	}
	field_set("NR", mygrid);
	ft8_tx(reply_message, tx_pitch);
}

void ft8_on_signal_report(){
	field_set("CALL", m2);
	if (m3[0] == 'R'){
		//skip the 'R'
		field_set("RECV", m3+1);
		sprintf(reply_message, "%s %s RRR", call, mycall);  	
		ft8_tx(reply_message, tx_pitch);
	}
	else{ 
		field_set("RECV", m3);	
		sprintf(reply_message, "%s %s R%s", call, mycall, report_send);  	
		ft8_tx(reply_message, tx_pitch);
	}
	enter_qso();
}

void ft8_process(char *message, int operation){
	char buff[100], reply_message[100], *p;
	int auto_respond = 0;

	if (ft8_message_tokenize(message) == -1)
		return;

	call = field_str("CALL");
	exchange = field_str("EXCH");
	report_send = field_str("SENT");
	report_received = field_str("RECV");
	mycall = field_str("MYCALLSIGN");
	tx_pitch = field_int("TX_PITCH");
	if (strcmp(field_str("FT8_AUTO"), "OFF"))
		auto_respond = 1;

	//use only the first 4 letters of the grid
	strcpy(mygrid, field_str("MYGRID"));
	mygrid[4] = 0;

	//we can start call in reply to a cq, cq dx or anyone else ending the call
	if (operation == FT8_START_QSO){
		ft8_on_start_qso(message);
		return;
	}

	// see if you are on auto responder, the logger is empty and we are the called party
	if (auto_respond && !strlen(call) && !strcmp(m1, mycall)){
		ft8_on_start_qso(message);
		return;
	}

	//by now, any message that comes to us should have our callsign as m1
	if (strcmp(m1, mycall)){
		printf("FT8: Not a message for %s\n", mycall);
		return;
	}
	hunt_qso_slots = 0;


	if (!strcmp(m3, "73")){
		hunt_target[0] = 0;
		hunt_mark_worked(m2);
		ft8_abort();
		ft8_repeat = 0;
		return;
	}

	//the other station has sent either an RRR or an RR73
	//this maybe arriving after we have cleared the log
	//we don't check it against any fields of the logger
	if (!strcmp(m3, "RR73") || !strcmp(m3, "RRR")){
		sprintf(reply_message, "%s %s 73", m2, mycall);	
		ft8_tx(reply_message, tx_pitch); 
		hunt_target[0] = 0;
		hunt_mark_worked(m2);
		enter_qso();
		call_wipe();
		ft8_repeat = 1;
	}	
	
	//beyond this point, we need to have a call filled up in the logger
	if (!strlen(call))
		return;


	//this is a signal report, at times, other call can just send their sig report
	if (m3[0] == '-' || (m3[0] == 'R' && m3[1] == '-') || m3[0] == '+' || (m3[0] == 'R' && m3[1] == '+')){
		ft8_on_signal_report();
		return;
	}
}

void ft8_init(){
	ft8_rx_buff_index = 0;
	ft8_tx_buff_index = 0;
	ft8_tx_nsamples = 0;
	pthread_create( &ft8_thread, NULL, ft8_thread_function, (void*)NULL);
}

void ft8_abort(){
	ft8_tx_nsamples = 0;
	ft8_repeat = 0;
}
