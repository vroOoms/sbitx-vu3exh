# Upstream bugs found while building on afarhan/sbitx

Everything below was hit on a stock sBITX v2 running upstream
[afarhan/sbitx](https://github.com/afarhan/sbitx) @ `14093e4` (v3.021) and
fixed in this fork. Symptoms first, so you can search what you're seeing.
Fix commits are in this repo's history; the ones marked **[PR-ready]** are
isolated enough to upstream directly.

## Build

1. **`oled.h: No such file or directory` — the repo does not build.**
   The "Added OLED support" commit references `oled.h` from `oled.c` and
   `sbitx_gtk.c` but the file was never committed. We reconstructed it
   (SSD1306 constants, init sequence, prototypes).

## FT8 / transmit path

2. **Transmissions that key up and immediately stop — the attempt is
   silently wasted.** **[PR-ready]** The TX trigger in `ft8_poll()` fires
   *anywhere inside* the 15 s parity window and starts the waveform at
   offset `seconds % 15`. A reply scheduled at second :29 keys instantly
   and transmits only the last ~1 s of the message — undecodable, and
   `ft8_repeat` is consumed. Fix: key only in the first 3 s of the
   window; later requests stay pending for the next parity slot.
3. **Starting any new QSO aborts a live transmission mid-flight.**
   `ft8_on_start_qso()` begins with `modem_abort(); tx_off();`. Decode
   batches land 1–3 s into the *next* slot — which is sometimes your own
   TX slot — so answering a station that called you (or tapping a line)
   chopped the transmission on air. Fix: while transmitting, the request
   is parked and fired the moment the TX completes.
4. **The FT8 tone blasts from the speaker at full volume and the
   SIDETONE control does nothing.** The `SIDETONE` field has a NULL
   handler; the DSP `sidetone` level stays at its compile-time default
   (full). Fix: a real handler + the saved level pushed at startup, and
   the sidetone muted for the duration of every transmission.
5. **`tx_on()`/`tx_off()` silently re-program the LO** through
   `set_operating_freq()` at key-up/key-down. Anything that guards or
   wraps the tuning path must let these same-frequency internal calls
   through, or transmissions break subtly.
6. Dead code: `ft8_tx()` computes a mid-slot sample index that is never
   used — the real keying lives in `ft8_poll()`. Misleading when reading
   the TX path.

## Threading / timers

7. **Changing frequency over rigctl (hamlib, port 4532) crashes or
   corrupts the GUI.** `set_freq` executed GTK calls directly from the
   socket thread (we saw freezes and phantom field changes, e.g. DRIVE
   jumping 100→33). Fix: route remote commands through the
   `remote_execute()` queue like the web UI does.
8. **The `ticks` counter in `ui_tick()` is reset by upstream code**, so
   any `if (ticks == N)` one-shot re-fires forever (our session marker
   flooded its log with 1700 rows). Use static guard flags.

## Web UI

9. The spectrum canvas has **no frequency scale** (we draw kHz ticks).
10. **`#keybd-container` wraps the entire bottom bar** — hiding the
    container (the obvious move to hide the keyboard) removes every
    control including the toggle itself. Only the inner `#keybd` table
    may be hidden.
11. **Pages are served with an ETag but no `Cache-Control`**, so
    browsers heuristically cache `index.html` and deployed UI changes
    don't show up. We serve `Cache-Control: no-cache` and stamp a
    visible `ui vNN` on the page (a long-lived tab never reloads itself
    and silently runs old JS).
12. The FT8 console line is **parsed positionally in three places**
    (device tap-reply tokenizer + two web parsers). Any change to the
    line format silently breaks tap-to-reply and the web lists.

## Operational gotchas

13. Control ports **8081 (text) and 4532 (rigctl) are single-client**:
    rapid reconnects or interleaving both ports starves commands. Keep
    one persistent connection per port.
14. **WM8731 codec init flake on cold boot** (bit-banged I2C): all-zero
    samples — solid green waterfall, S-meter stuck at S0, flat spectrum.
    Restarting sbitx re-runs codec init and fixes it.
15. **Oscillator init runs late in startup** (after the FFT wisdom
    load). Killing sbitx within ~30 s of a cold Pi boot leaves the
    Si5351 half-programmed (garbled BFO/LO). Give it 30 s.
16. `change_band()` **forces the per-band stored mode**, overriding the
    operator's current mode choice on every band change (we disable
    this; modes still save, they're just not forced).

## Hardware (sBITX v2, S.No 0919 — dead receiver repair)

Not upstream software bugs, but documented here because the symptoms
looked like software: RX totally deaf while TX worked fine. Root causes:
a cracked solder joint on **C211** (1 µF RX coupling cap) and **C95**
(47 µF tantalum, RX audio buffer supply filter) **failed short** — it ran
hot to the touch. Diagnosis trail (GPIO-driven T/R tests, HackRF signal
injection walk, VNA) is in the git history and FEATURES.md.
