# Architecture decisions (KilnFW)

Settled design decisions moved out of `TODO.md` once implemented, so the
plan file only tracks work still to do. This is reference material — if a
decision changes, update it here rather than leaving the old reasoning in
the plan.

## Web server / transport

- Web UI runs on-device: `esp_http_server`, static pages `EMBED_TXTFILES`d
  from flash. A third client alongside the UART PC link and the MCP server,
  not a replacement for either.
- Storage: NVS for credentials/config (small, wear-levelled), no LittleFS —
  never needed once NVS was split into per-concern partitions (see
  `docs/PROJECT_STATUS.md` and TODO.md 8).
- Push (WebSocket/SSE) vs. 2s polling: **polling stays**. Revisit only if a
  real firing shows 2s is too coarse.

## Relay ownership / gating

- One chokepoint for relay-ON: `relay_authority_on_blocked()` (global) plus
  `relay_authority_zone_blocked()` (per-zone, layered on top). Every caller
  (UART bridge, web UI, profile executor, autotune) goes through it.
- Relay ownership tags: `NONE` / `MANUAL` / `PROFILE` / `RULE`, one owner
  per relay. Starting a profile claims `PROFILE`; a manual `SET_RELAY`
  against an owned relay is **refused**, not auto-paused. Pausing hands the
  relay back to `MANUAL`; resuming reclaims `PROFILE`. Chosen deliberately
  over auto-pause-on-touch — a Stop button must never have an unrelated
  side effect. Built in `App/drivers/relay_authority.{c,h}`.
- `AUTOTUNE` as a distinct ownership tag was never built — a manual
  `SET_RELAY` during a running autotune step-test is not refused by any
  ownership mechanism today (autotune's own writes still go through the
  zone-blocked safety gate, so a real *fault* still wins).

## Relay rule engine (v1 design)

- One relay has a list of rules; each rule is a flat AND-of-conditions; any
  rule true commands the relay ON (OR across rules, AND within one). No
  nested boolean trees.
- Condition types: zone temperature vs. threshold, elapsed time since
  profile start, another relay's *commanded* (not physical) state.
- A relay is either profile/manual-controlled or rule-driven (`RULE`
  owner), never both.
- Rule evaluation runs on the same 1Hz control tick as the profile
  executor. Config/persistence built (`rules_http.{c,h}`); the evaluator
  itself was never built — saved rules are inert.

## Historical data / graph buffer

- RAM-only ring buffer, discarded on reboot (no persisted per-firing log).
- One sample per 30s; sized for a 24h firing (2880 samples).
- Packed to 8 bytes/sample (single-zone) after a 2026-08-12 DRAM-exhaustion
  incident — see `docs/PROJECT_STATUS.md`.

## Page organization

- Dashboard (home), Profiles, Settings (Thermocouples & Zones incl. PID
  tuning, Relays & Rules, Network) — see `docs/UI_PLAN.md` for the open
  part of the page/route map.
