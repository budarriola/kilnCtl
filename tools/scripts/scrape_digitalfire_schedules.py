"""Scrape digitalfire.com firing schedules into structured JSON.

Deliberately parses the HTML table rather than asking a summarizer: the
Celsius column reads "316 deg C/hr to 913C" and the LAST column is CUMULATIVE
elapsed time, not hold time. A language model reading the rendered page
conflated the two on the first attempt, which for a firing schedule is the
difference between a 15-minute soak and a 2h48m one.
"""
import html
import json
import re
import sys
import urllib.request

SLUGS = [
    "03dsff", "04dsdh", "bq1000", "brtf05", "brtf6", "brts6", "btfb04",
    "btsb04", "btsg05", "c04pltp", "c10rpl", "c5dhsc", "c6dhsc", "c6ired",
    "c6msgl1", "c6plst", "fscg1", "fscgb1", "fscgcl", "fscgwm", "fscrgl",
    "fshp1", "fshp3", "fsnm5", "mddcl", "plc6cr", "plc6ds", "qica",
]

UA = {"User-Agent": "Mozilla/5.0 (compatible; kilnCtl schedule import)"}


def clean(s):
    s = re.sub(r"<[^>]+>", " ", s)
    s = html.unescape(s)
    s = s.replace("\xa0", " ")
    return re.sub(r"\s+", " ", s).strip()


def parse_step(cell):
    """'316 C/hr to 913C' -> (316.0, 913.0). Returns None if unparseable."""
    t = clean(cell)
    m = re.search(r"(-?[\d.]+)\s*°?\s*C\s*/\s*hr\s*to\s*(-?[\d.]+)\s*°?\s*C", t, re.I)
    if not m:
        return None
    return float(m.group(1)), float(m.group(2))


def parse_hold(cell):
    """Hold column: blank, '15', '15 min', or 'h:mm'. Returns minutes."""
    t = clean(cell)
    if not t:
        return 0
    m = re.match(r"^(\d+):(\d{2})$", t)
    if m:
        return int(m.group(1)) * 60 + int(m.group(2))
    m = re.search(r"(\d+)", t)
    return int(m.group(1)) if m else 0


def scrape(slug):
    url = "https://digitalfire.com/schedule/" + slug
    raw = urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=30)
    doc = raw.read().decode("utf8", "replace")

    title = ""
    m = re.search(r"<title>(.*?)</title>", doc, re.S | re.I)
    if m:
        title = clean(m.group(1))

    tbl = re.search(r"<table[^>]*>(.*?)</table>", doc, re.S | re.I)
    if not tbl:
        return {"slug": slug, "url": url, "title": title, "error": "no table"}

    headers = [clean(h) for h in re.findall(r"<th[^>]*>(.*?)</th>", tbl.group(1), re.S | re.I)]

    steps = []
    for row in re.findall(r"<tr[^>]*>(.*?)</tr>", tbl.group(1), re.S | re.I):
        cells = re.findall(r"<td[^>]*>(.*?)</td>", row, re.S | re.I)
        if len(cells) < 4:
            continue
        parsed = parse_step(cells[1])
        if not parsed:
            continue
        ramp, target = parsed
        steps.append({"ramp_c_per_hr": ramp, "target_c": target,
                      "hold_min": parse_hold(cells[3]),
                      "cumulative": clean(cells[4]) if len(cells) > 4 else ""})

    return {"slug": slug, "url": url, "title": title, "headers": headers, "steps": steps}


out = []
for s in SLUGS:
    try:
        r = scrape(s)
    except Exception as e:  # noqa: BLE001
        r = {"slug": s, "error": str(e)[:120]}
    out.append(r)
    n = len(r.get("steps", []))
    print("%-9s %-2s steps  %s" % (s, n, r.get("title", r.get("error", ""))[:70]), flush=True)

json.dump(out, open(sys.argv[1], "w", encoding="utf8"), indent=1)
print("\nwrote", sys.argv[1])
