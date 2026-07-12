#!/bin/bash
# screenctl.sh off|on|<5-100>
export DISPLAY=:0 XAUTHORITY=/home/pi/.Xauthority
case "$1" in
  off) xset s off; xset dpms force off ;;
  on)  xset dpms force on ;;
  *)
    b="$1"
    bl=$(ls /sys/class/backlight/*/brightness 2>/dev/null | head -1)
    if [ -n "$bl" ]; then
      max=$(cat "$(dirname "$bl")/max_brightness")
      echo $(( b * max / 100 )) | sudo tee "$bl" >/dev/null
    else
      out=$(xrandr | awk '/ connected/{print $1; exit}')
      if [ "$b" -ge 100 ]; then v=1.0; else v=$(printf "0.00" "$b"); fi
      xrandr --output "$out" --brightness "$v"
    fi ;;
esac