- LCD: a shared top-bar module (`App/drivers/ui_topbar.{c,h}`) puts
  Back/Prev/Next as status-bar icons on every page and a Home icon on every
  page but home, replacing the old per-page in-content "Back" row.
  `ui_page_diagnostics.c` (Firmware / Internal RAM / PSRAM & storage,
  paged), `ui_page_thermo_faults.c` (per-channel MAX31856 fault bits,
  visibility only, no clear action), and the profile-creation flow
  (`ui_page_profile_builder_{zones,segment,review}.c`, reachable from
  `ui_page_config.c`'s hub) are all built. The nav hub itself now holds 11
  destinations across 2 pages (3 rows x 2 cells each, fixed-height
  internally-scrollable), up from the 7-destination/2-column layout of the
  2026-08-18 pass.
- **Profile creation departs from its original graph-view plan.** No
  `lv_chart` — the built flow is zone-pick -> per-segment ramp/hold-temp/
  hold-time editing via numeric-stepper cards (same pattern as other pages'
  +/- steppers) -> a review screen, then `profiles_save`. Simpler than the
  originally-sketched "draw the curve, drag a point" design and fits the
  ~264px no-scroll budget without a second sub-view.
- Web: `safety_page.html`, `diagnostics_page.html` (merges the LCD's
  diagnostics + board-health content since the web can scroll),
  `diagnostics/thermo` (`thermo_faults_page.html`), plus shared `nav.js`
  (header + bottom nav bar) and `app.js` (sticky Stop, connection-lost
  banner that dims `.kc-live-value` under `.kc-stale`, confirm-on-destructive
  including Start and Stop) are built and wired into all existing pages.
  `GET /api/status` gained fw version, build string, uptime, reset reason
  and heap figures for the new diagnostics page.
- Still open, not built: a `/settings` nav-hub page and a `/settings/manual`
  page (manual relay toggles moved off the dashboard) — `main_page.html`
  still carries its own inline Settings block, Danger zone, and manual relay
  toggles rather than the split described in `docs/UI_PLAN.md`. Backup/
  restore (settings + profile import/export) is also not started.
- `.kc-stale .kc-live-value` is wired to the connection-lost banner
  (`app.js`) but no page annotates its live numeric fields with that class
  yet, so the dim-on-stale behavior is inert until a page opts in.

## OTA-adjacent decisions

- ESP first when both processors need updating (USB-recoverable, and the
  Pico's own update path runs through the ESP) — unconditional rule, not
  "whichever is older."
- TLS for OTA and the web UI: planned (see `docs/UI_PLAN.md` §6), not
  authorized to build as of 2026-08-20. The existing HMAC challenge scheme
  proves knowledge of the AP password without transmitting it and is
  unaffected by TLS landing later.
- Pico image is staged in a dedicated `pico_img` partition (896K) then
  relayed over the isolated UART link — never held whole in ESP RAM on
  either side of the transfer.

## PSRAM

- Enabled 2026-08-17 once LVGL needed draw buffers for the ILI9488 (the
  named trigger from the original off-by-default decision). See
  `docs/PROJECT_STATUS.md`'s DRAM/PSRAM entries for the measured effects
  and the two internal-SRAM fires this caused/fixed along the way.
- GPIO 33-37 must stay unassigned (consumed by the R8 module's own PSRAM
  regardless of software config).

## LVGL / LCD rendering

- LVGL owns the ILI9488 outright; the old UART-remote-drawn `DISPLAY_CMD_*`
  path is dead code in firmware (never started from `main.c`). The PC-side
  MCP `display_*` tools that speak that old protocol are stale — decision
  on delete vs. restore vs. leave-as-is is still open (TODO.md 10.1).
- Shared backend rule (10.1a): a page's data access and actions must go
  through the same plain-C functions the HTTP handlers use — one backend,
  two front ends (JSON serialization, LVGL widget updates), never two
  independently-maintained readings of the same state.
- No-scroll rule: LCD pages must fit within ~264px content height (320px
  panel minus status bar/padding) with no page-level scrolling. A
  fixed-height widget scrolling internally (e.g. a list) is fine.
- Touch hit-testing: LVGL does first-match-in-z-order containment, not
  nearest-center. `ui_theme_apply_touch_area()` (dynamic per-widget
  extended click area) handles sizing; `ui_theme_register_touch_group()`
  (opt-in nearest-center arbitration within a registered cluster) handles
  the rare ambiguous-overlap case for dense layouts. See TODO.md 10.4.

## Command-queue / task-ownership pattern (TODO.md 10.14)

Full design and current phase status: `docs/ARCHITECTURE.md`. Summary of
the settled pattern: one owning FreeRTOS task per state-owning domain, a
small bounded queue, `<owner>_command_<verb>()` producer functions
(fire-and-forget or request/response), owner drains with a **bounded**
timeout so its own housekeeping isn't starved. Copied from
`firmware/SaftyFW/src/tasks/relay_owner.c`.

Ownership map: `kiln_io_owner` (relay/SX1509 writes), `thermo_owner`
(MAX31856 SPI access), `wifi_prov` (its own owner task, added last —
riskiest, touches the Wi-Fi driver's own event callbacks),
`profile_executor` (deliberately NOT converted — already correctly
mutex-guarded, converting a safety-critical state machine to a
drop-on-full-queue path was reviewed and rejected).
