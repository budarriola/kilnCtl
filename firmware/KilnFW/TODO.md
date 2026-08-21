# TODO — Web UI / Kiln Control Front End

Top-level ordering across both processors lives in [`../../ROADMAP.md`](../../ROADMAP.md);
this file owns the main-firmware detail. The cross-processor items — the swapped
safety-UART pins, `CommonFW`, and the safety-liveness gate on heating — are
sequenced there and tracked in [`../SaftyFW/TODO.md`](../SaftyFW/TODO.md).

This is a planning doc: it tracks work not yet done, plus enough context on
finished, adjacent work to act on what's left. A finished section is
collapsed to a short status line pointing at the doc that owns its detail —
`docs/PROJECT_STATUS.md` (dated engineering log), `docs/ARCHITECTURE.md` /
`docs/ARCHITECTURE_DECISIONS.md` (settled design), `docs/BRINGUP_HAZARDS.md`
(recurring bugs and their root causes), `docs/PID_CONTROL.md` +
`docs/GUARD_TEST_MATRIX.md` (control loop and thermal-safety detail), and the
per-feature docs (`WEB_UI.md`, `WIFI_PROVISIONING.md`, `PROFILES.md`,
`SAFETY_MODEL.md`, `UI_PLAN.md`, `UI_THEME.md`). **Most of the "still open"
items below are open specifically because no ESP32-S3/RP2040/thermocouple/
relay hardware combination exists in this environment to verify them
against** — treat "logic-verified"/"build-clean" language literally, not as
a synonym for done. Cross-references `docs/SAFETY_MODEL.md` throughout,
since the profile-execution engine here is a **new actor that commands
relays** and must go through the same safety-wins gate as the existing
PC/MCP link — not a parallel path that bypasses it.

## 0. Architecture decisions

**All settled and built.** Full writeup moved to
[`docs/ARCHITECTURE_DECISIONS.md`](docs/ARCHITECTURE_DECISIONS.md): web
server on-device, relay ownership tags, the rule-engine composition rules
(engine itself still unbuilt — evaluator has no caller), the history ring
buffer design, and storage (NVS, no LittleFS).

## 0.5 Page organization

Grouping: Dashboard (section 2, home/live status), Profiles (section 5),
Settings split into Thermocouples & Zones (section 3, incl. PID tuning),
Relays & Rules, Network (section 4). Safety-relevant controls stay reachable
in the fewest taps (phone-screen constraint). Built as described.

**Four more pages added to plan 2026-08-18** (web + LCD both):

- **Safety / Alarm page — still OPEN, blocked on M5.** Trip state + history +
  explicit "clear trip" button needs `LINK_PROTOCOL.md`'s `TRIP_EVENT` frame,
  which SaftyFW doesn't send yet. The send-command plumbing (`clear_trip`,
  `SET_CONFIG`) is already built end-to-end (`safety_link.c`, `dashboard_http.c`,
  `uart_bridge.c`, MCP `safety_set_tc_type`) — see `firmware/SaftyFW/TODO.md`'s
  Phase 9 for the receiving end. `ui_page_safety.c` today only shows the
  single latest-trip row (no scrollable history, no Clear button) — that's
  the LCD gap still open.
- **Diagnostics / System info page — DONE for the ESP-only half; SAFETY-LINK
  STATS HALF STILL BLOCKED ON M5.** `ui_page_diagnostics.c`/`.h` (fw version,
  uptime, heap/PSRAM, die temp) built 2026-08-20. A separate
  `ui_page_thermo_faults.c`/`.h` page (per-channel MAX31856 fault bits,
  visibility only, no clear action) was added the same day per explicit
  request to split diagnostics into multiple pages.
- ~~**Manual zone control page.**~~ **Declined by explicit user request
  (2026-08-20)** — no manual setpoint override bypassing a running profile.
  `ui_page_temperature.c` (per-zone reading + manual relay toggles) stays.
- **Backup / restore page — OPEN, not built.** Export/import saved
  profiles + zone/relay/network config as one blob. No blockers.

**Web page structure rework — planned in `docs/UI_PLAN.md`, NOT built
(2026-08-20).** Settings gets its own hub page (`GET /settings`), the
dashboard becomes scrollable and LCD-like, and Safety/Diagnostics/
Thermocouple-faults gain web equivalents (two web pages, not three, since
the web can scroll). Follow-up decisions recorded there: server-side
session-token auth on writes, manual relay toggles move to `/settings/manual`,
polling stays (no push), and **TLS for this UI and OTA (section 9.3) is
planned but not authorized to build**. See `docs/UI_PLAN.md` for the full
route map/detail; keep it in sync with the LCD side per section 10.5.

## 1. Wi-Fi provisioning and resilience — DONE, hardware-verified

Implemented in `App/drivers/wifi_prov.{c,h}` + `App/drivers/wifi_provision_http.{c,h}`
+ `wifi_provision_page.html`. AP fallback, single home/AP mode toggle (redesigned
2026-08-11 from the original three-flag model), editable AP identity, saved-network
list with forget (section 8.4), DHCP/static IP toggle (web only, 2026-08-20), and the
"losing Wi-Fi never stops the control loop" hard requirement are all built and
hardware-verified — AP-join, station-join, captive portal (real associated client,
DNS hijack + 302 to `/`), and a 35-minute soak with zero failures. Full detail and
every dated fix: `docs/WIFI_PROVISIONING.md` and `docs/PROJECT_STATUS.md`. Several
real bugs were found and fixed during hardware testing this pass (Wi-Fi
mode-switch ordering, `has_creds` disagreement, internal-SRAM task-registration
race, coredump partition sizing, UART log congestion) — condensed writeups in
`docs/BRINGUP_HAZARDS.md` rather than kept here.

Open items, not yet fixed:

- [ ] **Known gap**: a wrong-but-parseable static IP (bad gateway/subnet) reaches
      `CONNECTED` at the L2 layer, so `ap_fallback_timer`'s DHCP-timeout recovery
      does not self-heal a bad static config — not a full lockout (switch back to
      DHCP/AP via the same API, or power-cycle) but a real reachability edge case.
      See `docs/UI_PLAN.md`'s web work-queue item 6.
- [ ] **Not exercised against a real router**: the DHCP/static toggle is
      build-verified and flashed but has no live network in this environment to
      confirm actual join behavior against.
- [ ] **Wi-Fi driver log lines arrive with an empty body** (`W (...) wifi:` with
      nothing after the colon) roughly every 30s, so the ESP-IDF Wi-Fi driver's own
      diagnostics are invisible through `uart_log_bridge`. Low severity, not chased.

## 2. Web UI — Main / Dashboard page

DONE and hardware-verified — `App/drivers/dashboard_http.{c,h}`, `main_page.html`.
Live per-channel thermocouple/relay status, the desired-vs-actual graph
(`GET /api/history.csv`, section 6A.9), Start/Stop/Pause, and a saved-profile
picker are all built; all five pages (dashboard + 4 sub-pages) verified live to
be browsable with every card marked hardware-absent when the daughterboard/
expander are missing, rather than crashing or lying. `docs/WEB_UI.md` has the
full page/API inventory.

- [ ] **Still polled (2s), not pushed.** `CONFIG_HTTPD_WS_SUPPORT` is off; not
      yet justified. Revisit (WebSocket or SSE) if 2s polling proves too coarse
      once real hardware is attached and watched during a firing.

## 3. Web UI — Settings page

