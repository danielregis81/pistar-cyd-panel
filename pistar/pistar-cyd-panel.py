#!/usr/bin/env python3
"""pistar-cyd-panel — coletor que roda NO Pi-Star.

Lê o log do MMDVMHost (/var/log/pi-star/MMDVM-AAAA-MM-DD.log), monta o estado
"quem está falando agora" + últimos ouvidos e grava um JSON em memória (/run),
servido pelo nginx do próprio Pi-Star em http://<pi-star>/cyd/status.json.

- Nome e país: bases que o Pi-Star já baixa do RadioID (/usr/local/etc/nextionUsers.csv;
  reserva: DMRIds.dat + MCC do DMR ID) ou o Talker Alias.
- Bandeiras: arquivos RGB565 60x40 em /usr/local/share/pistar-cyd-panel/flags,
  servidos em /cyd/flags/<cc>.565.

Só biblioteca padrão; compatível com Python 3.9 (Pi-Star 4.x).
"""
import calendar
import glob
import json
import os
import re
import sys
import time
from functools import lru_cache

LOG_DIR = os.environ.get("PCP_LOG_DIR", "/var/log/pi-star")
IDS_FILE = os.environ.get("PCP_IDS", "/usr/local/etc/DMRIds.dat")
USERS_FILE = os.environ.get("PCP_USERS", "/usr/local/etc/nextionUsers.csv")
MMDVM_INI = os.environ.get("PCP_MMDVM_INI", "/etc/mmdvmhost")
SHARE = os.environ.get("PCP_SHARE", "/usr/local/share/pistar-cyd-panel")
OUT_DIR = os.environ.get("PCP_OUT", "/run/pistar-cyd-panel")
INTERVAL = float(os.environ.get("PCP_INTERVAL", "1"))
LAST_N = 8
BOOTSTRAP_BYTES = 300_000      # ao iniciar, lê o fim do log para já ter "últimos ouvidos"

with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "countries.json")) as _f:
    _C = json.load(_f)
MCC, COUNTRY, NAME2CC = _C["mcc"], _C["names"], _C["name2cc"]

TS = r"^M: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.\d+ "
RE_START = re.compile(TS + r"DMR Slot (\d), received (RF|network) (?:voice header|late entry) from (\S+) to (TG )?(\S+)")
RE_END = re.compile(TS + r"DMR Slot (\d), received (RF|network) end of voice transmission from (\S+) to (TG )?(\S+), "
                    r"([\d.]+) seconds(?:, (\d+)% packet loss)?, BER: ([\d.]+)%(?:, RSSI: (\S+) dBm)?")
RE_LOST = re.compile(TS + r"DMR Slot (\d), (?:RF voice transmission lost|network watchdog has expired)")
RE_ALIAS = re.compile(TS + r'DMR Slot (\d), Talker Alias "([^"]*)"')


def utc_epoch(s):
    # "AAAA-MM-DD HH:MM:SS" em UTC (o MMDVMHost registra em UTC). Sem strptime: é lento no Pi Zero.
    return calendar.timegm((int(s[0:4]), int(s[5:7]), int(s[8:10]), int(s[11:13]), int(s[14:16]), int(s[17:19]), 0, 0, 0))


def ascii_(s):
    import unicodedata
    return unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode().strip()


class MappedDB:
    """Arquivo grande mapeado em memória: busca rápida sem copiar o conteúdo para a RAM.
    Reabre sozinho quando o Pi-Star atualiza o arquivo (todo dia)."""
    def __init__(self, path, sep):
        self.path, self.sep, self.mm, self.mtime = path, sep, None, 0

    def refresh(self):
        import mmap
        try:
            mt = os.path.getmtime(self.path)
            if mt != self.mtime:
                with open(self.path, "rb") as f:
                    self.mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
                self.mtime = mt
        except (OSError, ValueError):
            self.mm, self.mtime = None, 0

    def line_for(self, call):
        if self.mm is None or not call:
            return None
        pos = self.mm.find(self.sep + call.encode() + self.sep)
        if pos < 0:
            return None
        start = self.mm.rfind(b"\n", 0, pos) + 1
        end = self.mm.find(b"\n", pos)
        return self.mm[start:end if end >= 0 else len(self.mm)].rstrip(b"\r").decode("utf-8", "ignore")


USERS = MappedDB(USERS_FILE, b",")   # RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY
IDS = MappedDB(IDS_FILE, b"\t")      # ID<TAB>CALL<TAB>NAME (reserva)


def norm(s):
    return ascii_(s).lower().replace("&", "and").replace(".", "").strip()


@lru_cache(maxsize=2048)
def _who(call, m1, m2):
    """-> (dmr_id, nome, iso2, país)"""
    line = USERS.line_for(call)
    if line:
        import csv
        row = next(csv.reader([line]), [])
        if len(row) >= 7:
            cname = row[6].strip()
            cc = NAME2CC.get(norm(cname), "") or (MCC.get(row[0][:3], "") if len(row[0]) == 7 else "")
            first = row[2].split(" ")[0] if row[2] else ""
            return row[0], ascii_(first), cc, ascii_(cname)
    line = IDS.line_for(call)
    if line:
        parts = line.split("\t")
        dmrid = parts[0]
        cc = MCC.get(dmrid[:3], "") if len(dmrid) == 7 else ""
        name = parts[2].split(" ")[0] if len(parts) > 2 else ""
        return dmrid, ascii_(name), cc, ascii_(COUNTRY.get(cc, ""))
    return "", "", "", ""


def who(call):
    USERS.refresh(); IDS.refresh()
    return _who(call, USERS.mtime, IDS.mtime)


