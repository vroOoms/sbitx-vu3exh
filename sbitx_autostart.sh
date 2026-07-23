#!/bin/bash
# sBITX autostart - launched at desktop login. Guarded so tapping the
# desktop icon (or a manual launch) never produces a second instance.
sleep 20                       # let the Pi settle: clocks, network, audio
if ! pgrep -x sbitx >/dev/null; then
    cd /home/pi/sbitx && ./sbitx >/tmp/sbitx_live.log 2>&1 &
fi
sleep 5
if ! pgrep -f "[a]udio_bridge.py" >/dev/null; then
    setsid /usr/bin/python3 -u /home/pi/sbitx/audio_bridge.py \
        >/tmp/audio_bridge.log 2>&1 </dev/null &
fi
