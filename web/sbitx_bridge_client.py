#!/usr/bin/env python3
# sBITX audio bridge client - plays the radio's RX stream into any audio
# device. Feed a virtual device (BlackHole on macOS, VB-Cable on Windows,
# snd-aloop on Linux) and point WSJT-X's input at it.
#
#   pip install sounddevice numpy
#   python3 sbitx_bridge_client.py --list
#   python3 sbitx_bridge_client.py --device "BlackHole 2ch"
#
# Latency is bounded (default 250 ms): if the machine or network falls
# behind, old audio is dropped rather than queued. FT8 and friends decode
# a window locked to the UTC clock, so a stream that lags by seconds
# decodes nothing at all - staying in sync matters more than continuity.
import argparse, collections, struct, sys, threading, urllib.request

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://192.168.0.108:8082/rx')
    ap.add_argument('--device', default=None,
        help='output device name or index (see --list)')
    ap.add_argument('--out-rate', type=int, default=48000,
        help='output device rate (default 48000, what WSJT-X expects)')
    ap.add_argument('--max-latency', type=float, default=0.25,
        help='seconds of audio to buffer before dropping (default 0.25)')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()
    try:
        import sounddevice as sd
        import numpy as np
    except ImportError:
        sys.exit("pip install sounddevice numpy   (then rerun)")
    if args.list:
        print(sd.query_devices()); return

    r = urllib.request.urlopen(args.url, timeout=10)
    hdr = r.read(44)
    rate = struct.unpack('<I', hdr[24:28])[0]
    out_rate = args.out_rate
    factor = out_rate // rate if rate and out_rate % rate == 0 else 1
    if factor == 1 and out_rate != rate:
        out_rate = rate                       # play at the stream's own rate

    buf = collections.deque()
    held = [0]                                 # samples currently queued
    lock = threading.Lock()
    maxq = int(args.max_latency * out_rate)

    def cb(outdata, frames, time_info, status):
        out = np.zeros(frames, dtype=np.int16)
        filled = 0
        with lock:
            while filled < frames and buf:
                c = buf[0]
                take = min(len(c), frames - filled)
                out[filled:filled + take] = c[:take]
                if take == len(c):
                    buf.popleft()
                else:
                    buf[0] = c[take:]
                held[0] -= take
                filled += take
        outdata[:] = out.tobytes()

    stream = sd.RawOutputStream(samplerate=out_rate, channels=1,
        dtype='int16', device=args.device, blocksize=1024,
        latency='low', callback=cb)
    stream.start()
    print("streaming %s -> %s  (%d Hz -> %d Hz, max %.0f ms latency)"
          % (args.url, args.device or "default output", rate, out_rate,
             args.max_latency * 1000))

    prev = np.zeros(1, dtype=np.float32)
    chunk = max(2, rate // 20) * 2             # 50 ms
    dropped = 0
    try:
        while True:
            d = r.read(chunk)
            if not d:
                break
            x = np.frombuffer(d, dtype='<i2').astype(np.float32)
            if factor > 1:
                xs = np.concatenate((prev, x))
                prev = x[-1:]
                y = np.empty(len(x) * factor, dtype=np.float32)
                step = xs[1:] - xs[:-1]
                for k in range(factor):        # linear interpolation
                    y[k::factor] = xs[:-1] + step * (k / float(factor))
                s = np.clip(y, -32768, 32767).astype(np.int16)
            else:
                s = x.astype(np.int16)
            with lock:
                buf.append(s)
                held[0] += len(s)
                while held[0] > maxq and len(buf) > 1:
                    held[0] -= len(buf.popleft())
                    dropped += 1
    except KeyboardInterrupt:
        pass
    finally:
        stream.stop(); stream.close()
        if dropped:
            print("dropped %d stale chunks to stay in sync" % dropped)

if __name__ == '__main__':
    main()
