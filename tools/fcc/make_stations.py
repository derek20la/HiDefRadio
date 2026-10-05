#!/usr/bin/env python3
"""
make_stations.py - builds HiDef Radio's built-in station list from FCC data.

The app ships two small text files (app/src/main/assets/stations/fm.txt and am.txt)
that say which stations are licensed on each frequency: call letters, city of
license, coordinates, power. The app uses them to

  * work out the call letters of stations whose RDS "PI" code can't be turned back
    into letters on its own (iHeart stations send the PI with its first digit
    replaced by 1 - nine calls are possible, but normally only ONE of the nine is
    licensed on the frequency you are tuned to),
  * tell a call sign from a name that merely looks like one (KTWV's HD Radio name is
    "WAVE" - no station "WAVE" is licensed on 94.7),
  * show the city of license, power and class in the signal details.

The data is the FCC's (public domain). Stations change, so refresh it now and then:

  1. In a web browser open these two links (each is one long page of text; the FM one
     takes a minute or two) and save each page (Ctrl+S) as fcc_fm.txt / fcc_am.txt:

     https://transition.fcc.gov/fcc-bin/fmq?state=&call=&city=&arn=&serv=&vac=&freq=0.0&fre2=107.9&facid=&class=&dkt=&list=4&dist=&dlat2=&mlat2=&slat2=&NS=N&dlon2=&mlon2=&slon2=&EW=W&size=9
     https://transition.fcc.gov/fcc-bin/amq?state=&call=&city=&arn=&serv=&vac=&freq=530&fre2=1700&facid=&class=&dkt=&list=4&dist=&dlat2=&mlat2=&slat2=&NS=N&dlon2=&mlon2=&slon2=&EW=W&size=9

  2. python3 make_stations.py fcc_fm.txt fcc_am.txt ../../app/src/main/assets/stations 2026-10-01
     (the last argument is the day you downloaded the files)

What goes in, what stays out
  FM: licensed (status LIC) full-power stations (FM), low power (FL), translators (FX)
      and boosters (FB) in the US; licensed stations with a call sign in Canada, Mexico
      and the other countries the FCC lists. Left out: construction permits and
      applications (not on the air yet), auxiliary/backup antennas (FS), allotments
      (FA), deleted stations (call letters starting with D).
  AM: licensed US stations; Canadian and Mexican stations as notified to the FCC.
      The FCC lists day and night operation as separate records - merged here.

File format (one station per line, grouped by frequency):
  # comment
  @10110                                   <- frequency: FM in units of 10 kHz (101.1 MHz), AM in kHz (@1260)
  KRTH|FM|B|H|Los Angeles|CA|US|34.2272|-118.0676|51|955
  call|service|class|H = HD Radio on file|city|state|country|latitude|longitude|ERP kW|antenna height above average terrain, m
  AM lines:
  KMZT|AM|B|Beverly Hills|CA|US|34.0|-118.3|20|7.5|dn
  call|AM|class|city|state|country|latitude|longitude|day kW|night kW|d/n = directional by day / by night
"""
import sys, os, re

def fields(path):
    with open(path, encoding="latin-1") as f:
        for line in f:
            p = [x.strip() for x in line.rstrip("\r\n").split("|")]
            if len(p) > 30:
                yield p

def num(text):
    """'51.    kW' -> 51.0 ; '-' or '' -> None"""
    m = re.match(r"\s*(-?\d+\.?\d*)", text)
    return float(m.group(1)) if m else None

def short(x, digits=3):
    """51.0 -> '51', 0.028 -> '0.028' (no trailing zeros)"""
    if x is None: return ""
    s = "%.*f" % (digits, x)
    if "." in s: s = s.rstrip("0").rstrip(".")       # only after a decimal point: 880 must stay 880
    return s if s else "0"

def degrees(hemi, d, m, s):
    try:
        v = float(d) + float(m) / 60.0 + float(s) / 3600.0
    except ValueError:
        return None
    return -v if hemi in ("S", "W") else v

def city_name(caps):
    """'LOS ANGELES' -> 'Los Angeles', 'MCALLEN' -> 'McAllen', "COEUR D'ALENE" -> "Coeur d'Alene" """
    words = []
    for w in caps.lower().split():
        w = "-".join(part[:1].upper() + part[1:] for part in w.split("-"))
        if w.startswith("Mc") and len(w) > 3: w = "Mc" + w[2].upper() + w[3:]
        w = re.sub(r"^([DdLl])'(\w)", lambda m: m.group(1).lower() + "'" + m.group(2).upper(), w)
        w = re.sub(r"^O'(\w)", lambda m: "O'" + m.group(1).upper(), w)
        words.append(w)
    name = " ".join(words)
    for small in (" Of ", " The ", " De ", " Del ", " La ", " Las ", " Los ", " And "):
        if name.find(small) > 0: name = name.replace(small, small.lower())
    return name.replace("|", "/")

