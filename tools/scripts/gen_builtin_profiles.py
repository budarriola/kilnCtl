"""Generate profiles_builtin_table.inc -- the catalog table ONLY -- from the scraped JSON.

Generated rather than hand-typed: 136 segments transcribed by hand is 136
chances to put a digit in the wrong place, and a wrong digit here is a ruined
firing. Re-run this if the source schedules ever change.

IMPORTANT: this script writes ONLY profiles_builtin_table.inc, which contains
nothing but the g_builtin_profiles[] table and its count. The API
implementation lives in the hand-written profiles_builtin.c, which #includes
this .inc. Do not point this script at profiles_builtin.c -- the whole reason
for the split is that regenerating must never be able to delete hand-written
code.

Usage:
  python tools/scripts/gen_builtin_profiles.py SCRAPED.json OUT.inc

where OUT.inc is firmware/KilnFW/App/drivers/profiles_builtin_table.inc
"""
import json
import re
import sys

src, out_c = sys.argv[1], sys.argv[2]
if not out_c.endswith(".inc"):
    raise SystemExit(
        "refusing to write %r: this generator emits only the table, into a .inc file. "
        "profiles_builtin.c is hand-written." % out_c)
data = json.load(open(src, encoding="utf8"))

# Titles as shown on the index page, keyed by slug. The <title> tag only says
# "<CODE> Firing Schedule", which loses the human-readable half.
TITLES = {
    "03dsff": "Cone 03 Fast Fire",
    "04dsdh": "Low Temperature Drop-and-Hold",
    "bq1000": "Plainsman Electric Bisque",
    "brtf05": "Bartlett Fast Glaze Cone 05",
    "brtf6": "Bartlett Fast Glaze Cone 6",
    "brts6": "Bartlett Slow Glaze Cone 6",
    "btfb04": "Bartlett Fast Bisque Cone 04",
    "btsb04": "Bartlett Slow Bisque Cone 04",
    "btsg05": "Bartlett Slow Glaze Cone 05",
    "c04pltp": "Plainsman Low Temperature Drop-and-hold",
    "c10rpl": "Plainsman Cone 10R Firing",
    "c5dhsc": "Plainsman Cone 5 Drop-and-Hold Slow-Cool",
    "c6dhsc": "Plainsman Cone 6 Drop-and-hold, Slow Cool",
    "c6ired": "Cone 6 Iron Reds",
    "c6msgl1": "Mastering Glazes Cone 6",
    "c6plst": "Plainsman Cone 6 Electric Standard",
    "fscg1": "Shimbo Crystal Schedule 1",
    "fscgb1": "Shimbo Crystal Holding Pattern 2",
    "fscgcl": "Shimbo Crystal Celestite Schedule",
    "fscgwm": "Wollast-O-Matte Fara Shimbo Crystalline Glaze",
    "fscrgl": "GC106 Base for Crystalline Glazes",
    "fshp1": "Shimbo Crystal Holding Pattern 1",
    "fshp3": "Shimbo Crystal Holding Pattern 3",
    "fsnm5": "Fa's Number Five",
    "mddcl": "Medalta Decal Firing",
    "plc6cr": "Cone 6 Crystal Glaze Plainsman",
    "plc6ds": "Cone 6 Drop-and-Soak",
    "qica": "Quartz Inversion Cracking Avoider",
}

rows = []
for entry in data:
    slug = entry["slug"]
    steps = entry.get("steps", [])
    if not steps:
        raise SystemExit("no steps for %s -- refusing to emit a partial catalog" % slug)
    code = re.match(r"([A-Za-z0-9]+)", entry["title"]).group(1).upper()
    if len(code) > 15:
        raise SystemExit("code too long for profile name field: %s" % code)
    rows.append((code, slug, TITLES[slug], steps))

with open(out_c, "w", encoding="utf8", newline="\n") as f:
    f.write("""/* GENERATED FILE -- do not hand-edit.
 *
 * Regenerate with:
 *   python tools/scripts/gen_builtin_profiles.py SCRAPED.json \\
 *       firmware/KilnFW/App/drivers/profiles_builtin_table.inc
 *
 * This file holds ONLY the catalogue table. It is #included by
 * profiles_builtin.c, which is hand-written and holds the API implementation
 * -- regenerating this file therefore cannot wipe hand-written code, which is
 * exactly why the split exists.
 *
 * Hand-transcribing %d segments is %d chances to misplace a digit, and a
 * misplaced digit in a firing schedule ruins a kiln load.
 *
 * Source: the published firing schedules at https://digitalfire.com/schedule
 * (see the Credits section on the web Profiles page). Values are taken from
 * each schedule's Celsius column verbatim. The site's tables also carry a
 * cumulative-elapsed-time column which is NOT a hold time -- it is not
 * imported, and conflating the two was the first mistake made reading these.
 *
 * Semantics note: a source step with rate 0 C/hr means "no rate limit, go as
 * fast as the kiln can" on a Bartlett-style controller. profile_executor.c
 * already reads ramp_c_per_hr <= 0 exactly that way (it jumps the setpoint
 * straight to the segment target), so 0 is imported unchanged.
 */

const builtin_profile_t g_builtin_profiles[] = {
""" % (sum(len(r[3]) for r in rows), sum(len(r[3]) for r in rows)))

    for code, slug, title, steps in rows:
        f.write("    {\n")
        f.write('        .code = "%s",\n' % code)
        f.write('        .title = "%s",\n' % title.replace('"', '\\"'))
        f.write('        .slug = "%s",\n' % slug)
        f.write("        .segment_count = %d,\n" % len(steps))
        f.write("        .segments = {\n")
        for s in steps:
            f.write("            { .target_c = %.1ff, .ramp_c_per_hr = %.1ff, .dwell_min = %d },\n"
                    % (s["target_c"], s["ramp_c_per_hr"], s["hold_min"]))
        f.write("        },\n")
        f.write("    },\n")

    f.write("};\n\n")
    f.write("const size_t g_builtin_profile_count =\n"
            "    sizeof(g_builtin_profiles) / sizeof(g_builtin_profiles[0]);\n")

print("wrote %s: %d profiles, %d segments"
      % (out_c, len(rows), sum(len(r[3]) for r in rows)))
