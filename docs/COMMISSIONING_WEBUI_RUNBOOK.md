# M18 Web-UI Phase Runbook

Prepares the web-interface leg of M18 commissioning
(`docs/COMMISSIONING_TEST_MATRIX.md`), owner order item 2 ("Web UI second"),
per `project_owner_decisions_2026_09_21_login`. This file adds the *how to
drive it* layer; the exhaustive control-by-control inventory already exists
in `COMMISSIONING_TEST_MATRIX.md`'s "Page-by-page inventory" and is not
duplicated here. Source snapshot: `origin/main` at 51effca5.

## Scope

18 page-shell HTML files under `firmware/KilnFW/App/drivers/http/` and
`firmware/KilnFW/App/drivers/net/` are served by KilnFW's httpd
(`route_tier_table.h` currently lists 157 `ROUTE_TIER(...)` rows -- the
matrix's stated 155 is one snapshot stale by two rows as of 51effca5, not
a discrepancy worth chasing further here). Pages:

`main_page.html`, `profiles_page.html`, `live_profile_page.html`,
`zones_page.html`, `safety_config_page.html`, `safety_page.html`,
`safety_commissioning_page.html`, `diagnostics_page.html`,
`settings_page.html`, `settings_display_page.html`, `kiln_configs_page.html`,
`backup_page.html`, `readiness_page.html`, `setup_wizard_page.html`,
`login_page.html`, and (under `drivers/net/`) `ota_page.html`,
`security_page.html`, `wifi_provision_page.html`. Static shared assets
(`theme.css`, `nav.js`, `app.js`, `commissioning_shared.js`) are served but
have no controls of their own.

Per the matrix's own count: ~120 distinct controls (161 raw `<button>`
matches across 18 pages before dedup), 157 total HTTP routes, of which
~30 are hardware-gated (relay-driving, OTA/reboot, CT sweep, factory reset,
E-stop verify, danger mode) and therefore **deferred**, not exercised in
this phase.

## Classification used below

- **Read-only** -- a GET, or a POST with no persisted side effect (status
  polls, page loads, abort/cancel paths that only stop something already in
  motion).
- **Reversible (restore step named)** -- a POST that changes board state but
  has a known, testable way back (e.g. save a PID gain, then restore the
  prior value; toggle a setting, then toggle it back; forget a Wi-Fi network
  added for the test, then re-add the bench network).
- **Heating/unsafe (deferred)** -- drives a relay, exits/enters a mode that
  bypasses safety gating, or performs an irreversible/destructive action not
  worth doing during this phase (factory reset "all", OTA flash, E-stop
  verify, danger mode, current sweep). These carry the matrix's own
  "Hardware-gated" tag and stay `BLOCKED` on this bench per that document's
  legend -- this phase does not change that.

