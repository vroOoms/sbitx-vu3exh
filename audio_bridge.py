#!/usr/bin/env python3
# sBITX RX audio bridge - streams the receiver audio as a WAV HTTP stream
# on port 8082. Works with anything: VLC, browsers, or the bridge client
# feeding a virtual audio device for WSJT-X on Mac/Windows/Linux.
import subprocess, struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RATE = 48000

class H(BaseHTTPRequestHandler):
    def do_GET(self):
        if not self.path.startswith('/rx'):
            self.send_response(404); self.end_headers(); return
        self.send_response(200)
        self.send_header('Content-Type', 'audio/wav')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        hdr = struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', 0xFFFFFFFF,
            b'WAVE', b'fmt ', 16, 1, 1, RATE, RATE*2, 2, 16,
            b'data', 0xFFFFFFFF)
        try:
            self.wfile.write(hdr)
        except Exception:
            return
        p = subprocess.Popen(['arecord', '-D', 'plughw:1,1', '-f', 'S16_LE',
            '-c', '1', '-r', str(RATE), '-t', 'raw', '-q'],
            stdout=subprocess.PIPE)
        try:
            while True:
                d = p.stdout.read(4096)
                if not d:
                    break
                self.wfile.write(d)
        except Exception:
            pass
        finally:
            p.kill()
    def log_message(self, *a):
        pass

print("sBITX audio bridge on :8082  (stream: /rx)")
ThreadingHTTPServer(('0.0.0.0', 8082), H).serve_forever()