def station_call():
    try:
        sect = ""
        with open(MMDVM_INI) as f:
            for line in f:
                line = line.strip()
                if line.startswith("["):
                    sect = line
                elif sect == "[General]" and line.lower().startswith("callsign="):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return ""


class State:
    def __init__(self):
        self.cur = None          # transmissão atual ou a última
        self.active = False
        self.last = []           # [{call, cc, start}]
        self.day = None
        self.calls, self.ccs, self.pending = set(), set(), []
        self.alias = {}

    def _count(self, call, t):
        day = time.strftime("%Y-%m-%d", time.localtime(t))
        today = time.strftime("%Y-%m-%d")
        if day != today:
            return
        if self.day != today:
            self.day, self.calls, self.ccs, self.pending = today, set(), set(), []
        if call not in self.calls:
            self.calls.add(call)
            self.pending.append(call)          # país resolvido aos poucos (resolve_some)

    def resolve_some(self, budget=0.3):
        """Resolve o país de algumas estações por ciclo, com limite de tempo
        (no Pi Zero cada busca custa de 0,1 a 1 s)."""
        t0 = time.time()
        while self.pending and time.time() - t0 < budget:
            cc = who(self.pending.pop(0))[2]
            if cc:
                self.ccs.add(cc)

    def start(self, t, slot, src, call, tg_prefix, tgt):
        if self.cur and self.active and self.cur["call"] == call and self.cur["slot"] == "TS" + slot:
            return                                   # late entry/repetição do mesmo QSO
        self._push_last()
        self.cur = {"call": call, "mode": "DMR", "slot": "TS" + slot,
                    "tg": ("TG " if tg_prefix else "") + tgt, "src": "RF" if src == "RF" else "Net",
                    "dur": "", "loss": "", "ber": "", "rssi": "", "start": t}
        self.active = True
        self._count(call, t)

    def end(self, t, slot, src, call, tg_prefix, tgt, dur, loss, ber, rssi):
        if not (self.cur and self.cur["call"] == call):
            self.start(t - int(float(dur)), slot, src, call, tg_prefix, tgt)
        self.cur.update(dur=dur, loss=(loss + "%") if loss else "", ber=ber + "%", rssi=rssi or "")
        self.active = False

    def lost(self):
        self.active = False

    def _push_last(self):
        c = self.cur
        if not c:
            return
        if self.last and self.last[0]["call"] == c["call"]:
            self.last[0]["start"] = c["start"]
        else:
            self.last.insert(0, {"call": c["call"], "start": c["start"]})
            del self.last[LAST_N:]

    def feed(self, line):
        m = RE_START.match(line)
        if m:
            return self.start(utc_epoch(m[1]), *m.groups()[1:])
        m = RE_END.match(line)
        if m:
            return self.end(utc_epoch(m[1]), *m.groups()[1:])
        m = RE_ALIAS.match(line)
        if m and self.cur:
            parts = m[3].split(None, 1)
            if len(parts) == 2 and parts[0] == self.cur["call"]:
                self.alias[parts[0]] = ascii_(parts[1])
            return
        if RE_LOST.match(line):
            self.lost()

    def snapshot(self, station):
        cur = None
        if self.cur:
            dmrid, name, cc, cname = who(self.cur["call"])
            cur = dict(self.cur, id=dmrid, name=name or self.alias.get(self.cur["call"], ""),
                       cc=cc, country=cname)
        last = [{"call": l["call"], "cc": who(l["call"])[2], "start": l["start"]}
                for l in self.last if not self.cur or l["call"] != self.cur["call"]][:LAST_N - 2]
        return {"ts": int(time.time()), "ok": True, "station": station, "active": self.active,
                "cur": cur, "last": last, "today": len(self.calls), "countries": len(self.ccs)}


def latest_log():
    files = sorted(glob.glob(os.path.join(LOG_DIR, "MMDVM-*.log")))
    return files[-1] if files else None


def write_json(obj):
    tmp = os.path.join(OUT_DIR, ".status.json.tmp")
    with open(tmp, "w") as f:
        json.dump(obj, f, ensure_ascii=True, separators=(",", ":"))
    os.replace(tmp, os.path.join(OUT_DIR, "status.json"))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    flags = os.path.join(OUT_DIR, "flags")
    if not os.path.islink(flags):
        try:
            os.symlink(os.path.join(SHARE, "flags"), flags)
        except OSError as e:
            print("aviso: não criei o link das bandeiras:", e, file=sys.stderr)

    st, station = State(), station_call()
    path, fh, buf = None, None, ""
    last_write, last_sig = 0, None
    while True:
        newest = latest_log()
        if newest and newest != path:                 # início ou virada do dia
            if fh:
                fh.close()
            path, fh = newest, open(newest, "r", errors="ignore")
            if last_write == 0:                        # só no primeiro arquivo: lê o fim
                size = os.path.getsize(path)
                if size > BOOTSTRAP_BYTES:
                    fh.seek(size - BOOTSTRAP_BYTES)
                    fh.readline()
            station = station_call() or station
        if fh:
            buf += fh.read()
            *lines, buf = buf.split("\n")
            for line in lines:
                st.feed(line)
        st.resolve_some()
        snap = st.snapshot(station)
        sig = json.dumps([snap["active"], snap["cur"], snap["last"]])
        now = time.time()
        if sig != last_sig or now - last_write >= 5:   # sempre que muda, e a cada 5 s (para o "ts")
            write_json(snap)
            last_sig, last_write = sig, now
        time.sleep(INTERVAL)


if __name__ == "__main__":
    main()