This runbook does not re-derive the tier or route for each control --
`route_tier_table.h` is the single authoritative source
(`COMMISSIONING_TEST_MATRIX.md`'s own caveat), and its page-by-page table
already carries route + tier + MCP tool + bench class per control. Below,
controls are re-grouped by the three classes above with a pointer back into
that table's rows.

## Read-only controls (safe to exercise anytime, any order)

All page loads / status GETs / poll loops on every page (dashboard poll,
profile list/detail reads, zones/safety/diagnostics page loads, OTA
interlock status, readiness page, session status poll, wifi status/scan/
networks list, kiln_configs list, backup export *read*, autotune/adaptive
status and trace reads, dualwrite/lwip/timing/stack-margin diagnostics
reads, coredump info/chunk, cfgfs status). Also client-side-only controls
that touch no route at all: theme toggle, New/Cancel/Add-segment buttons
before Save, wizard Start/Next/Back navigation, Cancel on Wi-Fi connect.
Abort-type controls (`sweepAbortBtn`, `atAbortBtn`, danger-mode
`dangerExitBtn`) are read-only *in effect* for this phase since they only
stop something already gated off on this bench.

Full row-level list: every row in `COMMISSIONING_TEST_MATRIX.md`'s tables
marked "Testable" whose Route is a GET, or whose Route note says
"client-side only", or whose action is Abort/Cancel/Stop.

## Reversible controls (restore step required, do in this phase)

These change persisted board state but have a defined way back. Restore
step given where it is not simply "set it back to the value you read
first":

| Control | Route | Restore |
|---|---|---|
| Start/Stop/Pause/Resume firing (bench 4 W load) | `/api/profile_exec/*` | `profiles_stop` leaves no persisted state; safe to leave stopped |
| Dismiss last run | `POST /api/profile_exec/ack_last_run` | none needed, one-way but harmless |
| Clear Trip | `POST /api/safety/clear_trip` | none needed (clears a latch, does not need un-clearing) |
| PID popup Apply / zones-page PID save | `POST /api/zones/pid` | re-read prior gains via `control_get_zones` before saving, restore afterward |
| Save profile / New / Import / Delete / Restore removed / Hide / Favorite | `/api/profile*` | use a throwaway profile name for save/import; delete it afterward; do not touch builtin profiles' hide/restore state beyond a toggle-back |
| Live-profile fork/save/decide/reload | `/api/profile/live*` | only reachable while a firing runs; `discardBtn` is the built-in undo -- always end a live-edit test with discard unless intentionally committing a save |
| Zones page Save | `POST /api/zones` | read full zones config first (`control_get_zones`), restore verbatim after |
| Rate-guard save | `POST /api/safety/rate_guard/auto` | read `safety_get_rate_guard` first, restore |
| Autotune start/accept | `/api/autotune/*` | bench gains are not representative (`project_bench_identification_limits`) -- accept only into a throwaway zone/profile context, or skip accept and rely on abort |
| Ramp-assist toggle | `/api/ramp_assist` | read enabled state first, restore |
| Adaptive-tune revert | `/api/adaptive_tune/revert` | reverts to firmware-default gains -- only run this if the current adaptive gains are already known/logged, since revert is itself the "undo" for adaptive tuning, not something that itself needs undoing |
| Safety-commissioning stage & commit, relay-type, CT cal/auto-zero/trim | `/api/safety/commissioning*` | CT calibration is already closed per `project_ct_calibration_closed_and_presence_branch` -- read current commissioning fields first (`safety_get_commissioning`), restore verbatim; do not leave the bench preset applied if a real value was there |
| Watchdog PANIC toggle | `/api/watchdog_cfg` | must be restored to enabled before ending the session -- disabling it masks real crashes |
| Relay-cycle counters reset/restore | `/api/relay_cycles/*` | the route itself provides `restore`, use it |
| Dual-write window restore-verified | `/api/dualwrite_window/restore_verified` | `cfg` partition is unformatted/inert on this bench -- expect a true no-op, confirm via status GET |
| Timezone save | `POST /api/settings/tz` | read current tz first, restore |
| Display-power save | `POST /api/settings/display_power` | read first, restore |
| Unit preference toggle | `POST /api/unit_pref` | read first, restore |
| Kiln-configs save-as-new/overwrite/clone/rename/delete | `/api/kiln_configs/*` | use a throwaway slot name; delete it afterward; never overwrite/delete an existing named slot |
| Backup export / restore-from-file | `/api/backup/*` | export first as the restore point, only import a file produced by that same export (round-trip only, per `project_backup_round_trip_coverage` -- Wi-Fi password will not round-trip, expect that) |
| Wi-Fi mode switch, connect, forget, DHCP/static | `/wifi`,`/provision`,`/forget`,`/ip_config` | **only add/forget a throwaway test network or duplicate the bench's own known-good network entry; never forget the bench's primary network without confirming a second path to the board first** (LCD `network_manage` page, or physical access) |
| Security page password/PIN/policy save, clear credentials | `POST /api/auth/security` | read `GET /api/auth/config` first; per `project_web_auth_verified_and_blinds_pctools`, enabling auth policy blinds PcTools clients until they log in -- plan the credential source (`KILNCTL_WEB_PASSWORD`/`KILNCTL_WEB_USERNAME` User-scope env vars per `reference_bench_web_credentials_env_vars`) before flipping this |
| Login / session extend | `/api/auth/login`, `/api/auth/session/extend` | never iterate logins (`project_login_latency_measured_4s`, `project_owner_decisions_2026_09_21_login` lockout ladder); one deliberate attempt with the known-good credential only |
| cfgfs format-confirm | `/api/cfgfs/format_confirm` | inert on this bench (`cfg` partition unformatted) -- confirm via status read before/after, do not treat as free of consequence on a board where `cfg` is mounted |
| Setup-wizard step saves (steps 0-7, 9-11) | various, folded into the underlying config routes | same restore obligations as the underlying route they submit to (zones/safety/security above); steps 2/5 rebuild channel/relay count are **not** reversible without a full zones reconfigure -- treat as a one-way commissioning action done deliberately once, not a toggle to test twice |

Controls explicitly **excluded** from "reversible" and deferred to the
heating/unsafe bucket even though they look like ordinary saves: any route
tagged "Hardware-gated" in the matrix (current-sweep start, tuning
recommendations, autotune accept if it will actually be *used* rather than
tested-and-reverted, OTA update/rollback/recovery-exit, danger-mode
enter/relay control, E-stop verify, factory-reset "all"/wifi/kiln/profiles
scopes, boot_guard_reset). These are deferred per the task's own scoping,
not merely cautioned.

## Heating/unsafe -- deferred

Everything the matrix marks "**Hardware-gated**": zones current-sweep start
and the tuning-recommendations read that depends on it; diagnostics danger
mode (enter, per-relay, exit is fine as it's the safe path back but entering
it is deferred); E-stop verify (bench jumper is fitted, not asserted, per
`project_estop_jumper_is_fitted` -- verifying the physical switch needs the
jumper pulled); factory-reset scopes beyond a single deliberate,
pre-announced full reset if the M18 plan calls for one; OTA update/rollback
of either processor via the web route (the sanctioned flash path is
`flash_firmware()`/JTAG per CLAUDE.md, not this browser control, and
`project_pico_ota_erase_watchdog_resets_safety_processor` /
`project_esp_pico_ota_crc_variant_blocks_every_update` are both fixed and
confirmed flashed already, so this is a "when scheduled" item, not blocked
by an open defect); setup-wizard CT verification sweep start (abort/skip are
fine).

## Known dead controls

Per memory note `project_signed_route_unsigned_web_client`: three controls
on `settings_page.html` send unsigned `fetch()` requests to routes that
require the OTA challenge/HMAC handshake, and get a 400
`missing or malformed X-Ota-Mac header` from every board in every
configuration -- there is no config that makes them work today:

- Reboot button (`swResetBtn`, `settings_page.html` around line 301) ->
  `POST /api/sw_reset`
- Factory-reset danger buttons (`settings_page.html` around line 320) ->
  `POST /api/factory_reset`
- cfgfs format-confirm (`settings_page.html` around line 274) ->
  `POST /api/cfgfs/format_confirm`

Do not spend phase time re-discovering these as "new" failures. If the M18
plan needs the underlying route exercised, use `sw_reset_esp` (MCP tool,
which signs the request correctly) or raw HTTP with a correctly computed
`X-Ota-Mac`, not the button.

## Routes absent at origin/main (checked, none found)

Every route named in `COMMISSIONING_TEST_MATRIX.md`'s page tables was
grepped against `route_tier_table.h` at 51effca5: 157 `ROUTE_TIER(...)`
rows are present and no page-referenced route came back missing. The
matrix's own header count (155) is one commit behind current `origin/main`
by two rows; re-run `grep -c 'ROUTE_TIER(' firmware/KilnFW/App/drivers/http/route_tier_table.h`
before trusting either number, since this file grows with almost every
change in this area (`check_uri_handler_cap.ps1`'s own note: "essentially no
headroom left").

## Harness verdict: can it click controls against the live board?

**No -- the existing CDP harness renders only; it cannot drive the live
board or click anything.**

- `firmware/KilnFW/App/test/ui_responsive_sweep.mjs` (invoked by
  `firmware/KilnFW/App/test/check_ui_responsive_sweep.ps1:18`) drives real
  headless Chrome over raw CDP via a plain WebSocket
  (`ui_responsive_sweep.mjs:1-10`), but explicitly serves every page from a
  **throwaway 127.0.0.1 static file server rooted at
  `firmware/KilnFW/App/drivers/`** that this same Node process spawns and
  kills (`ui_responsive_sweep.mjs:12-34`), "NOT from the live board" (its
  own header comment, line 13). The header comment gives two explicit
  reasons this must never be the live board: a firing may be in progress
  and the board's HTTP server has wedged under load before, and the tool
  needs to work from source before a given change is ever flashed
  (`ui_responsive_sweep.mjs:26-30`). Every `fetch()` a page issues against
  this static server 404s immediately -- app.js's heartbeat poll included --
  and the pages' own `.catch()` handlers show the disconnected banner, which
  is the intended/expected state for this harness, not a bug
  (`ui_responsive_sweep.mjs:30-33`).
- What it actually asserts, per its own header (lines 1-6): no horizontal
  overflow, no overlapping/occluded interactive elements, no undersized
  touch target, no off-viewport interactive element -- pure static-DOM
  geometry at 320/360/390/768/1280/1920 px. `grep` for CDP input-injection
  domains (`Input.dispatchMouseEvent`, `Input.dispatchKeyEvent`, `click`
  used as a real DOM click) over the file found no such usage -- the script
  measures box models and element geometry, it does not simulate a user
  interacting with a control. It does not exercise `/api/*` responses,
  live-data-populated layouts (a long zone list, many fault rows), or any
  JS branch gated on a successful fetch (its own stated limitation, lines
  36-40).
- `lint_pages.js` (`firmware/KilnFW/App/test/lint_pages.js`, wrapped by
  `tools/check_lint_pages.ps1`) is a static lint over the page source, not a
  browser session at all -- no rendering, no clicking.
- The matrix's own claim that `mcp__kilnctrl`'s `ui` group
  (`board_page_structure`, `list_buttons`, `press_button`) covers
  "structural/functional page checks without a human driving a browser" is
  **imprecise and should be corrected going forward**: `list_buttons`/
  `press_button` (`tools/PcTools/src/kilnctrl/actions.py:1-16`) are a
  generic front door onto **PcTools' own desktop Tk GUI** (`gui.py`)
  buttons -- "one entry per GUI button" mirroring `gui.py`'s widget
  callbacks -- not the KilnFW web pages at all. `board_page_structure`
  (`tools/PcTools/src/kilnctrl/mcp_server_page_structure.py:1-9`) *does*
  target a KilnFW web page over real HTTP against the live board (its own
  docstring: "Fetch one board web page and report its STRUCTURE... /settings/zones...
  is roughly 240 KB"), but it is a read-only GET + structural summary (byte
  size wire vs. decompressed, gzip check, element-id presence up to
  `max_ids`) -- it confirms a page arrived intact and declares expected
  element ids, it does not click, submit a form, or exercise a route's
  side effect.

**Conclusion for driving this phase:** there is no existing harness that
clicks a web control against the live board and observes the resulting
route/side-effect. The phase must be driven by direct HTTP (or the relevant
MCP facade tool where one exists, per the matrix's per-row "MCP tool"
column) against the live board's routes, using `board_page_structure` only
to confirm a page's shell/elements are present before/after a change, and
`check_ui_responsive_sweep.ps1`/`lint_pages.js` only as pre-existing,
unrelated regression coverage for static layout/lint -- neither substitutes
for exercising a control's actual effect. Building a live-board clicking
harness (CDP against the board's own served HTML, or a Puppeteer/Playwright
equivalent with `Input.dispatchMouseEvent`) is out of scope for this
runbook and would be new tooling work, not a gap in existing tooling to
route around silently, per `feedback_prioritize_mcp_improvements`.

## Recommended order

1. Read-only pass across all 18 pages (no state changes) to confirm every
   page and status route responds and matches `board_page_structure`'s
   structural expectations.
2. Reversible-controls pass, one page at a time, restoring after each
   control per the table above -- do the Wi-Fi and security/auth controls
   last, since they can each temporarily change how the board is reached or
   authenticated.
3. Heating/unsafe controls stay deferred until the owner explicitly
   schedules that portion of M18 (current-sweep, OTA-via-web, danger mode,
   E-stop verify, factory reset "all").
4. Log every result directly in `COMMISSIONING_TEST_MATRIX.md`'s existing
   per-row `Result` column (`PASS`/`FAIL`/`BLOCKED`/`N-A`, dated, with an
   evidence pointer) rather than a separate log, so the matrix stays the
   single source of truth this phase updates.
