# Browser checks for the KilnFW web UI

Three checks that catch classes of bug the host tests structurally cannot:
the host suite never renders a page, and `curl` never runs the JavaScript or
opens more than one connection at a time. Every bug listed below was reported
by the owner from a real browser and was invisible to everything else we had.

| Script | Needs a board? | Catches |
|---|---|---|
| `../lint_pages.js` | no | inline-script syntax errors |
| `topbar_check.js` | yes | Home/Menu misalignment on any page |
| `ui_sweep.js` | yes | overflow, overlap, small tap targets, console errors, dead buttons |

## Running them

```sh
# No hardware needed -- run this one on every page edit.
node firmware/KilnFW/App/test/lint_pages.js firmware/KilnFW/App/drivers

# These need a reachable board (override the address if it moved).
node firmware/KilnFW/App/test/webcheck/topbar_check.js
KILN_BASE=http://192.168.1.156 node firmware/KilnFW/App/test/webcheck/ui_sweep.js
```

Playwright is not vendored here. Install it wherever you run these:
`npm i playwright && npx playwright install chromium`.

## Why each exists

**`lint_pages.js`** — a comment written as `MAX31856_MASK_*` followed by `/`
closed its own block comment early, so the rest of the line parsed as code.
The syntax error killed the entire inline script, and the thermocouple faults
page sat on "Loading…" forever while its endpoint answered correctly in 160 ms.
`curl` saw a perfect page and a perfect API; only a browser saw the failure.

**`topbar_check.js`** — nav.js injects the topbar into every page, but Home is
an `<a>` and Menu is a `<button>`. Any page-local rule targeting bare `button`
hits one and not the other: `zones_page.html`'s `button { margin-top: 1em; }`
pushed Menu 13.6 px below Home. A second, subtler version survived that fix —
a `<button>` does not inherit the page font, and `line-height: normal` resolves
taller in a button than in a link, leaving the Menu label 3 px low on *every*
page.

**`ui_sweep.js`** — the "Download backup button goes out of frame on the phone"
class of report. Runs each page at phone, tablet and desktop widths.

## Two hard-won cautions

**Close every browser you open, and never run two of these at once.** The
board's HTTP server has a bounded socket pool; abandoned browser connections
wedge it until they are killed, and the symptom looks exactly like a firmware
hang. Killing the stray browsers brings it straight back.

**A clean sweep is weaker evidence than it looks.** These scripts load pages
sequentially, so they rarely open enough concurrent connections to reproduce a
load-dependent failure. A sweep reported "console errors: none" on the very
build whose faults page failed 3-for-3 in a normal browser tab. Treat a clean
run as "nothing obvious", not "verified".

## After flashing, hard-reload first

Assets are served with `Cache-Control: no-cache` as of 2026-08-22, but a
browser that cached them *before* that change can still hold a stale copy.
A page rendering markup the firmware no longer contains is the signature —
check that before debugging the page itself.
