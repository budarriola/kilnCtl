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

## PID / thermal guard / autotune

- **Guard 8 (cross-zone plausibility)** compares a zone's reading against
  every other zone's worst-disagreeing peer (not an average — an average
  lets one badly-wrong channel hide behind a healthy one), trips after a
  sustained window (600 s default), and blocks only its own zone. Ships with
  `cross_zone_max_delta_c` defaulting to 0 = **disabled** — the threshold
  needs a measured cross-gain matrix from a real coupled kiln, not a firmware
  constant, and no such matrix has ever been captured on hardware. Armable
  per-zone from Settings without a rebuild; `cross_zone_period_s` stays a
  firmware constant on purpose (arming needs one knob, not two).
- **Relative Gain Array** (`pid_autotune_rga()`) computes RGA over the
  largest principal sub-block whose every cell is measured, refusing an
  incomplete, singular, or non-finite matrix outright — a wrong RGA would
  tell an operator their zones are independent when nobody measured that.
  Cross-gain cells are filled by autotune's step test recording every
  configured zone's reading at each tick, not just the zone under test.
- **Feedforward** (`u_ff = (T_sp - T_amb)/K_dc + (dT_sp/dt)*tau/K_dc`) takes
  its rate from the segment's commanded ramp, not a tick-to-tick difference
  (timing noise at 1 Hz, amplified by tau). Ambient is the MAX31856 cold
  junction sampled once at firing start, not re-sampled, since it warms with
  the board over a long firing. Stays exactly 0 without an identified model
  or on a non-finite `K_dc`/`tau`. `pid_seed_bumpless()` subtracts the ff
  term internally (moved from the caller) so the bumpless-transfer seed
  stays exact with feedforward live.
- **Relay-feedback autotune** reuses the step-test engine's lock, tick task,
  trace buffer, and relay-authority claim rather than a parallel state
  machine; accepting a relay-test result writes gains but never a plant
  model, since a relay test measures one frequency-response point and a
  model from a prior step test must survive it.
- **Electrical load staggering** is phase-offset only:
  `heater_output_seed_phase()` truncates a zone's first time-proportioned
  window (permanent shift, not a one-time transient), assigned by each
  zone's rank among a run's active zones. The `max_simultaneous_relays` cap
  (the harder half) was deliberately not built — capping correctly means
  either changing what the pure `heater_output` module computes or
  overriding its output in a way that desyncs its internal bookkeeping from
  real hardware state, and neither was worked out with confidence.
- **Config reload while running** (`zones_config_generation()`, bumped only
  when in-RAM config actually changes) is checked once per control tick,
  after readings are stored and before control math, so an edit can never be
  half-applied across the decide/apply split. Tuning gains land bumplessly;
  mode/relay-mask changes force relays off under the old mask first; guard
  threshold edits apply immediately and are logged at WARN — chosen over
  requiring idle, since an operator correcting a ceiling for the ware
  currently in the kiln needs it to take effect now. A reload never touches
  latched trip state — editing a threshold must not become an undocumented
  way to clear a trip.
- **Unowned-relay sweep**: every tick, `profile_executor.c` forces off the
  intersection of (what the expander shows physically closed) ∩ (what this
  run ever commanded) ∩ (complement of what may legitimately hold it —
  active zones regardless of fault state, plus any zone under autotune).
  The "commanded by this run" term is what stops the sweep from chattering
  off a relay an operator is holding manually.