DONE — `/settings/zones` (`App/drivers/zones_http.{c,h}` + `zones_page.html`) and
`/settings/relays` (`App/drivers/rules_http.{c,h}` + `rules_page.html`).
Thermocouple calibration (offset, applied in firmware to every consumer including
UART/MCP as of 2026-08-13), thermo/relay count config, named zones, PID tuning
per zone (same page, per explicit request), and persistence are all built and
hardware-verified as reachable/functional with no thermocouple/relay hardware
attached. The relay alternate-function rule *editor* (DSL, `RELAY n DRIVEN`/`R
<rule> TEMP|TIME|RELAY`) is built and persists; the rule *evaluator* that would
make saved rules actually drive relays does not exist — `rules_http.h`'s own
comment and the page both say saved rules are inert (see section 0's rule-engine
design in `docs/ARCHITECTURE_DECISIONS.md`).

Max-ramp-rate ceiling is a **user-entered** value (`max_ramp_c_per_hr`), not
estimated from PID tuning/observed performance — the estimated alternative was
deliberately left undesigned as "more work, needs a design of its own."

## 4. Web UI — Network settings page — DONE, folded into section 1

Same page as section 1's provisioning page (`wifi_provision_page.html`) rather
than a separate route — scan/select SSID, mode toggle (superseded the original
local-only checkbox 2026-08-11), and live connection-state display are all built.
See section 1 and `docs/WIFI_PROVISIONING.md`.

## 5A. Shipped firing schedules (Digital Fire catalogue) — DONE 2026-08-20

The 28 published schedules at https://digitalfire.com/schedule ship in flash as
read-only profiles (ids `PROFILE_BUILTIN_ID_BASE`+index, separate from the 8 user
slots), hide-not-delete via an NVS mask, generated from a scraper
(`tools/scripts/gen_builtin_profiles.py`/`scrape_digitalfire_schedules.py`) rather
than hand-transcribed, and paged over the size-limited UART LIST command. Credited
on `profiles_page.html` with a source link per entry.

### 5A.1 Feasibility marking — red when the tuning says it cannot be fired

`profile_feasibility.c` judges each segment against the autotune-fitted first-order
model (`dT/dt = (K*u - (T - T_amb)) / tau`): max heating/cooling rate, an
`UNREACHABLE` verdict for a target above `T_amb + K`, and the existing
`max_ramp_c_per_hr` ceiling on top. With no tuned model the verdict is `UNKNOWN`
(never a false OK or false red). Verified on hardware that the badge agrees with
what `profiles_start()` actually refuses. DONE and hardware-verified 2026-08-20.

- [ ] **One global bridge stall in a 25-minute soak (2026-08-20), cause not
      found.** 78 polling ticks, 77 clean; at t=1031s EVERY bridge surface
      (control/profiles/autotune/wifi/touch) failed in the same tick — "ACKed but
      no reply within 3.0s" — then the next tick was clean. The ACK proves the
      link/owner task were alive, so whatever blocked was downstream of frame
      delivery and hit all five task inboxes at once — the shared flash-safe
      executor only serialises control/profiles/autotune, so it cannot alone
      explain wifi/touch stalling too. Suspect a long flash/NVS op or the
      log-bridge queue backing up. Not reproduced since. Worth a targeted soak
      logging per-surface latency percentiles rather than pass/fail.

### 5A.2 Still open

- [ ] **No LCD access to profiles at all** — pre-existing, more visible now with
      28 shipped schedules. No `ui_page_profiles.c`; the home page's Start uses a
      fallback chain (current non-idle profile, else last boot record). An
      operator at the kiln cannot choose a schedule on the panel. Costs flash
      (see below) and a page design that survives the no-scroll rule.
- [ ] **Known edge in the UNREACHABLE test (minor).** The steady-state ceiling
      test applies to *cooling* segments too, though a cooling target is reachable
      by construction. Only bites within 5°C of the ceiling on a descending
      segment, which no real schedule does. Left alone deliberately (already
      conservative in the safe direction); fix if it ever matters is to gate on
      `target > start_c`.
- [ ] **Flash headroom is the binding constraint: 4% free (~59 KB) in the
      `factory` app partition** — the smallest app partition on the board
      (1500K; `ota_0`/`ota_1` are 2048K each). ~8.1 MB of the 16 MB chip
      (`0x7F0000..0x1000000`) is still unallocated. Anything sizeable (an LCD
      profiles page, TLS, a second language) needs the partition table
      revisited first — relocate `factory` into the spare region and grow the
      app slots, not shrink anything currently stored. Two hard constraints on
      any repartition: app partitions must stay 64K-aligned, and the first three
      partitions (`nvs`/`phy_init`/`factory`) must stay byte-identical to
      ESP-IDF's stock `partitions_singleapp_large.csv` — live NVS data already
      sits at `0x9000` on the physical board.

## 5. Web UI — Fire profile creation page

DONE — `App/drivers/profiles_http.{c,h}` + `profiles_page.html`. Profile editor
(name, up to 12 segments of target/ramp-rate/dwell, `zone_mask`), save/load/delete
(8 NVS slots, versioned per section 8.2, same handlers the UART CONTROL bridge
uses), sanity-bound validation, and the feasibility check against each zone's
`max_ramp_c_per_hr` (hard reject over ceiling, warn within 20% margin) are all
built. The feasibility check re-running at profile-*start* time (not just save
time) is closed by section 6A.7's mid-firing ceiling re-check.

## 6. Firmware-side profile execution engine (implied by sections 2 and 5)

DONE — `App/drivers/profile_executor.c`, 1Hz tick. Segment stepping/ramp
interpolation, per-zone `OFF`/`BANGBANG`/`PID` control mode (live-reloadable
since 2026-08-12), the direction/rate sanity monitor (thermal_guard.c guards 1/2,
full detail in section 6A.3), relay-authority gating, fail-toward-off on pause,
and preemption by every existing safety mechanism are all built. Guards 1/2/4/7
are written but not yet exercised live (no thermocouple hardware to provoke
them) — tracked as a hardware-verification gap in section 6A.3/6A.8, not
reopened here.

## 6A. PID control, thermal protection, autotune, and zone interaction

**Status: built and hardware-verified where a thermocouple/kiln exists to verify
against; a large slice remains logic-verified only for lack of hardware.** Full
design and current status: `docs/PID_CONTROL.md` (control loop, guard table,
autotune flow, host-test summary) and `docs/GUARD_TEST_MATRIX.md` (one row per
guard: provocation, threshold, expected trip time, what the operator sees).
This section now only tracks what remains open.

Built: `pid.c` (positional PID, derivative-on-measurement, low-pass D filter,
anti-windup, functional-range blending, bumpless transfer at every
discontinuity, feedforward from the identified plant model, 2-DOF setpoint
weighting); `thermal_guard.c` (all 9 Klipper/Marlin-class guards, see
`docs/GUARD_TEST_MATRIX.md`); `heater_output.c` (time-proportioned duty,
min-on/off, contact-cycle accounting, phase-offset load staggering,
`max_simultaneous_relays` cap with deferred-not-dropped credit); `pid_autotune.c`
+ `autotune_engine.c` (step-test FOPDT fit + SIMC/ZN/Tyreus-Luyben tuning,
relay-feedback identification, cross-zone coupling-matrix capture, RGA display);
concurrent multi-zone execution with ramp-lock; the per-zone relay-authority
gate (6A.6); mid-firing config reload with bumpless transfer per field; a
host-side kiln plant simulator (`App/test/sim_plant.c`, multi-zone, injectable
faults) plus an on-target `KILNCTL_SIM_PLANT` build option
(`App/drivers/sim_backend.{c,h}`) for exercising the whole chain without real
hardware.

**Never run against a real firing.** No thermocouple/relay hardware has ever
completed the guard-provocation walk against real elements — every guard
proof to date is either a host-simulator test or, on the bench, guard 6
(sensor-invalid, trivially reachable with no thermocouple attached at all).
Treat every "implemented" claim in the linked docs as logic-verified unless
its own text says otherwise.

### 6A.0 What already exists to build on

