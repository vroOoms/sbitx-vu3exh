#!/usr/bin/env python3
# sBITX RX audio bridge - serves the receiver audio as a WAV HTTP stream
# on port 8082, for WSJT-X (via a virtual audio device), VLC, or a
# browser, on any OS.
#
# The audio comes from sbitx's internal RX tap (UDP 8083): the same
# 12 kHz demodulated stream its own FT8 decoder uses. The ALSA loopback
# the stock firmware writes drops samples and will not decode.
import array, socket, struct, threading, queue
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RATE = 12000        # the raw tap rate; clients resample if they need 48k
clients = []
clients_lock = threading.Lock()

def udp_reader():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', 8083))
    while True:
        data, _ = s.recvfrom(4096)
        with clients_lock:
            for q in clients:
                try:
                    q.put_nowait(data)
                except queue.Full:
                    pass          # a slow client must not stall the radio

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'

    def do_GET(self):
        if not self.path.startswith('/rx'):
            self.send_response(404); self.end_headers(); return
        self.send_response(200)
        self.send_header('Content-Type', 'audio/wav')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        hdr = struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', 0xFFFFFFFF,
            b'WAVE', b'fmt ', 16, 1, 1, RATE, RATE * 2, 2, 16,
            b'data', 0xFFFFFFFF)
        q = queue.Queue(maxsize=6)    # ~300ms: drop stale audio rather than lag
        with clients_lock:
            clients.append(q)
        try:
            self.wfile.write(hdr)
            while True:
                self.wfile.write(q.get())
        except Exception:
            pass
        finally:
            with clients_lock:
                if q in clients:
                    clients.remove(q)

    def log_message(self, *a):
        pass

threading.Thread(target=udp_reader, daemon=True).start()
print("sBITX audio bridge on :8082  (/rx, %d Hz from the RX tap)" % RATE)
ThreadingHTTPServer(('0.0.0.0', 8082), H).serve_forever()
