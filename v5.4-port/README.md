# v5.4 decoder port

Our weak-signal decoder improvements, ported to **drexjj/sbitx v5.4**
(64-bit Trixie). This is the ONE thing worth carrying onto v5.4 — WSPR,
FT4, and noise reduction were dropped as redundant (v5.4 and its bundled
WSJTX-Improved do them better).

## What it does
- **Decoder tuning** — kMin_score 10→5, kMax_candidates 120→400,
  kLDPC_iterations 20→30, kTime_osr 2→4. Catches weak signals the stock
  settings miss (took our decode from 54% → 79% of WSJT-X).
- **Decode window widened** — f_max 3000→5000 Hz, catches FT8 stations at the sub-band edge in crowded conditions (matches WSJT-X's wider window). Verified: decodes a synthetic 3500 Hz signal that f_max=3000 misses.
- **Multi-pass subtraction** — decode, re-encode each message, subtract
  it, re-search the residue. Finds weak signals masked by strong ones;
  in testing the sBitx found messages even WSJT-X missed.

Both live entirely in `src/modem_ft8.c`. No UI changes — every stock
v5.4 button is untouched. The improvement is invisible: the decode list
just gets deeper.

## Applying to a fresh v5.4
    cd /home/pi/sbitx
    patch -p0 src/modem_ft8.c < decoder-tuning-subtraction.patch   # (adjust paths)
    ./build sbitx

Or use port64.py (in the session scratchpad) which does anchor-based
edits robust to line shifts. wsprd (if ever needed) is already at
/usr/bin/wsprd on the 64-bit image.

## Do NOT raise the tuning further
kFreq_osr=4 breaks ft8_lib sync scoring (noise candidates, 0 decodes);
kMin_score<5 fills the candidate list with noise. Both tried, reverted.
