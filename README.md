# sBITX VU3EXH — Operator's Manual

Custom firmware for the sBITX v2/v3 built on [afarhan/sbitx](https://github.com/afarhan/sbitx).
This README is the **how-to-use manual**. For per-feature internals see
[FEATURES.md](FEATURES.md).

Everything works from two places:
- **The radio's touchscreen** — fields, taps and the `KBD` command line.
- **The web UI** — `http://<radio-ip>:8080` (passkey login). Same information,
  same commands, plus the log browser at the **LOG** button.

---

## 1. The AUTO modes — what the radio does on its own

The `AUTO` field (FT8 screen / web dropdown) has four settings. Each level
includes everything above it.

### OFF — fully manual
Nothing is transmitted unless you start it yourself: tap a CQ line to answer
it, or use the web UI's **Answer CQ / Answer Caller** buttons. (A USB
keyboard's F-keys still send the classic macros — F2 Call, F3 Reply,
F5 73, ... — the on-screen F-key row is gone, see below.)

### ON — answer people who call you
If someone transmits `VU3EXH <THEIRCALL> ...`, the radio replies and runs the
whole exchange automatically: signal report → rogers → 73 → **logged**.
It never starts contacts on its own.

### HUNT — work the band for you
Everything ON does, **plus** the radio answers CQs by itself:

1. Every decoded CQ goes into a **queue** with its signal report, trend
   (rising/falling), how often heard, and attempts so far.
2. Each 15 s slot, if the radio is free, it answers the **best** station:
   strongest signal, bonus if getting stronger, only stations heard in the
   last 90 s.
3. Before transmitting it moves your TX tone to the **clearest audio slot**
   (widest gap in the passband).
4. If the target answers → the QSO runs to 73 and is logged.
5. If the target is seen working **someone else** → abandoned instantly
   ("went with another station, next!") and the next queue entry is called.
6. If the target goes **silent for 2 slots** and a comparable station is
   waiting → abandoned ("fading"), next one called.
7. A completely silent target is dropped after the watchdog limit
   (see hunter modes below), and the attempt is remembered.

### ROBO — HUNT plus automatic band choice
Every 10 minutes (and immediately when you switch to ROBO) the radio asks
PSK Reporter which FT8 band is busiest **near you**, prints the table
(`FT8 spots/5min: 80m:0 ... 17m:40`) and where **your own signal** was heard
(`Your signal heard 61x/15min: 20m:61`). It QSYs only when another band is
clearly better (more than 2× your current band), never mid-QSO, and only to
bands you allowed.

**Kill switches:** tap `AUTO` to OFF, or the `skip` command to abort just the
current target, or `abort` to stop any transmission immediately.

---

## 2. Who the hunter will NOT call

These checks run **before** any automatic transmission. None of them affect
stations that call you directly — a directed call is always answered — and
none of them stop you tapping a decode manually.

| Rule | Behaviour | Control |
|---|---|---|
| **Already worked** | Anyone in your QSO log is never auto-called again. Survives restarts. | automatic (`data/qso_log.csv`) |
| **Attempts today** | Max 3 tries per station (2 in hyper) with growing pauses (2 min → 4 min). Counters survive restarts, reset at UTC midnight. | automatic (`data/hunt_day.csv`) |
| **They ignore us** | If we transmitted at a station 3 separate times and never got a single direct reply, we stop answering their CQs. Once every 24 h the radio makes **one probe attempt** to test whether they still ignore us; any reply clears them. | `ignored` shows the list |
| **Skip prefixes** | CQs from listed prefixes are never answered (default: `VU` — your own country). | `huntskip VU 8T` / `huntskip none` |
| **Bad SWR bands** | During every auto TX the SWR is watched live. Above **1.9 for 2 s** → transmission killed mid-flight, band blocked for 1 h, ROBO flees to the next allowed band. | automatic; `robobands` to preselect |
| **Band preselect** | HUNT/ROBO only operate on bands you allow. | `robobands 20m 17m 15m` / `robobands all` |
| **Junk callsigns** | A CQ must contain a structurally valid callsign (prefix+digit+1–4 letter suffix, `/` composites OK) and, if present, a valid grid — free-text like `YR50NADIA` is displayed but never answered. | automatic |
| **Skip this one** | Aborts the current hunter target and bars it for the rest of the day. | `skip` |