- **Relay contact-cycle accounting** persists lifetime on/off transition
  totals per relay in its own NVS key (`relay_cyc`), deliberately separate
  from the `zones_cfg` blob (whose loader treats any size change as
  corruption and would wipe every user's zone setup on an unrelated growth).
  Writes at most once per 10 minutes plus a flush on halt; counts saturate
  rather than wrap.
- **Reboot breadcrumb** (`run_state.{c,h}`) persists a fixed-size record
  (profile, zone mask, segment, target, elapsed, phase, fault guard/reason)
  to its own NVS key on every meaningful transition plus a 300 s refresh —
  not per tick (~200 writes over a 12h firing vs. ~43,000). A clean stop is
  recorded distinctly from an unplanned loss of power; nothing currently
  *acts* on a recovered record (no auto-resume), it is display/ack only.

## Historical data / graph buffer

- RAM-only ring buffer, discarded on reboot (no persisted per-firing log).
- One sample per 30s; sized for a 24h firing (2880 samples).
- Packed to 8 bytes/sample (single-zone) after a DRAM-exhaustion incident —
  see `docs/BRINGUP_HAZARDS.md`.

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
- **LCD profile browse/select/start is built**, reachable from the
  Configuration hub's "Profiles" cell: `ui_page_profiles.c` plus
  `ui_page_profiles_mine.c`/`_family.c`/`_builtin_list.c`,
  `ui_page_profile_detail.c`, and `ui_page_profile_segments.c`. This was
  blocked on flash headroom (`factory` partition down to 4% free); that
  blocker is gone now that the partition table was reworked (`factory`
  moved to `0x810000`, capacity `0x300000`, ~51% free on the last build —
  see TODO.md 9.1). Hardware-verified 2026-08-21 via injected touch:
  home -> config -> a Start confirm dialog raised and cancelled with relays
  confirmed off. The home page's Start button keeps its own fallback chain
  (current non-idle profile, else last boot record) independent of the new
  browse page.
- **Profile creation departs from its original graph-view plan.** No
  `lv_chart` — the built flow is zone-pick -> per-segment ramp/hold-temp/
  hold-time editing via numeric-stepper cards (same pattern as other pages'
  +/- steppers) -> a review screen, then `profiles_save`. Simpler than the
  originally-sketched "draw the curve, drag a point" design and fits the
  ~264px no-scroll budget without a second sub-view.
- Web: `safety_page.html`, `diagnostics_page.html` (merges the LCD's
  diagnostics + board-health content since the web can scroll),
  `diagnostics/thermo` (`thermo_faults_page.html`), `settings/manual`
  (`manual_page.html`, the per-relay toggles moved off the dashboard), plus
  shared `nav.js` and `app.js` (sticky Stop, connection-lost banner that
  dims `.kc-live-value` under `.kc-stale`, confirm-on-destructive including
  Start and Stop) are built and wired into all existing pages. `GET
  /api/status` gained fw version, build string, uptime, reset reason and
  heap figures for the new diagnostics page. `.kc-live-value` is annotated
  on every page's live numeric fields, so the stale-dim behavior is live
  everywhere, not just wired.
- **Global chrome rework (2026-08-21):** the bottom nav bar is gone;
  `nav.js`'s Menu button opens a top-down scrollable drop-down holding all
  10 board-configuration destinations plus a "Reset" item
  (`/settings#danger`); a Home button sits next to Menu on every page except
  the dashboard. The "kilnCtl" brand text and each page's duplicate `<h1>`
  are replaced by the page name in the top bar. `settings_page.html` now
  holds only the danger zone. `main_page.html` (dashboard) is reordered
  execCard -> history canvas -> picker row (profile select, Start) ->
  channels, with cold-junction temps, both section `<h2>`s, and the
  history/settings prose removed. Each thermocouple's relay status is shown
  by joining `/api/zones`' `thermo_mask`/`relay_mask` against `/api/status`'
  relay bits client-side; a channel with no mapping shows no relay state
  (never a fabricated one) — this was the one open design question and it
  resolved to "no state over guessed state."
- Unit preference (°C/°F) is shared and device-backed (`unit_pref.c/.h`),
  not an independent per-surface toggle: `GET /api/status`'s `temp_unit`,
  `POST /api/unit_pref`, UART `CONTROL_CMD_GET/SET_UNIT_PREF`, and the LCD's
  Configuration-hub toggle all read/write the same setting.

## LCD thermocouple-type page

- Third config-hub page (`ui_page_tc_types.c`), added 2026-08-21 because both
  existing config hub pages were genuinely full. One row per channel plus the
  safety processor, naming the type rather than the raw nibble, cycling
  B/E/J/K/N/R/S/T on tap. `zones_http` gained per-channel
  `get/set_tc_type`/`set_safety_tc_type`, shaped after `get/set_pid`.
- Deliberately does **not** push to the MAX31856 register on write: the
  existing web `POST /api/zones` doesn't either (only the boot-time apply
  does), so LCD and web behave identically rather than the LCD acquiring a
  side effect the web lacks.

## Config hub page 1 — the tap-target dump undercounts by design

Page 1 of the Configuration hub genuinely holds 6 items; the first two are
non-clickable "not built yet" placeholders. `kiln_ui.c`'s tap-target dump
only lists widgets carrying `LV_OBJ_FLAG_CLICKABLE`, so those two
placeholders never appear in a dump even though they render. Reading a
tap-target dump as a visual layout scan will therefore misreport an empty
top row that isn't actually empty — check the rendered page, not just the
dump, before concluding a layout is broken.

## Profile builder: Celsius-only editable fields

