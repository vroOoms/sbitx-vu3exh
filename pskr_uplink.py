#!/usr/bin/env python3
# PSK Reporter uplink for sBITX (VU3EXH)
# Tails data/ft8_decodes.csv and submits reception reports to report.pskreporter.info:4739
# Protocol per https://pskreporter.info/pskdev.html (IPFIX, enterprise 30351)
import socket, struct, time, random, csv, io, os, sys
from datetime import datetime, timezone

CSV_PATH = "/home/pi/sbitx/data/ft8_decodes.csv"
INI_PATH = "/home/pi/sbitx/data/user_settings.ini"
ANT_PATH = "/home/pi/sbitx/data/antenna.txt"
HOST, PORT = "report.pskreporter.info", 4739
SOFTWARE = "sBITX v3.021 custom"

def cfg(key, default=""):
    try:
        for ln in open(INI_PATH):
            ln = ln.strip()
            if ln.startswith("#" + key + "="):
                return ln.split("=", 1)[1].strip()
    except OSError:
        pass
    return default

MYCALL = (cfg("mycallsign", "VU3EXH") or "VU3EXH").upper()
MYGRID = cfg("mygrid", "MK82SX") or "MK82SX"
def antenna():
    try:
        a = open(ANT_PATH).read().strip()
        return a if a else "EFHW 49:1"
    except OSError:
        return "EFHW 49:1"

# record format descriptors, verbatim from the spec
RX_DESC = bytes.fromhex(
    "0003002c9992000400018002ffff0000768f8004ffff0000768f"
    "8008ffff0000768f8009ffff0000768f0000")
# senderCallsign, frequency, sNR(1), iMD(1), mode, informationSource(1), flowStartSeconds
TX_DESC = bytes.fromhex(
    "0002003c999300078001ffff0000768f800500040000768f"
    "800600010000768f800700010000768f800affff0000768f"
    "800b00010000768f00960004")

def vstr(s):
    b = s.encode("ascii", "ignore")[:254]
    return bytes([len(b)]) + b

def block(link, body):
    total = 4 + len(body)
    padded = (total + 3) // 4 * 4
    return link + struct.pack(">H", padded) + body + b"\x00" * (padded - total)

def sender_of(text):
    if "<" in text:
        return None
    toks = text.split()
    if not toks:
        return None
    cands = toks[1:] if toks[0] == "CQ" else toks[1:2]
    for t in cands:
        if any(c.isdigit() for c in t) and 3 <= len(t) <= 12 and t.replace("/", "").isalnum():
            return t
    return None

def parse_row(row):
    # v1: date,utc,dir,band,dial,score,snr,pitch,"msg"  v2: +pw10,vswr10 before msg
    if len(row) < 9 or row[2] != "RX":
        return None
    msg = row[10] if len(row) >= 11 else row[8]
    try:
        ts = datetime.strptime(row[0] + " " + row[1], "%Y-%m-%d %H:%M:%S")
        ts = int(ts.replace(tzinfo=timezone.utc).timestamp())
        dial = int(row[4]); snr = int(row[6]); pitch = int(row[7])
    except (ValueError, IndexError):
        return None
    call = sender_of(msg)
    if not call or call == MYCALL:
        return None
    return (call, dial + pitch, max(-128, min(127, snr)), ts)

def build_packet(reports, seq, rand_id, with_desc):
    rx_body = vstr(MYCALL) + vstr(MYGRID) + vstr(SOFTWARE) + vstr(antenna())
    tx_body = b""
    for call, freq, snr, ts in reports:
        tx_body += vstr(call) + struct.pack(">I", freq) + struct.pack(">b", snr) \
                 + b"\x00" + vstr("FT8") + b"\x01" + struct.pack(">I", ts)
    payload = (RX_DESC + TX_DESC if with_desc else b"") \
              + block(b"\x99\x92", rx_body) + block(b"\x99\x93", tx_body)
    hdr = struct.pack(">HHIII", 0x000A, 16 + len(payload), int(time.time()), seq, rand_id)
    return hdr + payload

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", 0))  # keep one source port for the whole session
    rand_id = random.getrandbits(32)
    seq = 1
    pkts = 0
    last_desc = 0
    queued = {}          # (call, MHz-band) -> report tuple
    reported = {}        # (call, MHz-band) -> last time sent
    # start from the end of the file minus the last ~10 minutes
    pos = 0
    try:
        pos = os.path.getsize(CSV_PATH)
    except OSError:
        pass
    next_send = time.time() + 60 + random.uniform(0, 30)
    print("pskr_uplink: %s %s ant=%s" % (MYCALL, MYGRID, antenna()), flush=True)
    while True:
        try:
            sz = os.path.getsize(CSV_PATH)
            if sz < pos:
                pos = 0  # file rotated
            if sz > pos:
                with open(CSV_PATH) as f:
                    f.seek(pos)
                    chunk = f.read()
                    pos = f.tell()
                for row in csv.reader(io.StringIO(chunk)):
                    r = parse_row(row)
                    if not r:
                        continue
                    key = (r[0], r[1] // 1000000)
                    if time.time() - reported.get(key, 0) < 3600:
                        continue
                    queued[key] = r
        except OSError:
            pass
        now = time.time()
        if now >= next_send and queued:
            reports = list(queued.values())[:80]
            with_desc = pkts < 3 or now - last_desc > 3500
            pkt = build_packet(reports, seq, rand_id, with_desc)
            try:
                sock.sendto(pkt, (HOST, PORT))
                pkts += 1
                seq += len(reports)
                if with_desc:
                    last_desc = now
                for r in reports:
                    key = (r[0], r[1] // 1000000)
                    reported[key] = now
                    queued.pop(key, None)
                print("sent %d reports (%d bytes, pkt %d)" % (len(reports), len(pkt), pkts), flush=True)
            except OSError as e:
                print("send failed: %s" % e, flush=True)
        if now >= next_send:
            next_send = now + 300 + random.uniform(0, 30)
        time.sleep(20)

if __name__ == "__main__":
    main()
