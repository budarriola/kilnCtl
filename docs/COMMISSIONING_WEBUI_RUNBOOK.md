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
(`route_tier_table.h` has 155 route rows -- counted directly with
`grep -c '^\s*ROUTE_TIER("' firmware/KilnFW/App/drivers/http/route_tier_table.h`,
which excludes the file's own header-comment prose and its `#define`; a
naive `grep -c 'ROUTE_TIER('` overcounts by 2 by matching those). Pages:

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
matches across 18 pages before dedup), 155 total HTTP routes, of which
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
  added for the test, then re-add the bench network). Excludes anything that
  drives a relay, writes safety-config fields, or touches firing/heat state
  even indirectly -- those go to deferred regardless of how "reversible" the
  write itself looks, since the risk is in exercising the write on a live
  board, not in undoing it afterward.
- **Heating/unsafe (deferred)** -- drives a relay, writes safety-gated
  config, starts/resumes a firing, or performs an irreversible/destructive
  action not worth doing during this phase. These carry the matrix's own
  "Hardware-gated" tag (or are safety-config writes this runbook adds to
  that bucket) and stay `BLOCKED` on this bench per that document's legend --
  this phase does not change that.

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
Abort/cancel/stop-type controls that only halt something already gated off
on this bench are read-only in effect for this phase: `sweepAbortBtn`,
`atAbortBtn`, danger-mode `dangerExitBtn`, and `Stop firing`
(`POST /api/profile_exec/stop`) -- Stop is listed here specifically because
it only ends a run, it never starts relay-driving behavior.

`POST /api/auth/session/extend` is not a button at all -- it is an automatic
background poll from `app.js:1128`, exercised passively by having any page
open, not something to click.

Full row-level list: every row in `COMMISSIONING_TEST_MATRIX.md`'s tables
marked "Testable" whose Route is a GET, or whose Route note says
"client-side only", or whose action is Abort/Cancel/Stop as above.

## Reversible controls (restore step required, do in this phase)

These change persisted board state but have a defined way back, and do not
touch relay/firing/safety-config state (see the class definition above for
why those are excluded even when a restore step exists for them):

| Control | Route | Restore |
|---|---|---|
| Dismiss last run | `POST /api/profile_exec/ack_last_run` | none needed, one-way but harmless |
| Clear Trip | `POST /api/safety/clear_trip` | none needed (clears a latch, does not need un-clearing) |
| PID popup Apply / zones-page PID save | `POST /api/zones/pid` (`main_page.html`, `setup_wizard_page.html`) | re-read prior gains via `control_get_zones` before saving, restore afterward |
| Save profile / New / Import / Delete / Restore removed / Hide / Favorite | `/api/profile*` | use a throwaway profile name for save/import; delete it afterward; do not touch builtin profiles' hide/restore state beyond a toggle-back |
| Zones page Save (`saveBtn`, `zones_page.html:2795` -- posts to `POST /api/zones`, not `/api/zones/pid`; the PID-only endpoint on this page is a separate row above) | `POST /api/zones` | read full zones config first (`control_get_zones`), restore verbatim after |
| Ramp-assist toggle | `/api/ramp_assist` | read enabled state first, restore |
| Adaptive-tune revert | `/api/adaptive_tune/revert` | reverts to firmware-default gains -- only run this if the current adaptive gains are already known/logged, since revert is itself the "undo" for adaptive tuning, not something that itself needs undoing |
| Watchdog PANIC toggle | `/api/watchdog_cfg` | must be restored to enabled before ending the session -- **disabling it masks real overflow-class crashes for as long as it stays off**, so keep the disabled window as short as possible |
| Relay-cycle counters reset/restore | `/api/relay_cycles/*` | the route itself provides `restore`, use it (this resets a counter, not a relay; no relay is driven) |
| Dual-write window restore-verified | `/api/dualwrite_window/restore_verified` | `cfg` partition is unformatted/inert on this bench -- expect a true no-op, confirm via status GET |
| Timezone save | `POST /api/settings/tz` | read current tz first, restore |
| Display-power save | `POST /api/settings/display_power` | read first, restore |
| Unit preference toggle | `POST /api/unit_pref` | read first, restore |
| Kiln-configs save-as-new/overwrite/clone/rename/delete | `/api/kiln_configs/*` | use a throwaway slot name; delete it afterward; never overwrite/delete an existing named slot |
| Backup export (read-only download, listed here only to note it is also the restore point for the deferred import path below) | `GET /api/backup/export` | n/a, no write |
| Wi-Fi mode switch, connect, forget, DHCP/static | `/wifi`,`/provision`,`/forget`,`/ip_config` | **only add/forget a throwaway test network or duplicate the bench's own known-good network entry; never forget the bench's primary network without confirming a second path to the board first** (LCD `network_manage` page, or physical access) |
| Security page password/PIN/policy save, clear credentials | `POST /api/auth/security` | read `GET /api/auth/config` first; per `project_web_auth_verified_and_blinds_pctools`, enabling auth policy blinds PcTools clients until they log in -- plan the credential source (`KILNCTL_WEB_PASSWORD`/`KILNCTL_WEB_USERNAME` User-scope env vars per `reference_bench_web_credentials_env_vars`) before flipping this |
| Login | `POST /api/auth/login` | never iterate logins (`project_login_latency_measured_4s`, `project_owner_decisions_2026_09_21_login` lockout ladder); one deliberate attempt with the known-good credential only |
| Setup-wizard step saves that touch only the routes above (non-safety, non-zones-rebuild steps) | various | same restore obligations as the underlying route they submit to |