def good_call(call, country):
    if not call or call in ("-", "NEW") or " " in call: return False
    if country == "US":
        # K... / W... only: a leading D marks a deleted station ("DKRJX")
        return re.match(r"^[KW][A-Z0-9]{2,5}(-[A-Z0-9]+)?$", call) is not None
    return re.match(r"^[A-Z0-9]{3,7}(-?[A-Z0-9]+)?$", call) is not None

def build_fm(path):
    best = {}                                   # (freq, call) -> row; several records per station happen
    rank = {"FM": 0, "FL": 1, "FX": 2, "FB": 3}
    for p in fields(path):
        call, service, status, country = p[1], p[3], p[9], p[12]
        if service not in rank or status != "LIC" or not good_call(call, country): continue
        mhz = num(p[2])
        if mhz is None: continue
        freq = int(round(mhz * 100))            # 101.1 MHz -> 10110
        erp = max([x for x in (num(p[14]), num(p[15])) if x is not None], default=None)
        haat = max([x for x in (num(p[16]), num(p[17])) if x is not None], default=None)
        lat, lon = degrees(p[19], p[20], p[21], p[22]), degrees(p[23], p[24], p[25], p[26])
        if lat is None or lon is None: continue
        row = dict(freq=freq, call=call, service=service, cls=p[7] if p[7] != "-" else "", hd="H" if p[6] == "H" else "",
                   city=city_name(p[10]), state=p[11], country=country, lat=lat, lon=lon, erp=erp, haat=haat)
        key = (freq, call)
        old = best.get(key)
        # keep the main licence: the lower service rank, then the higher power
        if old is None or (rank[service], -(erp or 0)) < (rank[old["service"]], -(old["erp"] or 0)):
            if old is not None and old["hd"]: row["hd"] = "H"
            best[key] = row
        elif row["hd"]:
            old["hd"] = "H"
    return list(best.values())

def build_am(path):
    st = {}
    for p in fields(path):
        call, status, country = p[1], p[9], p[12]
        if country not in ("US", "CA", "MX") or not good_call(call, country): continue
        if country == "US" and status != "LIC": continue
        khz = num(p[2])
        if khz is None: continue
        freq = int(round(khz))
        lat, lon = degrees(p[19], p[20], p[21], p[22]), degrees(p[23], p[24], p[25], p[26])
        if lat is None or lon is None: continue
        hours, kw, da = p[5], num(p[14]), p[15].startswith("Dir")
        key = (freq, call)
        row = st.get(key)
        if row is None or (status == "LIC" and row["status"] != "LIC"):
            # (outside the US a station can have a licensed and a planned record - the licensed one wins)
            row = st[key] = dict(freq=freq, call=call, service="AM", cls=p[7] if p[7] != "-" else "", city=city_name(p[10]),
                                 state=p[11], country=country, lat=lat, lon=lon, day=None, night=None, da="", status=status)
        elif status != row["status"]:
            continue
        if hours in ("DAY", "UNL") and kw is not None and (row["day"] is None or kw > row["day"]):
            row["day"] = kw
            if da and "d" not in row["da"]: row["da"] += "d"
        if hours in ("NIG", "UNL") and kw is not None and (row["night"] is None or kw > row["night"]):
            row["night"] = kw
            if da and "n" not in row["da"]: row["da"] += "n"
    return list(st.values())

def write(rows, path, title, line):
    rows.sort(key=lambda r: (r["freq"], r["country"] != "US", r["state"], r["city"], r["call"]))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(title)
        freq = None
        for r in rows:
            if r["freq"] != freq:
                freq = r["freq"]
                f.write("@%d\n" % freq)
            f.write(line(r) + "\n")

def main():
    if len(sys.argv) != 5:
        print(__doc__); sys.exit(1)
    fm_in, am_in, out_dir, day = sys.argv[1:]
    os.makedirs(out_dir, exist_ok=True)
    fm, am = build_fm(fm_in), build_am(am_in)
    head = "# HiDef Radio station list - source: FCC %s Query (public domain) - date %s - stations %d\n"
    write(fm, os.path.join(out_dir, "fm.txt"), head % ("FM", day, len(fm)) +
          "# @frequency in 10 kHz, then call|service|class|H=HD|city|state|country|lat|lon|ERP kW|HAAT m\n",
          lambda r: "|".join([r["call"], r["service"], r["cls"], r["hd"], r["city"], r["state"], r["country"],
                              "%.4f" % r["lat"], "%.4f" % r["lon"], short(r["erp"]), short(r["haat"], 0)]))
    write(am, os.path.join(out_dir, "am.txt"), head % ("AM", day, len(am)) +
          "# @frequency in kHz, then call|AM|class|city|state|country|lat|lon|day kW|night kW|d/n=directional\n",
          lambda r: "|".join([r["call"], "AM", r["cls"], r["city"], r["state"], r["country"],
                              "%.4f" % r["lat"], "%.4f" % r["lon"], short(r["day"]), short(r["night"]), r["da"]]))
    print("FM: %d stations, AM: %d stations -> %s" % (len(fm), len(am), out_dir))

if __name__ == "__main__":
    main()
