# False-positive notes for `audit_plan_doc_drift.ps1`

Written after the first full hand-triage of its output (81 findings, all
categories, 2026-09-03). Not authored by the script itself — sibling file,
since the constraints of that triage pass forbade editing any `.ps1`. Fold
these into the script's own header comment (or an allow-list) when someone
next has edit access to it.

## Shapes that fired and shouldn't have

**1. "formerly `X`" is not recognized as deletion/rename acknowledgement
language (detector A).** `firmware/KilnFW/App/drivers/README.md:13` reads
`panel_spi.c/.h` (formerly `ILI9488.c/.h`)`` — the doc already correctly
says the file was renamed, in the same breath it names the old path. The
ack-language allow-list needs `formerly` alongside whatever `deleted`/
`removed`/`renamed` phrases it already has.

**2. Dated, past-tense changelog entries read as present-tense claims
(detector A and B both).** Two shapes:
   - `firmware/KilnFW/TODO.md:2007`: a dated "Running total for this pass"
     entry naming `tests/test_kilnsim_testmgr.py`, true and current on the
     day it was written, false today only because kilnsim was deleted
     *after* that changelog entry was made. The file no longer exists, but
     the sentence was never wrong.
   - `firmware/KilnFW/TODO.md:251`: a `[x] ... DONE 2026-08-27` item citing
     `POST /api/rules` — the endpoint it names was itself deleted on
     2026-08-27, the same day. The detector has no way to know the doc's
     dateline predates (or is same-day as) the deletion. Any line under a
     `[x] ... DONE <date>` or dated changelog heading should probably be
     exempted from B/A entirely, or at least weighted down hard — a
     completed/dated entry is inherently describing a past state, not
     asserting a current one.

**3. Vendor/SDK paths outside the repo, misread as repo-relative (detector
A).** `firmware/SaftyFW/docs/BOOTLOADER.md:407` (`pico/flash.c`) and
`firmware/SaftyFW/docs/HARDWARE.md:299` (`src/boards/include/boards/
pico.h`) both cite real files — inside the **pico-sdk**, an external
dependency this repo doesn't vendor a copy of, not a path under any of the
repo's known source roots. The detector's "known source roots" list needs
either a pico-sdk carve-out, or these two need distinguishing surrounding
language (e.g. "pico-sdk's `X`") recognized as an external-reference marker
so they're not scored the same as an in-repo dead path.

**4. Prose contrasting `/api/` as a prefix, not asserting an endpoint
(detector B).** `firmware/KilnFW/docs/WIFI_PROVISIONING.md:328`: "Note all
three live at the server root rather than under `` `/api/` ``" — the
backtick span is a bare prefix used to say what these endpoints are *not*
under, not a route registration claim. Detector B should probably require
at least one path segment after `/api/` before treating a hit as an
endpoint assertion.

**5. Endpoints proposed in an explicitly not-yet-built plan section
(detector B).** `firmware/KilnFW/docs/UI_PLAN.md:67-68`: `/api/settings/
export` and `/api/profiles/export` appear under "## Open: settings import/
export (partial) and profile import/export" as a *naming convention* for a
feature not yet implemented ("Needs confirming actual field lists...
before implementation starts" two lines later). They were never live, so
"no `.uri` registration found" is correct but not a signal of drift — nothing
regressed. If a `##`/`###` heading in the same file says "Open" / "Not yet
built" / "planned", routes cited under it should be suppressed.

## Not a tool bug, but worth recording

**6. Detector D fired 33/33 as noise, exactly as the tool's own header
predicts.** Every D finding checked (representative sample across every
file it touched except the two under another agent's ownership) was either
a correctly-checked-off `[x]` item with residual "still open" phrasing
describing a *different* nearby sub-item, or a genuinely-still-open item —
several carrying explicit `**Re-swept 2026-09-03: already landed**`
verification stamps proving they'd been checked recently, not just left
stale. D's own doc comment already says "this does not prove the doc is
stale" and that held for every instance found. No proposed fix beyond what
the tool already says about itself — just confirming the warning is
accurate and D should stay advisory-only, not get a stricter default.