## Heating/unsafe -- deferred

Everything the matrix marks "**Hardware-gated**", plus every safety-config
and firing-state write this review moved here:

- **Start/Resume firing** (`POST /api/profile_exec/start`,
  `POST /api/profile_exec/resume`) and **Pause**
  (`POST /api/profile_exec/pause`) -- these start or continue relay-driving
  behavior even at the bench's 4 W load; only `Stop`/abort paths stay
  read-only (see above). Live-profile fork/save/decide/reload
  (`/api/profile/live*`) are gated the same way: that page is only
  reachable while a firing is actually running, so exercising it depends on
  first doing a deferred Start.
- **Autotune start and accept** (`POST /api/autotune/start`,
  `POST /api/autotune/accept`) -- start drives relays for a closed-loop step
  test, and accept persists gains derived from a bench load that is not
  representative of a real kiln (`project_bench_identification_limits`).
  Abort (`POST /api/autotune/abort`) stays read-only per the class above.
- **Rate-guard save** (`POST /api/safety/rate_guard/auto`) -- a safety-config
  write. Note: as of this snapshot **no page in `firmware/KilnFW/App/drivers/`
  actually posts to this route** (zero hits across every `.html`/`.js` file
  for `rate_guard/auto`) -- `safety_config_page.html`'s Save button is wired
  to something else or the route is currently reachable only via MCP/raw
  HTTP (`safety_set_rate_guard`), not a live web control. Treat this row as
  MCP/HTTP-only until a page control is confirmed; do not spend phase time
  hunting for a nonexistent button.
- **Safety-commissioning stage & commit, relay-type field, CT
  cal/auto-zero/trim** (`POST /api/safety/commissioning*`) -- all safety-
  config writes.
- **cfgfs format-confirm** (`POST /api/cfgfs/format_confirm`) -- erases the
  `cfg` filesystem; inert-but-still-destructive-in-intent on this bench.
- **Backup restore/import** (`POST /api/backup/import`) -- import is a
  destructive overwrite of board config; only the export/read side stays in
  the reversible bucket above.
- Setup-wizard steps 2/5 (channel/relay count rebuild), the CT verification
  sweep start (abort/skip stay read-only), and any wizard step that submits
  to `/api/safety/commissioning*` or `/api/zones/current_sweep/start`.
- Zones current-sweep start, the tuning-recommendations read that depends on
  it, diagnostics danger mode (enter, per-relay -- exit stays read-only),
  E-stop verify (bench jumper is fitted, not asserted, per
  `project_estop_jumper_is_fitted` -- verifying the physical switch needs
  the jumper pulled), factory-reset scopes beyond a single deliberate,
  pre-announced full reset if the M18 plan calls for one, and OTA
  update/rollback of either processor via the web route (the sanctioned
  flash path is `flash_firmware()`/JTAG per CLAUDE.md, not this browser
  control; `project_pico_ota_erase_watchdog_resets_safety_processor` and
  `project_esp_pico_ota_crc_variant_blocks_every_update` are both fixed and
  confirmed flashed already, so this is a "when scheduled" item, not blocked
  by an open defect).

