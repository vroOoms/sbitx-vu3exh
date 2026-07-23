#!/usr/bin/env python3
# sBITX audio bridge client - plays the radio's RX stream into any audio
# output device. Feed a virtual device (BlackHole on macOS, VB-Cable on
# Windows, snd-aloop on Linux) and point WSJT-X's input at it.
#
#   pip install sounddevice
#   python3 sbitx_bridge_client.py --list
#   python3 sbitx_bridge_client.py --device "BlackHole 2ch"
#
# Default stream: http://192.168.0.108:8082/rx
import argparse, struct, sys, urllib.request

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://192.168.0.108:8082/rx')
    ap.add_argument('--device', default=None,
        help='output device name or index (see --list)')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()
    try:
        import sounddevice as sd
    except ImportError:
        sys.exit("pip install sounddevice   (then rerun)")
    if args.list:
        print(sd.query_devices()); return
    r = urllib.request.urlopen(args.url, timeout=10)
    hdr = r.read(44)                       # take the rate from the stream
    rate = struct.unpack('<I', hdr[24:28])[0]
    print('stream rate: %d Hz' % rate)
    out = sd.RawOutputStream(samplerate=rate, channels=1, dtype='int16',
        device=args.device)
    out.start()
    print("streaming", args.url, "->", args.device or "default output")
    try:
        while True:
            d = r.read(4096)
            if not d:
                break
            out.write(d)
    except KeyboardInterrupt:
        pass

if __name__ == '__main__':
    main()