Profile pages honour the °C/°F display preference (segments list, builder
review) — but the profile builder's three *editable* callbacks
(`ui_page_profile_builder_segment.c`) deliberately stay Celsius. Converting
them needs the min/max bounds, the num pad's seed value, and the stored
result all converted, and `ui_num_pad_params_t` has no partial-conversion
mode. Captions read "Target C" so what's being typed is never ambiguous —
this is a visible, intentional seam, not a wrong setpoint, and must not be
"fixed" by an agent unaware of the constraint. Commented at the exact spots
in the source.

## Idle chart / pinned dots

The dashboard chart (web `main_page.html` and LCD `ui_page_home.c`) is always
shown above the profile-picker/Start row, rather than appearing only once a
profile is running. While idle, current per-channel temperatures render as
dots pinned to the left edge, updated in place (no line, no trend, no
rightward march); a profile start hands over to the existing trend rendering
unchanged. Deliberately no backend change — the history ring's sampling
gate, reset-at-start, and `elapsed_s` semantics are untouched; the idle dots
come from live readings both surfaces already poll (`/api/status` on the
web, the same zone reading `build_zone_row()` uses on the LCD). This mattered
because a second ring buffer would cost ~23KB against ~4167 bytes of
internal DRAM free after LVGL start.

LCD home chart is deliberately smaller/simpler than the history detail
page's (70px vs 110px, 30 points vs 60, actual-only, no legend) — a
content-budget trade, not an oversight; the desired-vs-actual comparison
stays on the history page. Its 120-byte backing array is kept separate from
`ui_page_history.c`'s arrays deliberately, since those are that chart's live
backing store and must not be aliased across two pages.

## Backup / restore

`backup_http.c` + `backup_page.html` (shipped 2026-08-21) export/import a
JSON blob that is **deliberately narrower** than the zones page: only PID
gains, FOPDT model, per-channel tc_type, and safety tc_type round-trip,
because those are the only four things `zones_http.h` exposes a getter *and*
setter for. Zone name, relay/thermocouple wiring, guard thresholds, temp
limits, and heater timing have getters only — exporting them would produce a
file that silently fails to restore, so they're excluded and the page states
this. Wi-Fi credentials are excluded in both directions: restoring them onto
a board on a different network either fails outright or silently joins
whatever shares the SSID.

Export streams JSON through a 256-byte chunk buffer, never a whole-document
heap buffer. Import validates every entry in a first pass and commits only
in a second, so a malformed file writes nothing; an unknown version is
refused outright rather than migrated on a guess. `ZONE_MODEL_K_MAX`/
`ZONE_MODEL_TIME_MAX_S` were moved (not mirrored) from `zones_http.c` into
`zones_http.h` so the validation pass and `zones_config_set_model()`'s
commit-time check cannot drift — closes a real half-applied-restore hole
where an out-of-range plant model was only caught after earlier entries had
already reached NVS.

Import gates on `ota_http_check_interlocks()` (safety link must be up), not
`heat_interlock` (the latter answers "may heat run during an update", the
opposite question) — this is a deliberate reuse of the OTA gate, not a bug,
but it means restore is refused whenever the safety link is down. Each
exported zone object carries a `"_":0` sentinel (`backup_http.c:214`) used
purely as a trailing-comma guard in the streaming writer — junk in an
otherwise user-facing format that consumers must ignore; recorded here so
nobody "fixes" it as a stray field without understanding why it's there.

## Static-IP AP-fallback fix

A wrong-but-parseable static IP (bad gateway/subnet) still associates at L2,
so `esp_netif` raises `GOT_IP` with no real DHCP exchange having happened,
and `do_ev_got_ip()` used to tear down the fallback AP on that signal alone —
reporting `CONNECTED` while actually unreachable. Fixed 2026-08-21: a static
join now stays in APSTA until an HTTP request is proven to have arrived at
the static address (`getsockname()` on the request-handling thread, since
the socket only lives for that request; the resulting state change is
posted to `owner_task` to preserve the single-writer invariant). DHCP join
behavior is unchanged. Worst case is now "AP stays up longer than needed",
never "both AP and station are down with no recovery."

## Zones page advisory warnings

`zones_page.html` warns (does not block) on a zone with no thermocouples
assigned and on a channel feeding two zones. Neither condition is blocked
because the backend contract already allows both: `zones_http.c` documents
accepting `thermo_mask=0`, and `thermo_combine()` has no exclusivity concept.
Refusing either in the UI would contradict what the backend already permits,
so advisory-only is the correct behavior, not a shortcut.

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
  `docs/BRINGUP_HAZARDS.md` for the measured effects and the two
  internal-SRAM fires this caused/fixed along the way.
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