## Formerly-dead controls -- now fixed, historical note only

An earlier draft of this runbook flagged three `settings_page.html` buttons
as permanently dead (unsigned `fetch()` against routes requiring the OTA
challenge/HMAC handshake, always 400). **That is stale as of this
snapshot** -- `settings_page.html`'s own header comment (lines 151-163)
records the fix: all three now sign through the shared
`window.kcOtaAuthedFetch` helper (`app.js:329`), the same handshake
`ota_page.html` already used. Current wiring:

- Reboot (`swResetBtn`, button at `settings_page.html:206`, handler at
  `settings_page.html:329`) -> `POST /api/sw_reset`, context `sw-reset`
- Factory-reset danger buttons (`settings_page.html:370`) ->
  `POST /api/factory_reset`, context `factory-reset`
- cfgfs format-confirm (`settings_page.html:314`) ->
  `POST /api/cfgfs/format_confirm`, context `factory-reset` (reuses that
  context deliberately, per that handler's own comment)

No action needed for this phase beyond normal testing of these controls
under their (now correct) deferred/heating classification above -- Reboot
and format-confirm are deferred per the buckets above; the danger buttons
are the factory-reset family, also deferred.

## Routes absent at origin/main (checked, none found)

Every route named in `COMMISSIONING_TEST_MATRIX.md`'s page tables was
grepped against `route_tier_table.h` at 51effca5: all are present as
`ROUTE_TIER(...)` rows and no page-referenced route came back missing.

## Harness verdict: can it click controls against the live board?

**No -- the existing CDP harness renders only; it cannot drive the live
board or click anything.**

`firmware/KilnFW/App/test/ui_responsive_sweep.mjs` (invoked by
`check_ui_responsive_sweep.ps1:18`) drives real headless Chrome over raw CDP,
but serves every page from a throwaway static file server this same process
spawns and kills, explicitly "NOT from the live board" (`ui_responsive_sweep.mjs:14`)
-- a firing may be in progress and the board's httpd has wedged under load
before. It asserts pure static-DOM geometry (overflow/overlap/touch-target/
off-viewport) at six widths; no click-simulation or input-injection exists
in the file. It cannot exercise `/api/*` responses, live-data-populated
layouts, or any JS branch gated on a successful fetch -- its own stated
limitation (`ui_responsive_sweep.mjs:36-44`). `lint_pages.js` is a static
source lint, no browser session at all.

The test matrix's claim that `mcp__kilnctrl`'s `ui` group covers this needs
correcting: `list_buttons`/`press_button`
(`tools/PcTools/src/kilnctrl/actions.py:1-16`) drive PcTools' own desktop Tk
GUI (`gui.py`), not the KilnFW web pages. `board_page_structure`
(`tools/PcTools/src/kilnctrl/mcp_server_page_structure.py`) does hit a live
web page over real HTTP, but is a read-only structural GET (byte size,
gzip, element-id presence) -- it never clicks or submits.

**Conclusion:** no existing harness clicks a web control against the live
board. This phase must be driven by direct HTTP (or the matrix's per-row MCP
facade tool), using `board_page_structure` only to confirm a page's shape
before/after a change. Building a live-board clicking harness is out of
scope here -- new tooling work, not a gap to route around silently, per
`feedback_prioritize_mcp_improvements`.

## Recommended order

1. Read-only pass across all 18 pages (no state changes) to confirm every
   page and status route responds and matches `board_page_structure`'s
   structural expectations.
2. Reversible-controls pass, one page at a time, restoring after each
   control per the table above -- do the Wi-Fi and security/auth controls
   last, since they can each temporarily change how the board is reached or
   authenticated.
3. Heating/unsafe controls stay deferred until the owner explicitly
   schedules that portion of M18 (firing start/pause/resume, autotune
   start/accept, safety-config writes, current-sweep, OTA-via-web, danger
   mode, E-stop verify, factory reset, backup import, cfgfs format).
4. Log every result directly in `COMMISSIONING_TEST_MATRIX.md`'s existing
   per-row `Result` column (`PASS`/`FAIL`/`BLOCKED`/`N-A`, dated, with an
   evidence pointer) rather than a separate log, so the matrix stays the
   single source of truth this phase updates.