See `docs/PID_CONTROL.md`'s module table. Two facts from `docs/HARDWARE.md`
that constrain the design: relays are EE2-12NUH electromechanical parts on
12V coils (expander pin high = energized), each on a 3-pin terminal block; a
"duty cycle" is therefore a slow time-proportioned window over I2C, not PWM.

- [ ] **Open question, blocks picking the duty-window period:** do the relays
      switch kiln elements directly, or drive external SSRs/contactors? A
      mechanical relay switching an element has a contact-life budget (~1e5
      ops) a short window would burn through in hours; an SSR has no such
      budget and allows much finer control. Until answered, the design
      assumes the pessimistic (mechanical, long window) case.

### 6A.1 Actuator model: time-proportioned output, not bang-bang

DONE — `App/drivers/heater_output.{c,h}`. Per-relay `window_ms`/`min_on_ms`/
`min_off_ms` (page-configurable per *zone*, not yet per individual relay —
see 6A.9), anti-chatter (an unachievably-short on-time renders as OFF, not a
minimum pulse), and NVS-persisted contact-cycle accounting (saturates rather
than wraps, exposed on Settings → Relays & Rules) are all built. Window
phase-offset across zones (load staggering) shipped as part of 6A.5.

### 6A.2 The PID loop itself

DONE — `App/drivers/pid.{c,h}`, pure C (no FreeRTOS/ESP-IDF/logging/I/O), so
it is host-testable against the simulator. See `docs/PID_CONTROL.md` for the
full form (derivative-on-measurement, anti-windup, functional-range blending,
bumpless transfer, feedforward, setpoint weighting). The
"cannot follow, cooling-limited" diagnostic is built
(`profile_exec_zone_status_t.cooling_limited`, exposed on `/api/control`) but
not yet exercised live — needs a real ramp-down faster than natural cooling,
which needs either a real kiln or `KILNCTL_SIM_PLANT` (neither run this pass).

### 6A.3 Thermal protection — the Klipper/Marlin-class safety suite

