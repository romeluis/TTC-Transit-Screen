#!/usr/bin/env python3
"""Generate web/subway_catalog.js: NTAS platform codes for every subway station.

There is no "list all stations" NTAS endpoint, so the catalog is built from
two sources:
  1. TTC GTFS stops.txt (Toronto Open Data) — every stop whose name contains
     "PLATFORM" is a subway platform candidate; its stop_code is the NTAS code.
  2. The NTAS API itself — each candidate code is probed and the response's
     authoritative "line" and "directionText" fields are used. Codes that
     return no data (defunct platforms, closures) are dropped.

Run while the subway is operating (roughly 06:00-01:30 Toronto time),
otherwise NTAS returns empty arrays and everything gets dropped.

Usage: python3 tools/gen_subway_catalog.py [--stops path/to/stops.txt]
Without --stops, the current GTFS zip is downloaded from Toronto Open Data.
"""

import argparse
import csv
import io
import json
import re
import sys
import time
import urllib.request
import zipfile
from collections import defaultdict
from pathlib import Path

CKAN_PACKAGE = (
    "https://ckan0.cf.opendata.inter.prod-toronto.ca/api/3/action/"
    "package_show?id=ttc-routes-and-schedules"
)
NTAS_URL = "https://ntas.ttc.ca/api/ntas/get-next-train-time/"
OUT_PATH = Path(__file__).resolve().parent.parent / "web" / "subway_catalog.js"

LINE_NAMES = {
    "1": "Line 1 Yonge-University",
    "2": "Line 2 Bloor-Danforth",
    "4": "Line 4 Sheppard",
    "5": "Line 5 Eglinton",
    "6": "Line 6 Finch West",
}

# Sort platforms of a station in a stable, sensible order.
DIRECTION_ORDER = ["north", "south", "east", "west"]


def http_get(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "ttc-transit-screen-catalog-gen"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def fetch_gtfs_stops():
    print("fetching GTFS package metadata...", file=sys.stderr)
    pkg = json.loads(http_get(CKAN_PACKAGE))
    zip_url = None
    for res in pkg["result"]["resources"]:
        if res.get("format", "").upper() == "ZIP" or res["url"].endswith(".zip"):
            zip_url = res["url"]
            break
    if not zip_url:
        sys.exit("no GTFS zip resource found in CKAN package")
    print(f"downloading {zip_url} ...", file=sys.stderr)
    blob = http_get(zip_url, timeout=600)
    with zipfile.ZipFile(io.BytesIO(blob)) as zf:
        with zf.open("stops.txt") as f:
            return f.read().decode("utf-8-sig")


def platform_candidates(stops_txt):
    """stop_code -> raw stop_name for every '... PLATFORM' stop."""
    out = {}
    for row in csv.DictReader(io.StringIO(stops_txt)):
        name = row.get("stop_name", "")
        code = row.get("stop_code", "").strip()
        if "PLATFORM" in name.upper() and code.isdigit():
            out[int(code)] = name.strip()
    return out


def probe_ntas(code):
    """Return (line, direction_text) or None when the code has no data."""
    try:
        data = json.loads(http_get(NTAS_URL + str(code), timeout=20))
    except Exception as e:  # noqa: BLE001 - network flake: skip, report
        print(f"  {code}: request failed ({e})", file=sys.stderr)
        return None
    if not isinstance(data, list) or not data:
        return None
    entry = data[0]
    line = str(entry.get("line", "")).strip()
    direction = str(entry.get("directionText", "")).strip()
    if not line or not direction:
        return None
    return line, direction


def station_name(gtfs_name):
    """'BATHURST STATION - EASTBOUND PLATFORM' -> 'Bathurst'.

    Line 5/6 names have no ' - ' separator ('X Station Eastbound Platform'),
    so the direction/platform suffix is stripped by pattern too.
    """
    name = gtfs_name.split(" - ")[0]
    name = re.sub(
        r"\s*(EAST|WEST|NORTH|SOUTH)?BOUND?\s*PLATFORM\b.*$", "", name, flags=re.I
    )
    name = re.sub(r"\s+PLATFORM\b.*$", "", name, flags=re.I)
    name = re.sub(r"\s+STATION(\s+LRT)?$", "", name, flags=re.I).strip()
    # Title-case but keep hyphens/apostrophes sane (ST GEORGE -> St George).
    name = " ".join(w.capitalize() for w in name.lower().split())
    name = re.sub(r"\b(\w)'(\w)", lambda m: f"{m.group(1)}'{m.group(2).upper()}", name)
    name = "-".join(p[:1].upper() + p[1:] for p in name.split("-"))
    return name


def direction_key(direction_text):
    d = direction_text.lower()
    for i, word in enumerate(DIRECTION_ORDER):
        if d.startswith(word):
            return i
    return len(DIRECTION_ORDER)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stops", help="path to an already-extracted stops.txt")
    args = ap.parse_args()

    stops_txt = (
        Path(args.stops).read_text(encoding="utf-8-sig")
        if args.stops
        else fetch_gtfs_stops()
    )
    candidates = platform_candidates(stops_txt)
    print(f"{len(candidates)} platform candidates, probing NTAS...", file=sys.stderr)

    # line -> station -> [(sort_key, {c, d})]
    lines = defaultdict(lambda: defaultdict(list))
    kept = 0
    for code in sorted(candidates):
        result = probe_ntas(code)
        if result is None:
            print(f"  {code}: no data, dropped ({candidates[code]})", file=sys.stderr)
            continue
        line, direction = result
        station = station_name(candidates[code])
        lines[line][station].append((direction_key(direction), {"c": code, "d": direction}))
        kept += 1
        time.sleep(0.15)  # be polite
    print(f"kept {kept} platforms on lines {sorted(lines)}", file=sys.stderr)

    catalog = {}
    for line in sorted(lines):
        stations = []
        for name in sorted(lines[line]):
            platforms = [p for _, p in sorted(lines[line][name], key=lambda t: t[0])]
            stations.append({"n": name, "p": platforms})
        catalog[line] = {
            "name": LINE_NAMES.get(line, f"Line {line}"),
            "stations": stations,
        }

    js = (
        "// Generated by tools/gen_subway_catalog.py from TTC GTFS stops.txt\n"
        "// + NTAS API probes. Regenerate if TTC renumbers stops (rare).\n"
        f"// Generated: {time.strftime('%Y-%m-%d')}\n"
        "const SUBWAY_CATALOG = "
        + json.dumps(catalog, ensure_ascii=False, separators=(",", ":"))
        + ";\n"
    )
    OUT_PATH.write_text(js, encoding="utf-8")
    print(f"wrote {OUT_PATH} ({len(js)} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()
