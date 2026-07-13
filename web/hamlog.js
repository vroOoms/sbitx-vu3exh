// hamlog.js — log-file parsers for the sBITX web dashboard (map.html,
// logs.html), so any ham can load their own logs and see the results.
// Accepts: ADIF (.adi/.adif from any logger), WSJT-X ALL.TXT, and the
// sBITX CSV formats. MIT license, VU3EXH 2026.

function hamlogBand(mhz){
  if (mhz >= 1.8  && mhz < 2.1)   return '160m';
  if (mhz >= 3.5  && mhz < 4.0)   return '80m';
  if (mhz >= 5.2  && mhz < 5.5)   return '60m';
  if (mhz >= 7.0  && mhz < 7.31)  return '40m';
  if (mhz >= 10.1 && mhz < 10.16) return '30m';
  if (mhz >= 14.0 && mhz < 14.36) return '20m';
  if (mhz >= 18.06&& mhz < 18.17) return '17m';
  if (mhz >= 21.0 && mhz < 21.46) return '15m';
  if (mhz >= 24.88&& mhz < 25.0)  return '12m';
  if (mhz >= 28.0 && mhz < 29.8)  return '10m';
  if (mhz >= 50   && mhz < 54.1)  return '6m';
  if (mhz >= 144  && mhz < 148.1) return '2m';
  return '?';
}

// ADIF -> rows shaped like data/qso_log.csv:
// [date, utc, band, dial_hz, call, grid, sent, recv, country, pw10, vswr10]
// returns {rows, mygrid, mycall}
function parseADIF(text){
  var body = text;
  var eoh = body.search(/<eoh>/i);
  if (eoh >= 0) body = body.slice(eoh + 5);
  var recs = body.split(/<eor>/i);
  var rows = [], mygrid = null, mycall = null;
  recs.forEach(function(rec){
    var f = {};
    var re = /<([A-Za-z0-9_]+):(\d+)(?::[^>]*)?>/g, m;
    while ((m = re.exec(rec))){
      f[m[1].toUpperCase()] = rec.substr(re.lastIndex, parseInt(m[2])).trim();
    }
    if (f.MY_GRIDSQUARE) mygrid = f.MY_GRIDSQUARE;
    if (f.STATION_CALLSIGN) mycall = f.STATION_CALLSIGN;
    else if (f.OPERATOR && !mycall) mycall = f.OPERATOR;
    if (!f.CALL || !f.QSO_DATE) return;
    var d = f.QSO_DATE;
    var date = d.slice(0,4) + '-' + d.slice(4,6) + '-' + d.slice(6,8);
    var t = (f.TIME_ON || '000000') + '000000';
    var utc = t.slice(0,2) + ':' + t.slice(2,4) + ':' + t.slice(4,6);
    var mhz = f.FREQ ? parseFloat(f.FREQ) : 0;
    var band = (f.BAND || '').toLowerCase() || hamlogBand(mhz);
    rows.push([date, utc, band, Math.round(mhz * 1e6) || 0,
      f.CALL.toUpperCase(), (f.GRIDSQUARE || '').toUpperCase().slice(0,6),
      f.RST_SENT || '', f.RST_RCVD || '',
      f.COUNTRY || f.DXCC || '', 0, 0]);
  });
  return {rows: rows, mygrid: mygrid, mycall: mycall};
}

// WSJT-X ALL.TXT -> rows shaped like data/ft8_decodes.csv (9 columns):
// [date, utc, dir, band, dial_hz, score, snr, pitch, msg]
// handles both "YYMMDD_HHMMSS ..." and the older "YYYY-MM-DD HH:MM ..." lines
function parseALLTXT(text){
  var rows = [];
  text.split('\n').forEach(function(ln){
    var t = ln.trim().split(/\s+/);
    if (t.length < 8) return;
    var date, utc, i;
    if (/^\d{6}_\d{6}$/.test(t[0])){
      var d = t[0];
      date = '20' + d.slice(0,2) + '-' + d.slice(2,4) + '-' + d.slice(4,6);
      utc = d.slice(7,9) + ':' + d.slice(9,11) + ':' + d.slice(11,13);
      i = 1;
    } else if (/^\d{4}-\d{2}-\d{2}$/.test(t[0]) && /^\d{2}:\d{2}/.test(t[1])){
      date = t[0];
      utc = t[1].length == 5 ? t[1] + ':00' : t[1];
      i = 2;
    } else return;
    var mhz = parseFloat(t[i]);
    if (!(mhz > 0)) return;
    var dir = (t[i+1] || '').toUpperCase() == 'TX' ? 'TX' : 'RX';
    // t[i+2]=mode t[i+3]=snr t[i+4]=dt t[i+5]=df, message afterwards
    var msg = t.slice(i + 6).join(' ');
    if (!msg) return;
    rows.push([date, utc, dir, hamlogBand(mhz), Math.round(mhz * 1e6),
      0, t[i+3], t[i+5], msg]);
  });
  return rows;
}

// what is this uploaded file? 'adif' | 'alltxt' | 'qsocsv' | 'deccsv' | 'sesscsv' | null
function hamlogSniff(name, text){
  var head = text.slice(0, 4000);
  if (/<eor>/i.test(head) || /<eoh>/i.test(head) || /\.adif?$/i.test(name)) return 'adif';
  if (/^\d{6}_\d{6}\s/m.test(head)) return 'alltxt';
  if (/^\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}\s+\d+\.\d+/m.test(head)) return 'alltxt';
  if (/^\d{4}-\d{2}-\d{2},\d{2}:\d{2}:\d{2},(RX|TX|TXEND),/m.test(head)) return 'deccsv';
  if (/^(START|META),/m.test(head)) return 'sesscsv';
  if (/^\d{4}-\d{2}-\d{2},\d{2}:\d{2}:\d{2},\d+m,/m.test(head)) return 'qsocsv';
  return null;
}