DONE — `App/drivers/thermal_guard.{c,h}`, a pure function of
`(setpoint, reading, commanded_duty, dt, guard_state)`. All 9 guards
(heating-failed, wrong-direction, runaway-with-heat-off, drift-at-setpoint,
absolute limits, sensor validity, frozen sensor, cross-zone plausibility,
control-tick liveness) are implemented, latch until an explicit operator
clear, fail toward off with retry, and escalate to the global fault mask or
a per-zone block as appropriate. `continue_on_zone_trip` (default: abort the
whole firing on any zone's trip) is configurable. Full per-guard status,
including which guards have been observed firing (guard-precedence findings:
several guards overlap on the same physical failure and the shortest window
wins — see `docs/GUARD_TEST_MATRIX.md`), lives in that doc, not here.

- [ ] **A guard must never be disable-able from the web UI without an
      explicit, logged, per-firing acknowledgement**, gated behind a Kconfig
      option off in production (Marlin's `THERMAL_PROTECTION_*` stance). No
      "disable thermal protection" affordance exists today, so there's
      nothing to gate yet — but the acknowledged, Kconfig-gated design itself
      isn't built either.
- [~] **Guard thresholds are per-zone config (done); Kconfig-tunable compile-time
      defaults are not.** The per-zone-override half closed 2026-08-16 (8
      threshold fields added to `zone_cfg_t`, `zones_page.html`'s "Advanced
      guard thresholds" disclosure). Fallback numbers stay `#define`s in
      `thermal_guard.c` — nothing asked for compile-time tunability, only
      per-zone at runtime, so this is a scope note, not a gap.

### 6A.4 PID autotune, for every zone

DONE for the step-test path (recommended first, per this section's original
reasoning: slow relay-cycling oscillation is thermally abusive on a kiln at
cone temperature) and for relay-feedback identification's math and on-target
state machine. `pid_autotune.c` + `autotune_engine.c`. Full detail in
`docs/PID_CONTROL.md`. Never run against real hardware — every on-target
autotune run aborts on guard 6 (sensor-invalid) before reaching a fit, since
no thermocouple is attached in this environment.

- [ ] **Gain scheduling by temperature band.** A kiln's plant gain is
      strongly temperature-dependent (radiative loss ~T^4). v1 ships a single
      band per zone, by design — `zone_cfg_t`/the NVS blob would need a band
      array to add this without a storage migration, and don't carry one yet.
- [~] **Autotune's predicted ramp ceiling is shown but not wired into
      `max_ramp_c_per_hr`.** `pid_autotune_estimate_max_ramp_c_per_hr()` is
      computed and displayed on `/settings/zones`; there is no one-click
      "adopt this ceiling" action — `autotune_accept()` only writes
      Kp/Ki/Kd, not the ramp field.

### 6A.5 Multi-zone interaction — "they are not really separate"

DONE for (a) detuned decentralized PID (concurrent multi-zone execution),
(b) coupling-matrix capture during autotune, (c) RGA display, and (d)
ramp-lock/setpoint governor — all hardware-unverified for lack of a real
cross-gain to measure against (the math is exercised by the host simulator's
coupled two-zone model). Electrical load staggering (phase-offset windows +
`max_simultaneous_relays` cap with deferred-not-dropped credit) is also
built. Full detail in `docs/PID_CONTROL.md`.

- [ ] **(e) Static decoupler / cross-feedforward** using the measured coupling
      matrix. Targeted for v1.5, after (a)-(d) are proven on a real firing,
      gated on the RGA from (c) saying it's warranted.
- [ ] **Cross-zone plausibility guard's threshold (6A.3 guard 8) still
      defaults to 0 (disabled).** The guard's logic is built and host-tested;
      what remains is informing the threshold from a measured coupling
      matrix rather than a hand-picked constant, which needs either a real
      matrix (no thermocouple hardware to capture one) or an operator
      willing to enter a number for their own kiln.
- (f) Full MIMO / model-predictive control: **decided out of scope** — the
  benefit over (a)+(d)+(e) doesn't justify the cost on a plant this slow, and
  there's no way to validate it safely on a device that fires unattended.
  Not open work, recorded as a decision.

### 6A.6 Safety plumbing that has to change

DONE: per-zone relay-authority blocking (`relay_authority_zone_blocked()`
layered on the global gate), the `THERMO`/`THERMAL_SANITY` fault-source
split, and closing the `SX_WRITE_REG`/`SX_SET_DIR` gate bypass. See
`docs/SAFETY_MODEL.md`'s updated summary table.

- [ ] **Ownership tags need `AUTOTUNE`**, alongside the `NONE`/`MANUAL`/
      `PROFILE`/`RULE` tags from section 0 (still not built as a real enum
      anywhere). A manual `SET_RELAY` from the PC during a running autotune
      step-test is not refused by any ownership mechanism today — a safety
      *fault* still wins via `relay_authority_zone_blocked()`, but an
      unrelated manual override arriving mid-test isn't.

### 6A.7 Task, timing, and module layout

DONE: all five modules exist (`pid.c`, `thermal_guard.c`, `heater_output.c`,
`pid_autotune.c`, `profile_executor.c`); 1Hz tick reading all channels once
per tick; task priority tied with the UART bridge tasks, below the
link-loss watchdog (closest achievable match to "above bridge, below
watchdog" given FreeRTOS priority integers); calibration applied on the
control path with guards seeing raw readings; mid-firing config reload with
per-field bumpless-transfer/cold-restart/immediate-apply rules; an
unowned-relay sweep; and the mid-firing `max_ramp_c_per_hr` re-check (warns,
doesn't block or re-run feasibility).

- [ ] **The tick must not block** — both bus accesses go through the
      existing `i2c_owner`/`esp_spi_owner` queues with bounded timeouts, but
      whether a timeout is actually treated as a bad read (feeding guard 6's
      debounce) rather than skipping guard evaluation was never confirmed by
      reading `MAX31856_read_all()`'s timeout contract.

### 6A.8 Verification — how any of this gets trusted

DONE: host plant simulator (single- and multi-zone, injectable faults),
on-target `KILNCTL_SIM_PLANT` build option, host unit tests for `pid.c`/
`thermal_guard.c`/`heater_output.c`/`sim_kiln.c` (see `docs/GUARD_TEST_MATRIX.md`
for the coverage table), and autotune validated against the simulator's known
ground truth. The on-target guard walk (start a profile, inject each fault
over `POST /api/sim`, watch the right guard trip) has not been performed —
was blocked by DRAM exhaustion (see `docs/BRINGUP_HAZARDS.md`), now fixed but
not yet exercised.

- [ ] **Bench test before kiln test**: a small resistive load and a
      thermocouple (soldering-iron element, heat gun into a can) to exercise
      the whole chain — real SPI reads, real I2C relay writes, real timing —
      at temperatures that cannot hurt anything.
- [ ] **First real firing is attended, low-temperature, and logged**, with
      the history/trace export (6A.9) reviewed afterward before anything
      runs unattended.

### 6A.9 UI and telemetry additions this section implies

DONE: history ring buffer gains `duty`/guard state; the dashboard graph
overlays desired/actual/duty with guard trips marked; `GET /api/control`
live status endpoint; CSV export of history and autotune traces (paginated
after an early out-of-memory bug, see `docs/BRINGUP_HAZARDS.md`).

- [ ] **Settings → Thermocouples & Zones page is only partially grown to
      match this section.** Control mode, `max_temp_c`/`min_temp_c`, per-zone
      heater timing, and a working autotune card are on the page. Still
      missing: bang-bang hysteresis (hardcoded 2°C, not exposed), per-band
      PID gains/fitted model (gain scheduling is unbuilt, 6A.4), and most
      guard thresholds beyond `sanity_rate_c_per_min` (partially closed by
      the "Advanced guard thresholds" disclosure, 6A.3).
- [~] **Per-relay `window_ms`/`min_on_ms`/`min_off_ms` is page-configurable
      per *zone*, not per individual relay** — the control path has no
      per-relay timing concept to expose.
- [ ] The 2s dashboard polling is probably fine for all of this; revisit
      push (WebSocket/SSE) only if watching a real firing proves otherwise
      (same open item as section 2).

### 6A.10 Suggested build order

Reference only, all steps through PID mode + feedforward + autotune + RGA
+ ramp-lock have shipped. Remaining: cross-feedforward decoupler (6A.5e),
load-staggering refinements, relay-feedback autotune as the routinely-used
alternate method.

### 6A.11 Open questions, collected

- [ ] Do the on-board relays switch elements directly or drive external
      SSRs/contactors? (Sets the duty-window period and contact-life budget
      — 6A.0.)
- [ ] Element power per zone and total supply/breaker capacity — decides
      whether load staggering (6A.5) is mandatory or optional.
- [ ] Maximum rated temperature of the kiln and of the thermocouples fitted
      (guard 5's `max_temp_c` is mandatory config; it needs a real number).
- [ ] Is there any active cooling or a vent that materially changes the
      plant (the rule engine can drive a vent relay — if it does, the
      identified model is only valid for one vent state, which the autotune
      procedure must record and the docs must state).
- [ ] Physical zone arrangement (stacked top/middle/bottom vs. side-by-side)
      — sets the expected coupling structure and a sanity check on the
      measured `K` matrix.
- [ ] The sanity rate the request deferred ("I will determine later"), plus
      first-pass values for every other threshold in 6A.3 — the current
      defaults are engineering guesses, explicitly labeled as such.

## 7. Documentation

DONE — `docs/WEB_UI.md`, `docs/WIFI_PROVISIONING.md`, `docs/PROFILES.md`,
`docs/PID_CONTROL.md`, `docs/GUARD_TEST_MATRIX.md`, and `docs/SAFETY_MODEL.md`'s
relay-caller summary table are all written and current. Keep them updated as
their subject areas change (see each doc's own dated-entry convention).


## 8. Storage partitioning, boot-time compatibility, and setup UX

**All five asks from this section are built** (per-concern NVS partitions,
boot-time version compatibility, the config-wizard/readiness page, and the
network status/saved-networks page). Requested 2026-08-12 after a board with
valid credentials reported itself unprovisioned. What remains open below is
purely hardware verification — none of it has touched a physical board.

### 8.1 One partition per concern

DONE — `wifi_nvs`, `kiln_nvs` (zones/rules/relay_cycles/run_state), and
`profiles_nvs` are split (`partitions.csv`), each with its own one-time
migration from the old shared default `nvs` partition (old copy never
deleted, so a rollback still finds working data), and each module owns its
own `nvs_flash_init_partition()`/`nvs_flash_erase_partition()` — no more
blanket `nvs_flash_erase()`. `POST /api/factory_reset` (`App/drivers/
factory_reset.{c,h}`) resets by scope (`wifi`/`kiln`/`profiles`/`all`), a
clean 400 on a missing/unrecognized scope, erase-then-reboot.

- [ ] **Factory-reset erase-then-reboot cycle not exercised on real
      hardware** — in particular, whether the "ok, rebooting" HTTP response
      reliably reaches the browser before the socket drops.

### 8.2 Boot-time compatibility check for every non-volatile section

DONE — every persisted structure (`zones_cfg_t`, rules, profiles,
relay-cycles) carries a schema version checked *before* any size-mismatch
wipe (a real ordering bug here was found and fixed 2026-08-13 — the version
check originally ran after the size check, defeating the whole point of a
migration path; exercised for real by the `continue_on_zone_trip` v1→v2
field addition). Three outcomes (load / migrate / refuse-newer-and-keep,
never wipe a newer-than-firmware blob), one boot-time report
(`App/drivers/nvs_report.{c,h}`, `nvs_sections` on `/api/status`,
partition-granularity only), and `zones_config_is_valid()` gating
`profile_executor_run()`/`autotune_engine.c` are all built.

`rules_http.c`/`relay_cycles.c`/`profiles_http.c` share the same
version-vs-size-ordering pattern as the pre-fix `zones_http.c` bug — not
independently fixed since none of them needed a field added this pass; fix
the same way whichever future change actually grows one of those structs.

- [ ] **Migration path never verified on real hardware** — that a board with
      real saved zone/rules/profile/relay-cycle data actually carries it
      forward into `kiln_nvs`/`profiles_nvs` on first boot after this
      change, and that `zones_config_valid` reads `true` after migration and
      `false` on a genuinely unconfigured board.

### 8.3 Config wizard page: what is set up, what is not

DONE — `GET /readiness` + `GET /api/readiness` (`App/drivers/
readiness_http.{c,h}`), a pure read-only aggregator over 11 items (network,
thermo/zone mapping, relay assignment, control mode, guard limits,
calibration, profiles, autotune, hardware present, storage compatible), each
`ok`/`not_done`/`cannot_yet`/`deliberately_off`, ordered so a page doesn't
nag about a step that's blocked on an earlier one.

Two honest, documented gaps left as `not_done`/`ok`-only rather than
inventing a distinction the storage doesn't carry: **control mode** has no
"explicitly set" bit distinct from its zero-initialized default, and
**calibration**'s `cal_offset_c == 0` has no "deliberate zero" convention the
way `max_temp_c`/`cross_zone_max_delta_c` do. Closing either needs a new
persisted flag per field — deliberately not added (out of this pass's scope).

### 8.4 Network page: live status, mode switch, and saved networks

DONE — live status (mode, join state, RSSI, IP, mDNS name, AP client count)
on the same page as the mode switch; a bounded (8-entry) saved-network list
with a documented RSSI tie-break and Forget; `GET /networks` merges saved +
scanned into one view rather than two lists; forgetting the last saved
network is allowed (drops to AP mode, never blocked); stored passwords are
never returned by `wifi_prov_get_saved_networks()` (structurally absent from
its output type) — except the board's **own** AP identity, an explicit,
user-confirmed scoped exception, always shown pre-filled for editing.

- [ ] **Nothing in this subsection has been flashed/tested on real
      hardware.** In particular: a pre-8.4 single saved network migrates
      into `nets[0]` and still joins on boot; two saved networks and the
      board actually prefers the stronger one; forgetting the last saved
      network drops to AP mode without stranding the operator; the periodic
      30s rescan (`rescan_timer_cb`) rejoins a saved network coming back
      into range without waiting for a disconnect event; and moving the
      scan-based tie-break off the Wi-Fi driver's disconnect-event handler
      (onto that periodic timer instead) doesn't introduce a noticeably
      slower reconnect when the active network is just flapping.

### 8.5 Sequencing

Reference only, all four sub-sections shipped in the dependency order this
called for (8.2's versioning before 8.1's partition split; 8.1 before 8.4's
saved list; 8.3 last as a view over the other three).

---

## 9. Firmware updates — ESP OTA and relaying the Pico's image

Full design (interlocks, shared authentication): [`../CommonFW/docs/
UPDATE_PROTOCOL.md`](../CommonFW/docs/UPDATE_PROTOCOL.md). RP2040 half:
[`../SaftyFW/docs/BOOTLOADER.md`](../SaftyFW/docs/BOOTLOADER.md). Two things
make this more than a normal OTA feature: the partition table can only be
changed over a cable (**you cannot OTA your way into being OTA-capable**, so
the first flash of the new table is a one-time USB step); and the ESP is the
only thing authorising a Pico update, making this endpoint the safety
processor's attack surface too.

**Everything through 9.6a is built and either host-tested or `idf.py build`
clean** (mutual version compatibility, the 8/16MB partition table +
rollback, HMAC-challenge auth with lockout, interlocks that refuse an update
unless idle/cool/safety-link-healthy, streamed transfer for both processors
via a `pico_img` staging partition, the web page, and MCP tools). **No
physical ESP32-S3 or Pico exists in this environment — nothing in this
section has ever run against real hardware.** Section 9.7 below is
therefore the entire hardware-verification surface still owed, and every
other open item below is a real, separately-named gap rather than a
hardware caveat.

### 9.0 Mutual version compatibility (prerequisite)

DONE — `ANNOUNCE_VERSION` burst at boot/on `boot_id` change (now via the
shared `kilnlink_announce` codec), bidirectional `KILNLINK_MIN_COMPATIBLE`
check, a mismatch treated as a dead link, the GUI naming both versions and
which is older (always "update ESP first"), and an explicit `GET_FW_VERSION`
retried until answered. Full detail: `LINK_PROTOCOL.md` sec 4.

- [ ] **Refuse to push a Pico image this build could not then talk to**
      (protocol-incompatible). Deferred, out of scope for this OTA pass —
      see 9.5's matching open item, which is the same gap restated at the
      transfer layer.

### 9.1 Partition table

DONE — confirmed N16R8 (16MB flash/8MB PSRAM); `partitions.csv` carries
`otadata` + `ota_0`/`ota_1` (2048K each) + `pico_img` (896K, corrected up
from an originally-proposed 512K — too small for SaftyFW's 832K app slot),
all above `0x200000` where nothing else lives. Rollback enabled
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`). `idf.py build` clean, no
overlap/overflow.

- [ ] **`esptool flash_id` never run** — size was confirmed via the board's
      product page (N16R8) instead; the buy-list and 3D-model records (both
      stale, claiming 8MB/N8R8) still need correcting at the source.
- [ ] **Reflash the bootloader on the physical board** — a new partition
      table alone does nothing; the flash size is baked into the bootloader
      header. One-time USB/serial step, not yet performed.
- [ ] **Pre-change partition table archived** so a rollback to pre-OTA
      firmware is possible — nothing to archive from yet, no physical flash
      has happened.
- [ ] **One-time serial flash documented as a prerequisite step**, not a
      footnote — still outstanding.
- [ ] **`nvs`/`wifi_nvs`/`kiln_nvs`/`profiles_nvs` read out and archived from
      the physical board with `esptool read_flash` before the new table is
      ever flashed for real.** The one irreversible step in this whole plan.

### 9.1a PSRAM — enabled 2026-08-17

DONE. Originally decided off (four reasons: no framebuffer needed since the
ILI9488 driver streams to its own GRAM, determinism/stall risk, a new boot
failure mode, DMA-buffer audit cost); reversed once LVGL (section 10) needed
draw buffers — the named trigger this decision itself called out in advance.
`sdkconfig`: `CONFIG_SPIRAM=y`/`_MODE_OCT=y`/`_BOOT_INIT=y`. LVGL's draw
buffers and allocator, plus several bridge-task stacks, now live in PSRAM
(see `docs/BRINGUP_HAZARDS.md` for the internal-SRAM race this interacted
with). R8 (with PSRAM) is now a hard requirement on any board reorder — an
R2/no-PSRAM part would break the LCD memory plan.

- [ ] **Second, weaker trigger**: TLS on the web server, or many concurrent
      HTTP connections — measure the heap before assuming either needs more
      PSRAM use.
- [ ] **Keep GPIO 33-37 unassigned** — consumed by the R8 module's own PSRAM
      regardless of software config. No conflict today (board uses 0-21,
      38, 43, 44, 47, 48); keep it that way on any future pin assignment.

### 9.2 Rollback

DONE — `esp_ota_mark_app_valid_cancel_rollback()` runs from a background
task gated on NVS-readable + safety-link-exchanging-frames + web-server-up,
all three fully wired (none stays permanently false without a reason).
`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` deliberately left off.

### 9.3 Authentication — the AP password, not sent over the wire

DONE — single-use 30s-expiry challenge, HMAC-SHA256 proof of knowledge (PSA
Crypto API), constant-time comparison, lockout after 3 failures (doubling to
15 min, per-endpoint), every attempt logged with source IP, and only the
derived key (never the literal PSK) ever reaches the comparison.

- [ ] **Not yet stated anywhere user-visible** that this scheme does not
      defend against anyone who already knows the AP password (that IS the
      credential) — the prose exists in `UPDATE_PROTOCOL.md` §2 but the OTA
      web page doesn't say it yet.
- [ ] **TLS — planned in `docs/UI_PLAN.md` §6, not authorized to build.**
      Adds confidentiality/integrity to the image transfer (today plaintext
      LAN); does not replace or weaken the HMAC scheme above, and is not
      image signing (that stays a separate, unplanned concern). Switching
      the shared `esp_http_server` instance to `httpd_ssl_start()` would
      cover every OTA route with no per-route change.

### 9.4 Interlocks

DONE — both update paths refused unless idle (no profile/autotune running,
no heater commanded, every zone below a configurable ceiling, safety link
healthy), refusals name the specific unmet precondition, and a single
in-RAM mutex refuses starting either update path while the other is in
progress. During a Pico update the safety-link-down alarm *text* is
suppressed (never the relay-on block itself).

### 9.5 Transfer

DONE — streamed `esp_ota_ops` POST handler for the ESP image (no full image
held in RAM), a `pico_img` staging partition + background relay task driving
the five `UPDATE_*` frames with gap-based retransmission (up to 10 rounds,
then always attempts `UPDATE_END` and lets the Pico's own CRC verification
arbitrate), whole-transfer socket timeouts, image magic/chip-ID validation
before `esp_ota_begin()`, `SAFETY_CMD_ANNOUNCE_REBOOT` sent before a routine
reboot so it doesn't trip the safety processor's S6b guard, the update
mutex's first real caller, an append-only (single-record) NVS update log,
and polled progress for both paths.

- [ ] **Protocol-version mismatch between an uploaded Pico image and the
      running ESP is not proactively warned about with a second
      confirmation** before the relay starts — explicitly out of scope for
      the pass that built the transfer path. The Pico itself still refuses
      an incompatible image on its own (`UPDATE_STATUS_ERR_VERSION_INCOMPATIBLE`),
      which is the protocol's required floor; this item is specifically
      about a proactive ESP-side warning.
- [ ] **`ota_record_t` has no image SHA-256 field** — flagged, not faked;
      would need hashing the stream as it passes through the transfer
      handler (mbedTLS/PSA is already linked for the HMAC path).

### 9.6 Web page

DONE — `GET /ota` (`ota_page.html`), per-processor version/commit/build-date/
dirty-flag/active-inactive-slot (ESP side), interlock state shown before the
file picker (unauthenticated `GET /api/ota/interlock`, file pickers hidden
entirely until it reports idle), progress bars, and an ESP rollback button
(`POST /api/ota/esp/rollback`, its own HMAC context so a push-authorization
signature can never double as a rollback authorization).

- [ ] **Pico build commit/dirty flag/build date are not retrievable** — a
      real protocol gap, not an oversight: `safety_parse_fw_version()` parses
      past those bytes to reach `boot_id` but never stores them. Needs a
      `safety_link.h`/`.c` change (new fields + getter), out of scope for the
      pass that built this page.
- [ ] **Pico rollback button is a disabled placeholder** ("not yet
      available") — no HTTP route exists; a Pico rollback trigger is
      expected to go over the safety UART link (`SAFETY_CMD_ROLLBACK`, 0x17,
      already built on the send side per `safety_link_send_rollback()`), not
      HTTP.

### 9.6a MCP tools (`tools/PcTools`)

DONE — `ota_get_challenge`/`ota_update_esp`/`ota_update_pico`/`ota_status`/
`ota_rollback_esp` all wrapped as `mcp__kilnctrl__` tools, unit-tested
against mocked HTTP. No physical board has ever answered a real request from
these tools. `image SHA-256` is unbuilt (same gap as 9.5's `ota_record_t`
note).

### 9.7 Verification

None of the following has been performed — no ESP32-S3/Pico hardware in
this environment.

- [ ] Power pulled mid-transfer, both processors — both still boot the old image
- [ ] Corrupt image rejected, both processors
- [ ] An image that boots but fails bring-up is rolled back with no intervention
- [ ] Update attempted while firing: refused, blocker named
- [ ] Wrong password: refused, locked out, logged
- [ ] Recovery from a deliberately bricked Pico over SWD

## 10. LCD touchscreen GUI (Klipper-style screen)

The LCD (ILI9488, physically connected) has its own touchscreen UI, kept in
sync with the web dashboard rather than duplicating/diverging from it (see
10.5). Usability/no-scroll budget audit and fix queue: `docs/UI_PLAN.md`.

### 10.1 Generic screen/page/widget framework

DONE — LVGL chosen and owns the ILI9488 outright (replacing the old
UART-remote-drawn `DISPLAY_CMD_*` path; this was an explicit user decision
once two independent draw-call owners proved to be an unresolvable race).
`App/drivers/kiln_ui.c`/`.h` is the page registry/switcher; every page is its
own `ui_page_<name>.c`/`.h` pair, never inlined into `kiln_ui.c`. Fixed a real
bug in the process: a touch waking the screen from idle previously set a flag
with nothing to repaint it — `lvgl_port.c`'s flush callback now forces a
redraw on the wake edge.

- [ ] **Decision pending: `tools/PcTools`' 12+ MCP `display_*` tools are
      stale.** They send `DISPLAY_CMD_*` frames the firmware can no longer
      answer (`uart_bridge.c`'s `display_bridge_task` is intentionally dead
      code — LVGL owns the display exclusively now). Not a crash risk (calls
      simply time out), but every invocation silently fails. Options: (a)
      delete the stale PC-side tools, (b) restore a minimal firmware handler,
      (c) leave as-is. Not pre-judged.
- [ ] Decide the color/asset story once the visual style (10.2) is picked —
      LVGL widgets are themeable, so this is a theme/style pass on stock
      widgets, not custom-drawn ones.

### 10.1a Shared backend with the web UI

**Standing rule, not a one-time task**: a page's data access and actions
must go through the same plain-C functions both the LCD and the web HTTP
handlers use — one backend, two front ends, never two independently
maintained readings of the same state. Built incrementally, split out of an
HTTP handler at the point a 10.3 LCD page first needs the same data (not
speculatively ahead of a real second caller). See `docs/ARCHITECTURE_
DECISIONS.md`'s LVGL/LCD section for the rule statement.

- [ ] As each 10.3 page is built, extract its backend data access from the
      matching HTTP handler into a shared plain-C getter/action function —
      actions (`profile_executor_run()`/`_pause()`/`_halt()`,
      `relay_authority_*`) are already single-implementation and correct as
      called from the LCD; it's specifically the *read* side each handler
      still inlines that needs splitting when a page reaches for it.
- [ ] Where a handler is split, update the HTTP handler to call the new
      shared getter too — never leave it calling the data-owning module
      directly while only the LCD gets the seam.

### 10.2 Visual style — match KlipperScreen

DONE — `App/drivers/ui_theme.h` + `docs/UI_THEME.md`: dark-navy palette, five
named accents, minimum touch target (72px), corner radius, padding, and status
bar height, all as a single shared source of truth (10.6 restyles the web
dashboard to match). First-pass values inferred from a description of
KlipperScreen's look, not extracted from its actual theme files — flagged as
unverified-against-real-hardware in `ui_theme.h` itself; sanity-check against
the physical ILI9488 and correct both files together if it's off. Font sizes
are not yet part of the shared table (LVGL font selection wasn't scoped in).

### 10.3 Page designs

DONE — home (zone cards, desired-vs-actual chart, Start/Stop/Menu),
Configuration hub (7 nav destinations), Temperature (per-zone reading +
manual relay toggles, no setpoint override per explicit decline), Network,
Safety Processor card, Board Health, Diagnostics, Thermocouple Faults, and
Touch Calibration (with a Cancel path, fixed 2026-08-19) all exist and are
rebuilt to fit the hard **no-scroll rule**: every page must fit ~264px of
real content height (320px panel minus status bar/padding), computed from
`ui_theme.h`'s real constants — not pixel-verified on hardware. Every field
comes from the same plain-C getters the web HTTP handlers use (10.1a).

The four pages this section added to the plan 2026-08-18 (Safety/Alarm,
Diagnostics, Thermocouple Faults, Backup/restore) are tracked in section 0.5,
not duplicated here.

- [ ] **`UART_TASK_ID_WIFI` (11) sometimes doesn''t register at boot** — PC-tool
      `wifi_get_status`/`wifi_scan` NACK "destination task not registered"
      with no corresponding failure logged. Does not affect the AP itself
      (comes up and accepts phone joins regardless), only PC/MCP tooling''s
      ability to query Wi-Fi state over UART. Not chased down; worth a
      dedicated pass with more boot instrumentation around
      `uart_bridge_start_wifi_task()`''s call site.
- [ ] **`ui_page_temperature.c`''s no-scroll fit depends on relay count per
      zone at runtime** — a zone with several relays wrapping onto a second
      button row could still overflow the ~264px budget. Real, unresolved
      risk without a fixed relays-per-zone cap or real hardware to check
      against.
- [ ] **`ui_page_network.c`''s worst-case fit (~268px against a ~264px
      budget) is computed, not hardware-confirmed**, and is the tightest of
      any page. If it turns out tight on real hardware: split Scan and Saved
      into their own sub-pages via `kiln_ui_show()`, the same pattern
      Safety Processor/Temperature History already use.

### 10.4 Touch hit-testing

DONE — confirmed by reading LVGL's source rather than assuming (`lv_indev_
search_obj()` does first-match-in-z-order rectangle containment, not
nearest-center). `ui_theme_apply_touch_area()` (dynamic per-widget extended
click area, sparse vs. compact-layout cases) and `ui_theme_register_touch_
group()`/`ui_theme_resolve_touch_target()` (opt-in nearest-center arbitration
for a registered cluster, built ahead of a real consumer per explicit
request) are both built. See `docs/ARCHITECTURE_DECISIONS.md`. No dense grid
page exists yet to actually register a touch group, but the mechanism is in
place and costs nothing when unused.

### 10.5 Web/LCD parity rule

- [ ] **Whenever either the LCD screen or the web interface changes,
      consider whether the other should change too.** If the answer isn't
      clear, ask the user rather than guessing; if it's clear-cut (e.g. a
      new zone field needs to show up in both places), make the matching
      change without asking. Applies to both directions — a web feature
      added later needs the same consideration for the LCD, not just LCD
      to web.

### 10.6 Web dashboard restyle to match the LCD

DONE — all seven web pages (`main_page.html`, `zones_page.html`,
`rules_page.html`, `profiles_page.html`, `wifi_provision_page.html`,
`readiness_page.html`, and later `ota_page.html`) restyled to the same
palette as `docs/UI_THEME.md`, applied only to each page's existing dark-mode
CSS variant (light mode untouched) via a duplicated `<style>` block per page
— a shared `theme.css` route was considered and rejected as riskier than
warranted for a visual-only change, since no single file registers all seven
pages' handlers. No `.c`/`.h`, element ID, or JS behavior changed.

- [ ] **Not visually verified against real hardware/browser** — a phone and
      the LCD's own browser should confirm the restyle once a build is
      possible in this environment.

### 10.6a Gzip the embedded web pages

DONE — the six original HTML pages plus `theme.css` are gzipped at CMake
configure time (Python `gzip` module, not a Unix binary — avoids the Windows
dev-machine wrinkle) and embedded pre-compressed; `zones_page.html` is
deliberately excluded and stays raw (confirmed safe: its handler sends no
`Content-Encoding` header). Content negotiation (`App/drivers/web_encoding.
{c,h}`) now serves gzip whenever a client doesn''t explicitly exclude it (per
RFC 9110 — a request with no `Accept-Encoding` header legally accepts any
coding) and returns 406 with an uncompressed body only when a client
explicitly excludes gzip (`identity`, `q=0` forms). The "compile CSS/JS to
native code for the LCD" half of the suggestion that prompted this was
evaluated and **rejected** — LVGL stays the LCD rendering backend.

- [ ] **Re-verify byte-for-byte that nothing about the page *content*
      changed** — this was a transport-encoding-only change; verify by
      comparison rather than trust.

### 10.7 Onboard IC temperature sensors

DONE — `App/drivers/board_temps.{c,h}` surfaces the ESP32-S3''s internal die
temperature and every MAX31856''s cold-junction reading. `GET /api/board_temps`
(web) and `ui_page_board_health.c` (LCD, reached from the Configuration hub)
both read the same `board_temps_get_live()` getter (10.1a''s shared-backend
rule) — deliberately a separate page/route from the main dashboard, since
this is board-health diagnostic data, not kiln-process data.

- [ ] **No styled web *page* for `GET /api/board_temps`** — only the LCD side
      got a page this pass; the JSON endpoint has no HTML equivalent yet.

### 10.8 Multi-thermocouple-per-zone (cross-reference: section 3)

DONE — a zone can have more than one thermocouple assigned
(`zone_cfg_t::thermo_mask`, `ZONES_CFG_VERSION` 3->4, migration fills the
implicit pre-10.8 one-channel-per-zone mapping so no existing zone silently
goes invalid). Combining function is **arithmetic mean of valid readings**
(a deliberate decision — outlier rejection / max-biased combining were
considered and left explicitly undecided, not guessed at) in the new pure,
host-tested `App/drivers/thermo_combine.{c,h}`. Both `profile_executor.c`
(control tick + run-start baseline) and `autotune_engine.c`'s step-test read
path consume the combined value; `thermal_guard.c`'s guard 6 extends to
"all assigned thermocouples invalid" for free, since it was already written
against an opaque `sensor_ok` bool.

- [ ] **`zones_page.html` has no UI for assigning more than one channel to a
      zone.** The config model, combiner, and control/guard read paths all
      support it; only the web settings page to actually set a multi-bit
      `thermo_mask` is missing (MCP/API callers can still set it directly).

### 10.9 LCD network settings page + QR codes for AP/site connect

DONE — `ui_page_network.c`/`.h` mirrors the web provisioning page (mode
readout, scan with the AP-mode "disabled" message, saved-network list with
forget, AP identity display, an on-screen-keyboard connect flow via
`lv_keyboard`), all through the same `wifi_prov.h` getters/setters the web
handlers use. QR codes ship on both surfaces: an AP-join QR (`ui_page_
network.c` and, compact, `ui_page_home.c` while in AP mode) and a
dashboard-URL QR (`kiln.local` preferred, raw IP fallback) once connected,
using LVGL''s `lv_qrcode` on the LCD and an embedded client-side JS QR
library (no CDN) on the web.

**Recurring hazard found twice this section: `sdkconfig` vs. `sdkconfig.
defaults`.** `sdkconfig` is gitignored/machine-local; several settings this
firmware depends on (`LV_USE_QRCODE`, `LWIP_MAX_SOCKETS=16`, the `SPIRAM_*`
block, `PARTITION_TABLE_CUSTOM`/`ESPTOOLPY_FLASHSIZE_16MB`) were only ever
set in the local file, so a fresh checkout would silently fail to build or
build a non-working image. All now pinned in the committed
`sdkconfig.defaults` with comments explaining the dependency and failure
mode, each verified by deleting `sdkconfig` and reconfiguring from
`sdkconfig.defaults` alone. Worth checking this class of gap again after any
future menuconfig change.

- [ ] **Editing the board''s own AP SSID/password from the LCD is not
      built** — `ui_page_network.c` shows them read-only;
      `wifi_provision_page.html` remains the only way to change them.

### 10.10 Safety processor GUI panel (ROADMAP.md M6)

DONE — safety-processor thermocouple, enclosure (cold-junction) temperature,
and power (Frame E, `kilnlink_power.{c,h}`, host-tested) are all plumbed
into `dashboard_status_t`/`GET /api/status` and a "Safety Processor" card on
`ui_page_home.c`, per `LINK_PROTOCOL.md` sec 7''s explicit placement (this is
kiln-process data an operator watches while firing, unlike 10.7''s
board-health diagnostics). SaftyFW now sends Frame E too, but guards S3/S4
are deliberately not wired to it yet — needs the per-channel CT-mapping
commissioning check on real hardware first (`SaftyFW/docs/CURRENT_SENSE.md`
sec 5).

- [ ] **Not hardware-verified, and cannot be from this environment.** No
      ESP32-S3/Pico is attached, and the isolated link doesn''t pass a byte
      end-to-end on the real board (`ROADMAP.md` M0) — every field reads
      `null`/"---" today by design, not by bug. Needs both M0 (link fixed)
      and a live Pico emitting Frame A/Frame E.

### 10.11 Liveness: 1.5 s fault, 30 s firing-abort (ROADMAP.md M6)

DONE — `LINK_PROTOCOL.md` sec 8''s two-timeout rule. `SAFETY_LINK_STALE_MS`
(1500) is a fixed ceiling, decoupled from the configurable poll period (a
reconfigured period can only make the fault fire sooner, never later). 30s
of continued silence while a firing is running/paused aborts it via the
existing guard-9 watchdog path (relays dropped and retried, `run_state`
records the end) rather than a new mechanism.

- [ ] **Not hardware-timing-verified.** No ESP32-S3/Pico is attached — whether
      the fault really asserts at 1.5s and the abort at 30s on real hardware
      is unverified; this closes the code gap, not the timing-verified gap.
      `safety_link_is_stale()` is a pure function that could be host-tested
      but lives in an ESP-IDF-only header today — left as a follow-up.

### 10.12 ESP → Pico context broadcast, `SAFETY_CMD_PUSH_CONTEXT` (ROADMAP.md M5)

DONE — `LINK_PROTOCOL.md` sec 4''s 0x07 frame (relay now/recent masks,
per-zone measured temp/fault/type/setpoint/active/relay-on/guard-tripped,
top-level profile-running/any-zone-faulted/heat-requested flags) is built
from real board state and broadcast every poll period via
`safety_build_and_send_context()`.

- [ ] **Not verified against a real Pico.** No RP2040 is attached — the frame
      is built/broadcast and matches the codec''s own contract, but nothing
      confirms a real SaftyFW build decodes it correctly. SaftyFW''s receive
      side is tracked separately in `firmware/SaftyFW/TODO.md`.

### 10.13 DIAG / TRIP_EVENT decode + dispatch (ROADMAP.md M5)

DONE — `LINK_PROTOCOL.md` sec 6''s Frame B (`SAFETY_CMD_DIAG`, 0x08) and Frame
D (`SAFETY_CMD_TRIP_EVENT`, 0x0D) are decoded and cached in `safety_link.c`
(`safety_apply_diag()`/`safety_apply_trip_event()`, `TRIP_EVENT` deduped on
`trip_seq`), passed through to `dashboard_status_t`/`GET /api/status`
(present only once a frame has ever arrived), and a "Last trip" row on
`ui_page_safety.c` (the only field that fit its no-scroll budget; DIAG''s
warn/trip masks are cached and HTTP-exposed but deliberately not added to
this page — a future dedicated diagnostics page is the better home). Also
mirrored onto the PC-link SAFETY UART task as two new query subcommands
(`SAFETY_CMD_GET_DIAG` 0x0C, `SAFETY_CMD_GET_TRIP_EVENT` 0x15), answered
purely from the cache. This closes "the ESP can receive and decode these
frames" — SaftyFW does not send either frame yet (tracked in
`firmware/SaftyFW/TODO.md`).

- [ ] **Not hardware-verified, and cannot be from this environment.** No
      ESP32-S3/Pico is attached — `safety_apply_diag()`/`safety_apply_trip_
      event()` have never decoded a frame that actually crossed the wire,
      only a clean cross-compile. Needs both the link fixed (M0) and a
      SaftyFW build that sends Frame B/D.
- [ ] **`pc_tools`/MCP client-side decode of the two new GET_DIAG/
      GET_TRIP_EVENT subcommands is not built** — the ESP-side answer exists;
      nothing on the PC side parses the reply yet.

### 10.14 Command queue between every control surface and the tasks that own state

Full task inventory, ownership doctrine, and honest verification status:
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Design rationale and the
build-order table: [`docs/ARCHITECTURE_DECISIONS.md`](docs/ARCHITECTURE_
DECISIONS.md). Filed from a real bench bug (2026-08-19): `ui_page_network.c`
froze the *entire* LCD display whenever Scan/mode-switch/connect ran a
blocking `wifi_prov_*()` call from the one task allowed to touch LVGL.

**Pattern** (copied from `firmware/SaftyFW/src/tasks/relay_owner.c`, already
proven on the RP2040 side): one owning FreeRTOS task per state-owning
domain, a small bounded queue, `<owner>_command_<verb>()` producer functions
(fire-and-forget or request/response), owner drains with a bounded timeout.

**Built**: `kiln_io_owner.c`/`.h` (Phase 1 — closes a real lost-update race:
UART/HTTP/LCD/profile-executor/autotune all wrote relay state with zero
coordination and two independently-duplicated safety-gate checks);
`thermo_owner.c`/`.h` (Phase 2 — architectural consistency, not a bug fix,
since `MAX31856.c`'s own per-channel lock already prevented a lost update;
lays the chokepoint Phase 6 will need); `wifi_prov.c`'s own owning task
(Phase 4 — the largest real bug in this section, `s_wifi` had zero locking
across four independent writers including the Wi-Fi driver's own event
callbacks; every `wifi_prov_*()` signature stayed unchanged so no caller
needed editing); and the LCD-side stopgap (`ui_page_network.c`'s three
worker-task-per-action job structs, now posting through the owners instead
of touching state directly).

**Phase 3 (`profile_executor` command queue) was reviewed and deliberately
skipped, not deferred** — all four run/pause/halt/resume entry points
already wrap their bodies in one correct mutex with no lost-update bug to
fix; converting a safety-critical state machine to a drop-on-full-queue path
would be pure regression risk for zero safety gain. Revisit only if Phase
6''s system-mode gate genuinely needs a choke point the lock can''t provide.

**Verification**: build-clean under `-Werror` throughout. Phase 4 reached a
partial hardware data point (flashed, boots, stays running) but the PC-link
UART in this environment is physically broken, so no Wi-Fi behavior itself
(AP fallback, rescan, a real join, concurrent-command contention) has been
observed. Phases 1/2 are build-verified only — no board, and separately no
thermocouple daughterboard, to observe the owned hardware paths working.
Two real THERMO protocol bugs (fabricated fault data, a silently-dropped
query reply) were found and fixed while bringing Phase 2 up on a real bench
— see `docs/BRINGUP_HAZARDS.md`. A bench stack-overflow crash-loop, initially
suspected to be related to this work, was root-caused separately (also in
`docs/BRINGUP_HAZARDS.md`) and was not caused by anything in this section.

- [ ] **Web side**: `dashboard_http.c`/`zones_http.c`/`profiles_http.c`/
      `rules_http.c`/`ota_http.c`/`wifi_provision_http.c` action-taking POST
      handlers should post commands instead of running inline on
      `esp_http_server`''s one shared worker task. `ota_http.c`''s transfer
      handlers and `provision_post_handler()` are the highest-value targets
      (longest/most blocking). OTA''s transfer handlers are explicitly
      out-of-scope for migration — they legitimately need to hold a
      streaming HTTP body open across the whole transfer.
- [ ] **Debug/PC-link UART side** (explicit user request): `uart_bridge.c`''s
      per-subsystem tasks (THERMO/IO already migrated via the owners above;
      DISPLAY is dead code and should be deleted rather than migrated) post
      commands to an owning task instead of running the dispatch switch
      body inline.
- [ ] **Phase 5: HTTP handler migration**, per-domain, alongside whichever
      owner (1/2/4) each handler calls into — not a separate final phase.
- [ ] **Phase 6** (added mid-Phase-1, user request): a **system-mode command
      gate**, distinct from the owner-task pattern above. The owners answer
      "can two writers race on this state"; this answers "is this *class* of
      command allowed at all given what the system is doing right now" —
      e.g. while a profile is firing, stop/pause/modify-this-run is fine, but
      starting a *different* profile, running autotune, or a raw GPIO/SX1509
      debug write should be refused outright. Needs to see
      `profile_executor`''s and `autotune_engine`''s state, which
      `kiln_io_owner`/`thermo_owner` have no business knowing about — a
      policy layer above the owners, consulted by every producer-facing entry
      point before a command is even built. Needs its own design pass before
      it''s built; do not implement ahead of the phases above landing.
- [ ] `profile_executor.c`/`relay_authority.c` gaining the same
      `relay_owner.c`-style queue is a candidate once the web/LCD callers
      that drive them are migrated — not urgent (current call pattern
      hasn''t been observed to freeze anything).
- [ ] `ui_page_network.c`''s three job structs are candidates to migrate onto
      `wifi_prov`''s real owner queue once a shared async shape exists,
      rather than staying page-local one-offs.