## 3. Hunter aggressiveness

`huntmode normal | medium | hyper` (device KBD or web dropdown; persists):

| | normal | medium | hyper |
|---|---|---|---|
| weakest CQ answered | −18 dB | −21 dB | −24 dB |
| patience with silent target | 10 slots | 6 slots | 4 slots |
| pause between attempts | 2/4 min | 1/2 min | 30 s |
| attempts per station | 3 | 3 | 2 |

Normal completes the most QSOs per attempt; hyper works the most stations
per hour on a busy band at the cost of more abandoned calls.

## 4. Reading the screen

Decode line: `135930 20m 18 -15 1791 ~ CQ YH1AA OI33`
= UTC time, **band it was received on**, decoder confidence, SNR, audio
offset, message. `--- 20m dial 14.074 MHz ---` banners mark every QSY.

Colors (same on device and web):

| Color | Meaning |
|---|---|
| yellow | fresh CQ — a station you can work |
| dim olive | CQ from a station already in your log |
| dim brown | CQ from a skip-listed prefix |
| green | grid/reply exchanges between others |
| light blue | signal reports (−15, R−08) |
| magenta | RRR / RR73 / 73 — QSOs completing |
| orange bold | messages addressed to YOU |
| red | your own transmissions |
| grey | queued/other |

**Scroll** the decode list by dragging it up/down (device touchscreen);
tap still selects. The web lists scroll natively and every row is clickable
to answer.

## 5. Commands

**Command buttons** — the top-row `WEB` button is now **`3D`**: it toggles
the 3D ↔ classic waterfall from any mode (the web UI `3D` button next to
KEYB sends the same toggle). On the FT8 screen the old on-screen F1–F8 row
is replaced by:

| Button | Action |
|---|---|
| `SILENT` | Mute audio + sidetone and blank the screen (touch to wake) |
| `SKIP` | Abort the current hunter target, bar it for today |
| `QUEUE` | Show the stations waiting to be answered |

`MENU` (prints the command list) sits bottom-right **beside `KBD`** on every
page except CW/CWR (that row is full), so commands are one tap away in any
mode. It hides while the on-screen keyboard is open.


Pick from the web `CMD` dropdown (each command shows a description), or type
on the device keyboard **with a leading backslash**: `\ftbest`, `\skip`,
`\queue`... (without the backslash, typed text is treated as message text;
the web CMD panel needs no backslash):

| Command | What it does |
|---|---|
| `queue` | Stations waiting to be answered (snr, trend, tries, age) |
| `ignored` | Stations blacklisted for ignoring us |
| `skip` | Abort current hunter target, bar it for today |
| `ftbest` | Live FT8 activity per band + where your signal is heard |
| `txbest` | Move TX tone to the clearest audio slot |
| `huntmode normal/medium/hyper` | Hunter aggressiveness |
| `huntskip VU ...` / `none` | Prefixes the hunter ignores |
| `robobands 20m 17m` / `all` | Bands HUNT/ROBO may use |
| `session antenna=EFHW 49:1` | Session note (antenna= also updates PSK Reporter info) |
| `span 25K/10K/6K/2.5K` | Waterfall span |
| `wf` | Toggle 3D / classic waterfall (persists) |
| `bmask` | Birdie-mask status |
| `screen off / on / 5..100` | Blank screen (touch wakes) / brightness % |
| `silent` / `wake` | Audio 0 + sidetone 0 + screen off / restore |
| `freq 14074` / `mode FT8` | Tune / mode |
| `abort` | Stop any transmission NOW |

## 6. Logs, stats and backups (web LOG button)

