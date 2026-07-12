#!/usr/bin/env python3
# QSO uploader for sBITX: tails data/qso_log.csv and pushes new QSOs as ADIF to
# eQSL.cc and/or QRZ.com logbook. Fill /home/pi/sbitx/data/upload_keys.ini first:
#
#   [eqsl]
#   user=VU3EXH
#   pass=your-eqsl-password
#   [qrz]
#   apikey=XXXX-XXXX-XXXX-XXXX     (QRZ Logbook API key, subscriber feature)
#
# Leave a section blank to skip that service. State kept in data/.upload_state.
import csv, io, os, time, json
import urllib.request, urllib.parse

QSO_CSV = "/home/pi/sbitx/data/qso_log.csv"
KEYS = "/home/pi/sbitx/data/upload_keys.ini"
STATE = "/home/pi/sbitx/data/.upload_state"
MYGRID = "MK82SX"

def read_keys():
    conf, sect = {}, None
    try:
        for ln in open(KEYS):
            ln = ln.strip()
            if ln.startswith("[") and ln.endswith("]"):
                sect = ln[1:-1]; conf[sect] = {}
            elif "=" in ln and sect and not ln.startswith("#"):
                k, v = ln.split("=", 1)
                conf[sect][k.strip()] = v.strip()
    except OSError:
        pass
    return conf

def adif_field(name, val):
    val = str(val)
    return "<%s:%d>%s" % (name, len(val), val) if val else ""

def row_to_adif(row):
    # date,utc,band,dial,call,grid,sent,recv,country,pw10,vswr10
    date = row[0].replace("-", "")
    tm = row[1].replace(":", "")[:6]
    freq_mhz = "%.6f" % (int(row[3]) / 1e6)
    rec = (adif_field("CALL", row[4]) + adif_field("QSO_DATE", date)
        + adif_field("TIME_ON", tm) + adif_field("BAND", row[2].upper())
        + adif_field("FREQ", freq_mhz) + adif_field("MODE", "FT8")
        + adif_field("RST_SENT", row[6]) + adif_field("RST_RCVD", row[7])
        + adif_field("GRIDSQUARE", row[5][:8]) + adif_field("MY_GRIDSQUARE", MYGRID))
    if len(row) > 9 and row[9].isdigit() and int(row[9]) > 0:
        rec += adif_field("TX_PWR", "%.1f" % (int(row[9]) / 10.0))
    return rec + "<EOR>"

def upload_eqsl(conf, adif):
    user, pw = conf.get("user", ""), conf.get("pass", "")
    if not user or not pw:
        return None
    data = ("SBITX upload<EOH>" + adif)
    url = ("https://www.eqsl.cc/qslcard/importADIF.cfm?ADIFdata="
        + urllib.parse.quote(adif_field("EQSL_USER", user) + adif_field("EQSL_PSWD", pw)
        + "<EOH>" + adif))
    with urllib.request.urlopen(url, timeout=30) as r:
        body = r.read().decode("utf-8", "ignore")
    ok = "Result: 1" in body or "successfully" in body.lower()
    return ("eqsl", ok, body[:120].replace("\n", " "))

def upload_qrz(conf, adif):
    key = conf.get("apikey", "")
    if not key:
        return None
    data = urllib.parse.urlencode({"KEY": key, "ACTION": "INSERT", "ADIF": adif}).encode()
    req = urllib.request.Request("https://logbook.qrz.com/api", data=data)
    with urllib.request.urlopen(req, timeout=30) as r:
        body = r.read().decode("utf-8", "ignore")
    ok = "RESULT=OK" in body or "RESULT=REPLACE" in body
    return ("qrz", ok, body[:120])

def main():
    done = 0
    try:
        done = int(open(STATE).read().strip())
    except (OSError, ValueError):
        pass
    print("qso_uploader: starting at row %d" % done, flush=True)
    while True:
        conf = read_keys()
        has_creds = conf.get("eqsl", {}).get("user") or conf.get("qrz", {}).get("apikey")
        try:
            rows = list(csv.reader(open(QSO_CSV)))
        except OSError:
            rows = []
        if has_creds and len(rows) > done:
            for row in rows[done:]:
                if len(row) < 9:
                    done += 1
                    continue
                adif = row_to_adif(row)
                for fn in (upload_eqsl, upload_qrz):
                    sect = "eqsl" if fn is upload_eqsl else "qrz"
                    try:
                        res = fn(conf.get(sect, {}), adif)
                        if res:
                            print("%s %s %s: %s" % (row[4], res[0],
                                "OK" if res[1] else "FAIL", res[2]), flush=True)
                    except Exception as e:
                        print("%s %s error: %s" % (row[4], sect, e), flush=True)
                done += 1
                open(STATE, "w").write(str(done))
        elif not has_creds and len(rows) > done:
            print("%d QSOs waiting - fill data/upload_keys.ini to upload" % (len(rows) - done), flush=True)
        time.sleep(60)

if __name__ == "__main__":
    main()
