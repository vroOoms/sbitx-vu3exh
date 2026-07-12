# sBITX Custom Firmware — VU3EXH

Customizations on top of upstream [afarhan/sbitx](https://github.com/afarhan/sbitx)
v3.021 (base commit `14093e4`). Developed July 2026 on an sBITX v2 (Raspberry Pi 4,
32-bit Raspberry Pi OS Buster). Callsign VU3EXH, grid MK82SX, antenna EFHW 49:1.

Everything below is in the working tree of this branch. Files touched:
`sbitx.c, sbitx_gtk.c, modem_ft8.c, modem_cw.c, hamlib.c, webserver.c, sdr_ui.h,
web/index.html, web/style.css, web/logs.html (new), pskr_uplink.py (new),
qso_uploader.py (new), screenctl.sh (new), oled.h (new), data/prefixes.csv (new)`.

---

## FT8 automation

### HUNT / ROBO modes (`FT8_AUTO` field: OFF / ON / HUNT / ROBO)
- **OFF** — fully manual; tap a decode + F-keys to reply.
- **ON** — upstream behavior: auto-responds to stations calling you.
- **HUNT** — auto-answers CQs on the current band and runs each QSO to 73.
- **ROBO** — HUNT + automatic band selection: every 10 min it fetches
  live FT8 activity from `pskreporter.info/cgi-bin/psk-freq.pl` (grid-localized
  by public IP), and QSYs to the busiest standard FT8 dial if it is clearly
  better (2x + 3 spots hysteresis). Never hops mid-QSO (defers, then hops).
  Also reports where **your own** signal was heard in the last 15 min
  (`retrieve.pskreporter.info` reception reports, summarized per band).

### Queue engine (modem_ft8.c `hunt_*`)
Every fresh CQ is enqueued with per-station stats: last SNR, SNR trend,
times heard, tries, last-heard age, and the exact console line to reply to.
Each decode batch the scheduler:
- watches the current target: if it is seen working **someone else** → abandon
  immediately ("went with another station, next!"); if not decoded for
  2 consecutive slots and a comparable candidate exists → abandon ("fading").
- if free, starts the best queue entry: score = SNR + trend bonus
  (rising +3 / falling −5), only stations heard in the last 90 s.
- `queue` command prints the live table (call, snr, trend, heard, tries, age).

### Hunter safeguards
- **Pre-TX log check**: stations already in `data/qso_log.csv` are never called
  again (`hunt_load_log`). Completed QSOs (73/RR73) are marked permanently.
- **3 attempts max** per station with **exponential backoff** (2/4 min;
  30 s flat in hyper mode) — CSMA-style graceful retry.
- **Watchdog**: abandons a silent QSO after N slots (10/6/4 by mode).
- **SWR guard**: SWR is monitored live during every auto transmission
  (readable only while RF flows). Sustained SWR > 1.9 for 2 s → TX aborted
  mid-flight, band blacklisted for 1 h, ROBO immediately flees to the next
  allowed band. Manual TX is never interrupted.
- **Band preselect**: `data/robo_bands.txt` ("all" or e.g. "20m 17m 15m",
  set via `robobands ...`) limits which bands HUNT/ROBO may use.
- **Prefix skip list**: `data/hunt_skip.txt` (default "VU", set via
  `huntskip ...`) — the hunter ignores CQs from these prefixes; stations
  calling you directly are still answered (separate code path).
- **Hunter modes** (`huntmode normal|medium|hyper`, persisted): SNR floor
  −18/−21/−24 dB, watchdog 10/6/4 slots, backoff 2 min/1 min/30 s, tries 3/3/2.

### Clear-TX-offset picker (`txbest`)
Collects every decode's audio offset per batch; picks the middle of the widest
free gap in 250–2950 Hz. Applied automatically before every hunter reply,
or manually via the `txbest` command.

### Band tagging + slot-accurate history
Every FT8 RX/TX console line carries the band ("135930 **20m** 18 -15 1791 ~ ...").
RX tags use the frequency latched at **slot start** (`ft8_slot_freq`), so decodes
printed just after a QSY are tagged with the band the audio was actually
received on. A banner ("--- 20m dial 14.074 MHz ---") marks every dial change.
The console line format is parsed positionally in `ft8_message_tokenize` (C) and
`FT8_stylize_message/FT8_message_chosen/FT8_new_message` (web) — all band-aware.

## Logging & reporting

- **`data/ft8_decodes.csv`** — every decode and transmission:
  `date,utc,dir,band,dial_hz,score,snr,pitch,pw10,vswr10,"message"`.
  TXEND rows carry the measured peak power (deciwatts) and VSWR (x10) of each
  transmission (peak-tracked from the SWR bridge during TX).
- **`data/qso_log.csv`** — one row per completed QSO (any path: FT8 auto,
  device OK button, web OK button — hooked inside `enter_qso`):
  `date,utc,band,dial,call,grid,sent,recv,country,pw10,vswr10`.
  Country from `data/prefixes.csv` (~430 prefixes, longest-match).
- **`data/sessions.csv`** — START row per app start; `session <note>` appends
  META rows; `session antenna=X` also updates `data/antenna.txt` (used by the
  PSK Reporter uplink).
- **Web log browser** (`/logs.html`, LOG button): QSOs / Decodes / **Missed CQs**
  (who called CQ and was never worked — last heard, how long ago, band, snr,
  count) / **Stats** (session length, stations heard vs workable, attempts vs
  completed, realistic capacity at ~90 s per QSO, efficiency %, average QSO
  time, TX airtime share) / Sessions. Search + band/direction filters,
  CSV downloads, one-click **backup** of all logs to a downloadable tarball
  (`/rest?do=backup` endpoint in webserver.c).

## Spotting network integrations

- **`pskr_uplink.py`** — PSK Reporter uplink daemon. Tails the decode CSV and
  submits reception reports via the documented IPFIX UDP protocol
  (report.pskreporter.info:4739): 5-min randomized batches, per-call+band
  1 h dedupe, fixed source port, antenna string from `data/antenna.txt`.
- **`qso_uploader.py`** — QSO uploader daemon: converts new `qso_log.csv` rows
  to ADIF and pushes to eQSL and/or QRZ Logbook. Credentials in
  `data/upload_keys.ini` (idles politely until filled).
- Suggested crontab (not installed by default):
  `@reboot /usr/bin/python3 /home/pi/sbitx/pskr_uplink.py >>/tmp/pskr.log 2>&1`
  `@reboot /usr/bin/python3 /home/pi/sbitx/qso_uploader.py >>/tmp/qso_upload.log 2>&1`

## Display / DSP

- **BMASK self-learning birdie mask** (sbitx.c, display FFT bins 1276–1796):
  per-bin persistence scores; stationary spurs gain (+2 per 8th frame when
  > floor+8 dB), real signals decay. QSY evidence is decisive (±80: birdies
  survive a dial move, signals do not). Score ≥ 200 → bin painted at the
  noise floor, skirt bins flattened. **Hysteresis**: masked bins hold while
  the spur is even faintly warm (> floor+4 dB) and fade only when it is gone
  (~15 s) — the spur family relocates between restarts (Si5351/codec init),
  so masks self-heal to wherever they land. **Persistent** across restarts
  via `data/bmask_scores.dat` (saved ~40 s). Kill switch: `echo 0 > data/bmask.txt`.
  `bmask` command prints masked/rising counts and dumps per-bin state.
- **AUTOREF adaptive waterfall reference** — noise-floor tracking display
  offset; fixes per-band color shifts.
- **Message-type colors** (device console fonts FONT_FT8_CQ/73/REPORT +
  matching web CSS): CQ yellow, grid/reply green, reports light blue,
  RRR/RR73/73 magenta, addressed-to-you orange bold, own TX red, queued grey.
  CQs from already-worked stations render dim olive; skip-listed prefixes dim
  brown (worked list refreshed from qso_log.csv every minute).
- **Console drag-scroll** — touch-drag the decode list to scroll 500 lines of
  history; view holds position while new lines arrive; tap still selects/replies.
- **Web spectrum frequency scale** — kHz labels every 2 kHz on the web
  waterfall (the stock v3 web UI drew none).
- Earlier wave (2026-07-01..03): spectral-subtraction NR (DNR field), auto-notch
  ANF, RNNoise AINR (`librnnoise` integration), band scanner with mode
  detection + tap-to-jump scan report, OPT one-tap optimizer, SMART advisor,
  S-meter overlay, KEEPMODE, WIDE view, CW squelch (modem_cw.c).

## Web UI

- Answer CQ / Answer Caller buttons; hunter-mode dropdown.
- Command dropdown with one-line descriptions for every custom command
  (ftbest, txbest, robobands, huntmode, huntskip, session, span, bmask,
  screen, silent, wake, queue, freq, mode, qrz, abort, clear).
- Fixed FT8 list row overlap (`.ft8-label` width/nowrap + li auto-height),
  wrong-token CQ classification, click-to-reply parsing; band-aware everywhere.
- On-screen QWERTY hidden behind a KEYB toggle (the container also holds the
  bottom control bar — only the key grid is hidden).
- Console replay on login = full history restored on page reload.

## System / misc

- **Thread-safe hamlib** (hamlib.c): `F` (set frequency) is routed through the
  GUI command queue (`remote_execute`) instead of calling GTK from the socket
  thread — the old path corrupted fields and crashed the UI.
- **SIDETONE actually works**: the field had no handler and the DSP default
  was full volume; now wired live + pushed at startup. `silent` / `wake`
  commands (audio 0 + sidetone 0 + screen off / restore).
- **`screenctl.sh` + `screen off|on|5..100`** — DPMS blanking (touch wakes;
  the waking touch also clicks) and brightness via backlight sysfs or xrandr.
- **cmd_exec additions**: span, bmask, ftbest, robobands, huntmode, huntskip,
  session, queue, screen, silent, wake, txbest.
- `oled.h` — reconstructed header missing from the upstream OLED commit
  (upstream builds fail without it).
- Note: upstream `ui_tick` resets its `ticks` counter — any one-shot there
  must use its own static guard flag.

## Known gotchas

- Ports 8081 (text commands, `?FIELD` queries) and 4532 (rigctl subset) are
  single-client; use one persistent connection, one line per write.
- Websocket protocol: `<cookie>\n<field>=<value>`; login with any cookie +
  `login=<passkey>`, then use the returned session id as the cookie.
- After a cold Pi boot, give sbitx ~30 s before killing it (oscillator init
  runs after the FFT wisdom load).
- FT8 decode lines are parsed positionally in three places (see Band tagging)
  — any format change must update all of them.