Every tab obeys the same filter bar: **free-text search** (call, grid,
country, message text), a **calendar date range** with one-tap chips
(Today / Yesterday / 7 days / 30 days / All), **band chips** and RX/TX
chips — all sized for a touchscreen.

- **QSOs** — every contact with band, grid, reports, **country**, and the
  **measured watts + SWR** of your final transmission.
- **Decodes** — everything heard and sent (`TXEND` rows carry power/SWR).
- **Missed CQs** — stations that called CQ and were never worked: last
  heard, how long ago, band, SNR. Your to-do list.
- **Report** — a general report of whatever the filters select: QSOs,
  unique calls, countries, stations/grids heard, transmissions, average
  power, per-band and per-day tables, activity-by-hour bars, plus the
  current session efficiency block.
- **Sessions** — one card per radio start. **Tap a session** to see only
  its QSOs; each card also has Decodes and Report buttons that filter the
  other tabs to that session's time window (a green banner shows the
  active session filter — tap ✕ to clear).
- CSV downloads + **Backup all logs** (tarball of everything on the SD card).

### The MAP dashboard (web MAP button, or `/map.html`)

A world map of your contacts and receptions, pskreporter style:

- **Big dots + curved lines** = stations you worked (color = band);
  **small dots** = stations you heard with their grids. ★ is you.
- Cards on top: QSOs, unique calls, countries, stations/grids heard,
  **best DX worked** and **furthest heard** (great-circle km from MK82SX).
- Overlay toggles: QSOs / Heard / Lines / **Maidenhead grid** / Labels /
  **day-night terminator**. Band chips and a calendar range filter
  everything, including the cards.
- **▶ Replay** animates your log over time with the slider — watch the
  contacts march across the globe as propagation changed.
- Tap any dot for call, grid, country, band, distance and when you
  worked/heard it. Data refreshes every minute.

## 7. Spotting networks

- **PSK Reporter** — `pskr_uplink.py` submits everything you decode;
  your station appears live on pskreporter.info. Antenna description comes
  from `session antenna=...`.
- **eQSL / QRZ Logbook** — `qso_uploader.py` pushes each logged QSO as ADIF.
  Put credentials in `data/upload_keys.ini` (eqsl user/pass, qrz apikey);
  it idles until you do.
- Start at boot (optional): add to `crontab -e`:
  ```
  @reboot /usr/bin/python3 /home/pi/sbitx/pskr_uplink.py >>/tmp/pskr.log 2>&1
  @reboot /usr/bin/python3 /home/pi/sbitx/qso_uploader.py >>/tmp/qso_upload.log 2>&1
  ```

## 8. Birdies (internal spurs)

Two detectors run continuously and paint spur bins down to the noise floor:
- **Continuity**: a bin whose minimum level never returns to the noise floor
  across FT8's 2 s slot gaps for ~18 s is a birdie — no real signal can do
  that. Self-arms ~18 s after any restart or QSY.
- **Learning**: strong stationary lines accumulate a score (faster on band
  changes — birdies survive a QSY, signals don't) that persists across
  restarts.

Spurs relocate when the radio restarts (oscillator init), so expect ~20 s of
visibility after a restart before the mask re-settles. A steady real carrier
parked for many minutes may eventually be masked too — QSY ±1 kHz clears it.
Kill switch: `echo 0 > data/bmask.txt`.

## 9. Good to know

- **SIDETONE** field = speaker volume during FT8/CW TX. Always starts at 30
  after a restart (hardcoded); adjust live any time, 0 is silent.
- Waking a blanked screen: touch it — that touch also *clicks*, so touch an
  empty corner.
- Restart etiquette: give the radio ~30 s after a cold Pi boot before
  killing it (oscillator init).
- REPEAT field = per-message retransmissions inside one attempt; the hunter
  layers its own attempt/backoff logic on top.
- ROBO is automatic unattended transmission — know your licence conditions;
  OFF is one tap away.
