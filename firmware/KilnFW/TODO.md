# TODO — Web UI / Kiln Control Front End

Top-level ordering across both processors lives in [`../../ROADMAP.md`](../../ROADMAP.md);
this file owns the main-firmware detail. The cross-processor items — the swapped
safety-UART pins, `CommonFW`, and the safety-liveness gate on heating — are
sequenced there and tracked in [`../SaftyFW/TODO.md`](../SaftyFW/TODO.md).

Planning doc; sections 1 and 4 (Wi-Fi provisioning/network settings) are
implemented and hardware-verified as of 2026-08-10. Section 10.14 (command
queue architecture for control surfaces) — Phases 1 (`kiln_io_owner`), 2
(`thermo_owner`), and 4 (`wifi_prov` owning task) — completed 2026-08-19;
Phase 3 deliberately skipped (documented decision). **Critical caveat on
Phase 4 verification**: the PC↔ESP UART link is physically broken in this
environment (separate from known Pi↔ESP break), so Wi-Fi behavior was NOT
observed; Phase 4 booted successfully but needs real Wi-Fi hardware testing
before shipping. Only the bare ESP32-S3 board is wired up so far (no
thermocouple daughterboard, relays, display, or safety RP2040). Everything
else below is still unbuilt. Captures the feature request as itemized work so
it can be scoped and sequenced before any code is written. Cross-references
`docs/SAFETY_MODEL.md` and `docs/PROJECT_STATUS.md` throughout, since the
profile-execution engine this introduces is a **new actor that commands relays**
and must go through the same safety-wins gate as the existing PC/MCP link —
not a parallel path that bypasses it.

## 0. Architecture decisions to make before writing code

These block sequencing, not just polish — get them settled first.

- [x] **Where does the web server run?** On the ESP32-S3 itself
      (`esp_http_server` + WebSocket/SSE for live data), serving a static
      mobile-optimized page bundle from flash/LittleFS. This is a *third*
      client alongside the existing UART PC link and MCP server, not a
      replacement for either — confirm that's the intent.
      **Settled for the Wi-Fi provisioning/network slice**: `esp_http_server`
      on-device, static page `EMBED_TXTFILES`-embedded from flash (no
      LittleFS needed yet — see `App/drivers/wifi_provision_http.c`). Whether
      the dashboard/profile pages (sections 2/3/5/6) reuse this same server
      or need WebSocket/SSE push is still open; this only settled the
      Wi-Fi-config slice.
- [x] **Who drives relays for a running profile?** `profile_executor.c` is
      the on-device profile-executor task, distinct from the UART bridge's
      `SET_RELAY` path but subject to the *same* gate
      (`relay_authority_on_blocked()`/`relay_authority_zone_blocked()`) —
      built, see section 6. The ownership arbitration below (which caller
      wins when a manual command and a running profile want the same relay)
      is also built: `relay_authority_claim_mask()`/`_release_mask()` at
      profile start/pause/resume/halt, checked by
      `relay_authority_manual_blocked_by_owner()` in `uart_bridge.c`'s
      `SX_SET_RELAY`/`SET_RELAY_MASK` and `dashboard_http.c`'s `/api/relay`.
      Logic-verified only (host build + `idf.py build` clean, `-Wall -Wextra
      -Werror`) — no relay/expander hardware is attached yet to observe a
      manual command actually get refused against a live claimed relay.
  - [x] Decide: does the profile executor call `kiln_io_set_relay`
        directly, or through the same bridge-level gate function refactored
        into `kiln_io`/a new `relay_authority` module so *every* caller
        (UART bridge, web UI, profile executor) shares one gate?
        **Settled and done (2026-08-10)**: extracted into
        `App/drivers/relay_authority.{c,h}` — `relay_authority_on_blocked()`
        is now the one implementation, and `uart_bridge.c`'s
        `io_relay_on_blocked()` is a thin wrapper calling it. Build-verified
        (behavior-preserving refactor; the actual relay/safety hardware
        isn't wired up yet to test the gating outcome itself, only that it
        still compiles and boots correctly).
  - [x] Decide: can the web UI's manual relay override and a running
        profile fight over the same relay? Needs an explicit ownership/lock
        model (e.g. starting a profile takes ownership of its assigned
        relays; manual override either pauses the profile or is refused).
        **Settled (2026-08-10, design only — no code yet, needs the
        profile executor from section 6 to exist first)**: each relay gets
        an owner tag — `NONE` / `MANUAL` / `PROFILE` / `RULE` (ties into the
        rule-engine decision below). Starting a profile claims `PROFILE`
        ownership of every relay assigned to its zones; a manual `SET_RELAY`
        against a `PROFILE`-owned relay is **refused**, not silently
        allowed and not an implicit pause — same "ask, but a real condition
        wins" precedent `relay_authority` already sets for the safety gate,
        just a different refusal reason. Pausing the profile (section 6's
        pause requirement) is the one explicit way to hand a relay back to
        `MANUAL`; resuming reclaims it. Chosen over auto-pause-on-touch
        because a control surface whose "Stop" button matters (the phone
        screen constraint in section 0.5) should never have a *side effect*
        as consequential as pausing a firing triggered by a different,
        unrelated action.
- [x] **Storage.** NVS (key-value, small, wear-levelled, already used for
      Wi-Fi credentials) vs. LittleFS (file-based, better for a list of
      named profiles with many segments and a static web asset bundle).
      Likely both: NVS for credentials/calibration/small config, LittleFS
      for profiles and the web bundle.
      **Settled for Wi-Fi**: NVS namespace `wifi_cfg` (ssid/pass/has_creds/
      local_only) — see `App/drivers/wifi_prov.c`. Verified on hardware to
      survive a reflash. LittleFS still undecided/unneeded until profiles
      (section 5) or the dashboard bundle (section 2) need it.
- [x] **Relay "alternate function" rule engine scope.** "Triggered based on
      multiple time/temperature conditions" is a small rule evaluator, not
      a single if/else — needs its own design: what conditions compose
      (AND/OR?), which zone's temperature a rule reads, whether rules can
      reference other relays' state, and how a rule interacts with the
      safety gate above (a rule commanding a relay ON is exactly as
      gate-able as a manual command).
      **Settled (2026-08-10, design only — no code, no relay hardware to
      build against yet)**:
  - Composition: a **relay has a list of rules, each rule a flat list of
    conditions ANDed together; any rule evaluating true commands that relay
    ON** (OR across rules, AND within one). No nested/parenthesized boolean
    expressions in v1 — a config editor for arbitrary boolean trees is a lot
    of UI and firmware for conditions nobody has asked for yet; "vent open
    above 500°F" is one condition, "vent open above 500°F while the heating
    relay is on" is one two-condition rule, and both are expressible without
    it. Revisit only if a real use case needs OR-inside-AND.
  - Condition types, v1: zone temperature vs. threshold (reads the same
    per-zone thermocouple reading section 3 already names); elapsed time
    since profile start; another relay's *commanded* state (the shadow
    `kiln_io` already tracks, e.g. `SX1509_RELAY*_PIN` shadows in
    `kiln_io.h`) — not its physical/read-back state, so a rule can't stall
    waiting on hardware it doesn't trust in the first place.
  - Assignment is mutually exclusive per relay: a relay is either
    profile/manual-controlled (`PROFILE`/`MANUAL` owner, per the ownership
    decision above) or rule-driven (`RULE` owner) — never both. Avoids a
    second, different ownership fight on top of the one just settled.
  - Safety: the rule evaluator is just another caller of
    `relay_authority_on_blocked()` (`App/drivers/relay_authority.c`) — a
    rule proposing ON gets refused exactly like a manual command or a
    profile step during a fault. No separate path, no special case.
  - Evaluation cadence: same control tick as the profile executor (section
    6) reads temperatures on, since that's already the task with current
    readings in hand — a separate rule-polling task would just be a second,
    possibly-inconsistent view of the same data.
- [x] **Historical data for the graph.** How much temperature history does
      the ESP32-S3 keep in RAM/flash for the "actual vs. desired" overlay,
      at what sample rate, and is it discarded on reboot or persisted
      per-firing (useful for a fired-piece log, costs flash wear).
      **Settled (2026-08-10, design only)**. **Built as designed
      (2026-08-11)** — see TODO.md 6A.9's history-ring-buffer bullet and
      `docs/PROJECT_STATUS.md` for what actually landed:
  - **v1 is RAM-only, discarded on reboot.** A persisted per-firing log is
    explicitly deferred, not rejected — it needs the LittleFS decision
    (section 0's Storage item) actually exercised first, and flash wear from
    an unbounded number of firings is its own budget question that
    shouldn't block the graph existing at all.
  - **Sample rate: one sample per 30s.** A firing runs hours, and the graph
    is a trend line, not a scope trace — 30s resolution is invisible at that
    zoom level while keeping the buffer small. Faster live values (if the
    dashboard wants a live numeric readout, section 2) come from the
    existing auto-report push, not this buffer.
  - **Sizing: a fixed-length ring buffer sized for a 24-hour firing at that
    rate** (2880 samples), each entry `{uint32 elapsed_s; float actual_c;
    float desired_c}` per zone in use (≤3 zones on this board) — roughly
    2880 × 12 bytes × 3 zones ≈ **~100 KB worst case** (fewer zones or a
    shorter firing costs less). That's a real but affordable slice of the
    ESP32-S3's SRAM alongside the Wi-Fi/lwIP/httpd stack already running;
    revisit the rate or switch to a downsampling scheme only if a real
    firing profile turns out to run past 24 hours or memory pressure shows
    up once the thermocouple/relay tasks are actually competing for it.
    Oldest samples overwrite once full — this is "this firing's trend,"
    not indefinite history, matching the RAM-only decision above.
  - **Correction (2026-08-12): "a real but affordable slice" was wrong.**
    Built as a single zone's buffer it was still 57.6 KB, and together with
    the autotune trace's 69 KB it left the board booting with 7 KB of free
    heap — httpd could not start and the Wi-Fi driver could not hold a
    client. This is exactly the "memory pressure shows up" case this bullet
    hedged about, and it showed up at one zone, not three. Resolved by
    packing each sample to 8 bytes (23 KB for the same 24 h at 30 s) rather
    than by shortening the firing or downsampling — the graph is unchanged.
    Per-zone history would still be 3× that and remains unbuilt.

## 0.5 Page organization

Requested explicitly: organize the pages/settings in a meaningful way rather
than as a flat list. Proposed grouping, to revisit once section 0's
architecture decisions are settled:

- **Dashboard** (section 2) — the default/home page: live status, graph,
  run controls, profile picker. Nothing configured here, only operated.
- **Profiles** (section 5) — create/edit/list firing profiles. Reads
  per-zone feasibility limits from Settings → Thermocouples & Zones but
  doesn't own that data.
- **Settings**, split into sub-pages rather than one long page:
  - **Thermocouples & Zones** (section 3) — channel count in use,
    calibration, zone naming/assignment, **and PID tuning per zone/thermocouple
    (section 3's PID item) lives on this same page** — settled, per explicit
    request, not a separate page/tab.
  - **Relays & Rules** (section 3's relay/alternate-function item) — relay
    count in use, zone assignment, alternate-function rule editor.
  - **Network** (section 4) — Wi-Fi mode, credentials, local-only toggle.
- Global nav should make the safety-relevant pages (Dashboard's run
  controls, and anything that can command a relay) reachable in the fewest
  taps, per the "phone screen" constraint — this is a mobile control
  surface for hitting Stop quickly, not a desktop admin panel.

**2026-08-18, added to plan (not yet built): four more pages worth having,
web + LCD both.** Surveyed what an operator or debugger reaches for that
isn't covered by Dashboard/Profiles/Settings/Network today:

- **Safety / Alarm page.** Current trip state (armed/tripped/degraded, which
  guard from `SAFETY_MODEL.md` §4 fired), a scrollable trip-event history,
  and the explicit "clear trip" action M4 requires (trips latch, clearing is
  a deliberate command — never folded into Dashboard's Stop button). Backing
  data is the `TRIP_EVENT` frame from `LINK_PROTOCOL.md` sec 6, which M5
  hasn't shipped yet — **blocked on M5**, page only worth building once
  trip events actually arrive over the link rather than being another stub.
- **Diagnostics / System info page.** Both processors' firmware versions
  (`GET_FW_VERSION`/`ANNOUNCE_VERSION`, sec 4/6), safety-link stats
  (`safety_get_link_stats`-equivalent: uptime, RX/TX counts, last-seen age),
  ESP heap/flash-free, IC temperatures (section 10.7). One place to look
  before reaching for a serial console — most of the data already exists
  from M0/M1 tooling, just not surfaced to the operator. Partly blocked on
  M5 for the link-stats half; the ESP-only half (heap/flash/IC temps) is
  buildable now.
- **Manual zone control page.** Section 2/10.3's "Temperature" nav item is
  currently a stub. Give it real per-zone content: current reading, manual
  setpoint override (bypassing the profile, for e.g. drying/venting a kiln
  without running a full program), and the zone's calibration offset —
  same data `Settings → Thermocouples & Zones` edits, this is the
  operate-time view of it rather than the configure-time one.
- **Backup / restore page.** Export saved profiles + zone/relay/network
  config as one downloadable blob (web) / to a file over the debug link
  (LCD is display-only for this, no removable storage), and re-import it —
  useful before an OTA update (section 9) and for cloning settings across
  more than one kiln. Depends on nothing else in this plan; buildable
  whenever picked up.

## 1. Wi-Fi provisioning and resilience — DONE, verified on hardware (2026-08-10)

Implemented in `App/drivers/wifi_prov.{c,h}` and
`App/drivers/wifi_provision_http.{c,h}` + `wifi_provision_page.html`, wired
into `App/main.c`. Built, flashed to the real ESP32-S3 board, and verified
live: AP fallback broadcasts (`kilnCtl`, WPA2-Personal), provisioning page
and `/status` serve correctly, oversized/malformed POSTs get a clean 400
with the server staying up, `local_only` persists across a full reflash, and
submitting real credentials joined an actual home network (fallback AP
dropped off the air within seconds of the join, confirming success). One bug
found and fixed during testing: local-only initially had no exit path via
the HTTP API (see `wifi_prov_set_credentials` / `wifi_prov_set_local_only`).

**Redesigned 2026-08-11 — single mode toggle, editable AP identity.** The
user reviewed the live page and found the three-flag model (has station
credentials? / `local_only`? / AP-fallback-on-join-failure?) confusing:
"local only mode as I see it is for when it is not connected to the home
wifi. there should be only one toggle that switches between home wifi and
access point mode." Replaced `local_only` (bool) with an explicit
`wifi_prov_mode_t { WIFI_PROV_MODE_HOME, WIFI_PROV_MODE_AP }`, persisted in
NVS key `mode` (u8, migrated read-only from the old `local_only` key on
first boot after this update — see `wifi_prov.c`'s `nvs_load()` — so a
device with real credentials already saved on the bench doesn't need
re-provisioning). Also made the fallback AP's own SSID runtime-editable
(`wifi_prov_set_ap_ssid()`, NVS keys `ap_ssid`/`has_ap_ssid`), matching the
AP password override that already existed
(`wifi_prov_set_ap_password()`/`ap_pass`/`has_ap_pass`) — previously only the
password could be changed without a rebuild, which didn't match the user's
ask that AP mode expose "network ssid and password" as one coherent pair.
`GET /status` now reports `mode` (`"home"`/`"ap"`) and `ap_ssid` alongside
the existing `state`/`ssid`/`sta_connected`/`sta_ip`; `POST /provision`
gained a `mode` field (`home`/`ap`, replacing `local_only=0`/`1`) and an
`ap_ssid` field (alongside the existing `ap_password`, both applied together
in one POST when present). `wifi_provision_page.html` now renders one
toggle with the credential fields relevant to whichever mode is selected
underneath it, instead of a separate "Local-only mode" section. The "don't
strand the phone" guarantee (AP+STA together until a home-mode join is
confirmed, AP-fallback timer brings the AP back if it doesn't land) is
unchanged — only the flag naming and the page's presentation of it changed,
not the state machine's actual join/fallback behavior. `pc_tools`'s Tkinter
GUI (`gui.py`'s Wi-Fi settings popup) updated to match: the local-only
checkbox became a home/AP radio pair, and the AP-password-only field grew an
AP-SSID field alongside it.

- [x] `esp_netif` + `esp_wifi` bring-up in `App/main.c`, alongside — not
      instead of — the existing UART/safety-link bring-up.
- [x] New Kconfig menu (`App/drivers/Kconfig` or a new `App/wifi/Kconfig`):
      `KILNCTL_WIFI_AP_SSID`, **`KILNCTL_WIFI_AP_DEFAULT_PASSWORD`** (per
      request — a compile-time default so a fresh board is usable without
      an existing network), AP channel, and whatever provisioning-mode
      constant is needed.
- [x] **AP fallback mode**: if no station credentials are configured, or
      the configured network can't be joined within a timeout, the device
      starts (or falls back to) its own AP so a phone can connect directly
      and provision it. This must coexist with, not block, everything else
      already running (UART link, thermocouple reads, relay control).
- [x] **"Local only" mode**: an explicit user choice (persisted, not just
      "no credentials yet") to run AP-only permanently and never attempt
      station mode. When this is set, the firmware must **never** prompt
      for or attempt to join a network — distinct from the fallback case
      above, which is "no network configured yet."
- [x] Simple provisioning page served from the AP: enter SSID + password,
      device attempts to join, reports success/failure back to the same
      page rather than dropping the AP before it's confirmed working
      (don't strand the phone with no way back in if the join fails).
- [x] **Hard requirement, explicit from the user**: losing Wi-Fi must never
      stop the control loop, relay safety behavior, or the UART/safety
      link — Wi-Fi is a client of the kiln, not a dependency of it. Model
      this the same way the existing `SAFETY_FAULT_SRC_PC_LINK` treats a
      lost UART link: detected and reported, never fatal.
- [x] **Hard requirement, explicit from the user**: a new HTTP/WebSocket
      connection, a malformed request, or a client disconnecting must never
      crash or restart the firmware. Every handler needs the same
      untrusted-input discipline already applied to the UART parsers (see
      the hardening pass referenced in `docs/PROJECT_STATUS.md`) — bounds
      checks, no unbounded allocation from a request, no assumption a
      client is well-behaved.
      Verified: oversized body and missing-field POSTs both return a clean
      400 and the httpd server stays responsive afterward.
- [x] Decide and document interaction with the existing link-loss watchdog:
      Wi-Fi loss should NOT trip `SAFETY_FAULT_SRC_PC_LINK` (that's the
      UART link specifically) — needs its own fault source or explicit
      non-fault treatment, consistent with "Wi-Fi loss is not fatal."
      **Settled: explicit non-fault.** No new `SAFETY_FAULT_SRC_*` bit.
      `wifi_prov.c` never touches `kiln_io`, the relay-authority gate, or
      `safety_link` — Wi-Fi state is only ever reported over the existing
      log link. The RP2040 safety-processor firmware (not in this repo)
      doesn't need to know about a link whose entire purpose is "may or may
      not exist."

## 2. Web UI — Main / Dashboard page

Live thermocouple/relay status and manual relay control are DONE and
hardware-verified (2026-08-10) — see `App/drivers/dashboard_http.{c,h}`,
wired into `main_page.html` at `/`. Profile controls and the graph were
deferred at first (no profile-execution engine existed yet), then built
once section 6/6A landed — see the checklist below, reconciled 2026-08-13.
`GET /api/status` and `POST /api/relay` are the new endpoints;
`POST /api/relay` is the second real caller of `relay_authority_on_blocked()`
(`App/drivers/relay_authority.c`), alongside the UART bridge.

Found and fixed during hardware testing: `MAX31856BusClass.initialized`
means "the shared SPI bus came up," not "a thermocouple answered" — with no
daughterboard attached this reported `thermo_ready:true` with an empty
channel list instead of "not attached." Fixed by deriving readiness from
`MAX31856_read_all()`'s returned count instead.

**Verified live (2026-08-11), all 5 pages, hardware-absent stub behavior**:
with the LCD, thermocouple daughterboard, and relay expander all physically
removed from the board (only the bare ESP32-S3, Wi-Fi, and the safety-link
opto-isolator present) and the device joined to a real home network over
Wi-Fi, every page route returned HTTP 200: `/` (dashboard), `/profiles`,
`/settings/zones`, `/settings/relays`, `/wifi`. `GET /api/status` correctly
reported `{"io_ready":false,"thermo_ready":false,"safety_ready":true}`;
`POST /api/relay` correctly refused with 400 rather than crashing;
`GET /api/rules` returned the expected DSL text; `GET /api/profiles`
returned `[]`. Nav between all pages confirmed present and consistent
(`main_page.html` links to all 4 sub-pages; each links back to `/`). This is
the "browsable even with HW absent" stub behavior the project explicitly
wants, confirmed working across the whole page set, not just the dashboard.

- [x] Live thermocouple status per channel (temperature, cold-junction,
      fault state, using the same fault decoding as `docs/MAX31856.md`).
      Verified live: reports `thermo_ready:false` correctly with no
      daughterboard attached (see the bug/fix note above); per-channel
      fault-bit decoding done in `main_page.html`'s JS, mirroring
      `THERMO_FAULT_*` (`App/drivers/uart_task_ids.h`).
- [x] Live relay status per relay (on/off — the "which named zone" part
      needs zones, section 3, not yet built).
      Verified live: reports `io_ready:false` correctly with no
      expander/relay board attached; `POST /api/relay` correctly refuses
      with 400 when `io` is absent rather than crashing.
- [x] Temperature graph: desired profile curve overlaid with actual
      measured temperature over time, updating live while a profile runs.
      **Built (2026-08-11, see 6A.9)**: canvas `historyChart` on
      `main_page.html`, fed by `GET /api/history.csv`, redrawn every 15s;
      guard trips marked on the timeline. Reconciled here 2026-08-13 — this
      bullet was left unchecked after 6A.9 shipped the same thing.
- [x] Start / Stop / Pause controls for the running profile.
      **Built**: `main_page.html`'s `runBtn`/`pauseBtn`/`stopBtn`, driving
      `profile_executor_run()`/`_pause()`/`_resume()`/`_halt()` (section 6).
- [x] List of saved profiles with a way to select one to run.
      **Built**: `main_page.html` fetches `GET /api/profiles` to populate
      the run picker (section 5's `profiles_http.c` is the backing store).
- [ ] All of the above driven live (WebSocket or SSE push), not polled —
      consistent with how the existing PC GUI already gets live device
      data from auto-report rather than polling (see `docs/UART_PROTOCOL.md`).
      **Partially settled, not fully done**: the live status/relay-control
      built above uses 2s polling, not push — `CONFIG_HTTPD_WS_SUPPORT` is
      off in `sdkconfig` and enabling it wasn't justified for this
      increment. Matches the polling pattern `wifi_provision_page.html`/
      `main_page.html` already established for Wi-Fi status. Revisit
      (WebSocket, or SSE via `httpd_resp_send_chunk`) if 2s polling proves
      too coarse once real hardware is attached and actually being watched
      during a firing.

## 3. Web UI — Settings page

Most of this section is now built, on `/settings/zones`
(`App/drivers/zones_http.{c,h}` + `zones_page.html`) and `/settings/relays`
(`App/drivers/rules_http.{c,h}` + `rules_page.html`), and hardware-verified
as reachable/functional (empty-but-structurally-correct) with no
thermocouple/relay hardware attached — see the note under section 2. Config
storage/validation is done; two things explicitly remain unbuilt because
they need a consumer that doesn't exist yet (live calibration application,
rule evaluation) — called out below rather than checked off.

- [x] Thermocouple calibration: per-channel offset (and possibly gain),
      stored persistently, applied on top of `MAX31856_read()`'s raw value
      — decide whether calibration lives in firmware (applied before the
      value ever reaches a client) or is a display-layer adjustment; doing
      it in firmware means every consumer (web UI, PC GUI, MCP) sees the
      same corrected number.
      **Storage done (2026-08-11), applied to reads since (reconciled here
      2026-08-13, then closed the one remaining gap same day)**:
      `zones_config_apply_cal()` (`zones_http.h`/`.c`) is called from
      `dashboard_http.c` (dashboard display), `profile_executor.c` (control
      math, guards see raw per 6A.3's explicit ordering — only the
      corrected value feeds PID/bang-bang), `autotune_engine.c` (fit/
      baseline math), `readiness_http.c` (the calibration-entered wizard
      item), and — as of this pass — `uart_bridge.c`'s THERMO READ payload,
      the one documented holdout (pc_tools/MCP consumers now see the same
      corrected number as the web UI). Offset only (no gain term — none was
      requested). `uart_bridge_ext.c`'s CONTROL response separately exposes
      `cal_offset_c` itself (the config value, not a reading) so a UART
      client can also inspect what offset is in effect. **Flashed and
      live-verified (2026-08-13)**: board reflashed over OpenOCD/JTOG,
      `thermo_read()` over UART reports `CH0..2: invalid [SPI read failed]`
      cleanly (no crash, no hang) with no daughterboard attached — the
      expected bare-board behaviour, exercising the new
      `zones_config_apply_cal()` call on the failed-SPI-read path (NaN
      passes through unchanged, per its documented contract) without a
      thermocouple present to actually offset. `safety_get_status()`/
      `io_read()` also answered as expected for this hardware config (TC
      invalid, IO task not registered — no expander attached). A real
      offset-applied number still needs the thermocouple daughterboard.
- [x] Configure how many thermocouples and relays are actually in use
      (the board supports up to 3 thermocouples / 4 relays; a smaller kiln
      may use fewer) — this is metadata that gates which channels the UI
      shows and which the profile executor is allowed to touch.
      **Implemented (2026-08-11)**: `zones_cfg_t.thermo_count`/
      `relay_count` (`App/drivers/zones_http.c`), validated against
      `MAX31856_CHANNEL_COUNT`/`KILN_IO_RELAY_COUNT`, served/edited via
      `GET`/`POST /api/zones`. Live-curled with no hardware attached and
      returned a structurally correct, zeroed response.
- [x] Named zones: assign a thermocouple and one or more relays to a
      named zone (e.g. "top", "bottom"), so the dashboard and profile
      pages can speak in zone names instead of raw channel/relay numbers.
      **Implemented (2026-08-11)**: one zone per configured thermocouple
      channel (zone *i* ↔ channel *i*, per `zones_http.h`'s scope note —
      TODO.md never asked for many-to-one), each with a `name` and a
      `relay_mask` bitfield ("bit N-1 = relay N belongs to this zone").
- [x] Relay alternate-function configuration: assign a relay to a rule
      (see the rule-engine design item in section 0) instead of / in
      addition to direct profile control — e.g. a vent relay triggered by
      a temperature threshold rather than the firing schedule.
      **Implemented, config-only (2026-08-11)**: `/settings/relays`
      (`App/drivers/rules_http.c`) is a hand-typed DSL editor
      (`RELAY n DRIVEN 0|1`, `R <rule> TEMP|TIME|RELAY ...`) matching
      section 0's composition rules (OR across rules, AND within one),
      parsed/validated and persisted to NVS. Checked off because the
      *configuration* surface exists; the rule *evaluator* does not
      (section 6, still unbuilt) — `rules_http.h`'s own comment and a
      user-visible notice on the page both say saved rules are inert until
      then, so this doesn't read as working automation.
- [x] Persist all of the above (NVS/LittleFS, per the storage decision).
      **Implemented (2026-08-11)**: NVS namespace `kiln_cfg`, keys
      `zones_cfg` and `rules_cfg` (`zones_http.c`/`rules_http.c`), same
      load-tolerant-of-"not found" convention as `wifi_prov.c`.
- [x] **PID tuning per zone/thermocouple, available on this same
      Thermocouples & Zones page** (settled per explicit request — not a
      separate Settings sub-page). Parameters (Kp,
      Ki, Kd, and whatever anti-windup/output-limit knobs the bang-bang-vs-PID
      decision in section 6 ends up needing), persisted alongside the rest
      of the zone config.
      **Implemented (2026-08-11)**: `pid_kp`/`pid_ki`/`pid_kd` fields on
      `zone_cfg_t`, editable inline on `zones_page.html` alongside name/
      calibration/ramp ceiling, same NVS blob. No anti-windup/output-limit
      knobs added yet — none were needed since the PID loop itself
      (section 6) doesn't exist to consume these values.
  - [x] These tuning values must feed the **feasibility check** on the
        profile page (section 5): the max achievable ramp rate is a
        function of the kiln's actual heating capability, which the PID
        loop's tuning and observed behavior are the only on-device source
        of truth for — not a hardcoded number.
        **Implemented (2026-08-11)**: `profiles_http.c`'s profile-save
        path calls `zones_config_get_max_ramp()` (`zones_http.c`) per
        segment and rejects the submission if a segment's ramp rate
        exceeds the zone's ceiling; warns (doesn't block) inside a 20%
        margin — see the next item.
  - [x] Decide how "max ramp rate" is derived: a user-entered ceiling per
        zone (simple, but just re-asks the user for a number they don't
        necessarily know), vs. something estimated from PID tuning and/or
        observed heating performance (more useful, more work, needs a
        design of its own before it's buildable).
        **Settled (2026-08-11): user-entered ceiling.** `max_ramp_c_per_hr`
        on `zone_cfg_t`, 0 meaning "never configured." Per `zones_http.h`'s
        header comment, the PID-estimated alternative is explicitly left
        undesigned ("more work, needs a design of its own before it's
        buildable") — the simple option was built, the harder one
        deliberately deferred, not forgotten.

## 4. Web UI — Network settings page — DONE (2026-08-10), same page as section 1

Rather than a separate page, the section 1 provisioning page
(`wifi_provision_page.html`) now covers this too — `esp_http_server`
listens on every netif, so it's reachable both from the fallback AP and
from the station link once joined, same content either way. Section 0.5's
page-organization note ("Network — Wi-Fi mode, credentials, local-only
toggle") is satisfied by this one page rather than a distinct
first-boot-vs-settings split; revisit only if the dashboard/settings nav
(sections 2/3) ends up wanting a dedicated route for it.

- [x] Choose Wi-Fi network: scan, select SSID, enter password (station
      mode) — this is the same underlying provisioning as section 1's AP
      page, but reachable once already connected/local, not just at
      first-boot.
      `GET /scan` (`wifi_prov_scan()` in `wifi_prov.c`) does a blocking
      active scan and returns SSID/RSSI/open-or-secure JSON; the page lists
      results tap-to-fill. Refused with 400 while local-only is set (scanning
      still means bringing the STA radio up, which local-only's contract
      says never happens) — this is a deliberate scope limit, not a bug.
- [x] Explicit "local connection only" toggle (the persisted choice from
      section 1) — selecting it stops the firmware from ever prompting for
      or attempting a network join when a connection is missing.
      `POST /provision` with `local_only=1`/`local_only=0` both handled
      explicitly now (`wifi_prov_set_local_only()`); disabling it with saved
      credentials present resumes the join immediately rather than sitting
      idle until new credentials are submitted.
      **Superseded 2026-08-11**: folded into the single mode toggle below —
      see section 1's 2026-08-11 note. `local_only=1` became `mode=ap`,
      `local_only=0` became `mode=home`; the "resume join immediately"
      behavior is unchanged, just reached via `wifi_prov_set_mode()` instead
      of `wifi_prov_set_local_only()`.
- [x] Show current connection state (AP-only / station-connected /
      station-configured-but-unreachable) so the user isn't guessing.
      `GET /status` now reports `state`, `ssid`, `local_only`,
      `sta_connected`, and `sta_ip`; the page renders a plain-language
      description (including the station IP once connected) and keeps
      polling while a join is in flight.
      **Updated 2026-08-11**: `local_only` replaced by `mode`
      (`"home"`/`"ap"`) in the JSON, and `ap_ssid` (the fallback AP's own,
      now runtime-editable, SSID) was added — see section 1's 2026-08-11
      note. `state` and its five values (`ap`/`unprovisioned`/`connecting`/
      `connected`/`reconnecting`) are unchanged except `local_only` was
      renamed to `ap` to match the mode value's own vocabulary.

## 5. Web UI — Fire profile creation page

**Backend and page built, undiscovered until this pass** — `profiles_http.c`/
`.h` + `profiles_page.html` implement every bullet below; the checklist was
just never updated when it landed (section 3/6 notes already referenced this
module in passing). Reconciled 2026-08-13, no new code.

- [x] Profile editor: named profile, ordered list of segments, each with
      target temperature, ramp rate (°/hour or °/min — decide units and
      whether it's configurable), and dwell/soak time.
      `profile_t`/`profile_segment_t` (`profiles_http.h`): `name` (15 char
      max), up to `PROFILE_MAX_SEGMENTS` (12) segments of
      `{target_c, ramp_c_per_hr, dwell_min}`, °C/hour fixed (not
      configurable — no request for °/min surfaced). `zone_mask` (not a
      single `zone_index`, per 6A.5's multi-zone update) selects which
      zone(s) the profile drives.
- [x] Save / load / delete profiles (persisted per the storage decision).
      `profiles_http_save()`/`profiles_http_get()`/`profiles_http_delete()`,
      8 slots, NVS-backed (`profiles_nvs` partition per section 8.1),
      versioned per section 8.2. `POST`/`GET`/`DELETE` on the profile CRUD
      API, same handlers shared with the UART CONTROL bridge
      (`uart_bridge_ext.c`) so both transports validate identically.
- [x] Validate profile parameters at creation time (ramp rate and target
      within sane/safe bounds — tie into the safety model rather than
      trusting client input, same principle as every other untrusted-input
      boundary in this project).
      `PROFILE_TARGET_C_MIN/MAX` (0–1400°C), `PROFILE_RAMP_C_PER_HR_MIN/MAX`
      (0–1000°C/hr), `PROFILE_DWELL_MIN_MAX` (24h) — explicitly documented
      as firmware sanity bounds, not a real kiln safety ceiling (no
      per-kiln-model authority exists to source one from).
- [x] **Feasibility check against each zone's actual capability** (derived
      from the PID tuning on the settings page, per section 3):
  - [x] A requested ramp-up or ramp-down rate the kiln cannot physically
        achieve must be flagged as infeasible before the profile can be
        saved/run, not discovered mid-firing as the real temperature falls
        behind the desired curve.
        Save path rejects any segment whose `ramp_c_per_hr` exceeds
        `zones_config_get_max_ramp()` for its zone.
  - [x] **Warn (not block) at 20% margin** — a segment within 20% of the
        zone's estimated ramp-rate ceiling gets a visible warning, distinct
        from the hard error for something outright impossible.
        `PROFILE_RAMP_WARN_FRACTION` (0.8) — save succeeds but
        `out_warning_count` reports how many segments crossed it.
  - [x] This check has to run both at profile-creation time (client-side
        preview is fine for UX, but the authoritative check must be
        on-device against the real per-zone ceiling) and probably again at
        profile-*start* time, since a profile could be created for one
        kiln/zone configuration and later run after settings changed.
        On-device check runs at save time (authoritative, per
        `profiles_http_save()` above); section 3's cross-reference confirms
        the same ceiling getter backs both. **Not reverified at
        profile-*start* time** — a zone's ceiling lowered after a profile
        was saved is not re-checked when that profile is later run. Left
        open, same gap as section 6A.7's "re-check `max_ramp_c_per_hr`
        against a running profile" bullet already tracks.

## 6. Firmware-side profile execution engine (implied by sections 2 and 5)

Not explicitly requested as a phrase, but required for "start/stop/pause" and
"the real temperature... in relation to the profile" to mean anything:

> **Section 6A below is the detailed design for everything in this section
> that touches temperature control**: the PID loop itself, the
> Klipper/Marlin-class thermal-protection suite (of which the
> direction/rate sanity monitor bullet below is one member), PID autotune
> for every zone, and the multi-zone interaction problem. Read 6A before
> implementing any bullet in this section — several of the items here are
> narrowed or superseded there, and 6A is where the concrete numbers,
> module boundaries, and failure cases live.

- [x] A task that steps through a selected profile's segments, computes the
      current target temperature (ramp interpolation / dwell hold), and
      drives the assigned zone's relay(s) toward it.
      **Implemented (2026-08-11)**: `App/drivers/profile_executor.c`'s
      1 Hz `executor_task_entry` tick, segment stepping and target-temp
      computation via `phase_start_target_c`/ramp math, relay drive via
      `apply_relay()`. Multi-zone concurrent execution (6A.5) is NOT
      built — one profile/one zone at a time, same as before this pass.
- [x] **Bang-bang PID control mode, selectable per zone in the Web UI**
      (per explicit request): the relay(s) assigned to a zone are driven
      on/off around the current target temperature using the zone's PID
      tuning (section 3) with hysteresis, rather than (or as an option
      alongside, if a smoother PWM/duty-cycle mode is ever added) true
      proportional output. "Selected in the Web interface" implies this is
      a mode/parameter on the zone or profile, not a global firmware
      setting — needs a concrete decision on where that selection lives
      (per zone in Settings, per segment in a profile, or both).
      **Implemented (2026-08-11)**: settled as per-zone in Settings, not
      per-segment. `zone_cfg_t.control_mode` (`OFF`/`BANGBANG`/`PID`,
      `App/drivers/zones_http.c`), dropdown on `zones_page.html`, read
      once at run-start by `profile_executor_run()` and dispatched each
      tick to `heater_output_bangbang` (fixed 2C hysteresis) or
      `pid_update` -> `heater_output_duty` (see `App/drivers/pid.c`,
      `App/drivers/heater_output.c`). Live-verified in BANGBANG mode only
      (no PID-mode hardware run performed). **Live-reloadable since
      2026-08-12** (6A.7): a mode change mid-run forces that zone's relays
      off, adopts the new mode, restarts the controller cold, and logs the
      switch at WARN.
- [x] **Direction/rate sanity monitor, per zone, running whenever that
      zone's heat is commanded on** (per explicit request — this is a
      *safety* check, not a control-quality one): while a relay is being
      driven to heat (or, symmetrically, while none are driven and the
      zone should be cooling), confirm the measured temperature is moving
      in the direction commanded, at a plausible rate. The explicit intent
      given: **the purpose is to catch a disconnected or broken
      thermocouple**, not to detect underpowered heating elements per se.
      **Implemented (2026-08-11)**: this is thermal_guard.c's guards 1
      (heating-failed/no-progress) and 2 (wrong-direction) — see 6A.3
      below for the detailed per-check status. Only guards 1/2/6/7 have
      actually been exercised (guard 6, live-tested end to end); 3/4/5/8/9
      are written but unexercised — see 6A.3/6A.8.
  - [x] Distinguish this from the ramp-rate feasibility check in section 5:
        feasibility is "can this profile be achieved at all" (a planning
        check, before/at profile start); this monitor is "is the
        thermocouple actually telling the truth right now" (a runtime
        check, continuous). A thermocouple reading a flat line while a
        relay is energized, or moving the *wrong* direction, is the
        signature to catch — an open/disconnected TC on this hardware
        reads a fault bit directly (`docs/MAX31856.md`,
        `THERMO_FAULT_OPEN`) which should also feed this, but a TC that's
        physically detached from the kiln body while still electrically
        connected won't set that bit and only this rate/direction check
        can catch it.
        **Implemented (2026-08-11)**: `thermal_guard.c` evaluates guards
        1/2 continuously against the raw reading (guards see raw, control
        sees calibrated, per `profile_executor.c`'s explicit ordering);
        the feasibility check in `profiles_http.c` remains the separate,
        planning-time gate — unchanged by this pass.
  - [x] "Reasonable rate ... I will determine later" — leave the
        rate threshold as a configurable parameter (Settings, probably
        alongside PID tuning per zone) rather than a hardcoded constant,
        since the user has explicitly deferred picking the number.
        **Implemented (2026-08-11)**: guard 1's rate uses the pre-existing
        `zone_cfg_t.sanity_rate_c_per_min` / `zones_config_get_sanity_rate()`
        (`App/drivers/zones_http.c`, was already on the settings page but
        had no consumer) — this pass is the first thing that actually
        reads it. Every *other* guard's threshold (wrong-dir rate/window,
        runaway rate/margin, drift period, sensor debounce count, frozen
        window) is still a firmware-wide constant in `thermal_guard.c`,
        not yet per-zone configurable — an honest gap, not fixed here.
  - [x] On trip: this is a safety fault, so it must flow into the same
        `SAFETY_FAULT_SRC_*` mechanism as everything else in
        `docs/SAFETY_MODEL.md` (new source, e.g. `SAFETY_FAULT_SRC_THERMAL_SANITY`,
        or folded into a broadened live `SAFETY_FAULT_SRC_THERMO` once that
        gap — already flagged as the top open item in SAFETY_MODEL.md — is
        implemented) so it blocks relay-on the same way every other fault
        does, not a separate ad hoc check that only the profile executor
        knows about.
        **Implemented (2026-08-11)**: `profile_executor.c`'s
        `escalate_guard_trip()` — guards 3/5/6 assert the global
        `SAFETY_FAULT_SRC_THERMAL_SANITY`/broadened `SAFETY_FAULT_SRC_THERMO`
        (guard 6 uses `THERMO`, per 6A.6's recommendation), which block
        `relay_authority_on_blocked()` everywhere; guards 1/2/4/7 only
        block this zone via the new `relay_authority_zone_blocked()`.
        Live-verified for guard 6 only (sensor-invalid trip, see report
        above); 1/2/4/7's escalation path is written but not yet exercised
        live for lack of a thermocouple to actually miss-track or drift.
- [x] Must call into the shared relay-authority chokepoint from section 0,
      not `kiln_io_set_relay` directly, so it is bound by the same
      safety-wins rule as every other caller.
      **Implemented (2026-08-11)**: `profile_executor.c`'s `apply_relay()`
      gates every ON command through the new
      `relay_authority_zone_blocked()` (`App/drivers/relay_authority.c`),
      itself layered on top of the pre-existing, unchanged
      `relay_authority_on_blocked()` global gate — never calls
      `kiln_io_set_relay*` directly without going through it first.
- [x] Pause must actually stop driving relays (fail toward off, matching
      the project's existing fail-safe convention), not just freeze the
      displayed progress.
      **Implemented**: `profile_executor.c`'s tick loop calls
      `force_relays_off()` (which calls `heater_output_force_off()` then
      `apply_relay(false)`) for any state other than `RUNNING`, which
      includes `PAUSED` — confirmed by reading the code path; pause/resume
      itself was not part of this pass's live hardware test (only the
      guard-6 trip and `profile_executor_halt()` were exercised on the
      real board).
- [x] Must be preemptable by every existing safety mechanism: link-loss
      watchdog, thermocouple fault (once section-0-adjacent live-fault
      gating from `docs/SAFETY_MODEL.md` is implemented — a running
      profile is the clearest case where that gap actually matters), the
      new direction/rate sanity monitor above, manual `SET_FAULT_OUT`, and
      the safety processor once it exists.
      **Implemented (2026-08-11)**: `relay_authority_zone_blocked()` is
      strictly additive on top of the existing global
      `relay_authority_on_blocked()`, so every pre-existing fault source
      (link-loss watchdog, boot-time `THERMO`, manual `SET_FAULT_OUT`,
      `SAFETY_FAULT_SRC_SAFETY_LINK`) already preempts the executor with
      no new code needed for those; the live thermocouple-fault gap this
      bullet calls out is now closed for guard 6 specifically (see above).
      Not verifiable end-to-end against the RP2040 safety-processor path
      since that firmware doesn't exist yet — a pre-existing, unrelated
      gap, not something this pass could close.

## 6A. PID control, thermal protection, autotune, and zone interaction

**Status: design only, nothing built.** Added 2026-08-11 on explicit request:
"PID robustly implemented with the same type of temperature-control safeties
that Klipper/Marlin 3D printers use, i.e. heat going in the correct direction
as expected", "PID autotune for all zones", and "it should also handle the
interaction of each other since they are not really separate, they will affect
each other."

**Status update (2026-08-11, later the same day): first slice built and
hardware-verified.** `pid.c`, `thermal_guard.c` (guards 1–7), and
`heater_output.c` now exist and are wired into a rewritten
`profile_executor.c` (6A.10's own suggested build order, items 1–3,
adapted to this codebase's existing single-zone-at-a-time executor --
**update (2026-08-11): this is no longer accurate, see below**). See the
checked-off items throughout this section for exactly what landed. Since
this section was first written, the host-side test harness (6A.8), the
step-test half of autotune (6A.4), the 6A.9 telemetry/UI slice (history
buffer, dashboard graph, `/api/control`, per-zone relay timing), and now
**concurrent multi-zone execution with ramp-lock (6A.5(a) and (d))**, and
**the cross-zone coupling-matrix capture during autotune (6A.5(b))** have
all landed — `profile_executor.c` was rewritten from single-zone to a
`zone_mask`-driven array of independent per-zone control/guard state
sharing one ramp/dwell schedule; see 6A.5's own bullets for what did and
didn't come with it. (b)'s matrix-fill code path exists and is
logic-verified but has never actually been exercised against a real
cross-gain — no thermocouple hardware is attached, so every live autotune
run so far aborts on guard 6 before reaching a fit. **Relay-feedback
autotune, everything downstream of a real filled matrix (RGA, static
decoupler, guard 8's proper threshold), gain scheduling, and electrical
load staggering are still entirely unbuilt** — this is still not the
finished design, just a substantially larger slice of it.

This section is deliberately long. Temperature control is the one part of this
firmware that can start a fire, and the failure modes it must survive (welded
relay contacts, an element that stopped heating, a thermocouple that fell out
of the kiln body but is still electrically fine) are exactly the ones a naive
PID loop cannot see. The rest of the project's discipline — one owner per
piece of state, one chokepoint for relay-on, refuse rather than guess — applies
here with no exceptions.

### 6A.0 What already exists to build on

| Piece | Where | State |
|---|---|---|
| Per-zone `pid_kp`/`pid_ki`/`pid_kd` | `zone_cfg_t`, `App/drivers/zones_http.c` | Stored in NVS, editable on `/settings/zones`, **consumed by nothing** |
| `zones_config_get_pid()` | `App/drivers/zones_http.h` | Getter written for a `profile_executor.c` that does not exist |
| `zones_config_get_relay_mask()` | same | ditto |
| `zones_config_get_sanity_rate()` | same | ditto; documents `PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN` as the fallback for an unset (0) value |
| `zones_config_get_max_ramp()` | same | user-entered ceiling, already enforced by `profiles_http.c`'s feasibility check |
| `profile_t` / `profile_segment_t` / `profiles_http_get()` | `App/drivers/profiles_http.h` | 8 slots, 12 segments, `{target_c, ramp_c_per_hr, dwell_min}`, one `zone_index` per profile |
| `relay_authority_on_blocked()` | `App/drivers/relay_authority.h` | The single relay-ON gate; **global, takes no zone/relay argument** — see 6A.6 |
| `SAFETY_FAULT_SRC_THERMAL_SANITY` (0x20) | `App/drivers/safety_link.h` | Bit already allocated and documented, never asserted by anything |
| `MAX31856Reading` | `App/drivers/MAX31856.h` | Already carries `fault_status`, `spi_failed`, `stale`, and NaN-on-invalid — every input the guards below need |
| History ring buffer (30 s samples, 24 h) | **Built (2026-08-11)**, single-zone not per-zone (6A.5 unbuilt) | `{elapsed_s, actual_c, desired_c, duty, guard}` in `profile_executor.c`; `GET /api/history.csv`, graphed on the dashboard — see 6A.9 |

Two facts from `docs/HARDWARE.md` that constrain the design: the relays are
**EE2-12NUH electromechanical** parts on 12 V coils (expander pin high =
energized), and each lands on a 3-pin terminal block (J3/J4/J8/J11). Every
relay command goes out over I2C to the SX1509, so a "duty cycle" here is a
slow time-proportioned window, not PWM.

- [ ] **Open question, blocks picking the duty-window period:** do these
      relays switch kiln elements directly, or do they drive external
      SSRs/contactors? An electromechanical relay switching an element has a
      *contact* life budget (order 10^5 operations) that a 10 s window would
      burn through in a few hundred hours of firing; driving an SSR's control
      input has essentially no such budget and allows a much shorter window
      and correspondingly finer control. Until answered, the design below
      assumes the pessimistic case (mechanical, long window) and makes the
      window a per-relay config value rather than a constant.

### 6A.1 Actuator model: time-proportioned output, not bang-bang

Section 6's existing bullet says "bang-bang PID control mode, selectable per
zone." Keeping that selectable is right, but bang-bang and PID are two
different control modes and the wording conflates them. Settle it as **three
per-zone modes**, chosen in Settings → Thermocouples & Zones:

1. `OFF` — zone is configured but never commands heat. Useful for a kiln with
    fewer zones than the board supports, and as the safe post-fault state.
2. `BANGBANG` — relay on below `setpoint - hysteresis`, off above
    `setpoint + hysteresis`. No PID math. Simple, robust, always available as
    a fallback if tuning is bad or autotune has never been run. Overshoot at
    high temperature is significant; this is the "it works today" mode.
3. `PID` — the loop in 6A.2, whose scalar output `u ∈ [0,1]` is rendered onto
    the relay by a **time-proportioning window**.

Time-proportioning rules (module `heater_output.{c,h}`):

- [x] Per-relay `window_ms` (default: 60 000 for a mechanical relay, 2 000 if
      the answer to the open question above is "drives an SSR"). The relay is
      on for `u * window_ms` at the start of each window, off for the rest.
      **Implemented (2026-08-11)**: `App/drivers/heater_output.c`,
      60 s default window, time-proportioned mode. Per-relay/per-zone
      override from the settings page is NOT built yet — see 6A.9.
- [x] Per-relay `min_on_ms` / `min_off_ms` (default 2 000). A computed on-time
      shorter than `min_on_ms` is rendered as **off for that window**, not as a
      minimum-length pulse — an unachievably short duty is genuinely closer to
      zero than to `min_on_ms/window_ms`, and rounding up is how a relay ends
      up chattering at 1 % demand.
      **Implemented (2026-08-11)**: `heater_output.c`, 2 s min-on/min-off,
      an unachievably-short on-time renders as OFF for the whole window per
      the anti-chatter rule above (not rounded up). Also implements the
      bang-bang mode's own min-on/min-off debounce for `BANGBANG` zones.
- [x] Duty quantization is therefore coarse (with a 60 s window and a 2 s
      minimum, the usable set is {0} ∪ [0.033, 1]). This is fine — a kiln's
      thermal time constant is minutes to tens of minutes — but the PID's
      derivative filter has to be slow enough (6A.2) not to react to the
      quantization itself.
      **Implemented (2026-08-11)**: the 30 s `d_filter_tau_s` low-pass in
      `App/drivers/pid.c` is the mitigation described here; sized against a
      60 s window per this bullet's own reasoning.
- [x] **Contact-cycle accounting**: count relay transitions per relay,
      persist the total to NVS periodically (not every transition — flash
      wear), expose it on the Relays page. A kiln controller that silently
      eats a relay's contact life is a controller that fails mid-firing at
      cone temperature. This also gives a data-backed answer to the window
      period question over time.
      **Done (2026-08-12)**: new `App/drivers/relay_cycles.{c,h}` owns the
      persisted per-relay totals — deliberately its own NVS key
      (`kiln_cfg`/`relay_cyc`) rather than a field added to the `zones_cfg`
      blob, whose loader treats any size change as corrupt and would have
      silently wiped every user's zone setup on the first boot after this
      update. `profile_executor.c` reports only the per-tick delta from
      `heater_output`'s RAM counter (a zone's relays switch as a group, so
      each relay in the mask takes the same count) and calls
      `relay_cycles_maybe_persist()`, which writes at most once per 10
      minutes and only when something changed; `profile_executor_halt()`
      forces a flush so a firing's wear survives a power-down straight
      afterward. Counts saturate rather than wrap — a wrapped contact-life
      counter reads as a brand-new relay, which is the one wrong answer that
      matters. Exposed as `relay_cycles` on `GET /api/status` and shown as a
      table on Settings → Relays & Rules, with the ~100k-operation contact
      budget explained and values past 80k highlighted.
      Build-verified and flashed; **not exercised** — no relay hardware, and
      no firing has run, so every count is still 0.
- [ ] **Window phase offsets across zones** — see 6A.5's load-staggering item;
      the offset belongs to this module even though the reason for it is
      interaction. Not built — 6A.5 (load staggering) is unbuilt.

### 6A.2 The PID loop itself

Module `App/drivers/pid.{c,h}`: **pure C, no FreeRTOS, no ESP-IDF, no
logging, no I/O.** State in a caller-owned struct, one `pid_update(state,
cfg, setpoint, measurement, dt_s)` returning `u ∈ [0,1]`. This constraint is
not stylistic — it is what makes the loop testable against a simulated kiln
on the host (6A.8), which is the only way to test the tuning and the guards
before there is a kiln to melt.

Form, and why each choice (these mirror what Marlin and Klipper converged on,
adjusted for a plant whose time constants are ~100x a hotend's):

- [x] **Positional PID with derivative on measurement**, not on error. A
      profile ramp steps the setpoint every tick; derivative-on-error turns
      each of those steps into a spike ("derivative kick"). Both Marlin and
      Klipper use derivative-on-measurement for the same reason.
      **Implemented (2026-08-11)**: `App/drivers/pid.c`, positional form,
      derivative-on-measurement.
- [x] **Low-pass the derivative** with a first-order filter, time constant
      `d_filter_tau_s` (default 30 s). Marlin's `PID_K1 = 0.95` at its loop
      rate is the same idea with a ~2 s constant; a kiln sampled at 1 Hz,
      with 0.0625 °C-resolution readings and a coarsely quantized actuator,
      needs a much longer one or D contributes pure noise.
      **Implemented (2026-08-11)**: `pid.c`, 30 s time constant.
- [x] **Anti-windup by conditional integration plus clamping**: (a) clamp the
      integral term so `Ki * I ∈ [0, 1]` (Marlin's `PID_INTEGRAL_DRIVE_MAX`
      equivalent, and Klipper clamps the integral to the output range the
      same way); (b) additionally freeze integration on any tick where the
      unsaturated output is already outside [0,1] *and* the error would push
      it further out. Both, not either — the clamp alone still lets I sit
      pinned at the ceiling through an entire ramp and then dump on arrival.
      **Implemented (2026-08-11)**: `pid.c`, conditional-integration +
      clamp anti-windup, matching both (a) and (b).
- [ ] **Output clamp [0,1]**. There is no active cooling: negative output is
      not merely clamped, it is *meaningless*, and the loop must not pretend
      otherwise. A setpoint falling faster than the kiln cools is an
      infeasible profile (section 5's check) or a job for a rule-driven vent
      (section 0's rule engine), never something the PID can fix. The
      controller should **report** "cannot follow, cooling-limited" rather
      than sit at u=0 looking healthy while the actual curve diverges.
      **Done (2026-08-13)**: `profile_exec_zone_status_t.cooling_limited`
      (`profile_executor.h`), true once a PID-mode zone's raw (pre-load-cap-
      boost) duty has sat at exactly 0 for
      `PROFILE_EXECUTOR_COOLING_LIMITED_HOLD_S` (60s) while still reading
      more than `PROFILE_EXECUTOR_COOLING_LIMITED_MARGIN_C` (2°C) above
      target — a debounced hold, not a raw instantaneous check, so a normal
      brief overshoot settling out doesn't nag. Cleared on any mode other
      than PID (there's no continuous u=0 to observe the same way in
      bang-bang/off). Exposed as `cooling_limited` in `/api/control`'s
      per-zone JSON (`dashboard_http.c`). Build-verified; not yet
      exercised live (needs a real ramp-down faster than the bare board's
      natural cooling, which needs either a real kiln or
      `KILNCTL_SIM_PLANT`, neither exercised this pass).
- [x] **Functional range / mode blending**: outside `pid_range_c` (default
      25 °C) from setpoint, run full-on (below) or full-off (above) instead of
      PID, and hold the integrator. This is Marlin's `PID_FUNCTIONAL_RANGE`
      and it exists precisely so that a 900 °C climb from cold doesn't spend
      an hour winding up the integrator. Re-entering the range must do a
      **bumpless transfer**: seed `I` so the loop's first output equals the
      output it is taking over from.
      **Implemented (2026-08-11)**: `pid.c`, `pid_range_c` default 25 °C,
      full-on/off blending outside it, integrator held; re-entry seeds via
      `pid_seed_bumpless()`.
- [x] **Bumpless transfer is required at every discontinuity**, not just that
      one: mode change, tuning change from the web UI mid-firing, profile
      resume after pause, autotune finishing. The rule: whenever the loop
      restarts, set `I = (u_desired - P - D) / Ki`.
      `pid_seed_bumpless()` (`pid.c`) is called for the functional-range
      re-entry above, for profile resume-after-pause
      (`profile_executor.c`'s `profile_executor_resume()`), for a
      **tuning change mid-firing** (since 2026-08-12, from
      `reload_zone_config()` on the 6A.7 config-reload path, seeded off the
      duty the zone last commanded), and — **reconciled 2026-08-13,
      already covered, not a separate call site** — for autotune finishing:
      `autotune_engine_accept()` writes new gains *and* the identified
      plant model through the same `zones_http` config `reload_zone_config()`
      already polls, so an autotune result landing mid-firing is
      indistinguishable from a manual tuning edit to that code path and
      gets the same `seed_bumpless_with_ff()` treatment
      (`profile_executor.c` lines ~802–852) — there was never a reason for
      a fifth, autotune-specific call site. A mode change deliberately does
      *not* use bumpless transfer: there is no state to carry between a
      duty fraction and a hysteresis latch, so that path forces the zone
      off and restarts the controller cold instead, by design (documented
      at the same call site) — the "every discontinuity" bar is met by
      every discontinuity that has a meaningful handover, which a mode
      change does not.
- [x] **Optional setpoint weighting (2-DOF PID)**, `b` on the proportional
      term (default 1.0). A kiln profile mostly *tracks a ramp* rather than
      *stepping to a target*, so `b = 1` is the right default; expose it only
      if a real firing shows overshoot at segment transitions.
      **Implemented (2026-08-11)**: `pid_cfg_t.b` in `pid.c`/`.h`, wired via
      `profile_executor.c`'s `PID_SETPOINT_WEIGHT_B` constant. Not exposed on
      the settings page, per this bullet's own "expose it only if" clause.
- [x] **Feedforward from the identified plant model** (default on once
      autotune has run, off before that):
      `u_ff = (T_sp - T_ambient) / K_dc + (dT_sp/dt) * tau / K_dc`,
      where `K_dc` (°C per unit duty, steady state) and `tau` come from the
      model 6A.4 fits. The first term supplies the duty the kiln needs just to
      *hold* the current setpoint; the second supplies the extra needed to
      *climb* at the commanded rate. With a plant whose dead time is tens of
      seconds, feedback alone always lags a ramp; feedforward is what makes
      the actual curve sit on the desired curve instead of a fixed offset
      below it. The PID then only corrects the model error. This is the
      single highest-value item in this section for control *quality*.
      **Built (2026-08-12)**, and it is the term this section called the
      highest-value item for control quality. `profile_executor.c` caches
      each zone's `{K_dc, tau}` at run start and refreshes it on the 6A.7
      config-reload path, so an autotune accepted mid-firing starts being
      used without restarting the run. `dT_sp/dt` comes from the segment's
      **commanded** ramp rather than a tick-to-tick difference of `target_c`:
      at 1 Hz a 100 °C/hr ramp moves the target ~0.03 °C, and dividing that
      by a jittering measured `dt_s` is mostly timing noise — which would
      then be multiplied by tau (hundreds of seconds) on its way to the duty.
      The climb term is zero while dwelling, while ramp-lock holds the target
      back, and on a step segment.
      Feedforward stays **exactly 0** with no identified model, with a
      non-finite or non-positive `K_dc`/`tau`, outside PID mode, and on a
      tick with no trustworthy reading — a kiln driven from a fabricated `K`
      is worse than one on feedback alone. Only the *sum* is clamped to
      [0,1], not each term: on a cooling ramp the climb term is legitimately
      negative and should reduce the hold duty. The value passed is what
      `pid_terms_t.ff` reports on `/api/control`, so the split is visible.
      **This broke bumpless transfer, which is worth recording**:
      `pid_seed_bumpless()` solves `integral = (u_desired - P)/Ki`, exact
      only while feedforward was always zero. With ff live the seed comes
      back one whole feedforward term high — and on a hot kiln ff is the
      largest term in the sum. Fixed at both call sites (the 6A.7 gain-edit
      reload and `profile_executor_resume()`) by seeding against
      `u_desired - u_ff`. A model appearing mid-run now also re-seeds, since
      ff going from 0 to most of the duty on top of an integral built to
      supply that same heat would peg the element until the integrator
      unwound. One real behaviour change: resume can no longer reproduce
      `u = 0` when ff > 0 — the integral floor is 0, so a resumed zone comes
      back at exactly its feedforward duty, which is the model's estimate of
      the hold cost with nothing accumulated on top.
      **Untested against a real kiln**: a wrong `K_dc` from a bad fit now
      reaches the duty directly, bounded only by the [0,1] clamp and the
      guards.
      **Prerequisite landed 2026-08-12**: the fitted model is now *persisted*
      per zone. Autotune fit `{K, tau, L}` and then threw it away; it now
      goes into `zone_cfg_t` (`model_k_dc`, `model_tau_s`,
      `model_dead_time_s`) through `zones_config_set_model()`, written at the
      same acceptance point the gains go through `zones_config_set_pid()`, so
      autotune still never touches NVS itself. Zero in any of the three means
      "no model identified" — the feedforward expression divides by `K` and
      needs a non-zero `tau`, and a zero dead time is not physically
      reachable on a kiln. Shown read-only on the zones page and echoed back
      in the whole-page submit, so an unrelated edit cannot silently wipe a
      model that took hours to measure. What remains is the executor-side
      arithmetic and the ambient reference below.
- [x] **Ambient reference for the feedforward term**: use the MAX31856's
      cold-junction reading (`cj_temperature_c`, already in every
      `MAX31856Reading`) at firing start, not a constant.
      **Built (2026-08-12)**: captured once in `profile_executor_run()`, from
      the same whole-bus read the ramp baseline already does — first channel
      with a non-failed, non-NaN CJ wins, since all the cold junctions are on
      one board and a single dead channel should not cost the run its
      ambient. Deliberately never re-sampled: the cold junction warms with
      the board through a firing, and re-reading it would drift the hold term
      downward over hours. With no valid CJ at start it falls back to 20 °C,
      logs at WARN, and leaves feedforward **on** — the error enters as
      `(delta_ambient)/K_dc`, so with a kiln's `K_dc` in the hundreds of
      °C-per-duty a 15 °C ambient error is under 2 % duty, cheaper than
      losing feedforward for the whole firing.
- [x] **All PID state is per zone**, in the executor's `zone_runtime_t`;
      `pid.c` itself owns no globals and no statics.
      **Implemented (2026-08-11)**: `pid.c` is pure (caller-owned
      `pid_state_t`, no globals/statics); `profile_executor.c` owns the
      single running zone's state (`s_exec`) — multi-zone `zone_runtime_t`
      array doesn't exist yet since only one zone runs at a time (6A.5
      unbuilt), but the per-zone ownership contract itself is satisfied.
- [x] Tick rate: **1 Hz** (see 6A.7). `dt_s` is passed in and is the *measured*
      elapsed time, not the nominal period — a tick delayed by a slow I2C
      transfer must not silently change the effective Ki/Kd.
      **Implemented (2026-08-11)**: `profile_executor.c`'s
      `executor_task_entry` ticks at `PROFILE_EXECUTOR_TICK_MS` (1 Hz) and
      passes measured `dt_ms` (from the actual tick-to-tick delta, not the
      nominal period) into `pid_update()`.

### 6A.3 Thermal protection — the Klipper/Marlin-class safety suite

This is the part the request emphasized. Everything below is a **safety**
check, not a control-quality metric: a trip drops heat and latches. Implement
in `App/drivers/thermal_guard.{c,h}` as a **pure function** of
`(setpoint, reading, commanded_duty, dt, guard_state)` returning a verdict,
for the same host-testability reason as `pid.c`. The executor owns the state
struct and acts on the verdict; the guard never touches a relay itself.

Per-zone state machine, since most checks only make sense in one phase:
`IDLE` → `APPROACHING` (setpoint far from actual, heat commanded) →
`AT_SETPOINT` (inside band) → `RAMPING` (setpoint moving) → `COOLING`
→ `TRIPPED`.

The checks, with their Klipper/Marlin ancestry named so the mapping is
auditable:

- [x] **1. Heating-failed / no-progress.**
      *Klipper `verify_heater` heating check (`heating_gain` 2 °C per
      `check_gain_time`); Marlin `WATCH_TEMP_PERIOD` / `WATCH_TEMP_INCREASE`.*
      While commanded duty ≥ `progress_duty_min` (default 0.5) and the zone is
      below setpoint by more than `hysteresis_c`, the reading must rise at
      least `sanity_rate_c_per_min * window` over a rolling
      `progress_window_s` (default 300 s — a kiln is not a hotend; 20 s
      windows are meaningless here). Uses the existing
      `zone_cfg_t.sanity_rate_c_per_min`, whose "0 means use the default, not
      disable" contract `zones_http.h` already documents. **This is the
      check the request's "heat going in the correct direction as expected"
      names, and the one that catches a thermocouple that fell out of the
      kiln body while staying electrically intact** — it reads near-ambient
      and flat while the elements are at full duty.
      **Implemented (2026-08-11)**: `App/drivers/thermal_guard.c`, guard 1.
      Written and consumed by `profile_executor.c`; not itself live-tested
      (no thermocouple attached to provoke it) — only guard 6 was exercised
      on the real board this pass.
- [x] **2. Wrong-direction.** Duty ≥ `progress_duty_min` and the reading is
      *falling* by more than `wrong_dir_rate_c_per_min` (default 1.0)
      sustained over `wrong_dir_window_s` (default 120 s). Distinct from
      check 1 because the causes differ: this catches a thermocouple wired to
      the wrong zone, two channels swapped at the connector, or a relay
      mapping error — cases where the loop will happily drive to full output
      forever and make things *worse*. A miswired kiln must fail on its first
      firing, not slowly cook.
      **Implemented (2026-08-11)**: `thermal_guard.c`, guard 2.
      **Tested on-target 2026-08-12 and NOT the guard that fired.** A
      simulated connector swap (`POST /api/sim swap=0,1`, added for this) was
      caught by **guard 1 at 478 s**: zone 0's element climbed to 33.3 °C
      while zone 0 read zone 1's untouched 20.1 °C sensor — flat under
      commanded heat is guard 1's signature. Guard 2 requires the reading to
      be *above setpoint and falling* while heat is commanded, which a PID
      loop makes rare by construction (above setpoint it commands zero duty).
      So this bullet's own framing needs correcting: "two channels swapped at
      the connector" is caught by guard 1, and guard 2 is a narrower backstop
      for a forced-duty case. Guard 2 has still never been observed firing.
- [x] **3. Runaway with heat off (welded contact / shorted SSR).** Duty = 0
      (and has been for `off_settle_s`, default 120 s) but the reading is
      *rising* by more than `runaway_rate_c_per_min` (default 1.0), or has
      risen more than `runaway_margin_c` (default 20 °C) above the highest
      setpoint since the fault window opened. **Neither Marlin nor Klipper
      covers this case well, and on a kiln it is the most dangerous single
      failure**: a mechanical relay that welds shut leaves an element at full
      power with the firmware convinced it commanded off. The response is not
      just "drop the relay" (which is exactly what has already failed) — it
      must assert the isolated fault line so the RP2040 and its K4 relay can
      act, which is the whole reason that hardware exists
      (`docs/SAFETY_LINK.md`).
      **Implemented (2026-08-11)**: `thermal_guard.c`, guard 3, escalates to
      the global `SAFETY_FAULT_SRC_THERMAL_SANITY` via
      `profile_executor.c`'s `escalate_guard_trip()`.
      **Live-tested on hardware against the simulated plant (2026-08-12)**:
      `POST /api/sim {zone:0, fault:relay_welded}` at a satisfied setpoint
      tripped it at 120 s. That run also **found a real false-positive bug**:
      the rise was measured from the start of the off-window but divided by
      the time since the *settle* window ended, so the first tick past the
      window reported a 4.3 °C drift as "257.74 °C/min" — meaning any zone
      idling at duty 0 and drifting up at all (a dwell, a neighbour's heat,
      ordinary coasting) would trip a welded-contact fault. Fixed by latching
      a separate rate baseline when the settle window ends, with the 20 °C
      margin check still referencing the original baseline; two host
      regression tests added. See `docs/PROJECT_STATUS.md`'s 2026-08-12
      guard-walk entry.
- [x] **4. Drift at setpoint.**
      *Klipper `verify_heater` maintenance check (`hysteresis`, `max_error`);
      Marlin `THERMAL_PROTECTION_PERIOD` / `THERMAL_PROTECTION_HYSTERESIS`.*
      In `AT_SETPOINT`, `|actual - setpoint| > drift_hysteresis_c` (default
      25 °C) continuously for `drift_period_s` (default 600 s) trips. Deliberately
      loose and slow compared to a printer's 4 °C / 40 s, because a kiln with
      a 60 s duty window and minutes of dead time legitimately wanders.
      **Implemented (2026-08-11)**: `thermal_guard.c`, guard 4, using its
      own `DRIFT_HYSTERESIS_C` (25 °C) settle/trip band per this bullet's
      own default — the guard-4 comment in `thermal_guard.c` documents a
      logic bug in the first draft (caught and fixed before shipping). Not
      live-tested.
- [x] **5. Absolute limits.** *Marlin `MAXTEMP`/`MINTEMP`.*
      Per-zone `max_temp_c` (kiln/element rating — a mandatory field, not
      optional, and validated at profile-save time in section 5's check too)
      trips **immediately**, no debounce, no window. Per-zone `min_temp_c`
      (default −20 °C) trips immediately as well: a reading below plausible
      ambient means a broken sensor, and this is Marlin's `MINTEMP` catching a
      disconnected thermistor. Also a firmware-wide ceiling derived from the
      configured thermocouple type's range.
      **Implemented (2026-08-11)**: `thermal_guard.c`, guard 5;
      `max_temp_c`/`min_temp_c` added to `zone_cfg_t`
      (`App/drivers/zones_http.c`) and the settings page. Live-verified the
      config side only (a zone was saved with `max_temp_c=1300`,
      `min_temp_c=-20` during the live test); the trip itself was not
      exercised (guard 6 tripped first, since no thermocouple is attached).
      "Firmware-wide ceiling derived from thermocouple type's range" is not
      built.
- [x] **6. Sensor validity.** `spi_failed`, `isnan(tc_temperature_c)`, or any
      of `THERMO_FAULT_OPEN` / `OVUV` / `TCRANGE` set → trip after
      `sensor_fault_debounce_ticks` (default 3, i.e. ~3 s) consecutive bad
      reads. Debounced rather than instant per `docs/SAFETY_MODEL.md`'s own
      note that "a single SPI glitch should probably not drop the kiln to safe
      state; a sustained fault should." `CJRANGE` degrades the reading's
      accuracy and should warn, not trip. Whether a single `TCHIGH`/`TCLOW`
      should trip is answered here: **no** — those are configurable thresholds
      the control loop owns, and check 5 is the safety-side limit.
      **Implementing this closes the top open gap in `docs/SAFETY_MODEL.md`**
      ("a live thermocouple fault during operation does not block relay-on"),
      which that document calls "the single most important thing left to do
      before this board fires anything unattended."
      **Implemented and live-verified (2026-08-11)**: `thermal_guard.c`,
      guard 6, debounce 3 consecutive bad reads. End-to-end tested on the
      real board (no thermocouple attached, so every read is invalid):
      tripped after exactly 3 consecutive bad reads, `state` went to
      `"faulted"`, `fault_guard` reported `7`
      (`THERMAL_GUARD_TRIP_SENSOR_INVALID`'s enum value — guard 6's trip,
      the enum numbering shifts by one past `TRIP_NONE=0`), relay stayed
      off throughout,
      and `profile_executor_halt()` correctly cleared it back to `"idle"`.
      This is the closure of `docs/SAFETY_MODEL.md`'s top gap, for the case
      of a running profile executor specifically — see that doc's updated
      gap language below.
- [x] **7. Frozen sensor.** The reading is bit-identical for
      `frozen_window_s` (default 600 s) while duty > 0. Catches a MAX31856
      that stopped converting while still answering SPI cleanly. The driver's
      existing `MAX31856Reading.stale` flag is the cheap first signal here;
      the value-identity check catches the case where `stale` doesn't fire.
      **Implemented (2026-08-11)**: `thermal_guard.c`, guard 7.
      **Attempted on-target 2026-08-12 and found to be preempted**: injecting
      `tc_frozen` on a heating zone trips **guard 1 at 481 s**, not guard 7 at
      600 s — correctly, since a frozen reading is flat while heat is
      commanded, which is guard 1's own signature, and guard 1's window is
      half as long. Guard 7's reachable window is therefore narrow: a zone
      holding at setpoint with duty above 0 but below `PROGRESS_DUTY_MIN`
      (0.5). Defence in depth working as intended, but it means guard 7 has
      still never been observed firing, and the matrix says so rather than
      assuming it works.
- [x] **8. Cross-zone plausibility.** Two zones in one chamber cannot differ
      by more than `cross_zone_max_delta_c` (default 150 °C) for longer than
      `cross_zone_period_s` (default 600 s). Catches a thermocouple that fell
      out (it reads low while its neighbors read high) even when checks 1 and
      2 are satisfied because that zone's elements really are heating the
      chamber. Only enabled with ≥2 configured zones; the threshold must be
      generous because real kilns genuinely stratify. See 6A.5.
      **Implemented (2026-08-12), and deliberately shipped disabled**:
      `thermal_guard.c`'s guard 8 compares this zone's raw reading against
      every other zone's from the *same* tick snapshot (the arrays
      `profile_executor.c` already fills), against the **worst** disagreeing
      peer rather than an average — with three zones an average would let one
      badly-wrong channel hide behind a healthy one — and trips only after a
      sustained `cross_zone_period_s` (default 600 s) outside the band.
      Untrustworthy peer readings are skipped, not compared against garbage.
      Per-zone by nature, so it blocks only its own zone via the existing
      `relay_authority_zone_blocked()` path.
      **`cross_zone_max_delta_c` still defaults to 0, which disables the
      check** — this bullet and 6A.5's last one are both explicit that the
      threshold must be informed by a measured cross-gain matrix, and no
      real matrix has ever been captured, so the firmware asks for the
      number rather than inventing one.
      **Armed by config (2026-08-12)**: it is now a per-zone field
      (*Cross-zone plausibility*) on the zones page, persisted with the rest
      of the zone config and read back through
      `zones_config_get_cross_zone_delta()`, which `profile_executor.c`
      hands to the guard. `cross_zone_period_s` stayed a firmware constant
      (600 s) — one knob is enough to arm the guard, and a second is easier
      to get wrong than to get value from. `z<i>_xzone` is the one
      *optional* field in `POST /api/zones`, so the MCP path and the older
      test harnesses (which post the original 14 fields) keep working; such
      a submit does clear a previously saved threshold, which is the same
      whole-page-submit semantics every other field has.
      **Observed on-target 2026-08-12, the first time this guard has ever
      fired on hardware**: two zones firing one profile with `z<i>_xzone` set
      to a deliberately tight 2 °C and `fault=tc_detached` on zone 1, tripping
      at **600 s** — `fault_guard: 9`, `"28.5C differs from zone 1's 20.5C by
      8.1C (>2.0C) for 600s"`. Two things that run taught, neither visible in
      the host tests: the guard is **symmetric**, so on a *two*-zone kiln both
      zones trip on the same tick and the run escalates to `FAULTED` through
      the "every active zone individually faulted" path even though the guard
      itself stayed per-zone (it takes three zones for the
      worst-disagreeing-peer comparison to isolate the bad channel); and the
      threshold is read **once, at run start**, so arming it mid-firing does
      nothing until the next start — see 6A.7's config-reload item.
      Host-tested in `test_sim_kiln.c`: a detached thermocouple in a coupled
      chamber trips it, and a healthy coupled kiln at equal duty does not.
      Writing that negative control surfaced something worth recording: two
      *healthy* zones driven at 1.0 and 0.6 duty legitimately settle more
      than 100 °C apart in the sim, which is precisely why this threshold
      cannot be a constant.
- [x] **9. Control-tick liveness.** If the executor task misses
      `tick_miss_limit` (default 5) consecutive ticks, or has not run for
      `tick_dead_ms` (default 10 000), *something else* must drop the relays —
      a control loop cannot be its own watchdog. Register the executor with
      the FreeRTOS Task Watchdog **and** have `monitor_task.c` (which already
      exists as a heartbeat and already watches the expander's owner task
      handle) check a last-tick timestamp and force `kiln_io_all_relays_off()`
      plus `SAFETY_FAULT_SRC_APP` if it goes stale.
      **Implemented (2026-08-11), by a different mechanism than sketched
      above**: `profile_executor.c` adds a second, independent FreeRTOS task
      (`profile_exec_wdt`) that checks the control task's last-tick
      timestamp every 2 s and force-drops relays plus asserts
      `SAFETY_FAULT_SRC_APP` if it goes stale >10 s — deliberately outside
      the control task itself, per this section's "a control loop cannot be
      its own watchdog" requirement. Does NOT register with the FreeRTOS
      Task Watchdog and does NOT go through `monitor_task.c` as the bullet
      suggested — a standalone task was judged to satisfy the same intent
      more simply. Not live-tested (would need to actually stall the
      control task to provoke it).

Common trip semantics — these are not per-check decisions:

- [x] **Latching, always.** A trip requires an explicit operator clear from
      the web UI (and cancels the running profile). No auto-recovery when the
      condition clears: a kiln that trips, silently recovers, and trips again
      is a kiln that will eventually do so unattended and unnoticed. Marlin
      requires a reset for the same reason. The web UI must show *which* check
      tripped, on which zone, with the numbers that tripped it — not "fault."
      **Implemented and live-verified (2026-08-11)**: `thermal_guard.c`
      reports `is_tripped == true` on every call until
      `thermal_guard_clear()`; `profile_executor_run()` refuses to start
      from `FAULTED` without an explicit `profile_executor_halt()` first.
      Live-verified: the guard-6 trip stayed `"faulted"` with `fault_reason`
      text and `fault_guard` number visible via `/api/profile_exec` until
      `profile_executor_halt()` (the Stop button's endpoint) cleared it back
      to `"idle"`.
- [x] **Fail toward off, and keep trying.** On trip: duty 0, relays off, and
      **retry the relay write every tick until it actually succeeds** — copy
      the pattern the link-loss watchdog in `uart_bridge.c` already uses,
      documented in `docs/SAFETY_MODEL.md` §3 ("one failed I2C transfer must
      not be the reason an element stays on").
      **Implemented (2026-08-11)**: `profile_executor.c`'s
      `executor_task_entry` calls `force_relays_off()` every tick for any
      state other than `RUNNING` (including `FAULTED`), which retries the
      relay-mask write via `kiln_io_set_relay_mask()` each tick rather than
      once — matching the link-loss watchdog's pattern. Live-verified: relay
      stayed off throughout the guard-6 trip.
- [x] **Escalation to the global fault mask.** Checks 3, 5, 6, and 9 assert
      `SAFETY_FAULT_SRC_THERMAL_SANITY` (or the broadened live
      `SAFETY_FAULT_SRC_THERMO` — decide which, see 6A.6), which blocks
      relay-on *everywhere* through `relay_authority_on_blocked()`. Checks 1,
      2, 4, 7, and 8 are per-zone by nature; see the per-zone gating problem
      in 6A.6, which must be solved before those can be enforced correctly.
      **Implemented (2026-08-11)**: `profile_executor.c`'s
      `escalate_guard_trip()` — checks 3, 5, 6 assert globally (6 via the
      broadened `THERMO`, 3/5 via `THERMAL_SANITY`, per 6A.6's own
      recommendation), checks 1, 2, 4, 7 block only this zone via the new
      `relay_authority_zone_blocked()` mask. Check 9's watchdog task asserts
      `SAFETY_FAULT_SRC_APP` directly (not through `escalate_guard_trip()`,
      since it isn't a `thermal_guard` verdict).
      **Updated 2026-08-12**: check 8 now exists and falls through the same
      `else` branch as 1/2/4/7 — per-zone block only, no global fault bit,
      which matches this bullet's own "checks 1, 2, 4, 7, and 8 are per-zone
      by nature." No code change was needed for that; it is what the
      escalation split already did for any non-global reason.
- [x] **Default policy on a single-zone trip: abort the whole firing.**
      Configurable, but this is the default and the rationale should be in the
      docs: in a multi-zone kiln the remaining zones keep dumping heat into a
      chamber whose temperature is now partly unmeasured, and the ware is
      already ruined. "Continue with the other zones" is the option that needs
      justifying, not the abort.
      **Reconciled 2026-08-13**: this note was stale — 6A.5's concurrent
      multi-zone execution landed 2026-08-11/12, so the question this
      bullet asks became live, and the code that answered it
      (`escalate_guard_trip()`'s per-zone branch) defaulted to the
      *opposite* of what was requested: a per-zone trip left the other
      zones running unless every active zone happened to fault too. **Built
      (2026-08-13)**: `zones_cfg_t.continue_on_zone_trip`
      (`zones_http.c`/`.h`, new field, ZONES_CFG_VERSION bumped 1→2),
      zero-initialized/migrated-default `false` = abort (matching the
      requested default exactly, since a v1 blob predating this field reads
      the new field as 0 with no explicit migration step needed).
      `profile_executor.c`'s `escalate_guard_trip()` now forces every other
      active zone off and faults the whole run on any per-zone trip unless
      `zones_config_get_continue_on_zone_trip()` is explicitly true —
      without asserting the board-wide safety-link fault bit, since the
      cause is this zone's physics, not a hardware condition threatening
      every zone. Checkbox + explanatory hint added to
      `zones_page.html`/`GET`+`POST /api/zones`. Build-verified; **not yet
      exercised live** — no daughterboard attached to actually trip a guard
      and watch the other zone abort (would need `KILNCTL_SIM_PLANT` in a
      separate build to test without one, not done this pass).
- [x] **No auto-resume across reboot.** Relays already come up off
      (`kiln_io_init`'s latch ordering); a firing must not restart itself
      after a brownout. Persist enough state to *tell the operator what was
      running and how far it got*, and require an explicit restart.
      Relays coming up off (`kiln_io_init`'s latch ordering) was already the
      case; the missing half — persisting *what was running and how far it
      got* — was **built 2026-08-12** as `App/drivers/run_state.{c,h}`.
      A 104-byte fixed record (`_Static_assert`-pinned so a layout change is
      a build error, not a silent one) in NVS namespace `kiln_cfg` under its
      own key `run_state`, never inside the `zones_cfg` blob — that module
      treats a size change as corruption and wipes the operator's zone
      config, a trap this codebase has already been bitten by. Written on
      every meaningful transition (start, segment change, pause, resume,
      fault, done, halt) plus a 300 s refresh while running: ~200 writes over
      a 12-hour firing against ~43,000 if written per tick.
      A clean end is recorded as *ended*, so the operator can tell "I stopped
      it" from "the power went out" — and a run left PAUSED is deliberately
      still interrupted, because a firing paused and never resumed because
      the power died is not a clean ending.
      On boot the record is loaded, logged loudly at WARN, and served at
      `GET /api/profile_exec`'s `last_run` key with a dismissible banner on
      the dashboard and `POST /api/profile_exec/ack_last_run` to clear it.
      There is no code path from a stored record to `profile_executor_run()`
      or to any relay — reading it is all it does.
      **Verified on-target 2026-08-12**: a run started on the bench unit
      faulted on guard 6 (no thermocouples), the board was rebooted, and the
      record came back with the profile name, segment, target, guard number
      and reason intact (`uptime_at_write_s: 70`, no fabricated wall-clock
      time — the board has no RTC), then acknowledged cleanly.
      Cost: **+240 bytes of `.bss`**, which matters on a board that was once
      left with 7 KB of heap.
      Known gaps: a per-zone guard trip that does not fault the whole run
      only reaches flash on the 5-minute refresh, so the record can be that
      stale about one zone dropping out; and a transition write that fails
      makes the record lie (a DONE that did not land reads back as
      interrupted), which is unfixable at that layer and logs at ERROR.
- [~] **Every threshold above is config, not a constant.** Kconfig supplies
      the compile-time defaults (matching how the rest of this firmware does
      hardware config), `zone_cfg_t` holds the per-zone overrides, and the
      Settings → Thermocouples & Zones page edits them. The request explicitly
      deferred picking the sanity rate ("I will determine later"), and every
      other number above is a first guess that a real firing will correct.
      **The per-zone-override half closed 2026-08-16** (the Kconfig-defaults
      half did not -- the fallback numbers stay `#define`s in
      `thermal_guard.c`, same as before this pass; nothing asked for them to
      become tunable at compile time, only per-zone at runtime). The
      remaining named thresholds (wrong-dir
      rate/window, off-settle, runaway rate/margin, drift period, sensor
      debounce count, frozen window) are now per-zone overrides too --
      `thermal_guard_cfg_t` grew the 8 fields, `zone_cfg_t`
      (`ZONES_CFG_VERSION` 2->3) stores them, a new bundled getter
      `zones_config_get_guard_thresholds()` reads them, `profile_executor.c`
      and `autotune_engine.c` both wire them into every `guard_cfg` they
      build (including the loud per-field mid-firing reload path, same
      discipline as every other guard threshold there), and
      `zones_page.html` exposes them behind a `<details>` "Advanced guard
      thresholds" disclosure per 6A.9's own note that the page needs to
      become collapsible for this reason. Same "0 = firmware default"
      convention as `sanity_rate_c_per_min` throughout -- unlike
      `cross_zone_max_delta_c`, 0 does not disable a guard. Host-tested (3
      new checks in `test_thermal_guard.c`, 221/221 passing) and
      `idf.py build` clean; **no hardware to observe an override actually
      change a trip on a real firing** -- same caveat as the ownership item
      above. Guard 1's `PROGRESS_WINDOW_S`/`PROGRESS_DUTY_MIN` and guard 4's
      `DRIFT_HYSTERESIS_C` remain firmware constants -- TODO.md's list above
      never named them.
- [ ] **A guard must never be disable-able from the web UI without an
      explicit, logged, per-firing acknowledgement.** If a "disable thermal
      protection" affordance exists at all it belongs behind a Kconfig option
      that is off in production builds — Marlin's stance on
      `THERMAL_PROTECTION_*`, and correct.
      Not addressed either way — no "disable thermal protection" affordance
      was added this pass, so there's nothing to gate, but the acknowledged
      Kconfig-gated design itself wasn't built. Left unchecked rather than
      claimed done by omission.

### 6A.4 PID autotune, for every zone

Requested for all zones. Two identification methods; build both, but the
recommendation is **step test first**, contrary to what most 3D-printer
firmware does, and the reason matters:

- **Relay-feedback (Åström–Hägglund) autotune** — the classic, and what
  Marlin's `M303` and Klipper's `PID_CALIBRATE` do. Drive full-on/full-off
  around a setpoint with hysteresis `h`, measure the sustained oscillation's
  peak-to-peak amplitude `a` and period `Tu`, then
  `Ku = 4d / (pi * sqrt(a^2 - h^2))` (the `sqrt` term is the hysteresis
  correction, which matters here because hysteresis is not optional with a
  mechanical relay) with `d` the relay half-amplitude in duty units.
  On a kiln this means **deliberately oscillating the chamber for several
  cycles at temperature**, where one cycle can be 10+ minutes. It is slow,
  thermally abusive, and at cone temperature it is the last thing anyone
  wants to do on purpose.
- **Open-loop step test with FOPDT fit** — from steady state, apply a fixed
  duty step (e.g. 0.5), log the response, fit a first-order-plus-dead-time
  model: static gain `K` (°C per unit duty), time constant `tau`, dead time
  `L`. Two-point (28.3 %/63.2 %) fit, with a least-squares refinement if the
  data supports it. One monotonic climb, no oscillation, no overshoot, and it
  yields the *model* rather than just a tuning — which the feedforward term
  (6A.2), the ramp-rate ceiling (below), and the interaction matrix (6A.5)
  all need and none of which fall out of a relay test.

- [x] Implement `App/drivers/pid_autotune.{c,h}` (pure identification/tuning
      math) + `App/drivers/autotune_engine.{c,h}` (on-target state machine
      driven from a 1 Hz tick). **Done (2026-08-11), step-test path only**:
      `pid_autotune.c` fits a FOPDT model from a step-response trace
      (two-point 28.3%/63.2% method) and computes SIMC gains from it, pure
      C with no ESP-IDF dependency -- validated in
      `App/test/test_pid_autotune.c` by running it against `sim_plant.c`'s
      known ground-truth K/tau/L (fitted K within 2%, tau within 5%, of the
      sim's true values).
      **Relay-feedback identification added 2026-08-12, pure math only**:
      `pid_autotune_fit_relay()` recovers `(Ku, Tu)` from a trace recorded
      under relay control — `Ku = 4d / (pi * sqrt(a^2 - h^2))`, with the
      hysteresis correction this section calls for and `a` as the
      oscillation's *half*-amplitude (the single easiest way to get `Ku`
      wrong by a factor of two, so the header and the tests both state the
      convention outright). It fits only the trailing complete cycles — the
      first ones are still converging toward the limit cycle — requires
      cycle-to-cycle consistency before calling a fit valid, and rejects
      `a <= h` cleanly instead of propagating a NaN out of the square root.
      `pid_autotune_tune_from_relay()` implements the ZN and Tyreus-Luyben
      rules the enum has always carried and the FOPDT path correctly refuses.
      **The on-target state machine landed later the same day**:
      `AUTOTUNE_METHOD_RELAY` reuses the existing engine, lock, tick task,
      trace buffer, relay-authority claim and abort paths, adding
      `RELAY_APPROACH -> RELAY_CYCLING`. Recording starts at the first
      high->low switch, not on arrival: that way the trace begins at a known
      relay phase and carries no monotonic approach ramp, which would drag
      the midline the fitter slices cycles against away from the
      oscillation's centre. The relay law runs every tick (1 Hz) rather than
      per recorded sample, so switch instants — which *are* Tu — are not
      quantised to the 10 s sample period.
      Defaults, each chosen against a kiln rather than a hotend: `u0 = 0.5`
      with `d = 0.35` (branches 0.15/0.85), because centring on 0.5 is what
      lets the largest `d` fit inside [0,1] **without clamping**, and a
      clamped branch makes the true half-amplitude smaller than the `d`
      handed to the fit — precisely the factor-of-two `Ku` error; `d > 0.5`
      is refused rather than clamped. `h = 2 °C` half-band, above type-K
      noise so the relay switches on the plant and not on noise, and small
      because `h` subtracts under the `sqrt(a^2 - h^2)`. 5 cycles (the
      fitter's 3 trailing plus 2 discarded as transient), since each extra
      cycle costs 10+ minutes of a hot kiln. Separate 4 h approach and 4 h
      cycling budgets, because those phases fail differently. A setpoint
      within 50 °C of the zone's guard limits is refused at start: the
      oscillation overshoots its band by however much dead time allows —
      the very quantity being measured — so a setpoint near the ceiling is a
      scheduled guard-5 trip hours later with the kiln hot. Default rule is
      Tyreus-Luyben; SIMC is refused at start rather than silently proposing
      zero gains after a multi-hour run.
      Accepting a relay result writes gains but **never** a plant model:
      a relay test measures one frequency-response point, infinitely many
      FOPDT plants share it, so there is nothing to write — and a model a
      previous step test measured survives untouched.
      Known limitation, documented at the code: **guard 3 has no off-window
      during cycling**, since the low branch is duty 0.15 and the
      welded-relay check never opens its window. Nothing is disabled; it
      simply has no off-period to observe, and guards 4 and 5 cover that case
      with a real setpoint. A full-off low branch would arm guard 3 but fire
      it on the ordinary case, because a kiln keeps climbing for minutes
      after heat is cut and guard 3 trips on that at 120 s.
      **Never run on hardware** — no thermocouples are attached, so every
      on-target autotune of either method aborts on guard 6 first.
      Host-tested against a genuine closed-loop limit cycle from
      `sim_plant.c`; the expected period is solved from the same
      describing-function condition including the relay's own `-asin(h/a)`
      phase lag, not from the plant's ultimate period — the two differ by
      28 % on this plant, and a tolerance loose enough to cover that gap
      would also hide real errors. **Still not built**: the on-target state
      machine that would actually drive the relay, per this section's own
      "step test first" recommendation.
      `autotune_engine.c` drives the real state machine:
      `IDLE -> SETTLING (180s at duty 0) -> STEPPING (records a trace every
      5s, up to 4h) -> DONE (fit + SIMC gains proposed) | ABORTED`. Owns the
      zone's relay(s) through `relay_authority_zone_blocked()`, same as
      `profile_executor.c`. **NOT implemented**: the relay-feedback
      (Astrom-Hagglund) method (`AUTOTUNE_RELAY` -- step test only, per this
      bullet's own "recommendation is step test first"), a distinct
      `AUTOTUNE` owner tag (section 0's owner-tag mechanism was never built
      generally -- see that section), and gain-scheduling bands (single
      band only, per this subsection's own "v1 may ship a single band").
- [x] **SIMC/lambda tuning from the fitted model, for the step-test path.**
      **Done (2026-08-11)**: `pid_autotune_tune_from_fopdt()` implements
      `Kc = tau / (K * (lambda + L))`, `Ti = min(tau, 4*(lambda + L))`,
      `Td = L/2`, default `lambda = 3*L` (override accepted, "tight" ==
      `lambda = L`), converted to pid.c's parallel `{Kp, Ki, Kd}` form and
      validated by-hand in `test_pid_autotune.c`.
      ZN (`Kp = 0.6*Ku`, `Ki = 2*Kp/Tu`, `Kd = Kp*Tu/8`, which is what Marlin
      uses) targets ~quarter-amplitude decay — it is designed to oscillate.
      On a hotend that costs a few degrees of ripple; on a kiln at 1200 °C it
      costs the firing and stresses the elements.
      **ZN and Tyreus-Luyben implemented 2026-08-12** in
      `pid_autotune_tune_from_relay()`, now that `pid_autotune_fit_relay()`
      produces the `Ku`/`Tu` they need. `pid_autotune_tune_from_fopdt()`
      still refuses them with zero gains — they are not derivable from a
      FOPDT model, and returning something plausible-looking would be worse
      than refusing. Neither rule is a default anywhere: the paragraph above
      is the reason, and it is repeated at the code.
- [ ] **Gain scheduling by temperature band.** A kiln's plant gain is strongly
      temperature-dependent — radiative loss goes as roughly T^4, and element
      resistance drifts with temperature — so one tuning fitted at 200 °C will
      be badly wrong at 1200 °C. Design for **2–3 bands per zone** (e.g.
      0–400 / 400–800 / 800+ °C), each with its own `{Kp, Ki, Kd, K, tau, L}`,
      interpolated at the band edges to avoid a step change in output. **v1
      may ship a single band** to keep scope sane, but `zone_cfg_t` and the
      NVS blob must carry the band array from the start so adding bands later
      is not a storage migration. Autotune then runs once per band.
      Not built — `zone_cfg_t`/the NVS blob still carry one `{Kp,Ki,Kd}` per
      zone, no band array, matching this bullet's own "v1 may ship a single
      band."
- [~] **Autotune replaces the deferred ramp-ceiling estimate.**
      **Partially done (2026-08-11)**: `pid_autotune_estimate_max_ramp_c_per_hr()`
      implements the exact `(K*u_max - (T-T_ambient))/tau` formula and its
      result (`predicted_max_ramp_c_per_hr`) is returned by `/api/autotune`
      and shown in the autotune status text on `/settings/zones`. **Not
      built**: it isn't shown "next to" the `max_ramp_c_per_hr` field
      specifically (it's in the autotune card below it), and there is no
      one-click "adopt this ramp ceiling" action — `autotune_accept()` only
      writes Kp/Ki/Kd, not `max_ramp_c_per_hr`. The number is visible and
      correct; wiring it into the ramp field itself is still open.
- [x] **Results are proposed, never auto-applied.** **Done (2026-08-11)**:
      `autotune_engine`'s `DONE` state exposes the fitted model, gains,
      predicted ramp ceiling, and a CSV trace link via `/api/autotune`; only
      `POST /api/autotune/accept` writes anything, through the new
      `zones_config_set_pid()` (`zones_http.c` stays the one NVS owner —
      `autotune_engine.c`/`pid_autotune.c` never touch NVS). "The rule used"
      is always SIMC in this pass (the only rule with an identification
      method behind it, see 6A.4's tuning-rule bullet above).
- [x] **Every guard in 6A.3 is armed during autotune**, plus most
      autotune-specific aborts. **Done (2026-08-11)**: thermal_guard runs
      every tick of `SETTLING`/`STEPPING` against the zone's own
      `max_temp_c`/`min_temp_c`/`sanity_rate_c_per_min` (there is no
      separate `autotune_max_temp_c` — it reuses the zone's configured
      ceiling rather than adding a second number to configure), a trip
      aborts and escalates exactly like `profile_executor.c` does. Total
      time budget (4h) enforced; exceeding it attempts a fit anyway and
      reports `ABORTED` with the fit's own rejection reason if the trace
      doesn't support one ("failing to reach steady state" is this path,
      not a separate detector). **Not built**: "failing to detect
      oscillation within N periods" (relay-test-specific, no relay test
      exists yet).
- [x] **Abortable from the web UI at any time**, and an autotune whose
      controlling HTTP client disappears must keep running safely.
      **Done (2026-08-11)**: `POST /api/autotune/abort`; the state machine
      runs entirely in `autotune_engine.c`'s own FreeRTOS task, not driven
      by or tied to any HTTP request, and still respects its 4h budget with
      no client attached.
- [x] **Persist the full autotune trace... RAM ring buffer during the run,
      downloadable as CSV.** **Done (2026-08-11), single-zone CSV**:
      `autotune_engine.c`'s `zone_trace[MAX31856_CHANNEL_COUNT][AUTOTUNE_ENGINE_MAX_SAMPLES]`
      (4h at 5s resolution) plus `GET /api/autotune/trace.csv` for the zone
      under test. Update (2026-08-11, 6A.5(b)): every configured zone's raw
      trace is now captured internally, not just the tested zone's, and
      fed into the coupling matrix — but only the tested zone's row is
      downloadable as CSV; the other zones' fitted `{K,tau,L}` are exposed
      only as `GET /api/autotune/matrix`, not their raw per-sample traces.
      **Not built**: raw per-zone CSV download for non-tested zones, and
      persistence to flash (correctly left optional/deferred per this
      bullet's own text).
- [x] **Autotune order for multiple zones is not "all at once."**
      Trivially satisfied in this pass: `autotune_engine` is a single global
      instance, so only one zone can be SETTLING/STEPPING at a time by
      construction — not because of any explicit ordering logic. The actual
      cross-zone coupling *logging* 6A.5(b) describes (recording every
      zone's response during zone i's step, to build the K matrix) **is now
      built** (2026-08-11) — see 6A.5(b) for the full writeup and the caveat
      that it's never been exercised against real hardware.

### 6A.5 Multi-zone interaction — "they are not really separate"

Correct, and this is the part that a per-zone-PID-and-hope design gets wrong.
Three zones sharing one chamber form a coupled MIMO plant: zone 1's elements
heat zone 2's thermocouple, with a gain that is not small — in a well-insulated
chamber the cross gain between vertically adjacent zones is often more than
half the direct gain. Three independent PID loops each treat their neighbors'
influence as an unmeasured disturbance and can fight each other into a slow
oscillation that neither loop's tuning explains.

Ordered by cost/benefit; ship a, b, and d in v1. **Update (2026-08-11): (a),
(b), and (d) all shipped** -- see (b)'s own note below for what "shipped"
means here given no thermocouple hardware exists to prove a real cross-gain
against.

- [x] **(a) Detuned decentralized PID.** **Done (2026-08-11)**:
      `profile_executor.c` rewritten from single-zone to concurrent
      multi-zone -- `profile_t.zone_mask` (replaces the old single
      `zone_index`) lets one profile drive more than one zone at once, each
      running its own independent `pid_state_t`/`heater_output_state_t`/
      `thermal_guard_state_t` against a shared setpoint. `lambda = 3*L`
      robustness default from 6A.4 already applies per zone, unchanged.
      Live-verified end to end on the real board (no thermocouple/relay
      hardware attached): configured 2 zones, created a 2-zone profile,
      started it, watched both zones run concurrently and independently
      trip guard 6 (sensor invalid, 3-read debounce) on their own schedules,
      and confirmed the whole run correctly escalated to `FAULTED` only once
      *every* active zone had individually faulted -- see
      `docs/PROJECT_STATUS.md` for the full trace.
- [x] **(b) Identify the coupling for free during autotune.** When zone *i*'s
      step test runs, **log every zone's response, not just zone i's**. That
      yields the full steady-state gain matrix `K[i][j]` (zone *j*'s
      temperature rise per unit of zone *i*'s duty) plus per-pair `tau` and
      `L`, at zero extra heating time. This is the single most important item
      in this subsection: without the matrix, every other interaction feature
      is guesswork. It also forces the ordering constraint: **zones must be
      autotuned one at a time, with the others held at zero duty**, or the
      responses are unattributable.
      **Done (2026-08-11)**: `autotune_engine.c`'s task loop already called
      `MAX31856_read_all()` (whole-bus read) every tick but only kept the
      tested zone's sample -- the fix logs every configured zone's reading
      into its own trace row at the same tick, zero extra SPI traffic.
      `finalize_fit()` now fits every zone's row against the same duty step
      that was applied to the zone under test (`pid_autotune_fit_fopdt()`,
      the same pure two-point-method function 6A.4's own model uses --
      no new identification math, just calling it once per zone), and
      writes the results into a new global `autotune_coupling_matrix_t`,
      row *i* filled each time zone *i*'s run reaches DONE. A zone whose
      baseline reading wasn't valid when STEPPING started (no sensor,
      `thermo_count` doesn't cover it) is left `valid:false` in that row
      rather than fit against garbage; likewise a momentary bad read on a
      non-tested zone mid-run carries forward its last valid value instead
      of shifting that zone's trace out of alignment with the tested zone's
      `t_s`. Exposed as `GET /api/autotune/matrix` (all
      `MAX31856_CHANNEL_COUNT`x`MAX31856_CHANNEL_COUNT` cells, RAM-only,
      lost on reboot -- not persisted to NVS, matching the trace's own
      lifetime) and a table on the Zones page, polled every 5s. The
      ordering constraint continues to hold as a side effect of
      `autotune_engine` being a single global instance plus the
      `profile_executor_run()`/`autotune_engine_run()` mutual per-zone
      exclusion (both from the (a) pass) -- unchanged by this pass, still
      the mechanism that makes "zone *i*'s duty step" attributable.
      **What "done" does NOT mean here**: no thermocouple daughterboard is
      attached to the bench unit, so every real autotune run so far aborts
      on guard 6 (sensor invalid) during SETTLING/STEPPING, before
      `finalize_fit()` ever runs -- the matrix-fill code path is
      logic-verified (compiles, the wiring is exercised by the same guard
      trip that (a) already proved works) but has never actually filled a
      cell against a real cross-gain. That can only happen once real
      sensors exist to prove it against.
      **Update (2026-08-12)**: the *math* is no longer unexercised — the new
      coupled host sim (6A.8) runs a duty step on one zone of a two-zone
      kiln with a known conductance and fits both zones with the same
      `pid_autotune_fit_fopdt()` this path calls, asserting a positive cross
      gain below the direct gain over a slower path
      (`test_sim_kiln.c`'s `test_cross_gain_matrix()`). What is still
      unproven is the *on-target plumbing* — `autotune_engine.c`'s
      per-zone trace capture and `finalize_fit()`'s matrix write have still
      never run to completion on the board, because every real run aborts on
      guard 6 first.
- [x] **(c) Compute and display the Relative Gain Array** from `K`. The RGA
      answers, quantitatively, whether independent per-zone loops are viable
      at all: diagonal elements near 1 mean the zones are effectively
      decoupled and (a) is sufficient; elements far from 1 (or negative) mean
      decentralized control is actively fighting itself and (e) is needed.
      Cheap to compute for a 3x3, and it turns "the zones affect each other"
      from a worry into a number on a page.
      **Built (2026-08-12)**: `pid_autotune_rga()` (pure math, host-testable,
      no allocation) computes RGA = K .* (K^-1)^T for 2x2 and 3x3, exposed
      through `autotune_engine_compute_rga()` on a caller-owned copy of the
      matrix so the served RGA always comes from the same snapshot of `K` as
      the served cells, published in `GET /api/autotune/matrix`'s new `rga`
      object, and rendered on the zones page under the coupling table.
      Three refusals, all deliberate, because a wrong RGA is worse than no
      RGA -- it would tell an operator their zones are independent when they
      are not: an incomplete matrix (it searches for the largest *principal
      sub-block* whose every cell is measured and reports which zones that
      covers, rather than zero-padding to 3x3 and producing a meaningless
      answer), a singular one (scale-aware `|det| <= 1e-4 * max|K|^n`, since
      a bare `det == 0` never fires on noisy fit output), and any non-finite
      entry. A large-but-trustworthy value is NOT refused -- that is a real
      measurement of a badly coupled kiln, and the page flags it instead.
      The page states the verdict from the worst actual diagonal element and
      names the zone, in a potter's terms ("one PID per zone is exactly
      right" vs. "turning this zone's heat up ends up pushing it the wrong
      way") rather than as a generic paragraph about control theory.
      Host-tested with 61 checks (suite 154 -> 215), including a
      non-symmetric case that catches a missing transpose, a near-singular
      matrix at scale that a `det == 0` test would wave through, and the
      row/column-sums-to-1 invariant on every valid case.
      **Still never fed real data**: no cross-gain cell has ever been filled
      on hardware, because every on-target autotune run aborts on guard 6
      with no thermocouples attached. Verified on the board only to the
      extent that the endpoint answers and refuses correctly
      (`"no 2 zones yet have every cross-gain between them measured"`).
      Only the steady-state gain is used; tau and L are discarded, since a
      frequency-dependent RGA would need per-pair transfer functions this
      firmware does not identify.
- [x] **(d) Ramp-lock / setpoint governor.** **Done (2026-08-11)**:
      `PROFILE_EXECUTOR_RAMP_LOCK_BAND_C` (25°C, matching this bullet's
      default). Implemented as incremental target stepping rather than the
      original tick-delta/phase_start_tick math: each control tick only
      steps the shared `target_c` (and only accumulates
      `segment_elapsed_s`, so the dwell timer only runs) while every active,
      *not-already-per-zone-faulted* zone is within band -- "slowest zone
      sets the pace" falls out of that directly, no separate governor logic
      needed. Excluding already-faulted zones from the lock check is the
      answer to this bullet's own timeout concern: guard 1 (no-progress)
      trips that zone independently on its own window regardless of
      ramp-lock, and once it has, ramp-lock stops waiting on it (a zone
      that's genuinely broken shouldn't get to hold the whole firing
      hostage forever). Visible in the UI: `ramp_lock_held`/
      `ramp_lock_lagging_mask` on `/api/profile_exec` and `/api/control`,
      shown on the dashboard's exec card ("Ramp-lock held -- waiting on
      ...") and in the pc_tools Firing Status popup. Per-profile/per-zone
      band override is not built (single firmware-wide constant, matching
      this bullet's own "default" framing -- no bullet here asked for it to
      be configurable).
- [ ] **(e) Static decoupler / cross-feedforward** using the matrix from (b):
      each zone's output is corrected by its neighbors' *commanded duty*
      changes through the measured cross gains, so a neighbor turning on is a
      known feedforward rather than an unmeasured disturbance. A 3x3 matrix
      multiply per tick — trivial compute, meaningful benefit. Target this for
      v1.5, after (a)–(d) are proven on a real firing, and gate it on the RGA
      from (c) saying it is warranted.
- [ ] **(f) Full MIMO / model-predictive control is explicitly out of scope.**
      Recorded here so it is a decision rather than an omission: the benefit
      over (a)+(d)+(e) on a plant this slow does not justify the firmware,
      tuning, and verification cost, and there is no way to validate it safely
      on a device that fires unattended.
- [~] **Electrical load staggering.** If total element draw exceeds what the
      supply/breaker can deliver with every zone on simultaneously, phase-offset
      each zone's time-proportioning window (zone *i* starts its window at
      `i * window_ms / n_zones`) and add a `max_simultaneous_relays` cap
      enforced in `heater_output`. Staggering also smooths the thermal
      cross-talk transients that (e) has to model, so it helps twice. Needs a
      config item for the number and a documented interaction with duty
      quantization (a capped zone's lost on-time must be *deferred*, not
      dropped, or its effective gain silently drops).
      **Both halves built (2026-08-11).** Phase-offset:
      `heater_output_seed_phase(state, window_ms, phase_offset_ms)` added to
      the pure `heater_output.c` module: truncates and forces OFF a zone's
      first time-proportioned window so its boundary lands `phase_offset_ms`
      earlier than an unphased zone's, permanently (every window after the
      first keeps the normal `window_ms` period, so the shift persists for
      the life of the run). Wired into `profile_executor.c`'s per-zone
      run-init: each PID-mode zone gets `phase_offset_ms = rank * window_ms
      / n_active_zones`, `rank` being that zone's 0-based position among
      *this run's own* active zones (not its raw zone index), so a 2-zone
      run gets a clean 50/50 split regardless of which two zone slots are
      in use. Bang-bang zones and single-zone runs are untouched (no window
      to offset). Host-tested (2 new checks) -- and writing that test
      caught a real pre-existing bug in `test_heater_output.c` itself:
      several `heater_output_state_t s;` locals were declared without a
      zero initializer, relying on `heater_output_reset()`'s
      cycle-count-preserving memset pattern being safe on first use -- true
      for every real caller (always zero-initialized static/BSS storage)
      but undefined behavior for an uninitialized stack local, and the new
      test blocks happened to shift stack layout enough to flip
      previously-lucky garbage into unlucky garbage. Fixed by
      zero-initializing every declaration in that file. 96/96 host checks
      pass.
      `max_simultaneous_relays` cap: new global (not per-zone) config field
      in `zones_http.c`'s `zones_cfg_t` (0 = unlimited, the existing/only
      behavior before this pass), settable via `POST /api/zones`
      (`max_simultaneous_relays`, optional -- a missing field defaults to 0
      rather than rejecting the request, so older callers keep working
      unchanged) and a new field on the Zones page. `profile_executor.c`'s
      control loop was split into a decide pass (every active zone computes
      its want-on, unchanged math) and an apply pass, with the cap decision
      in between: if more active zones want on than the cap allows, the
      zone(s) with the *lowest* `deferred_on_ms` (least already owed) are
      suppressed first, self-correcting the debt over time rather than
      starving one zone forever. A suppressed PID-mode zone's lost on-time
      is credited back as a duty boost on its own next window (spread over
      however many ticks until that zone's window naturally rolls over --
      no boundary-prediction needed, since `heater_output_duty()` only
      reads the duty argument at an actual boundary and silently ignores it
      mid-window, so crediting every tick and only debiting the ledger when
      the boost is confirmed consumed keeps the accounting exact even
      across several intervening ticks). This is the "deferred, not
      dropped" requirement this bullet asks for -- implemented entirely in
      the impure `profile_executor.c` orchestration layer specifically so
      `heater_output.c`'s own `relay_on`/`cycle_count` bookkeeping is never
      overridden or desynced from what it itself decided (the cap changes
      what hardware actually gets energized via the existing
      `apply_relay()` chokepoint, which already separately tracks
      `relay_commanded_on` for telemetry/guard purposes -- the guard
      suite's `commanded_duty` input correctly reads 0 for a capped-off
      zone this tick, not the nominal PID output, so it can't be fooled
      into thinking heat was delivered when it wasn't). Bang-bang zones
      participate in the cap but their suppressed on-time is NOT paid
      back -- documented limitation, no window concept to defer into for
      that mode. This is orchestration-layer logic (mutual exclusion,
      priority selection, credit ledger), not new pure-module math, so
      there's no new host test for it specifically; existing 96/96 host
      tests re-run clean (unaffected, since none of them touch
      `profile_executor.c`).
      **Live hardware verification**: build clean, smoke-tested via a live
      JTAG flash + boot (see `docs/PROJECT_STATUS.md`'s entry on the
      bootloader-wipe incident this same session), but a real multi-zone
      PID run exercising the cap end-to-end over HTTP was not completed --
      the board's bench Wi-Fi has been flapping on and off throughout the
      second half of this session (confirmed environmental/RF via a GDB
      backtrace showing every task, including `httpd`/`wifi`, parked in a
      normal blocking wait -- no crash, no deadlock -- and a re-read of
      `wifi_prov.c`'s reconnect logic, which is correct).
- [ ] **Cross-zone plausibility (guard 8 in 6A.3) belongs to this subsection's
      physics** — its threshold should be informed by the measured `K` matrix
      once available, rather than staying a hand-picked 150 °C forever.
      **Half-resolved (2026-08-12)**: guard 8's *logic* now exists and is
      host-tested (see 6A.3's guard-8 entry), wired to the multi-zone
      snapshot the executor already reads. What this bullet actually asks
      for — the threshold coming from a measured `K` rather than a
      hand-picked 150 °C — is still open, so the guard ships with the
      threshold unset (0 = disabled) rather than with a fabricated default.
      Filling it in needs (b)'s matrix to be captured against real sensors,
      or an operator willing to enter a number for their own kiln; neither
      has happened.
- [x] **A zone with no thermocouple of its own cannot be PID-controlled.**
      Still structural, as stated -- `zones_http.c` still maps zone *i* to
      channel *i* one-to-one; this pass's multi-zone work didn't change that
      mapping, only how many zones can be *driven* at once. Checked off
      because the bullet only asked that this be stated for the record, and
      it now additionally holds for `profile_t.zone_mask`: a mask bit can
      only be set for a zone `zones_config_get_thermo_count()` actually
      covers (enforced in `profiles_http.c`'s `parse_profile_fields()`).

### 6A.6 Safety plumbing that has to change

These are concrete gaps found while writing this design; each blocks part of
the above.

- [x] **`relay_authority_on_blocked()` is global and takes no zone or relay
      argument** (`App/drivers/relay_authority.h`). Per-zone guard trips
      (checks 1, 2, 4, 7, 8) need per-zone blocking, and
      `docs/SAFETY_MODEL.md` already flags the same question as open ("A kiln
      with independently-controlled zones may want a channel fault to only
      block *that* zone's relay"). Proposal: add
      `relay_authority_zone_blocked(safety, zone_index, uint32_t *out_sources)`
      layered *on top of* the global check — global faults still block
      everything, and a per-zone block mask (owned by the executor, published
      to relay_authority) blocks only that zone's relays. The global function
      keeps its exact current behavior so the UART bridge and web UI paths do
      not change meaning.
      **Implemented (2026-08-11)**: exactly as proposed —
      `relay_authority_zone_blocked()` and
      `relay_authority_set_zone_blocked()` (`App/drivers/relay_authority.c`/
      `.h`) layered on top of the unchanged
      `relay_authority_on_blocked()`. The UART bridge and dashboard's manual
      override still only call the unchanged global function, so their
      behavior is byte-identical to before this pass. Guard 8 (per-zone
      trip 8) doesn't exist yet, so only checks 1, 2, 4, 7 actually use the
      new per-zone mask today.
- [x] **Decide: new fault source, or broaden `SAFETY_FAULT_SRC_THERMO`?**
      `SAFETY_FAULT_SRC_THERMAL_SANITY` (0x20) already exists, unused, and its
      comment in `safety_link.h` distinguishes it from `THERMO` (the
      MAX31856's own fault bits). Recommendation: use **both, as designed** —
      `THERMO` broadened to mean "a thermocouple is faulted, at boot *or*
      live" (guard 6, closing SAFETY_MODEL.md's top gap), and
      `THERMAL_SANITY` for the inferential guards (1, 2, 3, 8) where the
      sensor is electrically fine but the *physics* disagrees. Two sources
      means the UI can say which kind of wrong it is.
      **Implemented (2026-08-11)**: decided and built exactly as
      recommended — `profile_executor.c`'s `escalate_guard_trip()` uses
      `SAFETY_FAULT_SRC_THERMO` for guard 6 (broadened from boot-only to
      live) and `SAFETY_FAULT_SRC_THERMAL_SANITY` for guards 3 and 5 (guards
      1/2/4/7 don't escalate globally at all — see the per-zone item above;
      guard 8 doesn't exist).
- [ ] **Ownership tags need `AUTOTUNE`** alongside section 0's settled
      `NONE`/`MANUAL`/`PROFILE`/`RULE`, with the same refusal semantics: a
      manual `SET_RELAY` against an autotune-owned relay is refused, not
      silently allowed.
      Still not built now that autotune (6A.4) does exist: a manual
      `SET_RELAY` from the PC over UART during a running autotune step-test
      is not refused by any ownership-tag mechanism — the general ownership-
      tag system this bullet describes was never built (section 0's own
      `NONE`/`MANUAL`/`PROFILE`/`RULE` tags aren't implemented as a real enum
      anywhere either, so `AUTOTUNE` isn't a special case, it's the same gap
      section 0 already has). `autotune_engine.c`'s own relay writes do go
      through `relay_authority_zone_blocked()`, so a *safety fault* still
      wins; it's specifically an unrelated manual override arriving mid-test
      that isn't refused.
- [x] **`SX_WRITE_REG` / `SX_SET_DIR` still bypass the gate entirely**
      (`docs/SAFETY_MODEL.md`, last gap). **Done (2026-08-11)**: closed with
      a per-pin guard, not a blanket refusal of either subcommand (the other
      12 expander pins stay fully reachable for debug) --
      `kiln_io_relay_pin_mask()` (new getter, `kiln_io.h`/`.c`) exposes the
      one already-existing relay-bit mask so `uart_bridge.c` doesn't
      duplicate this board's pin map. Two checks, matching how the two
      subcommands actually threaten a relay differently:
      `sx_write_reg_touches_relay_on()` refuses an `SX_WRITE_REG` to RegDataA
      /RegDataB only when it would set a relay pin's bit *high* while
      `relay_authority_on_blocked()` says no — the same fault-gated rule
      `SET_RELAY`/`SET_RELAY_MASK` already follow, just reached from the raw
      path too. `sx_set_dir_touches_relay()` refuses `SX_SET_DIR`
      unconditionally (not fault-gated) whenever it would flip a relay pin's
      bit to input, independent of any fault state: relay pins are always
      outputs on this board, and turning one into an input would also break
      the *OFF* path (the next `SET_RELAY` or executor OFF command couldn't
      reach the coil driver either), which a fault-gated check alone
      wouldn't catch. Both in `App/drivers/uart_bridge.c`; rebuilt, reflashed
      via OpenOCD, all pages re-verified live (no thermocouple/relay
      hardware attached, so only the refusal path itself was exercised, not
      an actual would-have-been-blocked relay command).
- [x] **Update `docs/SAFETY_MODEL.md`'s summary table** with the new rows
      (each guard, whether it is detected, whether relay-on is blocked, whether
      existing relays are dropped) and flip the two rows that this section's
      work turns from bold **No** into Yes.
      **Done (2026-08-11)** — see `docs/SAFETY_MODEL.md`'s updated summary
      table and gap language.

### 6A.7 Task, timing, and module layout

- [ ] **New module set**, each with one owner and a testable core:
      - `App/drivers/pid.{c,h}` — pure controller math (6A.2).
      - `App/drivers/thermal_guard.{c,h}` — pure guard logic (6A.3).
      - `App/drivers/heater_output.{c,h}` — duty → relay time-proportioning,
        min on/off, phase offset, simultaneous-on cap, cycle counting (6A.1).
      - `App/drivers/pid_autotune.{c,h}` — identification and tuning rules
        (6A.4).
      - `App/drivers/profile_executor.{c,h}` — the tick task that owns all of
        the above plus segment stepping, ownership tags, the rule evaluator
        (section 0 settled that it shares this tick), the history ring buffer,
        and ramp-lock (6A.5d). This is the only module in the set that talks
        to FreeRTOS, `kiln_io`, `relay_authority`, or the HTTP layer.
      4 of 5 modules exist (`pid.c`, `thermal_guard.c`, `heater_output.c`,
      `profile_executor.c`); `pid_autotune.{c,h}` does not (6A.4 unbuilt).
      Left unchecked because the bullet names all five.
- [x] **Tick rate 1 Hz.** MAX31856 conversions are ~100 ms in continuous mode
      and kiln time constants are minutes; 1 Hz is ample and keeps the guard
      windows in whole seconds. Read all channels once per tick via
      `MAX31856_read_all()` and hand the same snapshot to control, guards,
      rules, and history — one consistent view, which is exactly the reasoning
      section 0 already used to put the rule evaluator on this tick.
      **Implemented (2026-08-11)**: `profile_executor.h`'s
      `PROFILE_EXECUTOR_TICK_MS` (1000, i.e. 1 Hz); `executor_task_entry`
      calls `MAX31856_read_all()` once per tick. The rule evaluator does not
      exist yet, so "hand the same snapshot to ... rules" is not literally
      exercised, but the tick/read structure is in place for it.
- [x] **Task priority below the link-loss watchdog (6)**, above the bridge
      tasks. The watchdog's job is to drop relays when the PC vanishes; it
      must never be starved by the control loop.
      **Fixed (2026-08-11)**: the prior pass shipped `profile_executor` and
      `profile_exec_wdt` at priority 4 — correctly below the link-loss
      watchdog (6) but also below the UART bridge tasks (5), the opposite of
      "above the bridge tasks." FreeRTOS priorities are integers with
      nothing between 5 and 6, so "above bridge, below watchdog" isn't
      exactly representable without renumbering `uart_bridge.c`'s tasks too
      (out of scope) — both executor tasks now run at priority 5, tied with
      the bridge tasks rather than below them, and still strictly below the
      link-loss watchdog/`UART_PROTOCOL_TASK_PRIORITY` (6). Closest
      achievable match to the intent; documented as such in
      `profile_executor.c`'s task-creation comment. Live-verified: rebuilt,
      reflashed, all 5 pages and `/api/profile_exec` still responsive.
- [ ] **The tick must not block.** Both bus accesses go through the existing
      `i2c_owner` / `esp_spi_owner` request queues; use bounded timeouts and
      treat a timeout as a bad read (feeding guard 6's debounce), never as a
      reason to skip the guard evaluation entirely.
      Not verified either way this pass — left unchecked rather than assumed;
      would need reading `MAX31856_read_all()`'s timeout behavior against
      `esp_spi_owner` to confirm the "treat timeout as a bad read" contract
      specifically, which wasn't checked.
- [x] **Apply `zone_cfg_t.cal_offset_c` on the reading path** — section 3
      left this "stored only; not yet applied," and the control loop is the
      consumer that makes the decision real. Apply it in firmware (already the
      settled decision) at a single point so the web UI, PC GUI, MCP, guards,
      and PID all see the same corrected number. Note the ordering question
      explicitly: guards 5 and 6 should evaluate the **raw** reading (a
      calibration offset must not be able to hide an out-of-range sensor),
      while control and display use the corrected one.
      **Implemented, scoped down (2026-08-11)**: `zones_config_apply_cal()`
      (`App/drivers/zones_http.c`) is called by `dashboard_http.c`'s display
      and `profile_executor.c`'s control math, and guards evaluate the raw
      reading per the ordering requirement above (`escalate`/`thermal_guard_tick`
      run against `raw_c`, not the calibrated `s_exec.actual_c`). NOT called
      by `uart_bridge.c` — the PC/MCP UART consumers still see the raw
      MAX31856 value uncorrected; this was a deliberate scope cut (touching
      the already-stable `MAX31856.c` driver with a new calibration-provider
      hook was judged too risky for this pass), not an oversight.
- [x] **Config reload while running**: tuning edits from the Settings page
      must take effect without restarting the firing, via the bumpless
      transfer rule in 6A.2. Guard *thresholds* changing mid-firing is more
      dangerous — require the zone to be idle, or log it loudly as an operator
      action.
      **Built (2026-08-12).** `zones_http.c` keeps a monotonic
      `zones_config_generation()` (starts at 1, bumped at the three points
      where the in-RAM config actually changes: the POST commit,
      `zones_config_set_pid()`, and the NVS load; a rejected POST does not
      bump it). `profile_executor.c` compares it once per tick — on the
      unchanged path that comparison is the entire cost, no getters and no
      config walk — and on a change re-reads every active zone, after this
      tick's readings are stored and before any control math, so an edit can
      never be half-applied across the decide/apply split.
      Per field: **gains** land bumplessly
      (`pid_seed_bumpless()` off the duty the zone last commanded, or
      `pid_reset()` if this tick's reading isn't valid — seeding off a bad
      measurement would bake it into the integral); **mode changes** force
      the zone off and restart the controller cold, because there is no
      meaningful handover between a duty fraction and a hysteresis latch;
      **relay-mask changes** force the OLD mask off before adopting the new
      one, or those contacts would stay closed under a mask no zone can name
      any more; **heater timing** applies at the next window with
      `heater_output_state_t` untouched, since resetting it would discard the
      min-on/min-off timers and let a `window_ms` edit machine-gun a
      contactor; **guard thresholds** apply immediately and each one is
      logged at WARN with old → new as an explicit operator action — the
      "log it loudly" option above, chosen over "require idle" because the
      case that matters is an operator who has just realised a ceiling is
      wrong for the ware in the kiln *right now*. A reload never touches
      `thermal_guard_state_t`: editing a threshold must not become an
      undocumented way to clear a latched trip.
      **Verified on-target 2026-08-12** (sim plant): editing kp mid-run left
      the run `running` and undisturbed, and dropping `max_temp_c` below the
      zone's current reading tripped guard 5 **on the next tick** — the same
      edit before this change did nothing until the next start.
      Still run-start-only, and deliberately so: the active zone set (a
      shrunk `thermo_count` logs at ERROR and keeps the zone on its
      last-known settings with its contacts forced open, rather than
      silently dropping a zone out of a firing the profile was validated
      against) and `max_ramp_c_per_hr`, which is a feasibility gate checked
      when a run starts and is not re-checked against a running profile.
- [x] **Unowned-relay sweep.** Surfaced by building the reload above: the only
      thing that can open a relay is code that can still *name* its mask. A
      mask edit, or a zone disappearing from config, is handled explicitly
      now (the old mask is forced off before the new one is adopted) — but if
      that one `kiln_io_set_relay_mask()` call fails, those contacts stay
      closed and nothing downstream can address them again: halt, guard
      trips, and the per-zone watchdog path all go through the *live* mask
      too. Guard 9's watchdog calls `kiln_io_all_relays_off()`, but only on a
      stale tick. A cheap fix is a per-tick sweep: OR every active zone's
      mask, force the complement off.
      **Built (2026-08-12)**, and narrower than "force the complement off",
      which would have been wrong: `sweep_unowned_relays()` forces off the
      intersection of three masks — what the expander shadow says is still
      *closed*, what **this run** has ever commanded
      (`s_exec.claimed_relay_mask`), and the complement of what may
      legitimately hold a relay right now. That middle term matters: relays
      1–4 are not the executor's property. The dashboard's manual
      `/api/relay` and the UART bridge's `SET_RELAY` can energize any relay
      during a firing, gated only by `relay_authority_on_blocked()` and with
      no ownership concept at all (section 0's MANUAL/PROFILE/RULE tags were
      never built), so an operator holding a damper or blower on would have
      had it chattered off once a second by a "safety" feature. The third
      term includes **autotune's** zones: the two engines are mutually
      exclusive *per zone*, not globally, so a step test on zone 2 alongside
      a firing on zones 0–1 is supported and its contacts must survive.
      Using `kiln_io`'s shadow rather than a belief of the executor's own is
      deliberate — `kiln_io_set_relay_mask()` does not update the shadow from
      a failed write, so the sweep is honest about exactly the failure it
      exists to catch and self-terminates once the stray actually opens.
      Costs one mask compare per tick and zero I/O in the steady state.
      Still uncovered: a `force_all_relays_off()` that fails during `halt()`
      is retried by nothing but guard 9's stale-tick path, since the sweep
      only runs while RUNNING.
- [x] **Move the feedforward subtraction into `pid_seed_bumpless()`.**
      It solves `integral = (u_desired - P)/Ki`, which is only exact when the
      caller has already subtracted the feedforward term. Both call sites in
      `profile_executor.c` do that correctly (2026-08-12), but the correct
      home for it is `pid.c` — the function should take `ff_u` and subtract
      it itself, so the next caller cannot get it wrong. `pid.h`'s doc
      comment also still says the executor "always passes 0.0f" for
      feedforward, which stopped being true the same day.
      **Done (2026-08-13)**: `pid_seed_bumpless()` gained a `ff_u` parameter
      and now solves `integral = (u_desired - P - ff_u)/Ki` itself;
      `profile_executor.c`'s `seed_bumpless_with_ff()` wrapper just computes
      `u_ff` and passes it through instead of pre-subtracting. `pid.h`'s
      stale "executor always passes 0.0f" comment rewritten. Only call site
      updated (`profile_executor.c`); `test_pid.c` gained a second bumpless
      case asserting the ff-nonzero seed still reproduces `u_desired`, in
      addition to updating the existing case's now-5-arg call. Host-tests build and pass (216/216, up from 71 baseline — this repo's
      host-test count has grown a lot since 6A.8 first landed). On-target:
      built clean and reflashed over OpenOCD, board boots and answers
      protocol/version over UART — no live behavior to observe beyond that
      (the two call sites were already numerically equivalent; this only
      moves where the subtraction happens).
- [x] **`esp_wifi_set_config(AP) failed: ESP_ERR_WIFI_MODE` at boot.**
      Observed on every boot 2026-08-12, on two separate flashes. Not
      blocking — the station join succeeds and the fallback AP comes up when
      it is actually needed — but the AP config is being applied while the
      driver is in the wrong mode, so it is either a bring-up ordering bug or
      a dead call. Not diagnosed.
      **Diagnosed and fixed (2026-08-13)**: `wifi_prov_start()` called
      `apply_ap_config()` (which calls `esp_wifi_set_config(WIFI_IF_AP,
      ...)`) once, unconditionally, *before* any of its three branches'
      `esp_wifi_set_mode()` call — right after `esp_wifi_init()` the driver
      mode is `WIFI_MODE_NULL`, and `esp_wifi_set_config()` fails with
      `ESP_ERR_WIFI_MODE` whenever the current mode doesn't already include
      the target interface. Explains "every boot": the call ran ahead of
      mode selection regardless of which branch (AP-only/home/
      unprovisioned) was taken. **Same bug found in two more places** on
      inspection, not yet observed/logged but structurally identical:
      `ap_fallback_timer_cb()` (mode is STA-only right after a station
      join, per `on_ip_event()`, so bringing the fallback AP back up hit
      the same failure) and `wifi_prov_set_mode()`'s switch-to-AP branch
      (same STA-only-mode case, reachable from the network settings page).
      Fixed all three by reordering: `esp_wifi_set_mode()` first, config
      applied only on success. The two already-correct call sites
      (`wifi_prov_set_ap_ssid()`/`wifi_prov_set_ap_password()`, which check
      the *current* mode already includes AP before re-applying) were
      untouched — they were never broken, since by the time an operator can
      call them the driver is already up in a mode that includes AP.
      Host-independent (ESP-IDF Wi-Fi driver call, no host test coverage
      possible); build-verified clean, reflashed over OpenOCD, and the
      **boot log confirms the fix**: `get_device_log()` over the UART log
      bridge shows `wifi_prov: attempting station join to '...'` with no
      `esp_wifi_set_config(AP) failed` / `ESP_ERR_WIFI_MODE` line anywhere
      in the boot sequence, where it fired on every previous boot.
- [x] **Why did a board with valid saved credentials report "no saved
      credentials"?** On 2026-08-12 the board booted claiming first-boot
      provisioning and fell back to its AP, yet the `wifi_nvs` migration read
      the same default-partition namespace minutes later, found SSID and
      password, and joined on the first attempt. So the credentials were
      there and correct while the normal loader treated the board as
      unprovisioned — the two read paths disagree about what counts as
      provisioned (most likely the `has_creds` flag versus the presence of
      the strings). Worth pinning down: it is a live inconsistency in
      `wifi_prov.c`, and its symptom is a board silently dropping off the
      network.
      **Root cause found + fixed (2026-08-13)**: confirmed exactly the
      `has_creds` vs strings disagreement — `nvs_load_from()` read a missing
      `NVS_KEY_HAS_CREDS` key as `has_creds=false` unconditionally, even
      when `ssid` was non-empty right next to it. Fixed by inferring
      `has_creds` from a non-empty saved `ssid` when the flag key itself is
      absent (same shape as the existing legacy `mode`/`local_only`
      fallback in the same function) — a present flag key stays the
      authority, this only covers its absence.
- [x] **Manual relay control is not blocked during a firing.** Confirmed from
      the code while building the sweep above: `/api/relay`
      (`dashboard_http.c`) and the UART bridge's `SET_RELAY` /
      `SET_RELAY_MASK` / `SX_WRITE_REG` can energize *or de-energize* any
      relay while a profile runs — including one an active zone is
      time-proportioning, mid-window — gated only by the global
      `relay_authority_on_blocked()`. Nothing arbitrates. Section 0's
      MANUAL/PROFILE/RULE ownership tags are the designed fix and are
      unbuilt; the sweep deliberately does *not* substitute for them (it only
      touches relays this run itself claimed). Note the rules engine cannot
      hold a relay at all today: `rules_http.{c,h}` stores and validates rule
      config, but no evaluator task exists.
      **Built (2026-08-13)**: `relay_authority.{c,h}` gained the
      MANUAL/PROFILE/RULE ownership tags section 0 designed —
      `relay_authority_claim_mask()`/`relay_authority_release_mask()`/
      `relay_authority_get_owner()`/`relay_authority_manual_blocked_by_owner()`,
      per-relay (1-based, matching `kiln_io_set_relay`'s index), separate
      from and additive to the existing global/per-zone fault checks.
      `profile_executor.c` claims `PROFILE` for `claimed_relay_mask` on
      `profile_executor_run()`, hands it to `MANUAL` on
      `profile_executor_pause()` (per section 0's "pause is the one explicit
      way to hand a relay back to MANUAL" decision — not `NONE`, and
      resuming reclaims `PROFILE`), and releases to `NONE` on
      `profile_executor_halt()`. `dashboard_http.c`'s `relay_post_handler()`
      and `uart_bridge.c`'s `IO_CMD_SET_RELAY`/`IO_CMD_SET_RELAY_MASK`
      handlers now refuse a manual command (either direction, not just ON —
      a de-energize mid-window fights the executor's time-proportioning
      exactly as much as an unwanted energize) against an owned relay,
      before the existing safety-fault check runs. **Scope gap, left open
      on purpose**: `IO_CMD_SX_WRITE_REG`'s raw-register debug path was not
      extended — it addresses SX1509 pin bits, not the 1-based relay index
      this module's API takes, and mapping one to the other for a
      debug-only command was judged not worth the complexity this pass;
      it already refuses an ON while a safety fault is asserted (the
      pre-existing check), just not an ownership-only refusal. The rules
      engine's `RULE` tag is plumbed through but has no caller yet — no
      evaluator task exists (unchanged). Build-verified only; not exercised
      live (no relay hardware attached this session, and needs an actual
      profile run to observe the refusal). Build-verified via `ninja -j 24`
      in `firmware/KilnFW/build` — clean.
- [x] **Re-check `max_ramp_c_per_hr` against a running profile.** It is a
      run-start feasibility gate (section 5) and the reload path deliberately
      does not consume it, so an operator can now lower it mid-firing below
      what the running profile demands and nothing notices. Pre-existing, but
      config reload makes it reachable without stopping the run. Either
      re-run the feasibility check on reload and warn, or state in the UI
      **Built (2026-08-13)**: `reload_zone_config()` (`profile_executor.c`)
      compares the zone's live `zones_config_get_max_ramp()` ceiling against
      the currently-running segment's `ramp_c_per_hr` on every mid-firing
      config reload; a newly-infeasible ceiling logs
      `OPERATOR ACTION MID-FIRING` at WARN once (latched via
      `zone_runtime_t.max_ramp_warned` until it clears), matching this
      function's existing pattern for every other guard-threshold edit
      above it. Chose "warn, don't re-run/block" per the bullet's own
      second option — the running ramp itself is unchanged, since actually
      enforcing the new ceiling mid-ramp would mean either slowing an
      in-progress heat (its own safety question, not asked for here) or
      aborting the firing outright (which 6A.3's `continue_on_zone_trip`
      bullet, just above, treats as a deliberate, separate policy decision,
      not something a config edit should trigger as a side effect). No
      version bump needed (no new persisted field — reads existing config).
      Build-verified; not yet exercised live (needs a running firing to
      edit `max_ramp_c_per_hr` under, which needs the daughterboard or
      `KILNCTL_SIM_PLANT`, neither exercised this pass).

### 6A.8 Verification — how any of this gets trusted

The request is for a *robust* implementation, and none of the guards above can
be trusted until each one has been made to fire on purpose. There is no kiln
attached to this board yet, and several failures (welded contacts, a
disconnected element) cannot be provoked safely on a real one anyway.

> **Guard precedence, learned on-target 2026-08-12 and worth stating before
> the checklist below.** "Make each guard fire on purpose" turns out to be
> harder than it reads, because **several guards overlap on the same physical
> failure and the shortest window wins**. Measured, not theorised: a frozen
> thermocouple trips guard 1 at 481 s, not guard 7 at 600 s; a miswired
> (swapped) pair of thermocouples trips guard 1 at 478 s, not guard 2; and the
> scenarios that would produce guard 4's sustained excursion are caught first
> by guard 1 (dead element) or guard 3 (welded relay). That is defence in
> depth working — but it means a guard sitting unobserved is not evidence it
> is broken, *and* that a matrix row saying "guard N catches X" should name
> which guard catches X **first**. `docs/GUARD_TEST_MATRIX.md` now records
> both. Provoking guards 2, 4 and 7 specifically requires constructing the
> narrow conditions where their faster neighbours do not engage (e.g. duty
> above 0 but below `PROGRESS_DUTY_MIN`), which no test has done yet.
>
> **Guard 8 added to the observed list 2026-08-12**, once its threshold became
> settable: trip at 600 s on a two-zone run with `fault=tc_detached` and a
> tight 2 °C threshold. Precedence was not a problem here for a reason worth
> keeping: the faster guards were all dormant. Guard 1 needs
> `commanded_duty >= PROGRESS_DUTY_MIN` and the detached zone's relay was
> blocked, so no progress window ever opened. Five guards observed on target
> now (1, 3, 5, 6, 8); 2, 4 and 7 still need their narrow conditions built.

- [x] **Build a kiln plant simulator (host-side, single-zone slice)**,
      `App/test/sim_plant.c` + `sim_plant.h`. **Done (2026-08-11)**: first-order
      lag thermal model (heater power in, loss-to-ambient out, forward Euler)
      plus a ring-buffer sensor transport delay and a first-order lag on the
      reading itself, so a step in duty doesn't reach the "thermocouple"
      instantly. Standalone host C, no ESP-IDF dependency. **Not yet built**,
      still open: N coupled zones, inter-zone conductance, a radiative loss
      term, injectable faults (element dead, relay welded, TC detached/
      swapped/frozen/noisy), and the `KILNCTL_SIM_PLANT` Kconfig path that
      would let the on-target executor read from this sim instead of
      MAX31856/kiln_io — this pass only wired it into the host test binary
      below, not into firmware.
      **Update (2026-08-12): the multi-zone half is now built.** A `sim_kiln`
      layer was added to the same `sim_plant.{c,h}`: N coupled zones
      (`SIM_KILN_MAX_ZONES` 3) with a full `coupling_w_per_c[i][j]`
      conductance matrix (elements updated simultaneously from the same
      starting temperatures, so coupling isn't integration-order dependent),
      an optional per-zone radiative loss term
      (`radiative_coeff_w_per_k4`, Stefan-Boltzmann in absolute temperature
      — the thing that makes plant gain fall with temperature and one
      tuning wrong across a firing), a read-time `sensor_map[]` for modeling
      swapped connectors, deterministic per-tick sensor noise, and
      injectable per-zone faults: `ELEMENT_DEAD`, `RELAY_WELDED`,
      `TC_DETACHED` (electrically fine, physically out of the kiln body),
      `TC_FROZEN` (answers but stopped converting — deliberately noise-free
      so guard 7's bit-identity check can see it), `TC_OPEN` (NaN). The
      single-zone `sim_plant_step()` API is unchanged; its sensor half was
      factored into a shared `sensor_pipeline_step()` so a detached
      thermocouple can be driven from chamber air instead of its element.
      **`KILNCTL_SIM_PLANT` also landed, later the same day (2026-08-12)**:
      `App/drivers/sim_backend.{c,h}` plus a Kconfig menu ("Simulated plant
      (development only)", default **n**) compile the *same*
      `App/test/sim_plant.c` into the firmware — deliberately the same file
      the host tests use, since a second on-target copy of the model would
      be a second thing to keep honest. With it on, `profile_executor.c`,
      `autotune_engine.c`, and `dashboard_http.c` read simulated channels
      instead of MAX31856 ones, relay commands are fed to the model from
      the *post-safety-gate* decision in both `apply_relay()`
      implementations, and faults are injectable on the running board over
      `POST /api/sim` (`zone=<n>&fault=none|element_dead|relay_welded|
      tc_detached|tc_frozen|tc_open`), with `GET /api/sim` reporting each
      zone's true element temperature next to what its thermocouple claims.
      The model advances on wall-clock time (`esp_timer`), sub-stepped at
      ≤1 s with a 60 s catch-up cap, so any number of readers at any cadence
      can poll it without making simulated time run fast. Every entry point
      compiles to a `static inline` no-op when the option is off, so the
      production build is unchanged and there is no way to reach `/api/sim`
      from a production image.
      **Verified**: both configurations build clean (production default, and
      a sim-enabled build in a separate build dir; sim costs ~7 KB of app
      image). A sim image was then flashed over JTAG and **does boot and
      run** on the board.
      **Not yet done, no longer blocked**: the on-target guard walk (start a
      profile, inject each fault over `POST /api/sim`, watch the right guard
      trip) still hasn't been performed. It *was* blocked by the board's HTTP
      server being unreachable — which turned out to be **DRAM exhaustion**:
      the board reached `app_main` with 7 KB of free heap because this
      module's history buffer (59 KB `.bss`) and the autotune trace (70 KB)
      had claimed ~130 KB of ~196 KB before Wi-Fi started, so httpd could not
      allocate and the Wi-Fi driver could not even hold a client
      association. Both buffers are now packed (8 B and 2 B per sample
      respectively, unpacked at the API boundary so no caller changed) and
      the whole web UI is verified answering live over the fallback AP. See
      `docs/PROJECT_STATUS.md`'s 2026-08-12 on-target entry for the full
      trace, including the three logging/diagnosability defects fixed on the
      way (a safety-link retry-warning storm drowning every other log line,
      an unactionable task-start error message, and the log bridge silently
      replacing rather than teeing console output — that last one is what
      made the root cause visible at all).
- [x] **Host unit tests for `pid.c` and `thermal_guard.c`**. **Done
      (2026-08-11)**: `App/test/test_pid.c`, `test_thermal_guard.c`, and
      `test_heater_output.c` (added for the same reason, even though only
      pid.c/thermal_guard.c were named here — heater_output.c is the third
      pure module and the bug below was found through it), built and run with
      `App/test/build_host_tests.ps1` (MSVC, `cl.exe`, no ESP-IDF). 71
      checks, covering each guard's trip *and* non-trip case (healthy heating
      doesn't trip guard 1, a normal off/idle hold doesn't trip guard 3, a
      moving reading doesn't trip guard 7, never-having-settled doesn't trip
      guard 4). Also added `test_closed_loop.c`, wiring sim_plant + pid +
      heater_output + thermal_guard together the way profile_executor.c does,
      run for a simulated 4-hour firing: no false guard trip, reaches
      setpoint, and guard 3 still fires when the model is forced into a
      welded-relay condition — confirms the modules cooperate, not just that
      each passes in isolation.
      **Update (2026-08-12)**: `App/test/test_sim_kiln.c` added on top of the
      new multi-zone sim above — 96 checks became **120, all passing**. It
      covers (a) the model itself (a neighbor's elements really do heat an
      idle zone, zero conductance leaves it at ambient, the radiative term
      costs real ceiling temperature), and (b) one *provocation* per guard:
      each guard is now tripped by a physically-stated failure injected into
      the plant rather than by hand-fed numbers — dead element and detached
      thermocouple both trip guard 1, swapped connectors trip guard 2, a
      welded relay trips guard 3, a settled zone whose element dies at low
      duty trips guard 4, a low ceiling trips guard 5, an open TC trips
      guard 6 at exactly 3 reads, a frozen TC trips guard 7 at its 600 s
      window, and 1 °C of sensor noise on a healthy zone trips nothing.
      Guards 8 and 9 are still not covered (8 doesn't exist; 9 is a separate
      watchdog task, not a `thermal_guard` verdict).
      **Found and fixed a real bug in `heater_output.c` while writing these**:
      `heater_output_bangbang()` gated its debounce re-check on `want_on !=
      last_commanded_on` (the previous call's raw decision), not on `want_on
      != relay_on` (the actual output). Once `want_on` stopped changing
      call-to-call, the `if` block was never entered again, so a still-pending
      transition was silently dropped forever even though
      `since_last_change_ms` kept accumulating well past `min_on_ms`/
      `min_off_ms` — a bang-bang zone that requested ON once and then held
      that request could get stuck OFF permanently. Fixed to gate on
      `relay_on` directly; rebuilt, reflashed via OpenOCD, verified live.
- [x] **A test matrix, written down**, one row per guard: how it is provoked
      in the sim, how long it should take to trip, what the operator sees.
      This table belongs in `docs/` and is the artifact that makes "robust"
      checkable rather than asserted.
      **Done (2026-08-12)**: `docs/GUARD_TEST_MATRIX.md` — nine rows
      (guards 1–9) with provocation, threshold/window, expected time to
      trip, escalation (global fault bit vs per-zone block), and the exact
      `fault_reason`/`fault_guard` the operator sees, plus a coverage
      section separating what an automated host test actually asserts from
      what is only read off the source. Rows 8 and 9 are marked
      non-existent and not-a-`thermal_guard`-verdict respectively rather
      than padded out. It also documents a real wart worth fixing later:
      `/api/profile_exec` exposes the raw `thermal_guard_trip_t` enum, so
      `fault_guard: 7` means guard **6** and guard 7 reports 8 — see the
      enum-offset table in that doc.
- [x] **Autotune against the sim first** — the fitted `K`/`tau`/`L` can be
      compared to the sim's known parameters, which is the only place the
      identification math can be validated exactly.
      **Done**: the single-zone half landed 2026-08-11 in
      `App/test/test_pid_autotune.c` (fitted `K` within 2 %, `tau` within
      5 % of `sim_plant.c`'s ground truth). The **cross-gain half landed
      2026-08-12** in `test_sim_kiln.c`'s `test_cross_gain_matrix()`: a
      duty step is applied to zone 0 of a *coupled* two-zone kiln, both
      zones' traces are fitted with the same `pid_autotune_fit_fopdt()` the
      on-target 6A.5(b) matrix capture uses, and the fits are asserted to
      show a positive cross gain, smaller than the direct gain, over a
      slower path. That is the first exercise of 6A.5(b)'s math against a
      known non-zero cross gain — on-target it has still never filled a
      real cell (no thermocouple hardware).
- [ ] **Bench test before kiln test**: a small resistive load and a
      thermocouple (a soldering-iron element, a heat gun into a can) exercises
      the whole chain — real SPI reads, real I2C relay writes, real timing —
      at temperatures that cannot hurt anything.
- [ ] **First real firing is attended, low-temperature, and logged**, with the
      history/trace export from 6A.9 reviewed afterward before anything runs
      unattended.

### 6A.9 UI and telemetry additions this section implies

- [x] **History ring buffer entry gains `duty`** (and the guard state).
      **Done (2026-08-11)**: the ring buffer itself (section 0's design,
      never built until now) landed at the same time as the field it was
      supposed to gain -- `profile_executor.h`'s `profile_history_entry_t`
      is `{elapsed_s, actual_c, desired_c, duty, guard}`, 30s/sample, 2880
      entries (24h), scoped to the single zone `profile_executor.c` actually
      runs at a time (not "per zone" — 6A.5's concurrent multi-zone
      execution is unbuilt, so there's only ever one zone's history to
      keep). Paged accessors (`profile_executor_get_history()`/
      `_get_history_count()`) rather than an all-at-once copy — see the
      `/api/history.csv` bullet below for why that distinction mattered in
      practice, not just in theory.
- [x] **Dashboard graph** (section 2) overlays desired, actual, and duty.
      **Done (2026-08-11)**: canvas-based line chart on `main_page.html`
      (`historyChart`), no external chart library, fed by
      `GET /api/history.csv`, redrawn every 15s (the history itself only
      gains a sample every 30s) and on window resize. Guard trips are drawn
      as vertical red marks on the timeline. **Not built**: marking
      ramp-lock holds specifically — ramp-lock itself (TODO.md 6A.5(d))
      doesn't exist yet, so there's nothing to mark.
- [ ] **Settings → Thermocouples & Zones** grows: control mode
      (`OFF`/`BANGBANG`/`PID`), bang-bang hysteresis, per-band PID gains and
      fitted model, `max_temp_c`/`min_temp_c`, every guard threshold from
      6A.3, and an autotune launch/abort/results affordance. The page already
      hosts the PID fields per section 0.5's settled decision, so this stays
      one page — but it is getting large enough that the per-zone block should
      be collapsible.
      Partial: `control_mode` dropdown, `max_temp_c`/`min_temp_c`, and (this
      pass) per-zone relay `window_ms`/`min_on_ms`/`min_off_ms` inputs are on
      `zones_page.html`, and (also this pass, once 6A.4's step-test autotune
      landed) a working autotune launch/abort/results-accept card. Bang-bang
      hysteresis is still hardcoded (fixed 2C, not on the page), per-band PID
      gains/fitted model don't exist (single band only, gain scheduling
      unbuilt), and only `sanity_rate_c_per_min` of the guard thresholds is
      on the page (see 6A.3's "every threshold" bullet). Left unchecked
      because part of the bundle is still missing; the page also isn't
      collapsible yet despite now hosting even more per-zone fields than
      when that concern was first raised.
- [~] **Per-relay `window_ms`/`min_on_ms`/`min_off_ms` becomes
      page-configurable.** **Partially done (2026-08-11)**: landed on
      Settings → **Thermocouples & Zones** (`zones_page.html`), not Relays &
      Rules as this bullet names — `heater_output_cfg_t` is applied per
      *zone* in `profile_executor.c`/`autotune_engine.c` (a zone's relay
      group switches together as one unit; there is no per-individual-relay
      timing concept anywhere in the control path), so that's the
      granularity that was actually wired up: three new `zone_cfg_t` fields
      (`heater_window_ms`/`heater_min_on_ms`/`heater_min_off_ms`, 0 = use
      the firmware default), a `zones_config_get_heater_cfg()` getter, and
      both control-driving modules substituting the per-zone value when set.
      **Update (2026-08-12)**: the cycle count now IS on a page — Settings →
      Relays & Rules, fed by `relay_cycles` on `GET /api/status` (see 6A.1's
      contact-cycle bullet). `max_simultaneous_relays` did land on the Zones
      page with the load-staggering work; **still not built**: per-relay (as
      opposed to per-zone) timing, which the control path has no concept of.
- [x] **A live control-status endpoint** (`GET /api/control`). **Done
      (2026-08-11)**: dedicated endpoint (not just more fields bolted onto
      `/api/profile_exec`), backed by a new `pid_update_terms()` in
      `pid.c` (P/I/D/FF broken out; `pid_update()` is now a thin wrapper
      over it with `out_terms=NULL`, so there's one control-math
      implementation, not two that could drift — host-tested in
      `test_pid.c`). Reports mode, setpoint, actual, duty, the P/I/D/FF
      breakdown, and guard state. `ramp_lock_held` is present in the
      response but always `"n/a"` — reported explicitly rather than omitted,
      since ramp-lock (6A.5(d)) doesn't exist and a client should be able to
      tell "not currently held" apart from "this firmware has no concept of
      ramp-lock."
- [x] **CSV export** of the history buffer and of any autotune trace.
      **Done (2026-08-11)**: `GET /api/history.csv` and
      `GET /api/autotune/trace.csv`. **Found and fixed a real bug while
      live-testing this**: the first version of both handlers allocated one
      large buffer up front (~58KB entries + ~115KB text for history, ~69KB
      for the autotune trace) and `/api/history.csv` came back
      `500 out of memory` against the actual board — a size-report check
      of static DIRAM headroom before flashing looked fine and did not
      catch this, because the failure was against the separate runtime
      heap (already carrying Wi-Fi/lwIP/httpd), not static allocation.
      Fixed by paginating both `profile_executor_get_history()` and the new
      `autotune_engine_get_trace()` (oldest-first, caller-supplied
      start/count) and rewriting both handlers to stream via
      `httpd_resp_send_chunk()` in small (128-entry) batches — peak
      allocation is now independent of how many samples exist. Rebuilt,
      reflashed, both endpoints verified live returning `200` (empty-history
      output, since no firing has run on this hardware-less bench unit).
- [ ] The 2 s polling from section 2 is probably still fine for all of this;
      revisit push (WebSocket/SSE) only if watching a real firing proves it
      isn't.

### 6A.10 Suggested build order

1. `pid.c` + `thermal_guard.c` + `sim_plant.c` + host tests — no hardware
   needed, and this is where the design gets falsified cheaply if it is wrong.
2. `heater_output.c` + the per-zone gating extension to `relay_authority`
   (6A.6) + guard 6 (live thermocouple faults), which closes
   `docs/SAFETY_MODEL.md`'s top gap on its own, independent of everything else.
3. `profile_executor.c` with `BANGBANG` mode only, ramp-lock, history buffer,
   and the full guard suite armed. A working, safe, tuning-free firing engine.
4. `PID` mode + feedforward, single tuning band, tuned by hand or by a
   sim-derived starting point.
5. `pid_autotune.c` — step test, FOPDT fit, SIMC rules, cross-coupling matrix
   capture (6A.5b), results-proposal UI.
6. Ramp-ceiling estimate from the model (replacing/annotating the user-entered
   ceiling), RGA display, gain-scheduling bands.
7. Cross-feedforward decoupler (6A.5e), load staggering, relay-feedback
   autotune as the alternate method.

### 6A.11 Open questions, collected

- [ ] Do the on-board relays switch elements directly or drive external
      SSRs/contactors? (Sets the duty-window period and the contact-life
      budget — 6A.0.)
- [ ] Element power per zone and total supply/breaker capacity — decides
      whether load staggering (6A.5) is mandatory or optional.
- [ ] Maximum rated temperature of the kiln and of the thermocouples fitted
      (guard 5's `max_temp_c` is mandatory config; it needs a real number).
- [ ] Is there any active cooling or a vent that materially changes the plant
      (the rule engine can drive a vent relay — if it does, the identified
      model is only valid for one vent state, which the autotune procedure
      must record and the docs must state).
- [ ] Physical zone arrangement (stacked top/middle/bottom vs. side-by-side)
      — sets the expected coupling structure and a sanity check on the
      measured `K` matrix.
- [ ] The sanity rate the request deferred ("I will determine later"), plus
      first-pass values for every other threshold in 6A.3 — the defaults above
      are engineering guesses, explicitly labeled as such.

## 7. Documentation to write once the above is designed/built

- [x] `docs/WEB_UI.md` — page inventory, API/WebSocket message shapes,
      how it relates to the existing UART protocol (same device data,
      different transport).
      **Reconciled 2026-08-13**: already written and current (721 lines,
      last dated entry 2026-08-12) — this checklist item was simply never
      checked off when the doc landed. Covers the page inventory and API
      shapes as asked; still polling, not WebSocket, matching section 2's
      own unchecked "driven live" bullet (the "different transport" framing
      in this bullet was aspirational, not yet true either).
- [x] `docs/WIFI_PROVISIONING.md` — AP fallback vs. local-only mode, the
      Kconfig default password and why a default exists, resilience
      guarantees.
      **Reconciled 2026-08-13**: already written and current (508 lines,
      last dated entry 2026-08-12, covers the mode-toggle redesign and the
      saved-networks/8.4 work). Same as above — landed, never checked off.
- [x] `docs/PROFILES.md` — profile format, the execution engine, relay
      rule-engine syntax.
      **Reconciled 2026-08-13**: already written (436 lines). Same pattern.
- [x] `docs/PID_CONTROL.md` — **Done (2026-08-11)**: module layout, control
      modes, the loop's form, the guard table, the autotune flow, and the
      host-test summary, each cross-referenced to TODO.md 6A's dated
      "Implemented"/"Not built" notes rather than duplicating them. **Not
      built yet, and said so in the doc itself**: the multi-zone interaction
      handling (6A.5 doesn't exist), and the guard test matrix (6A.8's third
      bullet — a written, one-row-per-guard table is still owed; the guard
      coverage itself is exercised by `test_thermal_guard.c`, just not yet
      transcribed into that specific table format).
      **Both of those are now stale** (2026-08-12): 6A.5(a)/(b)/(d) shipped
      2026-08-11, and the guard test matrix now exists as
      `docs/GUARD_TEST_MATRIX.md`.
      **Reconciled 2026-08-13**: `PID_CONTROL.md` was in fact updated
      (references concurrent multi-zone execution, ramp-lock, and
      `GUARD_TEST_MATRIX.md` throughout, most recently dated 2026-08-12) —
      this note about it being the doc "to fix next" was itself the stale
      part. Not re-verified against this session's `pid_seed_bumpless()`
      ff-signature change or `continue_on_zone_trip` (see 6A.2/6A.3 above);
      those are small enough additions that the doc's existing description
      of both mechanisms is still accurate in substance, just not updated
      with today's date.
- [x] Update `docs/SAFETY_MODEL.md`'s summary table with the new relay
      caller (profile executor / web UI) and confirm every row still
      holds once it's a real third caller instead of a hypothetical.
      **Done (2026-08-13)**: the "only place in the firmware that directly
      gates a relay command" claim near the top was stale (the check moved
      into `relay_authority.c` and gained two more real callers,
      `dashboard_http.c`'s `POST /api/relay` and `profile_executor.c`,
      alongside `uart_bridge.c`) — corrected in place with a dated update
      block rather than rewritten, so the original text's history stays
      legible. Also documented `relay_authority_zone_blocked()` (the
      per-zone gate layered on top of the global one) and this session's
      `continue_on_zone_trip` run-level abort policy, which deliberately
      does *not* touch either relay-authority gate. Every row in the
      capability table itself was already accurate (spot-checked against
      current code, not rewritten).
- [x] Update `docs/PROJECT_STATUS.md` once any of this actually lands.
      **Done (2026-08-13)**: appended a dated entry (see the doc's own
      newest section) summarizing this session's changes: the
      `esp_wifi_set_config(AP)`/`ESP_ERR_WIFI_MODE` boot-ordering fix,
      `pid_seed_bumpless()` gaining the feedforward subtraction, the
      cooling-limited PID diagnostic, the mid-firing `max_ramp_c_per_hr`
      re-check, the `zones_http.c` NVS version-vs-size ordering bug found
      and fixed, and the `continue_on_zone_trip` abort-on-trip default
      policy — plus the same status this whole pass carried throughout:
      build-verified and reflashed over OpenOCD, board confirmed booting
      and answering the UART protocol, but none of the new behavior
      exercised against real hardware (no daughterboard/relay expander
      attached this session).

## 8. Storage partitioning, boot-time compatibility, and setup UX

Requested 2026-08-12, after the Wi-Fi credentials were moved into their own
`wifi_nvs` partition and the board was found reporting itself unprovisioned
while holding valid credentials. Five related asks, in the user's words:
kiln settings in their own NVM section, profiles in another, every
non-volatile section checked for firmware-version compatibility **on every
boot**, a config-wizard page showing what setup is complete and what is not,
and a network page that always shows the current mode/status while letting
you switch — with saved networks you can forget, whether or not they are
currently in range, "basically the same functionality as an Android phone
but with the ability to be an access point."

Nothing in this section is built. The notes below are a starting point, not
decisions already taken.

### 8.1 One partition per concern

Today: `wifi_nvs` (0x187000, 24K, credentials only, added 2026-08-12) and
the default `nvs` (0x9000, 24K) holding *everything else* — zone config,
relay rules, profiles, contact-cycle counters, the run-state breadcrumb.

- [x] **Split the default partition three ways**: `wifi_nvs` (exists),
      `kiln_nvs` (zone config, relay rules, guard thresholds, calibration,
      identified plant models, contact-cycle counters, run-state
      breadcrumb), and `profiles_nvs` (fire profiles only). The reason is
      the one that drove the Wi-Fi split, and it is a *failure-mode*
      argument rather than a tidiness one: **NVS corruption recovery is
      partition-wide.** One bad blob forces an erase that takes every
      namespace sharing that partition with it. A corrupt profile should
      not cost the operator their zone calibration, and neither should cost
      them the network access needed to fix it.
      **Built (2026-08-13)**: `kiln_nvs` (0x18D000, 64K: zones, rules,
      relay_cycles, run_state) and `profiles_nvs` (0x19D000, 384K) added to
      `partitions.csv`, carved from the previously-unused 460K tail.
- [x] **Sizing and layout.** 0x18D000..0x200000 (460K) is free on the 2 MB
      part. Profiles are the only one of the three that grows with use, so
      that partition wants the most headroom; zone/rule config is bounded
      and small. Keep `nvs`, `phy_init`, `factory` and `wifi_nvs` at
      byte-identical offsets when adding entries, exactly as the Wi-Fi
      split did — moving a partition silently invalidates everything stored
      in it. Open question: whether the then-mostly-empty default `nvs`
      stays as a scratch/migration area or is left deliberately unused. It
      cannot be removed without moving `phy_init`. Default `nvs` stays as a
      read-only migration source, never written again.
- [x] **Migration, one-time, per section.** Same shape as `wifi_prov.c`'s:
      read the old location, write through to the new one, do not delete
      the old copy (so a firmware rollback still finds working data), log
      once. Each section migrates independently — a failed profile
      migration must not block the zone-config one.
      **Built (2026-08-13)**: `zones_http.c`, `rules_http.c`,
      `relay_cycles.c`, `run_state.c`, `profiles_http.c` each got their own
      `nvs_partition_init()` (copied from `wifi_prov.c`'s pattern) and a
      one-directional `migrate_from_default_partition()`; `profiles_http.c`
      migrates per-slot so one bad slot can't block the rest.
- [x] **Recovery scoping.** Every module must use
      `nvs_flash_init_partition()` / `nvs_flash_erase_partition()` against
      *its own* partition. The blanket `nvs_flash_erase()` that motivated
      the Wi-Fi split (fixed 2026-08-12) is the pattern to keep out. The
      corollary: whoever owns the default-partition init today
      (`wifi_prov_start()`, by historical accident) should stop being the
      de-facto owner of everyone else's storage.
      **Built (2026-08-13)**: each module now owns its own partition's init
      and writes; `wifi_prov_start()` still initializes the default `nvs`
      partition (comment updated), but only because every module's
      migration read depends on it running first — it no longer owns
      anyone's persistence, only that one shared read-only precondition.
- [x] **Decide what "reset kiln config" means** once this exists. It
      becomes a per-partition operation, which is the point — but the UI
      then has to say precisely what each reset destroys, and "reset
      everything except Wi-Fi" is the one an operator actually wants when a
      board misbehaves somewhere inconvenient. (user says give the user a choice
      and offer a factory default option too)
      **Built (2026-08-13)**: new `App/drivers/factory_reset.c/.h`, registering
      `POST /api/factory_reset` on the shared httpd instance (same
      `wifi_provision_http_get_server()` pattern as every other `*_http.c`).
      Body is `application/x-www-form-urlencoded`, `scope=wifi|kiln|profiles|all`
      — matching this codebase's existing form-encoded POST convention
      (`http_form_find_field`) rather than introducing a JSON parser. No
      default scope: a missing or unrecognized value is a clean 400
      ("scope missing or malformed" / "unrecognized scope"), never a guess.
      Each scope maps to `nvs_flash_erase_partition()` on exactly the
      partition(s) it names (`wifi` → `wifi_nvs`; `kiln` → `kiln_nvs`;
      `profiles` → `profiles_nvs`; `all` → all three) — the same scoped-erase
      pattern every module's own `nvs_partition_init()` already uses, not the
      blanket `nvs_flash_erase()` this split was fixed to retire. After
      erasing, a short-lived task delays 500ms (so the HTTP response reaches
      the client first) and calls `esp_restart()`, since every module's load
      only runs at boot. `main_page.html` grew a "Danger zone" section with
      one confirm-then-POST button per scope, styled off the page's existing
      fault color, each button's confirm dialog naming exactly what that
      scope destroys. `main.c` calls `factory_reset_http_start()` alongside
      the other settings-page `_http_start()` calls, after
      `profiles_http_start()`. `App/drivers/CMakeLists.txt` SRCS updated; no
      new EMBED_TXTFILES entry (the UI lives in the existing `main_page.html`,
      not a separate page). Build verified clean via `ninja -j 24` in
      `firmware/KilnFW/build` (per the documented idf.py python-env workaround). **Not
      flashed or exercised on real hardware** — in particular, the actual
      erase-then-reboot cycle, and whether the "ok, rebooting" response
      reliably reaches the browser before the socket drops, are unverified
      on a board.

### 8.2 Boot-time compatibility check for every non-volatile section

The concrete problem has already bitten twice in one day: `zones_http.c`
treats any size change in its blob as corruption and silently starts
unconfigured, so adding one field wipes the operator's setup behind a single
`ESP_LOGW`. `run_state.c` versions its record; nothing else does.

- [x] **A schema version on every persisted structure**, checked on every
      boot, stored *next to* the data rather than inferred from its size.
      Size-as-version is exactly what makes today's behaviour
      indistinguishable from corruption.
      **Built (2026-08-13)**: `zones_cfg_t`, rules config, the profile
      struct, and the relay-cycles blob all gained a `uint8_t version`
      field + `_VERSION` `#define`, matching `run_state.c`'s existing
      pattern (which was left untouched).
- [x] **Three outcomes, not two.** Current code has "loads" and "wipe it".
      Add the middle one: *recognised older version* → migrate forward,
      logged, preserving what the operator entered. Reserve the wipe for
      genuinely unreadable data, and surface it in the UI afterwards rather
      than in a boot log nobody reads.
      **Built (2026-08-13)**: each module's loader now checks version
      before size-mismatch-wipe; a known-older version calls a
      `migrate_..._v1_to_current()` hook (currently a logged no-op, since
      v1 is the first version everywhere — the hook point is what matters);
      a newer-than-firmware version refuses to load and leaves flash
      untouched rather than wiping, so a rollback doesn't eat newer data.
      **Correction, found and fixed in `zones_http.c` (2026-08-13, later the
      same day this was first checked off)**: "checks version before
      size-mismatch-wipe" turned out not to be true — every module's loader
      actually checked `len != sizeof(*out_cfg)` **before** looking at
      `version`, so the one case this whole bullet exists for (a struct
      that *grows* — the only way a real firmware update adds a field) hit
      the size check first and got wiped every time, never reaching the
      migration path at all. `zones_http.c`'s `nvs_load_from()` fixed:
      version now decides first, using only "too short to even contain a
      version byte" as automatic corruption; the size check only applies
      once the blob claims to already be the current version (where a
      mismatch really can only mean corruption). Exercised for real by this
      same pass's own `continue_on_zone_trip` field addition (v1→v2) — the
      fix isn't theoretical, it is what makes that addition (and 6A.3's
      abort-on-trip default policy bullet below) safe to ship at all.
      **The other three modules with this exact pattern
      (`rules_http.c`, `relay_cycles.c`, `profiles_http.c`) were not
      fixed** — same bug, confirmed present by inspection, but none of them
      needed a field added in this pass, so fixing them was left for
      whichever future change actually grows one of those structs (fix it
      then, the same way, rather than speculatively touching three files
      nothing in this pass exercises). `run_state.c` also has the pattern
      but was deliberately excluded from 8.2's scope originally (it already
      had its own version field before this section existed) and is lower
      stakes — losing the "what was running" breadcrumb to a wipe is
      recoverable, losing zone/rule/profile config is not.
- [x] **One boot-time report.** A single place that walks every section
      (`wifi_nvs`, `kiln_nvs`, `profiles_nvs`, and whatever else exists) and
      records per section: present / version / matches this firmware /
      migrated / unreadable — exposed over HTTP for the wizard in 8.3 to
      render. It must handle a partition being *absent entirely*: a new
      firmware on an old flash layout, which `wifi_prov.c` already treats as
      `ESP_ERR_NVS_PART_NOT_FOUND`, and which is precisely what a fleet
      update creates.
      **Built (2026-08-13)**: new `App/drivers/nvs_report.c/.h`,
      `nvs_report_capture()` called from `main.c` after every NVS-owning
      module has started, exposed as `nvs_sections` on `/api/status`
      (`dashboard_http.c`). **Partition-granularity only** (present/mounted
      per partition) — does not yet report per-blob version/migrated status
      within a partition; extend if 8.3's wizard needs that finer detail.
- [x] **Forward compatibility, deliberately chosen.** Decide what a
      *newer*-than-expected version means: refuse and keep the data (safest,
      and it supports firmware rollback) versus wipe (never). Write the
      decision at the code — a rollback that eats the operator's config is a
      worse outcome than a firmware that refuses to fire until updated.
      **Built (2026-08-13)**: refuse-and-keep chosen everywhere, documented
      as a comment at each version-check site (see "Three outcomes" above).
- [x] **Tie it to the guards, not only the UI.** A kiln whose zone config
      failed to load must not be startable. Today an unconfigured zone
      simply cannot be commanded, which is safe by accident; make it
      explicit, and say so on the dashboard.
      **Built (2026-08-13)**: explicit `s_zones_config_valid` flag in
      `zones_http.c`, whole-partition granularity (the load already is
      all-or-nothing) — false on the wipe path, the newer-refuses path, an
      NVS partition that failed to come up, or a never-created namespace
      (first boot); true only after a real decoded load (current version or
      a migrated older one) or a freshly validated `POST /api/zones`.
      Exposed as `zones_config_is_valid()` (`zones_http.h`).
      `profile_executor_run()` and `autotune_engine.c`'s `begin_run_locked()`
      now refuse explicitly with a clear reason when it's false, ahead of
      (not instead of) the existing relay_mask==0 checks and
      relay_authority's gates — those are unchanged. `/api/status`
      (`dashboard_http.c`) reports it as `zones_config_valid` for the
      on-device dashboard and pc_tools' Zones panel.

**Needs verification (2026-08-13)**: `idf.py build` is clean (verified via
`ninja -j 24` in `firmware/KilnFW/build`, per the documented idf.py python-env
workaround). Not yet flashed/tested on hardware — in particular, confirm on
a board with real saved zone/rules/profile/relay-cycle data that the
one-time migration off the old default `nvs` partition actually carries it
forward into `kiln_nvs`/`profiles_nvs` rather than starting fresh, and that
`zones_config_valid` reads `true` after that migration and `false` on a
genuinely first-boot/unconfigured board (no hardware attached to this
session to check either).

### 8.3 Config wizard page: what is set up, what is not

- [x] **A page that answers "is this kiln ready to fire?"** as a checklist
      with per-item status and a link to the page that fixes each one.
      Candidates, all derivable from state that already exists: network
      configured; thermocouple count and zone mapping set; relays assigned
      to zones; control mode chosen per zone; guard limits (`max_temp_c`
      especially) set; calibration entered; at least one profile saved;
      autotune run per zone (or gains entered by hand); hardware present and
      answering (`io_ready` / `thermo_ready` / `safety_ready` from
      `/api/status`); every storage section compatible (8.2's report).
- [x] **Distinguish "not done" from "cannot be done yet."** Assigning
      relays to zones is meaningless before the thermocouple count is set,
      and autotune cannot run without a zone. Order and gate the items
      rather than presenting a flat list of red crosses.
- [x] **Distinguish "unset" from "deliberately off",** which this firmware
      already cares about: `max_temp_c == 0` means no ceiling, and
      `cross_zone_max_delta_c == 0` disables guard 8. Both are legitimate
      choices and neither should nag forever — but "I chose this" has to be
      recordable, or the wizard becomes noise that gets ignored, which is
      worse than no wizard at all.
- [x] **It is a status page first and a wizard second.** The value is
      answering "what is missing" on a board someone else set up six months
      ago, not walking a first-time user through screens in order.

**Built (2026-08-13)**: `App/drivers/readiness_http.{c,h}` + embedded
`readiness_page.html`, serving `GET /readiness` (the page) and
`GET /api/readiness` (the JSON it polls every 5s). Pure read-only
aggregator — no new storage, no new persisted fields: every item is computed
by calling the getters `zones_http.h` / `profiles_http.h` / `wifi_prov.h` /
`nvs_report.h` already expose, plus one new getter,
`dashboard_http_get_hw_ready()`, added to mirror `/api/status`'s
io_ready/thermo_ready/safety_ready without a second copy of that read logic.
Registered in `main.c` after `nvs_report_capture()`, added to
`CMakeLists.txt` SRCS/EMBED_TXTFILES, linked from `main_page.html`'s
Settings section. `/api/readiness` returns
`{"items":[{"key","label","status","detail","fix_url"}, ...]}` with
`status` one of `ok` / `not_done` / `cannot_yet` / `deliberately_off`. The 11
items: network configured (AP-only mode reports `deliberately_off`, not
`not_done`); thermocouple count/zone mapping; relays assigned to zones
(`cannot_yet` until thermo count is set); control mode chosen per zone (see
gap below); guard limits `max_temp_c` (`deliberately_off` when every
configured zone reads 0, per the field's own documented convention); the
guard-8 `cross_zone_max_delta_c` (`cannot_yet` below 2 zones, since the
guard is inert otherwise; `deliberately_off` at 0); calibration entered
(informational only, see gap below); at least one profile saved; autotune
run per zone or gains entered by hand (either satisfies it, per this
section's own wording); hardware present and answering; storage sections
compatible (`zones_config_valid` plus every `nvs_report_get()` section's
present/mounted).

Two honest, documented gaps left as `not_done`/`ok`-only rather than
inventing a distinction the storage doesn't carry: (1) **control mode**
— `zone_control_mode_t`'s OFF (0) is both the zero-initialized default of
an untouched zone and a legitimate "this zone isn't used" choice
(`zones_http.h`'s own doc comment), and there is no separate "explicitly
set" bit the way `max_temp_c`/`cross_zone_max_delta_c` effectively have via
their documented 0-means-off convention — so this item reports `ok` as soon
as the zone exists rather than falsely claiming `deliberately_off` when it
cannot actually tell. (2) **calibration** — `cal_offset_c` has no such
documented "0 is a deliberate choice" convention (a truly zero offset is
also a plausible honest reading), so a 0 there is reported `not_done`
rather than invented as `deliberately_off`. Closing either gap would need a
new persisted "explicitly set" flag per field, which this pass deliberately
did not add — TODO.md 8.3's brief was to read what already exists, not grow
the storage schema. Build verified clean via `ninja -j 24` in `firmware/KilnFW/build`.

### 8.4 Network page: live status, mode switch, and saved networks

Today the page shows provisioning controls and polls `/status`; there is one
set of station credentials and no way to forget them.

- [x] **Always show current status, on the same page as the switch** (2026-08-12):
      current mode (home/AP), whether the station is joined and to what,
      signal strength, IP address, the mDNS name (`kiln.local`), and — in AP
      mode or while falling back — the AP's own SSID and how many clients
      are connected. The mode toggle exists; the status half is what is
      missing, and a control that does not show its own result is the thing
      that has been unsatisfying about this page since the 2026-08-11
      redesign.
      **Built (2026-08-12)**: added `wifi_prov_get_sta_rssi()` + 
      `wifi_prov_get_ap_client_count()` to `App/drivers/wifi_prov.{c,h}`;
      extended `/status` endpoint JSON; redesigned `wifi_provision_page.html`
      with formatted status table showing mode/state/network/IP/signal/clients/mDNS.
      **Needs verification**: build + live test on hardware.
- [x] **Multiple saved networks, Android-style.** A list, each entry with
      SSID, saved/not, in-range/not (from a scan), a currently-connected
      marker, and a **Forget** action. Auto-join should prefer the strongest
      in-range saved network, with a documented tie-break. This is a real
      change of shape: `wifi_prov.c` stores exactly one SSID/password pair,
      so it needs a bounded list (5–10 entries) in `wifi_nvs`, versioned per
      8.2.
      **Built (2026-08-13)**: `wifi_prov.c` now stores a versioned
      `saved_nets_blob_t` (up to `WIFI_PROV_MAX_SAVED_NETWORKS` = 8 entries)
      under `NVS_KEY_SAVED_NETS` in `wifi_nvs`, 3-outcome-versioned per 8.2;
      one-time migration wraps a pre-8.4 single ssid/password into `nets[0]`
      the first time this runs. New API: `wifi_prov_add_network()`,
      `wifi_prov_forget_network()`, `wifi_prov_get_saved_networks()`
      (SSID-only — see "never display a stored password" below).
      `select_and_apply_join_candidate()` scans and picks the
      highest-RSSI in-range saved network; **documented tie-break**: equal
      RSSI favors the earlier entry in the saved list; no in-range match
      falls back to list order from index 0. Additionally (per a follow-up
      request, same day): a new periodic `rescan_timer_cb()` (every
      `WIFI_AP_FALLBACK_RESCAN_INTERVAL_MS` = 30s) re-scans and re-picks
      whenever `mode == WIFI_PROV_MODE_HOME` but the board isn't currently
      connected — i.e. sitting in AP fallback the operator didn't choose —
      so a saved network coming back into range is rejoined without
      waiting for the next disconnect event. **Correctness note**: the
      scan-based tie-break is deliberately NOT called from
      `on_wifi_event()`'s `WIFI_EVENT_STA_DISCONNECTED` branch, because
      that handler runs on the Wi-Fi driver's own event-loop task and
      `wifi_prov_scan()` blocks; that branch just retries the
      already-configured target immediately (fast, non-blocking), and the
      periodic timer (running on the separate esp_timer service task) is
      the only place the blocking scan-and-repick logic runs outside a
      caller's own HTTP-handler/app_main task.
- [x] **Show saved-but-not-detected networks** rather than hiding them —
      that is exactly how an operator diagnoses "the kiln cannot see the
      shop Wi-Fi from where it is standing". Merge scan results and the
      saved list into one view with clear markers, not two separate lists.
      **Built (2026-08-13)**: new `GET /networks` (`wifi_provision_http.c`)
      merges `wifi_prov_get_saved_networks()` with `wifi_prov_scan()` by
      exact SSID match into one array with `saved`/`in_range`/`connected`
      flags; degrades to saved-only (all `in_range:false`) if scanning
      isn't currently possible (AP-only mode or a transient scan failure)
      rather than erroring the whole response.
      `wifi_provision_page.html`'s `renderNetworkList()` replaces the old
      scan-only list, showing a "Saved" badge, the existing signal-bar
      helpers when in range, and a highlighted row when connected.
- [x] **Keep the AP capability first-class**, which is where this diverges
      from a phone: the board must stay reachable when no saved network is
      in range, so the fallback AP is a guaranteed floor rather than an
      afterthought mode. Forgetting the last saved network must therefore be
      *allowed*, not blocked — it lands the board in AP mode, which is
      recoverable, and the UI should say so before the operator confirms.
      **Built (2026-08-13)**: `wifi_prov_forget_network()` has no
      last-network guard (`has_creds` is now simply `count > 0`, so 0 saved
      networks falls through to the existing AP-fallback logic
      unmodified). The client-side warning lives in
      `wifi_provision_page.html`'s `forgetNetwork()`, gated on
      `savedCount === 1`, shown before the `POST /forget` request fires.
- [x] **Never display a stored password**, and make sure "forget" actually
      erases it rather than just unlisting the entry.
      **Built (2026-08-13)**: `wifi_prov_get_saved_networks()`'s output
      type (`wifi_prov_saved_network_t`) has no password field at all — a
      password is structurally unable to leave that function. Verified no
      `password` reference exists in either new HTTP handler
      (`networks_get_handler`/`forget_post_handler`). `wifi_prov_forget_network()`
      compacts the array in place and re-persists the whole blob with the
      decremented `count` — the forgotten entry is no longer reachable
      through `count` or any API, matching "forget" rather than "unlist".
      Note: the vacated struct slot's raw bytes past the new `count` aren't
      explicitly zeroed before the blob write, so stale password bytes may
      linger unindexed in flash past the live `count` boundary — fine for
      "no longer displayed or usable," worth a follow-up if a stricter
      "erased from flash" guarantee is ever required (e.g. before RMA'ing a
      board).
      **Follow-up (2026-08-13, explicit user request)**: the board's OWN AP
      SSID+password text boxes must always show the current values, unlike
      any *saved network's* password. Confirmed with the user this is a
      deliberate, scoped exception (only the board's own AP identity, never
      a home network password) before building it. New
      `wifi_prov_get_ap_password()` in `wifi_prov.{c,h}`; `/status` gained
      `ap_password` (plaintext, unauthenticated endpoint — accepted
      tradeoff per the user's confirmation, same reachability as the rest
      of this HTTP server); `wifi_provision_page.html`'s `poll()` now
      pre-fills `#apPassword` the same way it already pre-fills `#apSsid`
      (skipped while the field has focus, so it doesn't clobber an in-
      progress edit).
- [x] **Fix the provisioned/unprovisioned disagreement first** (its own open
      item above): on 2026-08-12 a board holding valid credentials reported
      itself unprovisioned while the migration read the same namespace and
      joined first try. A saved-networks list built on a loader that cannot
      reliably tell whether credentials exist would inherit that bug and
      multiply it by the number of entries.
      **Fixed (2026-08-13)**: see the bug entry above — `has_creds` now
      inferred from a non-empty `ssid` when its own flag key is absent.

**Needs verification (2026-08-13)**: `ninja -j 24` build is clean, all
object files confirmed newer than their sources. Not yet flashed/tested on
hardware. In particular: a board with a real pre-8.4 single saved network
migrates correctly into `nets[0]` and still joins on boot; two saved
networks and the board actually prefers the stronger one (test by moving
between two known APs); forgetting the last saved network drops the board
into AP mode without stranding it; the periodic 30s rescan
(`rescan_timer_cb`) actually rejoins a saved network that comes back into
range without waiting for a disconnect event; and the flagged-but-accepted
design tradeoff that `on_wifi_event()`'s disconnect handler no longer
re-scans on every disconnect (moved to the periodic timer instead, to keep
that blocking scan off the Wi-Fi driver's own event-loop task) doesn't
introduce a noticeably slower reconnect in the common case where the
already-active network is just flapping.

### 8.5 Sequencing

The dependency order here is real rather than bureaucratic: 8.2's versioning
should land **before** 8.1 splits the partitions, so the split's own
migration is version-checked instead of size-guessed; 8.1 before 8.4's saved
list, which needs somewhere versioned to live; and 8.3 last, since the wizard
is mostly a view over state the other three establish. The exception is
8.4's status display, which depends on none of it and is the smallest useful
thing on this list.

---

## 9. Firmware updates — ESP OTA and relaying the Pico's image

Full design, including the interlocks and the authentication both paths share:
[`../CommonFW/docs/UPDATE_PROTOCOL.md`](../CommonFW/docs/UPDATE_PROTOCOL.md).
The RP2040 half is [`../SaftyFW/docs/BOOTLOADER.md`](../SaftyFW/docs/BOOTLOADER.md).

Two things make this more than a normal OTA feature.

**The partition table has to change, and only a cable can change it.** OTA needs
`otadata` plus two app slots; a partition table can only be written over serial.
**You cannot OTA your way into being OTA-capable**, so the first flash of the new
table is a one-time USB operation that has to happen before any of this is
useful. `partitions.csv` also documents at length why `nvs`, `phy_init` and the
three NVS partitions above the app must keep their current offsets — live data
sits in them, and the split exists so a config wipe cannot strand the board off
Wi-Fi. Only `0x10000`..`0x187000` may be re-carved.

**The safety processor's image travels through here.** The ESP is the only thing
authorising a Pico update, which makes this endpoint the safety processor's
attack surface as well as its update path.

### 9.0 Mutual version compatibility (prerequisite)

Not strictly an update feature — it lands with the link — but the update paths
depend on it, so it is tracked here as a gate. Design:
[`../CommonFW/docs/LINK_PROTOCOL.md`](../CommonFW/docs/LINK_PROTOCOL.md),
`ANNOUNCE_VERSION`.

- [x] `ANNOUNCE_VERSION` = `0x0F` sent unprompted at boot, repeated against loss,
      and re-sent on every `boot_id` change — a Pico that just rebooted has
      forgotten who it was talking to. `safety_link.c`'s
      `safety_link_send_announce_version_burst()` sends a 4-frame burst
      (250 ms apart) at `safety_poll_task` startup and again whenever
      `safety_apply_fw_version()` sees the Pico's `boot_id` change. **2026-08-18,
      later same day**: the shared `kilnlink_announce.{c,h}` codec for this
      frame's payload was added to `CommonFW` (host-tested,
      `test/vectors/announce_vectors.json`) — at that point `safety_link.c` still
      built the frame by hand rather than calling into it. **2026-08-19:** closed —
      `safety_build_announce_version_payload()` now calls `kilnlink_announce_encode()`
      (component registered in `components/kilnlink/CMakeLists.txt`'s `SRCS`, which
      previously omitted `kilnlink_announce.c`); same fields, same burst
      cadence/timing, verified under `idf.py build`. `safety_parse_fw_version()` is
      untouched and stays hand-rolled on purpose — it parses the Pico's `FW_VERSION`
      (`0x0B`) reply, a longer, distinct wire layout (adds `config_version`/
      `config_crc`) that `kilnlink_announce_decode()`'s fixed length check would
      reject; the ESP never receives an inbound `ANNOUNCE_VERSION` frame itself
      (SaftyFW-side inbound parsing note below), so there is no receive-side call
      site for this codec in KilnFW
- [x] `KILNLINK_MIN_COMPATIBLE` published alongside the protocol version, and
      compatibility evaluated in **both** directions —
      `safety_link_versions_compatible()` implements
      `peer.protocol >= self.min_compatible && self.protocol >= peer.min_compatible`
      per `LINK_PROTOCOL.md` sec 4
- [x] A mismatch is treated exactly like a dead link: `SAFETY_FAULT_SRC_SAFETY_LINK`,
      heating blocked. `safety_update_health()` folds `version_mismatch` into
      the same fault-source assert as link loss. **A running firing being
      aborted on mismatch specifically was not verified this pass** — that is
      the same 30 s-silence abort path `ROADMAP.md` M6 tracks separately and
      was out of scope here
- [x] The GUI names **both** versions and which one is older. "Incompatible"
      without saying which side to update generates a question instead of
      answering one. **2026-08-18, later same day:** reuses the existing
      "Safety Processor" card (`5a3e459`'s card, `ui_page_home.c`'s
      `build_safety_card()`/`s_link_version_label`) rather than a new one, per
      10.1a's shared-backend rule. `safety_link.h`'s `SafetyLinkClass` gained
      `peer_protocol_version`/`peer_min_compatible` fields (the numbers
      behind the existing `peer_version_known`/`peer_version_compatible`
      bools, populated in `safety_apply_fw_version()`);
      `safety_link_get_peer_version_status()` grew two more out-params to
      expose them. `dashboard_http.c`'s `dashboard_get_status()` (the shared
      backend `ui_page_home.c` and `GET /api/status` both read) now carries
      `self_protocol_version` (this build's `UART_PROTOCOL_VERSION`, always
      known), `link_version_known`/`link_version_compatible`, and
      `peer_protocol_version`/`peer_min_compatible`; `GET /api/status`'s JSON
      gained matching fields (`null` until known, same convention as
      `safety_temp_c`). The LCD card shows "Link version: ESP N / Pico M
      (OK)" when compatible, or "ESP N / Pico M -- INCOMPATIBLE, <ESP|Pico> is
      older. Update ESP first." when not (older side picked by comparing the
      two protocol numbers), and "---" whenever `link_version_known` is
      false (no Pico attached, `ROADMAP.md` M0's current bench state).
      **Not verified against real mismatched hardware this pass** -- no Pico
      is attached in this environment, so only the "unknown, shows ---" path
      has actually been exercised; the "INCOMPATIBLE" rendering was verified
      by code inspection, not a live mismatched pair. `idf.py -C
      firmware/KilnFW build` succeeds
      (`KilnCtrl.bin` 0x1546d0 bytes, 9% free in `factory`)
- [x] The compatibility floor — framing, `ANNOUNCE_VERSION`, `FW_VERSION`,
      `UPDATE_*` — stays functional across any mismatch, so the fix can be pushed
      over the link rather than needing a debug probe. Neither side's frame
      dispatch (`safety_link.c`'s switch on `cmd`, `SaftyFW`'s
      `link_task.c` equivalent) gates ids `0x00`-`0x0F` on
      `peer_version_compatible` — **not verified end-to-end against a live
      mismatch this pass**, no hardware bring-up with two deliberately
      mismatched builds was run
- [x] **Explicit `GET_FW_VERSION` (`0x0B`) request, retried until answered**
      (2026-08-19, `ROADMAP.md` M6). Distinct from `ANNOUNCE_VERSION` above:
      that's the ESP telling the Pico who it is, unprompted; this is the ESP
      explicitly asking the Pico who *it* is. A Pico already running before
      this ESP boots has no reason to volunteer `FW_VERSION` on its own
      (`LINK_PROTOCOL.md` sec 4: unsolicited only "at Pico boot and on
      request"), so without this, its version would never be learned within
      that ESP boot cycle. `safety_link.c`'s `safety_poll_task()` now sends
      the `0x0B` request every poll period (~500 ms) for as long as
      `peer_version_known` stays false, via `safety_exchange(...,
      expect_status=false)` -- the same call shape already used for
      `REQUEST_ENABLE`-style calls, so no new retry/backoff machinery was
      added, just a periodic resend riding the poll loop's existing cadence.
      Build clean (`idf.py -C firmware/KilnFW build`), flashed to the bench
      ESP32-S3. **Not hardware-verified**: no Pico is attached in this
      environment (`ROADMAP.md` M0's bench-confirmed dead link) to confirm a
      real reply lands and the retries stop.
- [ ] **Refuse to push a Pico image this build could not then talk to.** That one
      action is what creates a lockout. Override must be explicit and separately
      confirmed -- **deferred, OTA-side work, out of scope for this pass**
- [x] When both need updating, the GUI states the order: **ESP first**, because
      the ESP is recoverable over USB and the Pico's easy path runs through it.
      **2026-08-18, later same day:** the incompatible-state message above
      always ends with "Update ESP first." as static text, not a computed
      decision -- re-read `UPDATE_PROTOCOL.md`'s wording (line ~420, "Update
      the ESP first when both need it... The GUI should say this rather than
      leaving the order to chance") and confirmed the rule is unconditional:
      it does not say "update whichever is older first," it says ESP,
      always, because the ESP is USB-recoverable and the Pico's own update
      path runs through it regardless of which side's protocol number is
      numerically behind

**SaftyFW side (2026-08-18, later same day pass):** intentionally not
re-touched here. Reading `firmware/SaftyFW/src/tasks/link_task.c` shows this
side already implements inbound `ANNOUNCE_VERSION` parsing
(`link_task_handle_announce_version()`) and the `DEGRADED_NO_CONTEXT`
transition on mismatch, entirely inside the link task -- never inside
`safety_core.c`, so `firmware/SaftyFW/tools/check_isolation.ps1`'s "the link
header never appears in `safety_core.c`" rule is respected by construction.
Nothing about mutual-version-check logic belongs in `safety_core.c` under that
rule, so there was no isolation-safe SaftyFW-side gap left for this pass to
fill; the only remaining SaftyFW-side item is the same codec-consolidation
follow-on noted above (`link_task.c`'s own hand-rolled encode/parse vs. the
new `kilnlink_announce` codec).

### 9.1 Partition table

**This changed once the image was actually measured.** A clean build on
2026-08-16 is **1167 KB** (0x1237A0), not the ~301 KB `PROJECT_STATUS.md` used to
claim. Two copies of that do not fit in the 1500 KB app region, so dual-slot OTA
is impossible in the 2 MB the firmware is configured for.

**Update 2026-08-17: flash size confirmed, sdkconfig fixed, bootloader
reflash still outstanding.** Confirmed against the LonelyBinary product page
for the board in hand (variant 43784065712285): it is an **N16R8 — 16 MB
flash, 8 MB PSRAM**. The buy lists (`N8R8`, 8 MB) and 3D model (`N8R2`, 8 MB)
are both stale and still need correcting at the source — that has not been
done yet. `sdkconfig` now declares `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` (was
`_2MB`).

**Update 2026-08-17 (later same day): partition table extended, rollback
enabled, host-build-verified.** `firmware/KilnFW/partitions.csv` now carries
`otadata` (8K) + `ota_0`/`ota_1` (2048K each) + `pico_img` (896K), all placed
at `0x200000` and above, exactly as this section proposed — see the file's own
header comment for the full offset arithmetic and the reasoning behind each
size. The six pre-existing entries (`nvs`, `phy_init`, `factory`, `wifi_nvs`,
`kiln_nvs`, `profiles_nvs`) are byte-identical to before this change (verified
by diff — the only lines removed were in the header comment's prose, not the
table itself). One correction versus the original proposal: `pico_img` is
**896K, not 512K** — `SaftyFW/bootloader/flash_layout.h`'s
`BOOTLOADER_SLOT_FLASH_SIZE` is 0xD0000 (832K) per Pico app slot, so 512K
cannot hold a full Pico image at all. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
is now set (was off). `firmware/KilnFW/App/main.c` now calls
`esp_ota_mark_app_valid_cancel_rollback()` from a background task gated on all
three of NVS-readable / safety-link-exchanging-frames / web-server-up — see
9.2 below. `idf.py -C firmware/KilnFW build` completes clean, and
`gen_esp32part.py`/`check_sizes.py` validated the new table with no
overlap/overflow (`KilnCtrl.bin` is 0x1273e0 bytes, 21% free in `factory`).
**None of this has touched physical hardware yet** — see the still-open items
below, which are unchanged by this pass.

The module was built with `CONFIG_ESPTOOLPY_FLASHSIZE_2MB` (now `_16MB`, see
above), and everything past 2 MB is unused. Either way the OTA partitions go
**above `0x200000`, where nothing
exists**, so no live data moves — the layout is sized for 8 MB and simply has
more room after it on a 16 MB part.

- [ ] **`esptool flash_id` first**, and then correct the buy lists and the 3D
      model reference. Three records disagreeing is worse than one being wrong,
      because each looks authoritative alone — size itself confirmed 2026-08-17
      by the LonelyBinary product page (N16R8) rather than `esptool flash_id`,
      but the buy-list and 3D-model records still need correcting, so this
      stays open
- [x] `CONFIG_ESPTOOLPY_FLASHSIZE` set to the **confirmed** size (done,
      2026-08-17)
- [ ] **Reflash the bootloader on the physical board** — the flash size is in
      its header, so a new table alone does nothing. This is still a one-time
      USB/serial step against real hardware that this pass could not perform
### 9.1a PSRAM — decided: stays off

**Decision (2026-08-16): leave `CONFIG_SPIRAM` unset. Do not enable PSRAM.**

The module carries 2 MB or 8 MB of it depending on which variant is actually
fitted, and none of it is used. That is the right answer today, for four
reasons and with one named condition that would reverse it.

**1. Nothing needs it, and the one thing that might is designed not to.** The
only workload on this board big enough to care is a display framebuffer:
480 x 320 x 3 = 450 KB, which does not fit in 512 KB of internal SRAM alongside
Wi-Fi. But `docs/ILI9488.md` drives the panel by streaming straight into its own
GRAM, with no framebuffer at all — the correct design for a panel that has its
own memory. A PSRAM framebuffer would render the same pixels twice: once into
PSRAM, then over SPI to the glass.

**2. Determinism.** PSRAM is behind a cache on a serial bus, and a miss stalls
the CPU for as long as the fetch takes. This firmware time-proportions heater
output, runs a PID tick, holds a 500 ms telemetry cadence, and depends on a link
whose 1.5 s silence blocks all heating. Adding a stall source to that in exchange
for memory nothing is asking for is a bad trade in the only direction that
matters.

**3. It creates a new boot failure mode.** PSRAM initialisation can fail. A
device that must fail safe then needs a defined answer to "came up with less
memory than the build assumed", which is more `kiln_enter_safe_state()` surface
bought for nothing.

**4. It puts every existing DMA buffer under audit.** PSRAM is not generally
DMA-capable, so allocations feeding SPI (three MAX31856 channels plus the panel),
I2C and two UARTs would all need `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` checked
by hand. Real work, real opportunity for a subtle bug, no benefit.

**It is also not a one-way door, and the doors open in different directions.**
Turning PSRAM on later is a config change plus that DMA audit. Turning it off
later, after code has grown to assume 8 MB of heap, is a rewrite. Staying off
until something concrete needs it is the cheap ordering.

**Reversed 2026-08-17: the named trigger fired, PSRAM turned on.** Section 10's
LVGL GUI (see 10.1 below) needs draw buffers for the ILI9488, which is exactly
the "locally-rendered UI" condition named above. `sdkconfig` now carries
`CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, and `CONFIG_SPIRAM_BOOT_INIT=y`
(hand-edited directly, same as this section's own flash-size fix above — no
`sdkconfig.defaults` exists in this project). The new
`firmware/KilnFW/App/drivers/lvgl_port.c`/`.h` allocates LVGL's two draw
buffers via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.

Revisiting the four original reasons against this specific, small (tens of
KB) use: **#1 (no framebuffer needed) does not apply** — this is a genuine
new use case the original design excluded, not a violation of it. **#2**
(determinism/stall risk) **and #3** (new boot failure mode) still stand as
general PSRAM caveats worth keeping in mind, but are not blockers for a
tens-of-KB display buffer rather than a full framebuffer. **#4 ("every
existing DMA buffer needs auditing") turned out not to apply to this use**:
`ILI9488_blit_data()` (in `ILI9488.c`) reads its caller's buffer with the CPU
and stages the RGB565->RGB666 conversion into the driver's own internal
DMA-capable scratch buffer before the actual SPI/DMA transfer, so the LVGL
PSRAM buffer itself is never touched by DMA and needed no audit. **Not
build-verified this pass** — no `idf.py build` was run against these
sdkconfig/driver changes.

- [x] Decision recorded; `# CONFIG_SPIRAM is not set` is deliberate, not an oversight
      — **superseded 2026-08-17, see "Reversed" above: `CONFIG_SPIRAM=y` now**
- [x] **Trigger to revisit: a locally-rendered UI on the ILI9488.** If the panel
      ever has to show something the ESP composes itself — needing read-back,
      compositing, or flicker-free partial redraw — that needs ~450 KB and PSRAM
      becomes required rather than optional. `docs/ILI9488.md` is cross-linked.
      **Fired 2026-08-17** — LVGL (10.1) is exactly this case; see
      `App/drivers/lvgl_port.c`/`.h`
- [ ] Second trigger, weaker: TLS on the web server, or many concurrent HTTP
      connections. Measure the heap before assuming either needs it
- [ ] **Keep GPIO 33-37 unassigned.** On an R8 (octal PSRAM) module those pins
      are consumed by the PSRAM inside the module whether or not the software
      enables it. The board currently uses 0-21, 38, 43, 44, 47 and 48, so there
      is no conflict — this is to keep it that way
- [ ] Note for sourcing: the **flash** is what earns the module's keep, since OTA
      needs it. The PSRAM on an R8 part is being paid for and not used. That is an
      acceptable trade for a one-off, not a reason to specify R8 on a reorder
- [x] `otadata` at `0x200000`, `ota_0`/`ota_1` 2 MB each above it, `pico_img`
      staging partition (896K, corrected up from the originally-proposed 512K
      — too small for SaftyFW's 832K app slot). Full layout in
      [`../CommonFW/docs/UPDATE_PROTOCOL.md`](../CommonFW/docs/UPDATE_PROTOCOL.md) §3
      and `partitions.csv`'s own header comment (2026-08-17)
- [x] **`factory` kept**, not reclaimed — with `otadata` erased the bootloader
      falls back to it, which is the only recovery path that needs no cable
- [x] Offsets confirmed against the live table before flashing (host-build
      verified: `gen_esp32part.py` validated no overlap/overflow, and the six
      pre-existing entries are byte-identical to before) — **not yet confirmed
      against the physical board**, which is still a one-time serial step
- [ ] Pre-change table archived so a rollback to pre-OTA firmware is possible
      (nothing to archive from yet — no physical flash has happened)
- [x] Slot size checked against the **measured** 2026-08-16 image (1167 KB);
      2048K slots leave 1.75x headroom
- [ ] One-time serial flash documented as a prerequisite step, not a footnote
      — still outstanding, this pass is build-verification only
- [ ] **`nvs`/`wifi_nvs`/`kiln_nvs`/`profiles_nvs` read out and archived from
      the physical board with `esptool read_flash` before the new table is
      ever flashed for real.** The one irreversible step in this whole plan;
      this pass has no hardware access and could not perform it

### 9.2 Rollback

- [x] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (was off; flipped 2026-08-17,
      confirmed by a clean `idf.py build`)
- [x] `esp_ota_mark_app_valid_cancel_rollback()` wired in, gated on all three
      preconditions — NOT called at the end of `app_main()`. Implemented as a
      background FreeRTOS task (`ota_rollback_confirm_task()` in `App/main.c`)
      started right after `nvs_report_capture()`, which polls until:
      - **NVS readable**: captured once from `nvs_report_get()`'s `mounted`
        flags at task-start time (this reflects every NVS-owning module that
        already ran by that point in `app_main()`)
      - **safety link exchanging frames**: checked live, every poll, via
        `safety_link_get_status(&safety, &st)->link_up` — that field is
        already exactly "a valid status within `SAFETY_LINK_UP_PERIODS`
        polls", so this is a genuine "currently exchanging", not "ever
        received one frame". No new `safety_link.h` getter was needed
      - **web server answering**: captured once from `dashboard_http_start()`'s
        return value
      All three fully wired — none of them is a flagged gap. If any is false at
      boot it stays false (nothing re-derives them), so the image correctly
      stays `PENDING_VERIFY` rather than being force-confirmed
- [x] `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` left off — not touched, per the
      instruction to leave it off unless a security version is maintained

### 9.3 Authentication — the AP password, not sent over the wire

- [x] `GET /api/ota/challenge`: 16 random bytes, single use, 30 s expiry.
      **2026-08-17**: `App/drivers/ota_http.c`'s `ota_challenge_get_handler()`,
      `esp_fill_random()` entropy, host-tested state machine in
      `ota_auth.{h,c}` (246/246, `test_ota_auth.c`).
- [x] Client proves knowledge via
      `HMAC-SHA256(HMAC-SHA256(ap_password, "kilnctl-ota-v1"), nonce || context)`
      where context is `"esp"` or `"pico"`. **2026-08-17**:
      `ota_http_verify_request()`'s `hmac_sha256()`, via the PSA Crypto API
      (`psa_import_key()` + `psa_mac_compute()`, not mbedtls's classic
      `mbedtls_md_hmac()` family, which is gated behind
      `MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS` in this vendored mbedtls
      4.x/TF-PSA-Crypto build).
- [x] Constant-time comparison; the nonce is invalidated whether or not it matched.
      **2026-08-17**: `ota_auth_constant_time_equal()`;
      `ota_auth_nonce_invalidate()` runs on both the match and mismatch paths.
- [x] Lockout after 3 failures, doubling to a 15-minute ceiling, per endpoint.
      **2026-08-17**: `ota_auth_lockout_record_failure()`/`_record_success()`,
      independent `s_lockout_esp`/`s_lockout_pico` instances. A stale/expired/
      never-issued nonce does **not** count as a failure — deliberate, see
      `ota_http.h`: that is the client's timing, not a wrong-password guess.
- [x] Every attempt logged with source IP. **2026-08-17**: `get_client_ip()`
      (`httpd_req_to_sockfd()` + `getpeername()`) on every challenge issue and
      every verify attempt, via `ESP_LOGI`/`ESP_LOGW`.
- [x] The derived key, not the literal PSK, is what gets compared — a bug that
      leaks the compared bytes must not leak the Wi-Fi password. **2026-08-17**:
      confirmed, `key = HMAC-SHA256(ap_password, context_string)` is the only
      thing ever fed to the comparison; `key` is `memset` to zero right after use.
- [ ] Documented plainly: this does not defend against anyone who already knows
      the AP password, because that is the credential. Prose exists in
      `UPDATE_PROTOCOL.md` §2; left unchecked since nothing user-visible (the
      OTA web page, §9.6, not yet built) says it yet.

### 9.4 Interlocks

- [x] Both update paths refused unless: no profile or autotune running, no heater
      commanded, `run_state` idle, every zone below a configured ceiling
      (default 100 °C), and the safety link healthy.
      **2026-08-17**: `ota_interlock_check()` (`App/drivers/ota_interlock.{h,c}`)
      is the pure, host-tested precondition logic (mirrors `ota_auth.c`'s
      pure/impure split, same header-comment reasoning copied over and
      adapted) -- no ESP-IDF/FreeRTOS dependency, takes a plain
      `ota_interlock_snapshot_t` + per-zone `ota_interlock_zone_snapshot_t[]`
      and returns OK or a refusal reason. `ota_http_check_interlocks()`
      (`App/drivers/ota_http.{h,c}`) is the ESP-IDF glue that builds the
      snapshot: `profile_executor_get_status()` for run state,
      `autotune_engine_is_active()`, `safety_link_get_status()`,
      `run_state_boot_record_interrupted()` for the reboot-survived-a-firing
      case, and -- deliberately NOT from `profile_executor`'s own `zones[]`
      array, which only reflects zones in the *last/current run* and reads
      as empty while IDLE (see the long comment in `ota_http.c` explaining
      why that would silently skip every zone precisely when this interlock
      matters most) -- per-zone temperature (`MAX31856_read_all()` +
      `zones_config_apply_cal()`) and heater-commanded state
      (`kiln_io_read()`'s `relay_shadow` masked by
      `zones_config_get_relay_mask()`), the same direct-hardware-read
      pattern `dashboard_http.c`'s `/api/status` already uses. Default
      ceiling is `OTA_INTERLOCK_TEMP_CEILING_C` (100.0f) -- no config_store
      item exists for this yet on the ESP side, same honest gap as
      SaftyFW's own hardcoded `UPDATE_TASK_TEMP_CEILING_C` (SaftyFW/TODO.md
      10.9). `ota_http_start()` gained `io_or_null`/`thermo_bus_or_null`
      parameters (was safety-only) to support the direct hardware reads;
      `App/main.c`'s call site updated to match `dashboard_http_start()`'s
      own pointers. 30 new host tests, `App/test/test_ota_interlock.c`
      (276/276 total, up from 246/246).
- [x] Refusals name the specific unmet precondition — "zone 2 is at 340 °C", not
      "update failed". **2026-08-17**: every refusal path in
      `ota_interlock_check()` names the specific zone (0-based index, matching
      this codebase's existing `"zone %u"` convention, e.g.
      `profiles_http.c`'s feasibility-check messages) and value --
      `"zone 2 is at 340 C"`, `"zone 1 heater is commanded on"`, `"safety
      link is down"`, `"a profile is running"`/`"...is paused"`, `"an update
      is already in progress"`, `"a firing was interrupted by a reboot and
      has not been acknowledged"` -- never a generic string.
- [x] Refuse to start either update while the other is in progress.
      **2026-08-17**: single in-RAM mutex, `ota_http_update_try_begin()`/
      `_update_end()`/`_update_in_progress()` (`App/drivers/ota_http.{h,c}`),
      guarded by the same `s_ota_lock` the nonce/lockout state already uses.
      One slot across BOTH processors, not one per processor, per TODO.md's
      own "single update mutex" phrasing --
      `ota_interlock_snapshot_t::other_update_in_progress` is checked FIRST
      in `ota_interlock_check()` (cheapest, and orthogonal to kiln state).
      **Not built this pass**: no transfer handler exists yet to actually
      call `ota_http_update_try_begin()`/`_end()` (that's 9.5) -- the mutex
      is real and tested, but nothing acquires it today. The functions are
      exposed for 9.5's future POST handlers to call on the first byte of a
      transfer and release on every exit path.
- [x] During a Pico update the link goes quiet, so `SAFETY_FAULT_SRC_SAFETY_LINK`
      asserts and blocks heating. **Correct — do not special-case it.** Suppress
      only the alarm *text*, never the block in `relay_authority_on_blocked()`.
      **2026-08-17**: nothing in this pass touches `relay_authority.c` --
      confirmed unmodified. There is no Pico-update transfer handler yet
      (9.5) to trigger this path at all, so there is nothing to special-case
      or verify end-to-end; recorded here as "correctly not touched" rather
      than "verified," since it cannot be exercised without 9.5's code
      existing.

### 9.5 Transfer

- [x] Streamed `esp_ota_ops` POST handler; a full image will not fit in RAM.
      **2026-08-17**: `POST /api/ota/esp` (`ota_esp_post_handler()` +
      `ota_esp_do_transfer()`, `App/drivers/ota_http.c`), registered in
      `ota_http_start()` alongside the existing `/api/ota/challenge` GET.
      MAC carried in a request header (`X-Ota-Mac: <64 hex chars>`, design
      choice documented in `ota_http.h`), never the URL. Order: header
      present/well-formed (400) -> `ota_http_verify_request()` (403) ->
      `ota_http_check_interlocks()` (custom "409 Conflict" status --
      `httpd_err_code_t` has no 409 entry, so `httpd_resp_set_status()` is
      used directly rather than `httpd_resp_send_err()`) ->
      `ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP)` (409 if another
      update is already in progress) -- all four checked before a single
      body byte is read. The image header (`sizeof(esp_image_header_t)`, 24
      B) is buffered and validated (magic + chip ID, see the checklist item
      below) before `esp_ota_begin()`; the rest streams through a 4 KB
      `static` chunk buffer (not stack -- see `ota_http.c`'s comment on why,
      given `wifi_provision_http.c`'s existing 8192-byte handler stack
      budget) straight into `esp_ota_write()`, one `httpd_req_recv()` per
      `esp_ota_write()`, no read-ahead. Every exit path (success, refused
      image, mid-transfer read/write failure) funnels through one cleanup
      block in `ota_esp_do_transfer()` that calls `esp_ota_abort()` if
      needed, updates the progress snapshot, appends the NVS record
      (`ota_record_append()`), and calls `ota_http_update_end()` exactly
      once -- the "single cleanup path" TODO.md asked for.
- [x] Pico relay: the five `UPDATE_*` frames. **2026-08-17**:
      `App/drivers/ota_pico_relay.{h,c}` (new module, a FreeRTOS background
      task) drives `UPDATE_BEGIN`/`UPDATE_DATA`/`UPDATE_END`/`UPDATE_ABORT`
      against `SaftyFW`'s already-frozen wire contracts (`src/update/
      image_header.h`'s 36-byte header, `src/tasks/link_frame.h`'s command
      ids 0x10-0x14, `src/tasks/update_task.c`'s `UPDATE_STATUS` reply
      layout) -- mirrored byte-for-byte in comments/#defines rather than
      shared, since `SaftyFW` is a separate repository/build target this
      project cannot `#include` from. **Not "codecs living in CommonFW"** as
      this checklist item originally envisioned -- that would need a shared
      build target neither firmware currently has; flagged as a scope
      deviation from the item's exact wording, not silently reinterpreted.
      `App/drivers/safety_link.{h,c}` gained the sending/receiving halves:
      `safety_link_send_update_frame()` (thin wrapper over
      `uart_protocol_send_broadcast()`, same call shape as
      `ANNOUNCE_VERSION`'s) and `safety_link_get_update_status()` (a new
      `case LINK_FRAME_UPDATE_STATUS_CMD` in `safety_drain_inbox()` caches
      the parsed frame, same pattern as `safety_apply_status()`/
      `safety_apply_fw_version()`). `App/drivers/uart_task_ids.h` gained
      `SAFETY_CMD_UPDATE_{BEGIN,DATA,END,ABORT,STATUS}` (0x10-0x14).
      **Ambiguity flagged, not silently resolved**: `UPDATE_END`'s payload
      is described as "4 B: image CRC32 repeated" -- read here as "the same
      crc32 sent in `UPDATE_BEGIN`, sent again", i.e. 4 bytes total, matching
      how `update_task_process_end()` actually reads it (compares against
      `s_header.crc32`, logs but does not act on a mismatch -- the
      read-back-from-flash CRC is what really gates acceptance). See
      `ota_pico_relay.c`'s header comment.
      **Retransmission-round accounting is a simplification, not an exact
      mirror** of `update_receiver.h`'s round-counting algorithm: this side
      retries whatever chunks the Pico's gap reports name, for up to
      `RELAY_MAX_RETRANSMIT_ROUNDS` (10, matching `UPDATE_MAX_RETRANSMIT_
      ROUNDS`) rounds, then always attempts `UPDATE_END` regardless and lets
      the Pico's own final CRC verification be the arbiter of correctness --
      per `UPDATE_PROTOCOL.md`'s own "acknowledging each frame is not what
      makes the transfer correct -- the final verify is." If the Pico
      reports `UPDATE_STATUS_ERR_RETRANSMIT_CAP` (or any other terminal
      failure) mid-retry, this side aborts immediately rather than
      exhausting its own round budget separately.
- [x] **The Pico image is relayed, never staged** -- **superseded, and
      corrected against real hardware numbers 2026-08-17**: this checklist
      item's own "no spare partition" premise is exactly what section 3's
      later 8/16 MB partition-table pass overturned -- `pico_img` (896K)
      now exists and IS the design this codebase uses (`UPDATE_PROTOCOL.md`
      section 3: "A staging partition also solves the relay problem").
      `POST /api/ota/pico` (`ota_http.c`'s `ota_pico_do_stage()`) streams
      the browser upload straight into `pico_img` at Wi-Fi speed (one
      `httpd_req_recv()` per `esp_partition_write()`, same "never read
      ahead" discipline as the ESP path, for the same reason: nothing
      downstream of the write has anywhere to put read-ahead data), then
      `ota_pico_relay.c`'s background task does the slow ~35 s+ relay
      afterward, reading `pico_img` chunk-by-chunk via `esp_partition_read()`
      -- never holding the whole image in RAM on either side of the split.
- [x] Socket timeout covers the whole transfer, not one chunk. **2026-08-17,
      both paths now**: `ota_esp_do_transfer()` raises the OTA connection's
      `SO_RCVTIMEO` to 30 s via `setsockopt()` on that one socket, rather
      than bumping `wifi_provision_http.c`'s server-wide `recv_wait_timeout`
      (which would also loosen every other endpoint's timeout). This is a
      per-recv-call timeout, not a whole-transfer deadline: as long as no
      single gap between chunks exceeds 30 s, a multi-minute transfer is
      fine -- see `ota_http.c`'s comment on the handler for why that is what
      "covers the whole transfer" means here. `ota_pico_do_stage()` sets the
      same 30 s per-connection timeout on its own socket for the (fast,
      Wi-Fi-speed) staging write; the actual ~35 s+ isolated-link relay
      happens entirely AFTER this handler has already responded and released
      (or handed off) the HTTP connection, so no HTTP-level timeout applies
      to it at all -- see `ota_pico_relay.h`'s header comment on why the
      relay is a background task rather than something the HTTP connection
      stays open for.
- [x] `UPDATE_DATA` sent unacknowledged, retransmitting only the ranges the Pico's
      gap reports name. **2026-08-17**: `ota_pico_relay.c`'s relay task sends
      the whole image sequentially first (unacknowledged broadcasts, per
      `safety_link_send_update_frame()`), then polls
      `safety_link_get_update_status()` for gap reports and retransmits only
      the named chunk indices, for up to `RELAY_MAX_RETRANSMIT_ROUNDS` (10)
      rounds -- see the 9.5 Transfer entry above for the "simplification, not
      an exact mirror of the round-counting algorithm" caveat, and for why
      that is a deliberate, documented choice rather than an oversight.
- [x] ESP image magic and chip ID verified **before** `esp_ota_begin()`.
      **2026-08-17**: `ota_esp_do_transfer()` reads exactly
      `sizeof(esp_image_header_t)` bytes, checks `hdr.magic ==
      ESP_IMAGE_HEADER_MAGIC` and `hdr.chip_id == ESP_CHIP_ID_ESP32S3`
      (`esp_app_format.h`), refuses (400, specific reason) on either
      mismatch, and only then calls `esp_ota_begin()` -- the buffered header
      bytes are written first via `esp_ota_write()` so nothing is re-read
      from the socket.
- [ ] `SAFETY_CMD_ANNOUNCE_REBOOT` sent before the ESP reboots, so a routine
      update does not trip S6(b) on the safety processor. It suppresses the trip
      for a bounded window and grants **no** permission to heat. **Not
      built** -- explicitly out of scope this pass (the link command does
      not exist yet in `CommonFW`/`SaftyFW`). The transfer handler above
      sets the boot partition but does not itself reboot the board; nothing
      in this pass touches the reboot path or the safety link.
- [x] Single update mutex across both processors — a second tab, or an agent
      racing a human, is refused rather than interleaved. **2026-08-17**: now
      actually acquired -- `ota_esp_post_handler()` calls
      `ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP)` before reading any
      body byte (409 if refused), and `ota_esp_do_transfer()`'s single
      cleanup path calls `ota_http_update_end()` on every exit. 9.4 already
      built and tested the mutex itself; this is the first real caller.
- [x] Append-only update record in NVS: timestamp, processor, image SHA-256,
      version before and after, result. **2026-08-17, partial**:
      `App/drivers/ota_record.{h,c}`, new module mirroring `run_state.c`'s
      shape (versioned blob, explicit reserved padding, `KILN_NVS_PARTITION`,
      load-tolerant philosophy) — deliberately NOT a multi-slot rotation:
      "append-only" is implemented as "the last record persists" (one NVS
      key, overwritten per update), a documented scope decision (see
      `ota_record.h`'s header comment) rather than the fuller
      sequence-number/slot-rotation history. Real fields: `uptime_s`
      (no wall clock, same reasoning as `run_state.h`), `processor` ("esp"),
      `version_before` (`esp_app_get_description()->version` of the running
      image), `version_after` (`esp_ota_get_partition_description()` read
      back from the partition just written -- real, not a placeholder),
      `success`, and a specific `reason` string. **Not real**: no
      `image SHA-256` field -- not built this pass, flagged rather than
      faked (would need hashing the stream as it passes through
      `ota_esp_do_transfer()`; mbedTLS/PSA is already linked, see
      `ota_http.c`'s `hmac_sha256()` for the precedent a future pass can
      follow). `ota_record.c` is ESP-IDF-coupled throughout and is NOT
      host-tested, matching `run_state.c`'s/`relay_cycles.c`'s own precedent
      (neither has a host test file either) -- see `ota_record.h`'s header
      comment for why splitting out `ota_record_fill()` alone for testing
      would be process for its own sake.
- [x] Progress pushed to the GUI at least every 2 s for both paths. **2026-08-17,
      both paths now, polled not pushed**: TODO.md's own parenthetical scoped
      this down from a WebSocket/SSE channel (out of scope) to "log
      progress... and track it in a small in-RAM state the existing
      dashboard could poll later" -- ESP path: `ota_http_get_esp_progress()`
      (`ota_http.h`) exposes a `static volatile` phase +
      percentage pair, updated by `ota_esp_do_transfer()` roughly every 10%
      (also `ESP_LOGI`'d at the same cadence). Pico path:
      `ota_pico_relay_get_status()` (`ota_pico_relay.h`) exposes the same
      shape (phase/percent/last_error), updated by the relay task at each
      phase transition and roughly every 10% during the streaming/
      retransmit phases; `GET /api/ota/pico/status` (`ota_http.c`) is a
      small new JSON endpoint on top of it -- built because it was a small
      addition given the getter already existed, per this task's own
      "build this if it's a small addition ... skip if it meaningfully
      expands scope" guidance. No poller/dashboard route calls either getter
      yet -- both are exposed for TODO.md 9.6's future web page, same
      pattern as 9.4's `ota_http_check_interlocks()` being exposed before
      anything called it.
- [ ] Protocol-version mismatch between the uploaded Pico image and the running
      one warned about, with a second confirmation — that is the case where a
      successful update leaves the two processors unable to talk. **Not
      built** -- explicitly out of scope this pass (task brief: "do not add
      ESP-side pre-checking of compatibility beyond what's trivial to read
      from `safety_link_get_peer_version_status()`"). The Pico itself still
      refuses an incompatible image on its own
      (`UPDATE_STATUS_ERR_VERSION_INCOMPATIBLE`, surfaced as a translated
      string by `ota_pico_relay.c`'s `format_update_error()`), which is the
      floor UPDATE_PROTOCOL.md's own precondition table requires -- this
      item is specifically about a proactive, second-confirmation warning
      BEFORE the relay starts, which is not built.

**Not built this pass, by design** (see the task's own scope statement):
`SAFETY_CMD_ANNOUNCE_REBOOT` (does not exist in `CommonFW`/`SaftyFW` yet),
the rest of the 9.6 web page, and 9.7's physical-hardware verification (no
hardware in this environment -- host-build/`idf.py build` verified only).

**Follow-on, same day (2026-08-17):** the alarm-text suppression flagged
above as an open gap was wired in a small follow-up pass --
`safety_link_set_update_in_progress()` plus the two `ota_pico_relay.c` call
sites (see 9.6's checklist entry below for the full description). **The
interlock/fault-assertion logic itself is correctly left alone** (only the
`ESP_LOG*` line changes; `SAFETY_FAULT_SRC_SAFETY_LINK` still asserts
unconditionally on `!up`).

`idf.py -C firmware/KilnFW build` clean (zero new warnings). Host tests
276/276, unchanged from the previous pass's baseline -- this pass's new
code (`ota_pico_relay.{h,c}`, the `safety_link.{h,c}`/`ota_http.{h,c}`
additions) is ESP-IDF-coupled throughout (FreeRTOS task, UART broadcast
send/receive, `esp_partition` flash I/O), the same "little to nothing
genuinely pure this pass" situation `ota_record.c`'s previous-pass entry
already named -- no test split was forced to make this look more
host-tested than it is.

### 9.6 Web page

- [ ] Per processor: running version, build commit, build date, dirty flag,
      active slot, and the version sitting in the inactive slot
- [ ] Interlock state shown **before** the file picker, with the blocker named
- [ ] Progress bar, and a rollback button per processor
- [ ] Reachable only when the kiln is idle
- [x] Suppress the "safety processor not responding" alarm TEXT (not the
      block) while a Pico relay is genuinely in progress. **2026-08-17**:
      `safety_link_set_update_in_progress()` (`App/drivers/safety_link.{h,c}`),
      called by `ota_pico_relay.c`'s relay task (true at the top of
      `relay_task_fn()`, false at its single `done:` exit, every outcome).
      `safety_update_health()`'s down-link branch checks the flag and logs
      `ESP_LOGI("...Pico update relaying (expected)")` instead of the usual
      `ESP_LOGW`/`ESP_LOGE` — the line right below it that asserts
      `SAFETY_FAULT_SRC_SAFETY_LINK` on `!up` is untouched, unconditional,
      exactly as before. **Reaches only as far as a real operator-facing
      surface currently exists** — this codebase still has no GUI fault-text
      display at all (`dashboard_http.c` exposes only the raw bit via
      `safety_link_get_status()`), so this wiring targets the ESP_LOG*
      stream, which is the only "alarm text" that exists today. A future GUI
      fault-text page should read the same `safety_link_get_status()` bits
      and would need no further change here.

### 9.6a MCP tools (`tools/PcTools`)

Not a web-page item, but tracked here since it consumes the same
`/api/ota/*` endpoints this section built. Full detail:
[`../CommonFW/docs/UPDATE_PROTOCOL.md`](../CommonFW/docs/UPDATE_PROTOCOL.md)
section 6.

- [x] **2026-08-18.** Four `mcp__kilnctrl__` tools wrapping `ota_http.c`'s
      HTTP surface (`ota_get_challenge`, `ota_update_esp`, `ota_update_pico`,
      `ota_status`) added in `tools/PcTools/src/kilnctrl/ota_http_client.py`
      + `mcp_server.py`. Deviates from `ota_rollback(processor)` +
      "logs the image hash" as originally sketched — see
      `UPDATE_PROTOCOL.md` section 6 for why (no HTTP rollback endpoint
      exists yet; no local SHA-256 computed this pass). Unit-tested against
      mocked HTTP only (`tools/PcTools/tests/test_ota_http_client.py`,
      15 tests) — **no physical board exercised**, same "host-build/config
      only, no hardware in this environment" caveat as the rest of this
      section.

### 9.7 Verification

- [ ] Power pulled mid-transfer, both processors — both still boot the old image
- [ ] Corrupt image rejected, both processors
- [ ] An image that boots but fails bring-up is rolled back with no intervention
- [ ] Update attempted while firing: refused, blocker named
- [ ] Wrong password: refused, locked out, logged
- [ ] Recovery from a deliberately bricked Pico over SWD

## 10. LCD touchscreen GUI (Klipper-style screen)

**Status: planning only, nothing built.** Requested 2026-08-17. The LCD
(ILI9488, `DISPLAY` task, `firmware/KilnFW`) is physically connected and
currently shows a static "kilnCtl Ready" string (`ROADMAP.md` M1) — this
section is the real UI on top of that. Deliberately scoped as its own
section rather than folded into section 2, since it's a second front end
(touchscreen, not HTTP) that has to stay in sync with the web dashboard
rather than duplicate/diverge from it.

**Usability/cleanup plan**: see `firmware/KilnFW/docs/UI_PLAN.md` for the
per-page no-scroll budget audit (LCD) and phone/tablet responsiveness audit
(web), plus a prioritized, individually-shippable fix queue covering both.

### 10.1 Generic screen/page/widget framework

**Status update (2026-08-17): rendering backend decided (LVGL) and first
slice built.** The evaluation below is resolved — LVGL was chosen — and a
minimal LVGL bring-up landed the same day: `firmware/KilnFW/App/drivers/
lvgl_port.c`/`.h` do `lv_init()`, a partial-redraw display driver flushing
through `ILI9488_blit_begin/data/end`, two `KILNCTL_LVGL_BUF_ROWS`-tall draw
buffers allocated in PSRAM (Kconfig-configurable; see 9.1a's reversal above),
a pointer input device reading the NS2009 touch controller (raw-ADC-to-pixel
mapping via new Kconfig calibration knobs `KILNCTL_TOUCH_CAL_SWAP_XY`/
`_INVERT_X`/`_INVERT_Y`, explicitly documented as unproven bench guesses
needing calibration — same honesty as the pre-existing
`KILNCTL_TOUCH_Z1_MAX_THRESHOLD`), a 1 ms `esp_timer` `lv_tick` source, and a
task driving `lv_timer_handler()`. **What's actually on screen is only a
placeholder "kilnCtl" label on a black background** — none of 10.3's page
content, none of 10.2's KlipperScreen theme, and no generic "simplified
Tkinter" app-layer on top of LVGL's raw API yet; the "thin app-layer on top
of LVGL's existing screen/widget/style objects" framing below still applies,
just now with a settled backend to build it on rather than an open
evaluation. **Not build-verified this pass** — no `idf.py build` was run.

An architectural decision came with this: **LVGL now owns the ILI9488
outright, replacing the old UART-remote-drawn `DISPLAY_CMD_*` path**, rather
than the two coexisting. `main.c`'s normal boot path no longer calls
`uart_bridge_start_display_task()`. This was an explicit user decision
(recommended option: "LVGL replaces remote draw") made because two
independent owners issuing draw calls to the same `ILI9488Class` at the same
time was an unresolvable race.

This same change fixes a real bug, not a hypothetical one: previously a
touch woke `screen_idle`'s internal flag but nothing ever repainted the
panel afterward (`screen_idle.h`'s own comment already said waking "is just
a flag flip ... whatever owns the UI is responsible for repainting real
content" — nothing owned the UI, so the screen stayed dark after
auto-blanking even though a touch registered). `lvgl_port.c`'s flush
callback now watches for the `screen_idle` off->on edge and calls
`lv_obj_invalidate(lv_screen_active())` to force a full redraw on wake.
Touch ownership of the NS2009 also moved from `screen_idle_task`'s own
polling loop to `lvgl_port.c`'s input-device callback, which forwards real
presses into `screen_idle_inject_touch()` so `screen_idle`'s idle-timer/wake
semantics are unchanged, just fed from a different source — `main.c` now
passes `touch = NULL` to `screen_idle_init()` for this reason.

**PC/firmware DISPLAY task & tools mismatch (2026-08-19):** `uart_bridge.c`'s
`display_bridge_task` is intentionally dead code — `main.c` no longer calls
`uart_bridge_start_display_task()` since LVGL now owns the ILI9488 rendering
exclusively. However, `tools/PcTools`' MCP server still exposes 12+ live
`display_*` tools (`display_read_id`, `display_reset`, `display_set_power`,
`display_set_rotation`, `display_set_text_cursor`, `display_set_text_style`,
`display_clear`, `display_draw_line`, `display_draw_rect`, `display_fill_rect`,
`display_print`, `display_send_image`) that send `DISPLAY_CMD_*` frames over
the UART bridge protocol — frames the firmware can no longer answer, since the
task was never started. Not a crash risk (frames are simply unhandled/timeout),
but any invocation of these MCP tools today will silently fail. **Decision
pending:** (a) delete the stale PC-side tools, (b) restore a minimal firmware
handler to support them, or (c) leave as-is. Document the fork in the road
here; don't pre-judge which path to take.

- [x] A small, generic "screen" abstraction — pages + widgets, roughly a
      simplified Tkinter: a page owns a widget tree, widgets draw
      themselves and expose a tap/press callback, the screen manager swaps
      the active page and handles global chrome (nav bar, back button).
      Framework-agnostic on paper; decide the rendering backend before
      building it, since that decision shapes the widget base class.
      **Rendering backend decided (LVGL, 2026-08-17); app-layer abstraction
      first slice built the same day — `App/drivers/kiln_ui.c`/`.h` is the
      page registry/switcher (register-by-name, build-on-first-show, cached
      thereafter, `lv_screen_load()` to switch). One rule enforced from the
      start rather than retrofitted later: kiln_ui.c/.h must never contain a
      page's own widget tree — every page is its own `ui_page_<name>.c`/`.h`
      pair (see `ui_page_home.c`/`.h`, the first and so far only page,
      registered as `"home"`), so the UI's page count growing (10.3 wants at
      least four) never turns kiln_ui.c into the one file every page change
      touches. `ui_page_home.c` also pulls its colors from the new
      `ui_theme.h` (10.2) instead of hardcoded `lv_color_black()`/`_white()`,
      so it's already a real (if trivial) consumer of the theme, not just
      the theme sitting unused.**
  - [x] **Evaluate LVGL** as the rendering/widget backend — it already has
        the object model this section wants (screens, widgets, styles,
        input devices) and an ESP-IDF component exists
        (`lvgl/lvgl` + `lvgl/lv_port_esp32` or the IDF component registry
        `espressif/lvgl` package), so "write a simplified Tkinter" may
        mean "write a thin app-layer on top of LVGL's existing screen/
        widget/style objects" rather than a widget toolkit from scratch.
        Open to alternatives (u8g2 is text/mono-only and too low-level for
        this; TFT_eSPI has no widget model) — LVGL is the front-runner
        specifically because it already solves the touch-hit-testing
        problem (10.4) internally, so evaluate whether its own indev
        driver can be reused instead of writing a custom one.
        **Decided 2026-08-17: LVGL chosen, first slice built — see status
        update above. It also now owns the ILI9488 outright in place of the
        old UART-remote-drawn path, an explicit user decision (see above),
        not something this evaluation bullet originally anticipated.**
  - [ ] Decide the color/asset story once the visual style (10.2) is
        picked — LVGL widgets are themeable, so the "Klipper screen" look
        is a theme/style pass on top of stock widgets, not custom-drawn
        widgets.

### 10.1a Shared backend with the web UI

**Status: decided, not yet executed.** Explicit requirement, 2026-08-17: "use
the same back end code as the web ui for lcd stuff... i want them to have
mostly the same functionality just different interfaces." This is a real
constraint on how every remaining piece of section 10 gets built, not a
someday-nice-to-have, so it's recorded here before 10.3 writes a single real
page.

**The problem it's guarding against**: `App/drivers/dashboard_http.c` (and
`zones_http.c`, `profiles_http.c`, `rules_http.c`, `readiness_http.c`) today
mix three things in one function per endpoint — reading state out of
`profile_executor`/`relay_authority`/`zones_http`/etc., applying any request
action, and serializing the result to JSON — with no seam between them. See
e.g. `dashboard_http.c`'s `status_get_handler()`: it calls straight into the
data-owning modules and writes JSON in the same function body. If the LCD
side (10.3) copies this pattern and calls those same data-owning modules
directly from its own page code, the "same functionality" the user asked for
degrades over one release into two independently-maintained readings of the
same state, which is exactly the drift this repo's own instructions (see
this file's top and `ROADMAP.md`) already warn about for the web/LCD pair
specifically — see 10.5.

**The rule going forward**: a page's *data access and actions* — "what is
zone 0's current temperature and heater state," "start this profile," "set
this relay" — must go through the same plain-C functions for both interfaces.
Two front ends (JSON serialization for `esp_http_server`, LVGL widget updates
for `kiln_ui`) sit on top of one backend, not two backends that happen to
agree today. Concretely, an existing HTTP handler that isn't already split
this way gets split *at the point 10.3 first needs the same data* — e.g. when
the home page (10.3's real replacement for `ui_page_home.c`'s placeholder)
needs per-zone temp/heater status, `dashboard_http.c`'s `status_get_handler()`
gets its data-gathering pulled out into a plain function (e.g.
`dashboard_get_status(dashboard_status_t *out)`) that the HTTP handler then
serializes and the new LCD page then renders — not reimplemented from
scratch against `profile_executor`/`zones_http` a second time. This is
deliberately *not* done as one big mass refactor of every existing handler up
front: extracting a shared getter nobody consumes yet is speculative work
against an interface (10.3's pages) that doesn't exist, and this codebase's
own convention throughout (`TODO.md`'s intro, every "design only" section
that waited for a real consumer before committing to a shape) is to build
the seam when the second caller actually shows up, not before.

- [ ] As each 10.3 page is built, extract its backend data access from the
      matching HTTP handler into a shared plain-C getter/action function
      (naming pattern: `<module>_get_<thing>()` for reads, reuse the
      existing action functions directly for writes — e.g.
      `profile_executor_run()`/`_pause()`/`_halt()`, `relay_authority_*`,
      already plain C and already the single implementation each HTTP
      handler calls, so a button on the LCD calling the same function is
      the *already-correct* pattern for actions; it's specifically the
      *read* side each handler currently inlines that needs splitting)
- [ ] Where a handler is split, the HTTP handler must be updated to call the
      new shared getter too (not left calling the data-owning modules
      directly while only the LCD gets the new seam) — otherwise this
      creates the third thing it exists to prevent: two different call paths
      to the same data, one of which nothing forces to stay in sync with the
      other's bug fixes
- [ ] 10.7 (onboard IC temps) and 10.8 (multi-thermocouple-per-zone
      averaging) were in progress the same day this rule was written —
      when 10.3 reaches for either, check whether their read paths already
      landed as plain getters (matching this rule) or as handler-inlined
      logic (needing the same split described above) rather than assuming
      either way from this note

### 10.2 Visual style — match KlipperScreen

**Status update (2026-08-17): palette/spacing source of truth first slice
built.** `firmware/KilnFW/App/drivers/ui_theme.h` (LVGL `lv_color_hex()`
macros, no `.c` needed) and `firmware/KilnFW/docs/UI_THEME.md` (the same
palette as a table, plus a "Web dashboard parity" section pointing 10.6 at
these exact hex values) now exist: a dark-navy background
(`UI_THEME_COLOR_BG`, `#1a1f2b`) and lighter card background
(`UI_THEME_COLOR_CARD`, `#242a3a`), near-white primary/secondary text
(`UI_THEME_COLOR_TEXT_PRIMARY` `#f0f0f0`, `UI_THEME_COLOR_TEXT_SECONDARY`
`#9aa0ae`), and five named accents (`UI_THEME_ACCENT_1`..`_5`: orange
`#e8974e`, purple `#a15fd6`, teal `#3ec6c6`, green `#5cc06e`, red/amber
`#d6555f` held back for alarm/attention use) for 10.3 to assign per
configured zone/metric without this header knowing about zones itself. Also
added: a minimum touch target (`UI_THEME_MIN_TOUCH_TARGET_PX`, 72px, budgeted
against the 480x320 panel), a corner radius (`UI_THEME_CORNER_RADIUS_PX`,
10px), a padding/gap value (`UI_THEME_PADDING_PX`, 8px), and a status bar
height (`UI_THEME_STATUS_BAR_HEIGHT_PX`, 32px).

**Explicitly a first-pass, unverified-against-real-hardware palette** — no
KlipperScreen theme file or physical LCD was in hand this pass, only a
description of the reference screenshots (dark navy bg, white text, a small
rotating accent-color set per row/zone/nav-icon). Every constant is commented
in `ui_theme.h` with that caveat, same honesty as the existing
`KILNCTL_TOUCH_CAL_SWAP_XY`/`KILNCTL_TOUCH_Z1_MAX_THRESHOLD` guesses — sanity
-check against the physical ILI9488 once available and correct both files
together if it's off. **Nothing consumes this yet**: `kiln_ui.c`'s
placeholder home page still draws `lv_color_black()` untouched (10.1's
pre-10.2 bring-up screen, deliberately left alone), and 10.3's real pages
(the ones that would actually apply this theme) are not built.

- [x] Adopt the **KlipperScreen** (the 3D-printer touchscreen UI) layout
      and color style as the visual reference: dark theme, large
      touch-friendly buttons, a persistent top status bar, bottom/side nav
      between pages. Pull concrete values (palette, spacing, font sizes)
      from KlipperScreen's own theme files rather than reinventing them.
      **Done as a first-pass approximation — see status update above; no
      actual KlipperScreen theme file was available this pass, so values
      are inferred from a description of the reference look, not extracted
      from KlipperScreen's source.**
- [x] Whatever theme constants this produces (colors, fonts, spacing)
      should be a small shared table — 10.6 wants the web dashboard
      restyled to visually match, and a single source of truth for the
      palette is what keeps the two from drifting the moment one changes.
      **Done: `ui_theme.h` + `docs/UI_THEME.md`, see status update above.
      Font sizes are not yet part of this table — LVGL font selection
      wasn't scoped into this pass; add it here if/when 10.3 needs one.**

### 10.3 Page designs

**Status update (2026-08-18): home page's three previously-deferred items now
built -- zone names, the desired-vs-actual graph, and a profile picker.**
Building on the 2026-08-17 pass (scrollable content area with one card per
configured zone, a profile/run-state card, and Start/Stop +
Configuration/Temperature nav buttons -- see git history for that pass's own
detail), `App/drivers/ui_page_home.c` now also has:

- **Zone names.** `zones_config_get_name()` (new getter, `zones_http.c`/`.h`)
  reads the operator-chosen name `zone_cfg_t::name` already stored -- the
  JSON API (`GET /api/zones`) could already show it, but nothing plain-C
  could read it before this pass. Each zone card now shows that name, falling
  back to "Zone N" only for a zone that has genuinely never been named (an
  empty string is a valid, different case from "getter failed" -- see the
  getter's doc comment in `zones_http.h`).
- **The desired-vs-actual temperature graph.** A real `lv_chart` (LVGL v9's
  built-in chart widget) replaces the placeholder card: two line series
  (actual, desired), windowed to the most recent 60 ring-buffer samples (30
  minutes at the buffer's 30s/sample cadence -- legible on a 480px-wide
  panel; the buffer itself holds up to 24h/2880 samples but that's not
  something a small LCD can show usefully all at once).  **One combined
  chart, not one per zone** -- `profile_executor.h`'s history ring buffer is
  itself single-series ("scoped to a single representative zone," not
  per-zone data), so a per-zone chart would just be N-1 empty charts and one
  real one; see `build_graph_card()`'s comment. Data comes from
  `profile_executor_get_history()`/`_get_history_count()`, which already
  existed as the exact plain-C getters `GET /api/history.csv`
  (`history_csv_get_handler()`) streams from -- no new getter was needed
  here, TODO.md section 0's ring buffer was already extracted correctly.
  The chart only repaints when `profile_executor_get_history_count()` has
  actually advanced since the last `UI_PAGE_HOME_REFRESH_MS` tick (tracked
  via a last-seen-count static), not every tick, since the buffer itself
  only gains a new sample every 30s. Y-axis range is computed dynamically
  from the plotted window each repaint (min/max +/-10% padding) rather than
  a guessed fixed ceiling.
- **A profile picker.** An `lv_dropdown` on the profile/run-state card, above
  Start/Stop, populated by looping the 8 fixed NVS slots through
  `profiles_http_get()` -- the same read-only accessor `profile_executor.c`
  already calls; no new `profiles_http.c` getter was needed, this page is
  just the second caller of an existing one.  `start_btn_cb()` now prefers
  the picker's selection (tracked via a "has the operator actually touched
  the dropdown" flag, set only by `LV_EVENT_VALUE_CHANGED`, not by the
  initial populate) and falls back to the previous pass's "whatever's
  already known this boot" logic only when the picker was never touched --
  documented in `start_btn_cb()`'s comment as an intentional behavior split,
  not a placeholder.

Per 10.1a, every number on the page comes from `dashboard_get_status()`,
`profile_executor_get_status()`, and now `profile_executor_get_history()`/
`_get_history_count()` and `profiles_http_get()` -- all plain-C getters the
HTTP handlers already used or use themselves -- and every button (Start,
Stop) calls the exact same `profile_executor_run()`/`profile_executor_halt()`
`dashboard_http.c`'s POST handlers call, not a reimplementation.

**Not build-verified this pass** — no `idf.py build` was run (none is
available in this environment), and none of this has touched the physical
ILI9488/NS2009 panel or the real LVGL v9.5.0 chart/dropdown widgets at
runtime. The chart code was written against a direct read of
`components/lvgl/src/widgets/chart/lv_chart.h`/`.c` (point-count-before-
ext-array ordering, `lv_chart_set_series_ext_y_array()`'s ownership
semantics, `LV_CHART_POINT_NONE`'s value) rather than guessed API, but that
is not the same as having compiled or run it.

**Main / status page** (the default page, mirrors section 2's web
dashboard but is the touchscreen's home, not a secondary view):

- [x] Each configured zone, its current temperature, and its heater
      on/off status (same data as `GET /api/status`, section 2), **now
      labeled with the zone's configured name, not just "Zone N" — see
      status update above.**
- [x] Graph of the current profile's target curve with all zones'
      actual temperature progressing along it (same data/shape as
      section 2's `historyChart` / `GET /api/history.csv` — reuses the
      existing history ring buffer, section 0's "Historical data for the
      graph" item, rather than a second buffer). **Built — see status
      update above for why it's one combined chart, not "all zones" plural
      (the ring buffer only ever tracks one representative zone).**
- [x] Name of the currently selected/running kiln profile
- [x] Time remaining and elapsed time, **shown both as text and as a
      progress bar** — per-segment (elapsed into the segment / dwell
      remaining), matching what `profile_executor_get_status()` actually
      tracks and what section 2's own web dashboard already shows; there is
      no whole-profile total-remaining figure anywhere in the backend to
      show instead.
- [x] Stop button
- [x] Start button — **profile-picker gap closed this pass, see status
      update above.**
- [x] "Configuration" nav item — opens the deeper config / settings menus
      (maps to section 3's Settings pages: Thermocouples & Zones, Relays &
      Rules, Network). **Stub page only, unchanged this pass.**
- [x] "Temperature" nav item — individual per-zone manual control (maps to
      section 2's manual relay override / per-zone target, touchscreen
      equivalent of the dashboard's relay controls). **Stub page only,
      unchanged this pass.**
- [x] WiFi connection state / station IP / mDNS hostname readout in the
      status bar. **Added 2026-08-18 (separate follow-up within this same
      pass, see status update below) — not in the original page-designs
      list, added on explicit request.**
- [ ] **Added to plan 2026-08-18, not yet built:** "Temperature" nav item's
      stub above becomes the real manual-zone-control page from 0.5's new
      page list — setpoint override + calibration readout, same getters
      section 3's zone settings already exposes.
- [ ] **Added to plan 2026-08-18, not yet built:** Safety / Alarm page
      (0.5) — trip state, trip-event history, explicit clear-trip action.
      Blocked on M5's `TRIP_EVENT` frame; do not build against a stub data
      source.
- [ ] **Added to plan 2026-08-18, not yet built:** Diagnostics / System info
      page (0.5) — fw versions both processors, safety-link stats, ESP
      heap/flash, IC temps (10.7). Link-stats half blocked on M5; the rest
      is buildable now.
- [ ] **Added to plan 2026-08-18, not yet built:** Backup / restore page
      (0.5) — export/import profiles + zone/relay/network config as one
      blob. No blockers.

**Status update (2026-08-18, follow-up): WiFi/IP/mDNS status readout added to
the home page's status bar.** `s_status_label` (right-aligned, next to the
"kilnCtl" title) now shows connection state text, the station IP when
connected, and `kiln.local` when mDNS is up — built by `wifi_status_text()`
in `App/drivers/ui_page_home.c`. Per 10.1a's shared-backend rule, every piece
of this comes from getters `wifi_provision_http.c`'s `status_get_handler()`
already calls: `wifi_prov_get_mode()`, `wifi_prov_get_state()`,
`wifi_prov_is_sta_connected()`, `wifi_prov_get_sta_ip()` (all `wifi_prov.h`,
unchanged). The mDNS half needed no new getter at all — `main.c` sets the
hostname unconditionally (`mdns_hostname_set("kiln")`, ~line 243) whenever
`mdns_init()` succeeds, and the espressif `mdns` component's own
`mdns_hostname_get()` (`managed_components/espressif__mdns/include/mdns.h`)
already answers "did that succeed" (`ESP_ERR_INVALID_STATE` if mDNS was never
initialized/hostnamed), so this page calls that directly instead of adding a
tracking flag next to `main.c`'s `mdns_init()` call. `App/drivers/CMakeLists.txt`
now lists `mdns` under the `drivers` component's `REQUIRES` (it wasn't
previously a `drivers`-component dependency, only an `App`/`main`-component
one) so `ui_page_home.c` can include `mdns.h` at all — `main.c` can't export a
header back to `drivers` the other way (that component already `REQUIRES
drivers`, so the reverse would be circular).

**Status update (2026-08-18): no-scroll rewrite -- hard requirement, LCD
pages must never require scrolling.** `ui_page_home.c`'s own prior comment
admitted "sizing has never been checked against the real panel... scrolling
is the safe fallback rather than guessing pixel-perfect fixed heights" --
that stopped being acceptable. This codebase's actual runtime canvas is
480x320 landscape (the ILI9488 panel is natively 320x480, but Kconfig's
default `KILNCTL_DISPLAY_ROTATION` is 1, landscape, and `ui_theme.h`'s own
spacing constants were already "budgeted against the 480x320 landscape
panel" -- so that's the resolution this rewrite budgeted against, not the
panel's native portrait dimensions). Real budget used: 320px tall, minus
`UI_THEME_STATUS_BAR_HEIGHT_PX` (32), minus `2*UI_THEME_PADDING_PX` outer
padding (16), minus the bar-to-content gap (~4-8px) =~ 264px of real content
height to fit into, computed from real constants (`ui_theme.h`) and
`LV_FONT_DEFAULT`'s real montserrat_14 metrics, not guessed.

- **`ui_page_home.c` rebuilt to fit.** Kept: zone rows (name/temp/heat --
  the file's own "kiln-process data the operator watches while firing"
  framing), a compact one-line run-state summary + time/progress line + slim
  progress bar, and a Start/Stop/Menu action row (Menu replaces the old
  separate Configuration+Temperature nav buttons). Moved off entirely:
  - The AP-join QR card (10.9) -- **deleted, not moved**. It duplicated
    `ui_page_network.c`'s own AP-mode QR card (same payload, same gating
    condition) -- 10.9's own audit note already flagged this as a
    duplicate, not unique content.
  - The Safety Processor card (10.10/ROADMAP.md M6) -- moved to a new page,
    `ui_page_safety.c`/`.h`, reachable from `ui_page_config.c`'s "Safety
    Processor" nav item.
  - The desired-vs-actual temperature chart -- moved to a new page,
    `ui_page_history.c`/`.h`, reachable from `ui_page_config.c`'s
    "Temperature History" nav item.
  - The profile picker dropdown -- **deleted, not moved**. Start now always
    uses the fallback chain (current non-idle profile, else the last boot
    record) that already existed for "picker untouched" -- a real,
    documented capability loss (no more picking an arbitrary saved profile
    from the LCD; the web dashboard still can) traded for the hard no-scroll
    requirement.
  Zone-row padding trimmed (`UI_THEME_PADDING_PX/4`) and the run-state card
  compacted (one summary line instead of two) to leave a real double-digit
  margin against the ~264px budget, not a razor-thin fit.
- **`ui_page_config.c` grew from 4 to 7 nav destinations this pass**
  (Temperature moved here from home's old dedicated button; Safety Processor
  and Temperature History are new) and switched from a single-column list to
  a 2-column flex-wrap grid at a shorter row height (44px, using
  `ui_theme_apply_touch_area(..., true)`'s documented "extend an undersized
  widget's effective click area toward `UI_THEME_MIN_TOUCH_TARGET_PX`"
  behavior) so all 7 destinations plus Back fit the same ~264px budget.
- **`ui_page_board_health.c`, `ui_page_temperature.c`** compacted (smaller
  padding/gaps; `ui_page_temperature.c`'s relay buttons shrunk from 72px to
  36px tall) to fit the same budget. `ui_page_temperature.c`'s fit still
  depends on how many relays are assigned per zone at runtime
  (`zones_config_get_relay_mask()`) -- a zone with several relays wrapping
  onto a second button row could still overflow; this is a real,
  unresolved risk, not something this pass could rule out without a fixed
  relays-per-zone cap or real hardware to check against.
- **`ui_page_network.c` follow-up pass (2026-08-18): Scan/Saved made mutually
  exclusive, list block and QR row made mutually exclusive once connected.**
  The prior pass's remaining overflow came from Scan and Saved both being
  stacked at once (~300-350px on their own). Fixed by adding a small
  Scan/Saved toggle row (36px buttons) that shows exactly one 70px list at a
  time (shrunk further from 90px), and, once connected, hiding the whole
  list block behind a "Change network" button so the STA-connected QR row
  gets the space instead -- tapping it swaps back via `s_manage_open`, with
  a "Show QR" button to return. Worst case now: not-connected ~248px content
  + ~20px gaps ~= 268px against the ~264px budget (tight, computed not
  measured); connected-not-managing ~280px (QR row instead of list block).
  Both are much closer to budget than the prior 300-350px, but still not
  hardware-confirmed. The scan/saved `lv_list` widgets stay internally
  touch-drag-scrollable at their fixed height on purpose -- a bounded,
  self-contained list scrolling itself is normal, contained UI, not the
  "whole page scrolls" problem this rewrite targets (explicit user
  direction 2026-08-18: page-level scrolling must never happen, but a
  fixed-height list widget scrolling internally via touch-drag is fine and
  is the sanctioned way to show a variable-length list). Real build
  (`idf.py -C firmware/KilnFW build`) verified clean after this change.
  **Still open, if the ~268px worst case turns out tight on real hardware:**
  split Scan and Saved into their own sub-pages via `kiln_ui_show()`, the
  same pattern Safety Processor/Temperature History already use.
- **Every page's `scr` and `content` containers now explicitly clear
  `LV_OBJ_FLAG_SCROLLABLE`** (`ui_page_home.c`, `ui_page_config.c`,
  `ui_page_board_health.c`, `ui_page_network.c`, `ui_page_temperature.c`,
  plus the two new pages) so a future accidental overflow clips visibly
  instead of silently turning scrollable again.
- **No real hardware was available to visually confirm any of this.** Every
  number above was computed against real constants (`ui_theme.h`,
  `LV_FONT_DEFAULT`), not guessed, and the build (`idf.py -C firmware/KilnFW
  build`) succeeds clean with all new/changed files compiling -- but nobody
  has looked at the actual ILI9488 panel to confirm pixel-perfect fit.
  Treat the "fits" claims above as "computed with a real margin," not
  "pixel-verified," except `ui_page_network.c` and (for many-relay zones)
  `ui_page_temperature.c`, which are honestly flagged above as still likely
  to overflow in some configurations.

### 10.4 Touch hit-testing

**Status update (2026-08-17): LVGL's real hit-test behavior checked (not
assumed) and infrastructure built on top of it.** 10.1's evaluation note
flagged "check whether LVGL's own indev/hit-testing already implements an
equivalent nearest-target-within-tolerance model" as unconfirmed. This pass
actually read the code instead of taking that note's optimism at face value:
`components/lvgl/src/indev/lv_indev.c`'s `lv_indev_search_obj()` (the
function every touch point actually goes through) walks the widget tree
depth-first, testing children topmost-z-order-first, and returns the *first*
object whose bounding box contains the point via
`components/lvgl/src/core/lv_obj_pos.c`'s `lv_obj_hit_test()` ->
`lv_obj_get_click_area()` -> `lv_area_is_point_on()` — plain rectangle
containment, no distance-to-center comparison anywhere, no arbitration
between two candidate boxes that both contain the point. **This is not
nearest-center-within-tolerance; it is first-match-in-z-order.** The
optimistic reading in 10.1's note was wrong on the arbitration question.

What LVGL *does* provide, confirmed in the same read: `lv_obj_set_ext_click_area(obj, size)`
(`lv_obj_pos.c`) symmetrically expands one widget's own bounding box by
`size` px on every side before that containment test runs — a real,
built-in, per-widget-tunable answer to the *sizing* half of this section's
ask (generous halo for sparse buttons, little/none for a dense grid), just
not the *arbitration* half.

- [x] Map a raw touch coordinate to the widget the user meant to press,
      **using LVGL's built-in `lv_obj_set_ext_click_area()` rather than a
      custom nearest-center hit-tester** — see the status update above for
      why: no custom point-to-widget resolution exists to replace, LVGL's
      own hit test is what runs today and what any custom layer would have
      to sit in front of.
  - [x] The allowed offset is **dynamic**, based on local widget density and
        button size. Built as `ui_theme_apply_touch_area(lv_obj_t *widget,
        bool compact_layout)` in the new `App/drivers/ui_theme.c` (`ui_theme.h`
        gained its first `.c` companion this pass) — reads the widget's own
        size via `lv_obj_get_width()`/`_height()`, then calls
        `lv_obj_set_ext_click_area()` with an extension computed from that
        size and the `compact_layout` flag: a sparse widget gets a generous
        fixed margin (further extended if the widget itself is smaller than
        `UI_THEME_MIN_TOUCH_TARGET_PX`, so the effective clickable square
        still reaches that minimum); a compact widget gets at most half of
        `UI_THEME_PADDING_PX`, capped there specifically so two adjacent
        compact cells' expanded boxes can never overlap in their shared gap.
  - [x] Confirmed by actually reading LVGL's source (see status update
        above) rather than assumed — done.
- [x] Explicitly handle **compact button layouts** (e.g. a numeric keypad or
      a dense settings grid) as their own density case, not just the sparse
      main-page buttons above. `ui_theme_apply_touch_area()`'s
      `compact_layout` parameter is exactly this — a caller building a
      keypad passes `true`, a caller building main-page buttons passes
      `false`; see the function's own doc comment in `ui_theme.h` for the
      exact numbers and the reasoning behind the compact cap.

**Nothing calls this yet.** 10.3 (real page/button layouts) doesn't exist —
only `ui_page_home.c`'s placeholder label — so there is no real button grid
to size. This is infrastructure for 10.3 to call when it builds one; forcing
it onto the placeholder label would have nothing meaningful to demonstrate.

**Status update (2026-08-18): the open item below is now built, as opt-in
infrastructure, ahead of a real consumer.** The user explicitly asked for it
ahead of a real dense layout, the same position `ui_theme_apply_touch_area()`
itself was in when it was written last pass (nothing called it yet either,
at the time).

- [x] Custom post-process step for the ambiguous-overlap case, built on top
      of `touch_read_cb()` in `lvgl_port.c`, opt-in via a new registry in
      `ui_theme.c`/`.h` (same file `ui_theme_apply_touch_area()` lives in):
  - **Registration**: `ui_theme_register_touch_group(lv_obj_t **widgets,
        size_t count)` lets a page opt a specific cluster of widgets (e.g. a
        future dense keypad/settings-grid page) into nearest-center
        arbitration with each other. Widgets never registered in a group are
        completely unaffected — they keep relying on LVGL's default
        z-order-first-match hit-test alone, which is correct for the
        overwhelming majority of layouts (sparse buttons, non-overlapping
        extended click areas). Small fixed-size registry
        (`UI_THEME_TOUCH_GROUP_MAX_GROUPS` = 4,
        `UI_THEME_TOUCH_GROUP_MAX_WIDGETS` = 16 per group) — deliberately not
        a dynamic allocation for a mechanism meant to serve one or two dense
        clusters at a time.
  - **Arbitration**: `ui_theme_resolve_touch_target(lv_obj_t *default_target,
        lv_point_t point)` — given the widget LVGL's own
        `lv_indev_search_obj()` picked, returns it unchanged unless it's a
        member of a registered group, in which case it checks every group
        member whose `lv_obj_get_click_area()` (LVGL's own already-extended
        box — the exact box `ui_theme_apply_touch_area()` sized, read back
        rather than re-derived) contains the touch point, and returns
        whichever candidate's real (unexpanded, `lv_obj_get_coords()`)
        center is closest to the point (squared-distance comparison, no
        `sqrt` needed).
  - **How the redirect actually reaches LVGL** — read `lv_indev.c` to confirm
        rather than guess, per this section's own established habit:
        `touch_read_cb()` calls the public `lv_indev_search_obj()` itself
        against the raw point (the same call LVGL's real pipeline is about
        to make), passes the result through
        `ui_theme_resolve_touch_target()`, and if arbitration picked a
        *different* widget, overwrites `data->point` with that widget's own
        center before returning. `_lv_indev_read()`
        (`components/lvgl/src/indev/lv_indev.c` ~line 765) copies
        `data->point` straight into `indev->pointer.act_point` right after
        the read callback returns, and `indev_proc_press()`
        (~line 1264/1270, via `pointer_search_obj()` ~line 1656) re-resolves
        the press from that exact point through LVGL's own public search
        path. So the override only has to correct the *point* the next
        official search will use — it never needs to touch
        `indev_obj_act`/`act_obj` directly, synthesize an event, or
        duplicate any part of LVGL's press/release/drag state machine.
  - **Cost when unused**: `ui_theme_touch_groups_active()` is one flag check;
        `touch_read_cb()` skips the extra `lv_indev_search_obj()` call
        entirely unless at least one group is registered — true of every
        page today, so this costs nothing yet.

**Nothing calls `ui_theme_register_touch_group()` yet** — same situation as
`ui_theme_apply_touch_area()` when it was written: no dense grid page (10.3)
exists yet to have overlapping extended click areas in the first place. A
future dense layout opts in by building its widget array as usual, calling
`ui_theme_apply_touch_area(widget, true /* compact */)` per widget as
10.3-style pages already would, and then passing that same array to
`ui_theme_register_touch_group()` once — no change to widget creation itself
is required.

### 10.5 Web/LCD parity rule

- [ ] **Whenever either the LCD screen or the web interface changes,
      consider whether the other should change too.** If the answer isn't
      clear, ask the user rather than guessing; if it's clear-cut (e.g. a
      new zone field needs to show up in both places), make the matching
      change without asking. Applies to both directions — a web feature
      added later needs the same consideration for the LCD, not just LCD
      to web.

### 10.6 Web dashboard restyle to match the LCD

- [x] Rework the existing web pages (`main_page.html`,
      `zones_page.html`, `rules_page.html`, `profiles_page.html`,
      `wifi_provision_page.html`) to use the same KlipperScreen-derived
      style (10.2) as the LCD, so the two front ends read as one product.
  - [x] Look for a small CSS approach rather than a JS framework, given
        this is served from ESP32 flash with no build step today: either
        a hand-written shared `style.css` embedded alongside the existing
        pages (cheapest, no new dependency, matches how the pages are
        built now), or a tiny CSS-only utility library if one closely
        matches KlipperScreen's look. Avoid anything requiring Node/
        bundling — the existing pages are static files `EMBED_TXTFILES`'d
        straight from flash (section 0's web-server decision), and a
        build pipeline would be new infrastructure this project doesn't
        have.

  **Status update (2026-08-17): all six pages restyled, palette pulled
  verbatim from `docs/UI_THEME.md`, no new files/routes.** Also covers
  `readiness_page.html`, which this section's page list above missed but
  which shares the exact same theming structure as the other five and was
  restyled identically.

  **Approach taken: duplicated `<style>` block, not a shared `theme.css`
  route.** A 7th `EMBED_TXTFILES`'d file served at its own route was
  considered (per this section's original note) but each page's HTTP
  handler lives in a *different* `.c` file (`wifi_provision_http.c`,
  `zones_http.c`, `rules_http.c`, `profiles_http.c`, `readiness_http.c`)
  with its own `httpd_register_uri_handler` call and its own extern
  `_binary_..._start`/`_end` symbols — there's no single hub file that
  already serves all six. Adding one shared CSS route cleanly would mean
  touching CMakeLists.txt plus five separate `.c` files' registration
  code, blind, with no build tool available this pass to verify any of
  it. That's a bigger, riskier change than this pass should take for a
  visual-only restyle, so each page keeps (and now duplicates) its own
  `<style>` block instead, per the section's own fallback guidance.

  Each of the six files' existing light/dark toggle (`--bg`/`--fg`/
  `--muted`/`--border`/`--button-bg`/`--card-bg`/... custom properties,
  `#themeBtn`, `localStorage['kilnctl-theme']`, `data-theme` attribute)
  is untouched and still fully functional. What changed: a new `:root {
  --ui-bg: #1a1f2b; --ui-card: #242a3a; --ui-text-primary: #f0f0f0;
  --ui-text-secondary: #9aa0ae; --ui-accent-1: #e8974e; --ui-accent-2:
  #a15fd6; --ui-accent-3: #3ec6c6; --ui-accent-4: #5cc06e; --ui-accent-5:
  #d6555f; --ui-radius: 10px; --ui-padding: 8px; --ui-touch: 72px;
  --ui-status-bar: 32px; }` block (exact hex/px values from
  `docs/UI_THEME.md`, same in all six files) was added, and only the
  *dark* variant of each page's existing toggle (`@media
  (prefers-color-scheme: dark) :root:not([data-theme="light"])` and
  `:root[data-theme="dark"]`) now points its `--bg`/`--fg`/`--muted`/
  `--card-bg`/etc. at these `--ui-*` values instead of its own
  hand-picked hex, so default (no stored preference, dark OS) and
  explicit dark mode match the LCD; explicit light mode is untouched.
  Page-specific status/verdict colors (`--ok`/`--warn`/`--bad`/`--err`/
  `--not-done`/`--cannot-yet`/`--off`/`--fault-color`/`--on-color`/
  `--link`) were remapped onto the five accents in the dark variant only
  (green=ok/on, orange=warn, teal=link, red=fault/err/not-done, held-back
  red also doubles as the one `--fault-color`/danger use per the doc's
  "accent-5 reserved for alarm/stop" note). `border-radius: 4px` became
  `var(--ui-radius)` (10px) everywhere it appeared. `main_page.html` and
  `zones_page.html` got `min-height: var(--ui-touch)` (72px) added to
  their interactive controls (`button`, `.relay-toggle`, `.run-btn`,
  `.danger-btn`, `a.button`, `select`); the other four pages got it on
  their generic `button`/list-row selectors too since none of them are
  meaningfully smaller-touch-target pages than the other two. Zone-group
  headers on `main_page.html` get a 4-way rotating accent color
  (`nth-of-type(4n+1..4)`) matching the LCD's per-zone coloring scheme,
  CSS-only (no JS/element-ID changes).

  No `.c`/`.h` file, no element ID, no `getElementById`/`querySelector`
  target, no `fetch()`/POST body, and no form field changed — grepped
  each file's `<script>` block against the diff before finishing to
  confirm. Not built or run against real hardware/browser this pass (no
  build tool available); worth a visual check on a phone and the LCD's
  browser once a build is possible, same caveat as 10.7's status note
  below it.

### 10.6a Gzip the embedded web pages

Added 2026-08-17, evaluating an outside suggestion against this project's
actual architecture rather than applying it as-is: the standard "ESP32 web
server is slow" advice (SPIFFS/LittleFS with many small `.css`/`.js` files,
serve gzipped, bundle into an SPA) mostly does not apply here — every page is
already a single self-contained HTML file with inline `<style>`/`<script>`,
`EMBED_TXTFILES`'d straight from flash (section 0's web-server decision), so
there is no multi-file-request problem to solve and no SPA rewrite is
justified. The one piece worth taking: **gzip the embedded HTML blobs**,
since `esp_http_server` can serve a pre-gzipped body directly with a
`Content-Encoding: gzip` header, no runtime compression cost, and it shrinks
both flash usage and transfer time for free on any client that sends
`Accept-Encoding: gzip` (every real browser does).

The "compile CSS/JS to native C++ for the LCD display" half of that same
suggestion (Gaya-style build-time CSS→struct compilation, JS→bytecode/QuickJS
for on-glass UI) is NOT adopted — it would replace the LVGL work this project
already built and reviewed (`kiln_ui.c`, `ui_theme.c`, `lvgl_port.c`, every
`ui_page_*.c`) with an unproven, much smaller-ecosystem toolchain for no
concrete problem it solves. Recorded here as a decision, not silently
ignored: LVGL stays the LCD rendering backend (10.1's own decision, unchanged).

- [ ] CMake step (likely in `App/drivers/CMakeLists.txt`, alongside the
      existing `EMBED_TXTFILES` list) that gzips each of the six HTML pages
      at build time before embedding, OR embeds them raw and gzips is
      pre-generated and checked in -- decide which based on whether this
      project's build already has a `gzip`-capable tool on PATH in every dev
      environment (Windows dev machines are the wrinkle here -- CMake's
      `find_program(GZIP)` or a Python-based `gzip` module call avoids
      depending on a Unix tool being installed)
- [ ] Each HTTP handler that currently does
      `httpd_resp_set_type(req, "text/html"); httpd_resp_send(req, page, len)`
      needs `httpd_resp_set_hdr(req, "Content-Encoding", "gzip")` added, and
      must not be sent to a client that didn't advertise
      `Accept-Encoding: gzip` in its request (check the request header;
      fall back to serving the uncompressed blob for a client that doesn't
      support it, or keep both blobs embedded and pick one at request time --
      decide which based on how much flash headroom this actually costs)
- [ ] Re-verify byte-for-byte that nothing about the *content* changes --
      this is a transport-encoding change only, same risk shape as 10.1a's
      `dashboard_get_status()` extraction (behavior-preserving refactor,
      verify by comparison rather than trust)

### 10.7 Onboard IC temperature sensors

- [x] Several ICs on the board (MAX31856s, the ESP32-S3 itself, and any
      other part with an on-die/onboard temp sensor) expose their own
      temperature reading, separate from the thermocouple-measured kiln
      temperature. Surface these on **a separate menu/page** — both LCD
      (a nav item alongside 10.3's Configuration/Temperature items) and
      web (a new route, not mixed into the main dashboard) — since this
      is board-health diagnostic data, not kiln-process data, and mixing
      the two would make the main page harder to read at a glance.
      **Done: LCD nav item/page and the JSON route both built and
      build-verified (see status update below); a styled HTML web page for
      the route is still open, tracked in that update.**

  **Status update (2026-08-17): web JSON endpoint built, LCD side not
  started.** `App/drivers/board_temps.c`/`.h` is a new driver module that
  surfaces both real onboard sources:
  - The ESP32-S3's own internal die-temperature sensor, brought up via
    ESP-IDF's `driver/temperature_sensor.h` (`temperature_sensor_install()`/
    `_enable()`/`_get_celsius()`), init-once in `board_temps_start()`
    (called early in `app_main`, independent of every other bus) and read
    on demand through `board_temps_get()`.
  - Each active MAX31856's `cj_temperature_c` (its own cold-junction/local
    temperature, already computed by every `MAX31856_read_all()` call for
    the thermocouple linearization math) — nothing new read from hardware,
    `board_temps_get()` just accepts the same `MAX31856Reading` array
    `dashboard_http.c` already gets and republishes the field under a
    board-health name.

  `GET /api/board_temps` (registered by `board_temps_http_start()`,
  wired into `app_main` right after `dashboard_http_start()`, same
  NULL-tolerant `thermo_bus` pointer) returns
  `{"esp32_c": <float or null>, "thermo_cj_c": [<float or null>, ...]}` —
  null (not 0) for whichever field has no valid reading this request.

  **NOT VERIFIED AGAINST THE INSTALLED TOOLCHAIN OR REAL HARDWARE THIS
  PASS.** The `temperature_sensor.h` API surface (function names/
  signatures/`TEMPERATURE_SENSOR_CONFIG_DEFAULT`) is written from
  documented ESP-IDF v5.0+ behavior, not confirmed against this repo's
  actual installed IDF v6.0.2 headers — no `managed_components/` manifest
  or installed SDK checkout was reachable to grep from inside the repo
  this pass.

  **Status update (2026-08-18): build-verified against the real toolchain,
  LCD nav item/page built.** `idf.py -C firmware/KilnFW build` (via
  `Microsoft.v6.0.2.PowerShell_profile.ps1`) now runs clean end to end --
  `KilnCtrl.bin` 0x153cd0 bytes, 9% of the app partition free. The guessed
  `temperature_sensor.h` API surface (`temperature_sensor_install()`/
  `_enable()`/`_get_celsius()`, `TEMPERATURE_SENSOR_CONFIG_DEFAULT`) and the
  `esp_driver_tsens` CMake `REQUIRES` component name both turned out to be
  correct as written against the installed IDF v6.0.2 headers -- no
  API-surface fix was needed in `board_temps.c`/`.h`. The actual build
  breakage found and fixed this pass was unrelated to the guessed API:
  `App/drivers/CMakeLists.txt`'s 10.6a gzip `execute_process()` block ran
  during ESP-IDF's early `CMAKE_BUILD_EARLY_EXPANSION` REQUIRES-harvest
  pass (where `CMAKE_CURRENT_SOURCE_DIR` isn't the real source tree yet)
  and needed an `if(NOT CMAKE_BUILD_EARLY_EXPANSION)` guard; plus a handful
  of small C/LVGL-v9.5.0 fixes (`(void)TAG;` at file scope, `LV_OPA_TRANSP`
  vs the nonexistent `LV_OPA_TRANSPARENT`, a format-truncation buffer size)
  across `ui_page_home.c`/`ui_page_config.c`/`ui_page_temperature.c`/
  `ui_page_board_health.c` and `settings.h`'s touch-cal macro aliasing --
  see commit `3335276`.

  The LCD/LVGL nav item is now built: `App/drivers/ui_page_board_health.c`/
  `.h`, a standalone page (not folded into the home/dashboard page, matching
  the web side's separate-route decision) reached via a "Board Health"
  button on `ui_page_config.c`'s Configuration hub and registered as
  `kiln_ui_register_page("board_health", ...)` in `kiln_ui.c`. It calls
  `board_temps_get_live()` (the plain-C getter `board_temps.c` already
  exposed per 10.1a's shared-backend rule) directly -- the exact same read
  `GET /api/board_temps` performs, not a second implementation -- and shows
  the ESP32-S3 die temp plus one row per possible MAX31856 channel
  (`MAX31856_CHANNEL_COUNT`, always all rows drawn), each independently
  null-tolerant ("n/a" in secondary text when that field's `_valid` flag is
  false), refreshed on a 1 s `lv_timer`.

  **Still unverified: real hardware.** No ESP32-S3/MAX31856 board is
  attached in this environment -- actual die-temp/cold-junction readings
  and on-panel LCD rendering have not been confirmed live, only that the
  code builds and the logic matches the JSON endpoint's already-established
  contract. Also still open, unchanged from before: a real web *page* for
  `GET /api/board_temps` (10.7 only asked for the JSON endpoint + LCD page
  this pass; a styled web page is future 10.3/10.6-adjacent work).

### 10.8 Multi-thermocouple-per-zone (cross-reference: section 3)

**Status update (2026-08-17): first slice built.** The config model, the
combining function, and the control/guard read paths this section names are
all wired end to end. Not yet done: `zones_page.html` has no UI for
assigning more than one channel to a zone (server-side default keeps every
existing/UI-driven zone on its legacy single channel — see below).

**Status update (2026-08-18): the two remaining gaps from the above closed.**
`thermo_combine.c`/`.h` is now wired into the host test suite
(`App/test/build_host_tests.ps1`, `test_thermo_combine.c`), and
`autotune_engine.c`'s step-test read path now reads its zone's combined
multi-channel value through `thermo_combine()` rather than the legacy single
channel — see both bullets below for detail. `zones_page.html`'s missing
multi-assign UI is still open, untouched this pass (out of this pass's named
scope). Nothing here has run against real hardware (no thermocouples
attached this session, same caveat every other guard/control item in this
file carries).

- [x] Section 3's zone model is currently one thermocouple channel per
      zone (`zones_http.h`'s scope note, section 3's "Named zones" item).
      Extend it to allow **more than one thermocouple assigned to the
      same heater zone**, with the zone's control temperature computed
      from a combining function across its assigned thermocouples rather
      than a single channel.
      **Done**: `zone_cfg_t::thermo_mask` (`App/drivers/zones_http.c`), a
      bitfield alongside the existing `relay_mask` — bit N-1 = MAX31856
      channel N belongs to this zone, same convention `relay_mask` already
      established, matching this file's own "the natural pattern to follow"
      framing rather than a second single-index field.
      `zones_config_get_thermo_mask()` (`zones_http.h`) mirrors
      `zones_config_get_relay_mask()`'s naming and "false means cannot
      answer" contract. Validated against `thermo_count` on POST the same
      way `relay_mask` is validated against `relay_count`.
      Persistence: `ZONES_CFG_VERSION` bumped 3→4 — this file already had a
      versioned-blob migration mechanism (`nvs_load_from()`/
      `migrate_zones_cfg_v1_to_current()`), the same one the 2026-08-12
      relay_cycles.c note (section 6A.1) says a field addition needs, so
      this is the "bump the version" branch of that note's two safe options,
      not the untested one. Unlike every field version 4 could have grown
      by leaving a migrated blob's new bytes zeroed, thermo_mask=0 is NOT a
      safe default for an existing zone (it means "no thermocouple
      assigned," i.e. permanently invalid) — `migrate_zones_cfg_v1_to_current()`
      explicitly fills bit i for zone i on any migrated blob, reproducing
      the exact implicit mapping every pre-10.8 zone was already using. The
      POST handler applies the identical fallback when `z%u_thermo_mask` is
      simply absent from the submitted body (current `zones_page.html`,
      `pc_tools`/MCP, the test harnesses) — see `parse_zone_fields()`'s
      comment for why an absent-means-0 default (`z%u_xzone`'s convention)
      would have been wrong here specifically. An explicit `0` sent by a
      client that DOES know the field is honoured as a real "no
      thermocouple," reaching the combiner as a zero-valid-readings zone.
  - [x] Decide the combining function — arithmetic mean of all assigned,
        valid (non-faulted) readings is the default candidate; open
        question whether outlier rejection or a max/min-biased combiner
        is ever wanted (e.g. control off the hottest reading, not the
        average, if the goal is "never let any point in the zone
        overshoot"). Needs a decision, not just "mean," before building —
        the terms aren't interchangeable for a safety-adjacent control
        input.
        **Decided for this pass: arithmetic mean**, per this section's own
        stated default. Outlier rejection and a max/min-biased combiner
        remain undecided and unbuilt — left as an explicit open question,
        not guessed at, matching this item's own instruction.
        `App/drivers/thermo_combine.c`/`.h` (new): pure C, no FreeRTOS, no
        ESP-IDF, no logging, no I/O, same discipline pid.h documents and for
        the same reason (host-unit-testable).
        **Host-test wiring done (2026-08-18)**: `App/test/build_host_tests.ps1`
        now builds `thermo_combine.c` alongside pid.c/thermal_guard.c/
        heater_output.c, and `App/test/test_thermo_combine.c` (new, wired
        into `test_main.c` matching `test_pid.c`'s pattern exactly — same
        `test_common.h` `TEST_CHECK`/`TEST_CHECK_NEAR` macros, one
        `run_test_thermo_combine(void)` entry point) covers: all-valid-
        channels mean; one masked-in channel faulted and dropped (not
        zeroed — mean of the survivors, not skewed toward 0); all masked-in
        channels faulted returning NaN with `out_valid=false`; a channel
        excluded by the mask never contributing even when its own
        `channel_ok` is true; and the mask/validity independence and
        past-`channel_count`-bits-ignored cases the header comment
        documents but the first pass's own tests never exercised.
  - [x] A thermocouple that faults (open/short, per `MAX31856_read_all()`)
        must not silently corrupt the combined value — drop it from the
        combination and fall back per however many remain; zero
        remaining valid readings for a zone is the existing "thermocouple
        invalid" case, unchanged.
        **Done**: `thermo_combine()` takes parallel per-channel
        reading/validity arrays and drops any channel whose validity flag is
        false rather than including it as 0 or a stale value. The validity
        check itself is NOT reinvented — `profile_executor.c` still computes
        it exactly as before (`spi_failed`/`isnan`/`THERMO_FAULT_OPEN|OVUV|
        TCRANGE`), and just hands the combiner the same array it always
        built. Zero valid channels among a zone's mask returns NaN with
        `*out_valid = false`, mirroring `zones_config_apply_cal()`'s
        NaN-passthrough convention — same "thermocouple invalid" case a
        single unassigned/faulted channel has always produced, unchanged.
  - [x] The direction/rate sanity monitor (section 6, "6A" thermal guards)
        and PID/bang-bang control (6A.1/6A.2) both currently read one
        `MAX31856Reading` per zone — both need to consume the combined
        value instead, and thermal_guard's guard 6 (sensor-invalid) needs
        its own definition of "invalid" extended to "all assigned
        thermocouples for this zone are invalid," not just one.
        **Done for the control-tick and run-start baseline read paths in
        `profile_executor.c`**: the main tick's per-channel raw read is now
        immediately combined per zone (via `thermo_combine()` against that
        zone's live `thermo_mask`) before `pid_update_terms()`/bang-bang and
        `thermal_guard_tick()` ever see it, and the ramp baseline read at run
        start does the same so a multi-thermocouple zone starts its ramp
        math from the same combined number the first control tick computes.
        Guard 6's extended definition falls out for free: `thermal_guard.c`
        was already written against an opaque caller-supplied `sensor_ok`
        bool (see `thermal_guard_input_t`'s doc comment) and never needed to
        know how many channels fed it — no code changed inside
        `thermal_guard.c` itself, only its and `thermal_guard_input_t`'s doc
        comments, to say explicitly that `sensor_ok` is now the zone's
        combined verdict.
        **Done for `autotune_engine.c`'s step-test read path (2026-08-18)**:
        this was the one read path the 2026-08-17 pass explicitly named as
        left out. `task_entry()`'s per-tick read now follows
        `profile_executor.c`'s exact split — read every physical channel's
        raw value/validity into `ch_raw_c`/`ch_ok` first, then combine.
        Because this file's `zone_baseline_c`/`zone_trace`/coupling-matrix
        arrays are indexed by physical channel standing in for zone (the
        pre-10.8 channel-i-is-zone-i mapping, still load-bearing for the
        cross-gain fit against every OTHER configured zone — 6A.5(b) scope,
        untouched), only the slot for `s_at.zone_index` itself — the zone
        actually under test — is overwritten with
        `thermo_combine(ch_raw_c, ch_ok, MAX31856_CHANNEL_COUNT, tmask,
        &combined_valid)` against that zone's live `thermo_mask`; every peer
        zone's slot keeps the legacy single-channel reading it always had.
        That combined value is what `actual_c`/`actual_valid`, the SETTLING
        baseline, the recorded trace, and the guard's raw `measurement_c`
        input all read from this tick forward — the step, the guards, and
        the eventual FOPDT fit are now driven by the same multi-channel
        number a running profile would use for this zone, not a stale
        single-channel proxy. Sample cadence and every fitting/guard
        equation are unchanged; only the temperature this file feeds them
        moved from single-channel to combined. Not host-tested: this file
        depends on FreeRTOS/ESP-IDF (task, semaphore, `esp_log`) and was
        never in `build_host_tests.ps1`'s source list before this pass
        either — only the pure-math `pid_autotune.c` fitting module is
        host-tested (`test_pid_autotune.c`, `test_sim_kiln.c`), and neither
        references channels, zones, or `thermo_mask`, so neither needed any
        change for this to stay correct.

### 10.9 LCD network settings page + QR codes for AP/site connect

**2026-08-18, added to plan (not yet built).** The LCD only has a read-only
WiFi/IP/mDNS status line on the home page (10.3's status-bar readout, added
this session). The user wants a real network settings page on the LCD
mirroring what `wifi_provision_page.html` already does on the web (section
1 / 8.4: mode toggle, scan, connect, saved-network list with forget, AP
identity display) — plus a QR code, on both surfaces, for easy phone
connection.

**Status update (2026-08-18): built, LCD + web, both committed.**
`ui_page_network.c`/`.h`, `wifi_status_ui.c`/`.h`, and the web QR encoder
(`wifi_provision_page.html`) all landed this pass — see the bullets below
for what shipped vs. what's still open.

**Status update (2026-08-18, later same day): the deferred `ui_page_home.c`
AP-mode QR is now also built** — see the "QR code: join the board's
fallback AP from a phone" bullet below for the details (new
`build_ap_qr_card()`/`refresh_ap_qr()`, not a shared helper with
`ui_page_network.c`, and why). This closes out the one bullet-level decision
this section had left open.

**Build-config note:** finishing this section pushed `KilnCtrl.bin` past
the `factory` app partition's 1500K budget (0x172ba0 used vs. 0x177000
available, ~1% free before this pass even started — see partitions.csv's
own sizing comment). Rather than touch `partitions.csv` (live NVS data
sits on the partitions after `factory`, per that file's own repeated
warnings against resizing anything without a bench read-out first),
switched to `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (`-Os`, was
`_DEBUG`/`-Og`) — reclaimed ~125K, landing at 9% free. `sdkconfig` itself
is gitignored (generated, machine-local), so this is pinned in the new,
committed `sdkconfig.defaults` instead — a fresh checkout/reconfigure
picks it up automatically, no per-builder memory required.

- [x] **`ui_page_network.c`/`.h`** — new LCD page, reachable from
      `ui_page_config.c`'s settings hub (same nav pattern as `zones`/
      `relays`/`board_health`). Per 10.1a's shared-backend rule, every
      control on this page calls the exact same `wifi_prov.h` getters/
      setters `wifi_provision_http.c`'s handlers already call — no parallel
      read of `wifi_prov.c` internals:
      - Mode + state readout: `wifi_prov_get_mode()`/`_get_state()`/
        `_is_sta_connected()`/`_get_sta_ip()`/`_get_sta_rssi()` (same set
        `status_get_handler()` uses, and the same set `ui_page_home.c`'s
        new `wifi_status_text()` already calls — factor the common
        "human-readable WiFi state" formatting out of `ui_page_home.c` into
        a small shared helper both pages call, rather than a second copy of
        that switch statement).
      - Scan: `wifi_prov_scan()` — LVGL list of results (`ssid`/`rssi`/
        `secure`), tap-to-select feeding a saved-network add flow. Mind
        `wifi_prov_scan()`'s existing `ESP_ERR_NOT_SUPPORTED` refusal in
        AP-only mode (section on `scan_get_handler`) — the LCD page needs
        the same "scanning disabled in AP mode" messaging the web page
        shows, not a silent empty list.
      - Saved networks: `wifi_prov_get_saved_networks()` / `_forget()` —
        list with a per-row forget button, mirroring
        `wifi_provision_page.html`'s `renderNetworkList()`/`forgetNetwork()`.
      - AP identity (SSID/password the board's own fallback AP presents):
        `wifi_prov_get_ap_ssid()`/`_get_ap_password()` — same values
        `status_get_handler()` already exposes (that handler's own comment
        explains why the board's OWN AP password is fine to show on a
        settings surface, unlike any saved network's password, which this
        codebase never surfaces anywhere).
      - Add-network / mode-switch forms: on-screen keyboard for SSID/
        password entry (LVGL's `lv_keyboard` widget) — this is the one
        piece with no direct web-page equivalent to mirror (the web page
        gets free text entry from the browser), so it needs its own design
        pass when this gets built, not just a port.
      - Touch targets follow `ui_theme.h`'s existing conventions
        (`ui_theme_apply_touch_area()`, `UI_THEME_MIN_TOUCH_TARGET_PX`) same
        as every other LCD page.
      **Audited 2026-08-18 (checkbox was stale -- code already did all of
      this from the 25cbe96 pass, box just never got ticked):** mode
      readout, scan (with the AP-mode "Scanning disabled in AP mode"
      message), saved-network list with per-row forget + confirm dialog,
      AP identity display, and add-network flow are all real, calling the
      exact `wifi_prov_*` getters/setters this bullet names -- see
      `ui_page_network.c`'s own header comment for the full call mapping.
      **On-screen keyboard: built, not stubbed.** The one piece this bullet
      flagged as needing its own design (no web equivalent) is a real
      `lv_keyboard_create()` bound to the connect-modal's password
      `lv_textarea` (`build_connect_modal()`) -- tap a scan result, the
      SSID is pre-filled from the tap (matching the bullet's own "SSID
      pre-filled from the tap, only the password needs a keyboard" framing),
      the keyboard handles password entry, Connect calls
      `wifi_prov_add_network()`. Mode switch needs no keyboard (two buttons,
      no free text). **Genuine gap found and fixed this pass**: the
      `LV_USE_QRCODE` Kconfig flip this page depends on lived only in the
      gitignored, machine-local `sdkconfig`, not in the committed
      `sdkconfig.defaults` -- a fresh checkout would fail to build. Added to
      `sdkconfig.defaults` alongside the existing `-Os`/CLIB-malloc entries.
      **Still not built**: editing the board's own AP SSID/password from
      this page (read-only here, per the file's own header comment --
      `wifi_provision_page.html` remains the only way to change it).
- [x] **QR code: join the board's fallback AP from a phone.** When the
      board is in AP or AP+STA-fallback mode (`wifi_prov_get_mode() ==
      WIFI_PROV_MODE_AP` or `wifi_prov_get_state() ==
      WIFI_PROV_STATE_UNPROVISIONED`/`AP_MODE`), render a QR code encoding
      the standard WiFi-join URI format phones already recognize natively
      (no app needed — both iOS and Android camera apps parse this):
      `WIFI:T:WPA;S:<ap_ssid>;P:<ap_password>;;` (or `T:nopass;` if the AP
      password is ever made optional — it currently is not, per section 1).
      Shown on `ui_page_network.c` (this page is exactly where an operator
      standing at the kiln needs it) and worth considering for
      `ui_page_home.c` too when the board is in AP mode, since that is
      precisely the state where nothing else on the LCD is more useful to
      show. Not shown when already connected to a home network (a QR code
      for a network the phone can't join is dead weight on the screen).
      **2026-08-18: the `ui_page_home.c` half landed.** New
      `build_ap_qr_card()`/`refresh_ap_qr()` in `ui_page_home.c` add a
      compact (90px, vs. the network page's 140px — this page is already
      dense, per its own header comment) AP-join QR card, first thing in the
      scrollable content area right under the status bar, `LV_OBJ_FLAG_HIDDEN`
      by default and revealed only under the exact same condition
      `ui_page_network.c`'s `refresh_cb()` uses
      (`wifi_prov_get_mode() == WIFI_PROV_MODE_AP` or
      `wifi_prov_get_state() == WIFI_PROV_STATE_UNPROVISIONED`/`AP_MODE`),
      re-evaluated every refresh tick since the operator could flip mode from
      `ui_page_network.c` while this page sits in the background (pages are
      never torn down). Same `WIFI:T:WPA;S:<ssid>;P:<password>;;` payload,
      same `lv_qrcode_create()`/`lv_qrcode_update()` calls, same
      "only re-encode when the string actually changed" gate
      (`s_ap_qr_last`) as `ui_page_network.c`'s copy. **Not factored into a
      shared helper** (e.g. in `wifi_status_ui.c`, which already holds the
      one shared "WiFi state to text" formatter this section's first bullet
      asked for): `ui_page_network.c` is out of scope to touch this pass
      (a separate, already-shipped concern), so a shared QR-building helper
      today would have exactly one real caller (`ui_page_home.c`) and leave
      `ui_page_network.c`'s copy unconverted regardless — that's not the
      "one implementation instead of two copies" outcome a shared helper is
      for, just a relocation with a detour. Revisit if `ui_page_network.c`
      is ever touched again for an unrelated reason. Build: clean,
      `-Wall -Wextra -Werror`, zero new warnings; flash partition free space
      unchanged at 10%.
- [x] **QR code: open the web dashboard from a phone.** When `sta_connected`
      is true, render a second QR code encoding a plain URL — prefer
      `http://kiln.local` (10.3's status line already resolves and shows
      this) with the raw `sta_ip` as a fallback/second QR if mDNS isn't
      guaranteed to work on the phone's network (some Android versions and
      most enterprise/guest WiFi don't do mDNS reliably — worth showing
      both rather than picking one). Same placement question as above:
      `ui_page_network.c` for sure, `ui_page_home.c` optionally once
      connected. **Audited 2026-08-18, checkbox was stale**: both QRs are
      real in `ui_page_network.c`'s `refresh_cb()` (`s_dashboard_qr`
      resolving via `mdns_hostname_get()` with a `kiln.local` fallback,
      `s_ip_qr` from `wifi_prov_get_sta_ip()`), each gated on
      `update_qr_if_changed()` so they don't re-encode every tick. Not
      added to `ui_page_home.c` — this bullet only says "optionally", and
      that page is already dense per its own header comment.
- [x] **Web side**: `wifi_provision_page.html` already shows the AP SSID/
      password as text (`status_get_handler()`'s `ap_ssid`/`ap_password`
      fields) but has no QR code either — a phone that's already
      Wi-Fi-connected enough to load this page doesn't need the AP-join QR,
      but a *second* phone/device someone wants to hand the AP credentials
      to would. Low priority relative to the LCD QR (the LCD is reachable
      with zero network connectivity at all, which is exactly the
      bootstrapping problem a QR code solves; a browser already implies
      connectivity exists somewhere), but worth the small addition once the
      LCD version exists, generating from the same WIFI: URI string so
      there's one format to test instead of two. **Audited 2026-08-18,
      checkbox was stale**: `#apQrSection`/`#apQrCanvas` plus the `KilnQr`
      IIFE (a byte-mode port of the same `qrcodegen.c` LVGL vendors) are
      real, embedded client-side per the file's own header comment — no
      CDN, no server-side QR library needed.
- [x] **QR rendering**: LVGL v9.5.0 (vendored submodule, section 10.1)
      ships `lv_qrcode` (`LV_USE_QRCODE` in `lv_conf.h`/Kconfig) — confirm
      it's enabled in this project's LVGL config (it is off by default in
      upstream LVGL) before assuming it's available; if off, this is a
      one-line Kconfig/`lv_conf.h` flip, not a new dependency. `lv_qrcode_create()`
      + `lv_qrcode_update()` take a size and a raw byte string (the `WIFI:`
      or `http://` string above) and render directly to an `lv_obj_t` — no
      external QR-encoding library needed for the LCD side. For the web
      side, no server-side QR library exists in this codebase yet; either
      generate the WIFI:/URL string server-side and render client-side with
      a small embedded JS QR library (matching this project's "no CDN,
      everything embedded" convention — `theme.css`'s own embedding
      precedent), or skip the web QR entirely per the above priority note
      and revisit later. **Audited 2026-08-18**: `LV_USE_QRCODE` was
      confirmed off by default and flipped on, but only in the local,
      gitignored `sdkconfig` — never carried into the committed
      `sdkconfig.defaults`, so a fresh checkout would silently fail to
      build (`lv_qrcode_create()` undeclared). Fixed this pass: added
      `CONFIG_LV_USE_QRCODE=y` to `sdkconfig.defaults`. Web side skipped
      the "no server-side library" question entirely by generating and
      rendering client-side instead, per the bullet's own first option.
- [ ] Cross-reference 10.5 (web/LCD parity rule) once built: if the LCD
      gets a real network settings page, the "any time changes to either
      the screen or the web interface are made it should be considered
      whether to change the other" rule from the original LCD plan request
      applies here directly — the AP-join QR code in particular is a place
      the LCD is now strictly ahead of the web page, which is fine (the web
      page's equivalent gap is called out above, low priority, not a
      silent omission).

### 10.10 Safety processor GUI panel (ROADMAP.md M6)

**2026-08-18: built, web + LCD, both on the main dashboard/home page (not a
separate board-health-style page).** ROADMAP.md M6's checklist item "GUI
shows safety temperature, enclosure temperature and power" — the data
plumbing built now, independent of the link itself being bench-confirmed
dead (ROADMAP.md M0).

**Where, and why (unlike 10.7's board-health page):** `LINK_PROTOCOL.md`
sec 7 ("What the ESP web GUI should show") explicitly names
`dashboard_http.c` as the home for this — "A 'Safety Processor' panel,
always visible" — because safety/enclosure temperature and power are
kiln-process data an operator watches while firing, the opposite of 10.7's
board-health diagnostics (ESP die temp, MAX31856 CJ temps as a *board
health* signal), which earned their own separate page/route precisely to
keep that diagnostic noise off the main page. So this landed on the
existing `GET /api/status` endpoint and the existing LCD home page
(`ui_page_home.c`), not a new route/page.

- [x] **`dashboard_status_t` (`dashboard_http.h`) gained three fields**:
      `safety_temp_valid`/`safety_temp_c`, `enclosure_temp_valid`/
      `enclosure_temp_c`, `power_valid`/`power_w`. `dashboard_get_status()`
      (`dashboard_http.c`) fills the first two straight from
      `safety_link_get_status()`'s cache — `tc_temp_c` (`LINK_PROTOCOL.md`
      sec 6 Frame A, `SAFETY_CMD_GET_STATUS`, bytes 2..5, the safety
      processor's own thermocouple) and `cj_temp_c` (same frame, bytes
      6..9, the MAX31856 cold junction — sec 7's "Enclosure temperature ...
      this is the electronics enclosure, not the kiln"). `safety_link.c`
      already decodes this frame (`safety_apply_status()`, 9.0's
      `ANNOUNCE_VERSION` work landed alongside it) and already sends NaN,
      never 0, when `TEMP_VALID` is clear or nothing has arrived — `_valid`
      is computed here as `!isnan()`, not re-derived from the flag byte a
      second time.
      **2026-08-18 update: power now has a real codec and dispatch path.**
      `firmware/CommonFW` gained `kilnlink_power.{c,h}` (Frame E's 55-byte
      layout, host-tested — `test/test_power.c` + `test/vectors/
      power_vectors.json`, mirroring `kilnlink_status.c`'s round-trip/
      byte-exact-vector/hostile-input structure). `safety_link.c` hand-parses
      the same layout in a new `safety_apply_power()` (mirroring the
      `kilnlink_power_decode()` byte offsets — this component's ESP-IDF
      wrapper still compiles only `kilnlink_crc.c`/`kilnlink_frame.c`, same
      as Frame A, so `safety_link.c` continues the existing
      hand-rolled-parser convention rather than linking the codec directly)
      and `safety_drain_inbox()` now dispatches `SAFETY_CMD_POWER` (`0x0E`,
      newly defined in `uart_task_ids.h`) to it, caching the result in
      `safety_link_status_t` (`power_ever_received`, `power_total_w`,
      `power_channel_w[3]`, `power_mains_voltage_v`, etc. — extended, not a
      parallel struct). `dashboard_get_status()` now populates
      `power_valid`/`power_w` from that cache instead of hard-coding
      `false`/`NaN`. **2026-08-18 update: SaftyFW now sends Frame E too.**
      `link_task.c` gained `link_task_send_power()` (2 s cadence, same
      pattern as Frame B/DIAG), pulling `current_task_get_power()` and
      calling `kilnlink_power_encode()` from CommonFW directly (SaftyFW,
      unlike this ESP-IDF component, already links the full `kilnlink`
      static library). `current_sense.c`/`.h` gained `mains_voltage_v`,
      `calibrated`, `any_clipped`, `p_total_w` and `energy_wh` fields on
      `current_sense_power_t` so `link_task.c` does not need calibration-
      struct visibility to build the frame. Guards S3/S4 remain
      deliberately NOT wired to the current-sense output — SaftyFW's own
      `docs/CURRENT_SENSE.md` section 5 requires the per-channel CT-mapping
      commissioning check to pass on real hardware first, and no RP2040/CT
      hardware is attached to either build machine. So: **this still reads
      `null`/"---" here**, but the reason has changed again — it is now
      "no hardware attached to receive/verify a real Frame E against,"
      not "the sender doesn't exist."
- [x] `GET /api/status` gained `safety_temp_c`/`enclosure_temp_c`/`power_w`,
      each JSON `null` (not `0`) when its `_valid` flag is false — same
      null-tolerant convention `GET /api/board_temps` (10.7) established,
      applied here rather than reinvented.
- [x] LCD: `ui_page_home.c` gained a "Safety Processor" card (new
      `build_safety_card()`, three labels refreshed every `refresh_cb()`
      tick from the same `dashboard_get_status()` call this page already
      makes for zones/relays — TODO.md 10.1a's shared-backend rule, no
      second read of `safety_link.c`). Each row independently shows "---"
      when its field is invalid, matching 10.7's `ui_page_board_health.c`
      "n/a" convention (different placeholder text, same idea: never a
      plausible-looking number for missing data).
- [x] `idf.py -C firmware/KilnFW build` (via
      `Microsoft.v6.0.2.PowerShell_profile.ps1`) clean: `KilnCtrl.bin`
      0x154190 bytes, 9% of the `factory` partition free (was already at 9%
      free after 10.9; this pass's three extra floats/bools and one LCD
      card did not measurably move it).
- [x] `idf.py -C firmware/KilnFW build` re-run after the Frame E wiring
      above: clean, no new warnings. `ctest` in `firmware/CommonFW/build`
      (Ninja/MSVC, via `vcvars64.bat`): 7/7 host suites pass, including the
      new `test_power`.
- [ ] **Not hardware-verified, and cannot be this session.** No ESP32-S3/
      Pico is attached in this environment, and ROADMAP.md M0 already
      established the isolated link doesn't pass a byte end-to-end on the
      real board — so on real hardware today every one of these three
      fields still reads `null`/"---", by design, not by bug. Real
      verification needs both M0 (link fixed) and a Pico actually emitting
      Frame A/Frame E; SaftyFW does not build or send Frame E yet, so the
      new `safety_apply_power()` path has never been exercised against a
      live peer, only against the host codec's own round-trip/vector tests.
      Nothing here can be exercised against live safety telemetry until
      then.

### 10.11 Liveness: 1.5 s fault, 30 s firing-abort (ROADMAP.md M6)

**2026-08-18.** `LINK_PROTOCOL.md` sec 8's two-timeout rule: closes both
open M6 checklist items — `SAFETY_FAULT_SRC_SAFETY_LINK` redefined as "no
telemetry within 1.5 s", and 30 s of continued silence aborting a running
firing. Reuses the existing fault/abort machinery both items point at
rather than building parallel paths.

- [x] **1.5 s fault, decoupled from the configured poll period.**
      `safety_link_up_locked()` already worked out to 1.5 s at the default
      500 ms poll period (`SAFETY_LINK_UP_PERIODS=3`), but that arithmetic is
      period-relative — reconfiguring the poll period via `SET_POLL_PERIOD`
      would silently move the safety threshold along with it. Added
      `SAFETY_LINK_STALE_MS` (1500) and a pure, host-testable
      `safety_link_is_stale(uint16_t age_ms, uint32_t threshold_ms)` inline
      to `safety_link.h`; `safety_update_health()` in `safety_link.c` now
      ORs `safety_link_is_stale(age, SAFETY_LINK_STALE_MS)` into its `up`
      computation before the existing `safety_link_set_fault_source(link,
      SAFETY_FAULT_SRC_SAFETY_LINK, !up || version_mismatch)` call — so a
      reconfigured poll period can only make the fault fire *sooner* than
      the period-relative check, never later. At the default config the two
      agree and this is a no-op; the fixed ceiling is what the ROADMAP item
      actually asks for.
- [x] **30 s silence aborts a firing**, wired into the existing guard-9
      watchdog rather than a new task. `profile_executor.c`'s
      `watchdog_task_entry()` already runs every `WATCHDOG_CHECK_PERIOD_MS`
      (2 s) checking for a stuck control task; it now also calls
      `safety_link_get_status(s_exec.safety, &safety_status)` each tick and
      checks `safety_link_is_stale(safety_status.age_ms,
      SAFETY_LINK_FIRING_ABORT_SILENCE_MS)` (new constant, 30000, in
      `safety_link.h`). If silence exceeds 30 s while a firing is `RUNNING`
      or `PAUSED`, it faults the run the same way guard 9 does for a stuck
      control task: `s_exec.state = PROFILE_EXEC_FAULTED`, `fault_reason`
      set, `kiln_io_all_relays_off()` called, and `run_state_note()` records
      the ending outside the lock. Sec 8's "relays dropped and retried until
      the write succeeds" is handled by a third branch: once already
      faulted, each subsequent tick the link stays silent re-calls
      `kiln_io_all_relays_off()` without re-triggering `run_state_note()`.
      Does not separately assert `SAFETY_FAULT_SRC_SAFETY_LINK` — the 1.5 s
      check above already does that well before 30 s elapses, so a second
      assertion would be redundant, not additive.
- [x] `idf.py -C firmware/KilnFW build` (via
      `Microsoft.v6.0.2.PowerShell_profile.ps1`): clean, no new warnings.
- [ ] **Not hardware-timing-verified.** No ESP32-S3/Pico is attached in this
      environment (same gap as 10.10 above and ROADMAP.md M0's dead-link
      finding), so whether the fault really asserts at 1.5 s and the abort
      really fires at 30 s on real hardware is unverified — this closes the
      code gap, not the hardware-timing-verified gap. The comparison logic
      itself (`safety_link_is_stale()`) is a pure function and could be
      host-unit-tested the way `kilnlink_power`/`kilnlink_status` are in
      `firmware/CommonFW`, but it lives in `safety_link.h` (ESP-IDF-only
      driver, not a CommonFW host-buildable component) so no host test
      harness exists for it today — left as a follow-up, not attempted this
      pass to avoid scope creep into restructuring where the comparison
      lives.

### 10.12 ESP → Pico context broadcast, `SAFETY_CMD_PUSH_CONTEXT` (ROADMAP.md M5)

**2026-08-18.** `LINK_PROTOCOL.md` sec 4's 0x07 frame, built from real
board state and sent every poll period — closes ROADMAP.md M5's "ESP → Pico
context frames, including `relay_recent_mask`" item.

- [x] **`components/kilnlink/CMakeLists.txt`** now also compiles
      `CommonFW/src/kilnlink_context.c` — the codec landed in the earlier
      2026-08-18 pass was linked-but-unused until now; this is its first
      real caller.
- [x] **`safety_link.h`**: `SafetyLinkClass` gained `context_io`/
      `context_thermo_bus` (`void*` — kept untyped so this header stays free
      of a hard `kiln_io.h`/`MAX31856.h` dependency, same minimal-include
      rule `relay_authority.h` documents), `relay_last_on_tick[4]` +
      `relay_last_on_tick_valid[4]` for the recent-mask rolling window,
      `context_seq`, and `context_sample_counter[3]`. New public setter
      `safety_link_set_context_sources(link, io_or_null, thermo_bus_or_null)`
      and constant `SAFETY_LINK_CONTEXT_RECENT_WINDOW_S` (180 — clears sec
      4's "≥ 150 s, two heater windows plus decay margin" floor against
      `HEATER_WINDOW_MS` = 60000 in `profile_executor.c`).
- [x] **`safety_link.c`**: `safety_build_and_send_context()` builds a
      `kilnlink_context_t` and sends it via `kilnlink_context_encode()` +
      `uart_protocol_send_broadcast()` — same broadcast call site
      `safety_link_send_announce_version_once()` already uses. Field
      sources:
      - `relay_now_mask` / `relay_recent_mask`: `kiln_io_get_relay_shadow()`
        plus `safety_context_update_relay_recent()`, a poll-task-only
        rolling window keyed on `xTaskGetTickCount()` per relay bit.
      - Per-zone `measured_c`/`tc_fault`: one `MAX31856_read_all()` burst
        per push, matched back to `zone_index` via `MAX31856Reading::channel`
        (readings are packed by *initialized-channel position*, not channel
        number, per that function's own doc comment).
      - Per-zone `tc_type`: `MAX31856_get_config()` per channel.
      - Per-zone `sample_counter`: incremented in
        `context_sample_counter[i]` only when `MAX31856Reading::stale` is
        false for that channel's read this push — i.e. only when a fresh
        conversion was actually consumed at this call site, per sec 4's
        "Implementation note for KilnFW" (never incremented per-frame).
      - Per-zone `setpoint_c`/`ACTIVE`/`RELAY_ON`/`GUARD_TRIPPED`, and the
        top-level `PROFILE_RUNNING`/`ANY_ZONE_FAULTED`/`HEAT_REQUESTED`
        flags: `profile_executor_get_status()` (the same free accessor
        `dashboard_http.c` already reads) — `setpoint_c` is the one shared
        `target_c` for every currently-active zone (`profile_executor.c`'s
        "detuned decentralized PID against one shared setpoint"; there is no
        per-zone setpoint to report), `NaN` for an inactive zone, same
        invalid-value convention as `measured_c`.
      - `CONTEXT_VALID`: always set once this driver is initialized — an
        absent `thermo_bus`/`io` empties the numbers (`zone_count = 0`,
        `relay_now_mask = 0`), it does not make them untrustworthy.
      - `SIM_PLANT`: `#if CONFIG_KILNCTL_SIM_PLANT`.
      Called from `safety_poll_task()` right after `safety_update_health()`,
      every iteration — same `CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS` cadence
      sec 4 specifies, independent of whether that iteration's `GET_STATUS`
      exchange got a reply (this is a broadcast, not part of that
      request/reply pairing).
- [x] **`uart_task_ids.h`**: added `SAFETY_CMD_PUSH_CONTEXT` (0x07) to the
      `SAFETY_CMD_*` list for documentation/consistency with
      `kilnlink_context.h`'s `KILNLINK_CONTEXT_CMD` — `safety_link.c` itself
      encodes through the shared codec rather than by hand, so it doesn't
      reference this macro directly.
- [x] **`main.c`**: `safety_link_set_context_sources(&safety, io_ready ? &kio
      : NULL, thermo_bus.initialized ? &thermo_bus : NULL)` called right
      after `profile_executor_start()`, same non-fatal NULL-tolerant wiring
      convention as every other `*_start()` call in `app_main`.
- [x] `idf.py -C firmware/KilnFW build` (via
      `Microsoft.v6.0.2.PowerShell_profile.ps1`): clean, no new warnings.
- [ ] **Not verified against a real Pico.** No RP2040 is attached in this
      environment (ROADMAP.md M0's bench-confirmed dead link, same gap as
      10.10/10.11 above) — the frame is built and broadcast, encoding
      matches `kilnlink_context_encode()`'s own contract, but nothing here
      confirms a real `SaftyFW` build decodes it correctly. `SaftyFW`'s
      receive side for this frame is separately tracked in
      `firmware/SaftyFW/TODO.md`.

### 10.13 DIAG / TRIP_EVENT decode + dispatch (ROADMAP.md M5)

**2026-08-19.** `LINK_PROTOCOL.md` sec 6's Frame B (`SAFETY_CMD_DIAG`, `0x08`,
26 bytes) and Frame D (`SAFETY_CMD_TRIP_EVENT`, `0x0D`, 29 bytes) got
host-tested codecs in `firmware/CommonFW` (`kilnlink_diag.{c,h}`,
`kilnlink_trip.{c,h}`) in an earlier pass; this one closes the other half —
`KilnFW`'s `safety_link.c` decoding and caching them. Both frames are
Pico → ESP telemetry the Pico is not yet building (`SaftyFW`'s send path for
either is explicitly out of scope here — a separate pass, tracked in
`firmware/SaftyFW/TODO.md` — is what wires `link_task.c` to
`safety_core.c`'s guard state), so this closes "the ESP can receive and
decode these frames", not "the ESP has ever received a real one."

- [x] **`uart_task_ids.h`**: added `SAFETY_CMD_DIAG` (`0x08`) and
      `SAFETY_CMD_TRIP_EVENT` (`0x0D`) to the `SAFETY_CMD_*` list, next to
      the existing `SAFETY_CMD_POWER` (`0x0E`) entry and documented the same
      way — this ESP-IDF component still compiles only
      `kilnlink_crc.c`/`kilnlink_frame.c`/`kilnlink_context.c`
      (`components/kilnlink/CMakeLists.txt`), so `safety_link.c` hand-parses
      both new frames rather than linking `kilnlink_diag.c`/`kilnlink_trip.c`
      directly, same reasoning as `SAFETY_CMD_POWER`.
- [x] **`safety_link.h`**: `SAFETY_LINK_DIAG_FRAME_LEN` (26),
      `SAFETY_LINK_TRIP_EVENT_FRAME_LEN` (29), and the DIAG flags/boot-reason/
      state-byte constants mirrored from `kilnlink_diag.h`
      (`SAFETY_LINK_DIAG_FLAG_*`, `SAFETY_LINK_DIAG_BOOT_*`,
      `SAFETY_LINK_DIAG_STATE_*`, `SAFETY_LINK_DIAG_CONTEXT_AGE_NEVER`).
      `safety_link_status_t` gained twelve `diag_*` fields (trip/warn masks,
      uptime, boot reason, context-frame health counters, TX-drop counter,
      state, flags) and nine `trip_*` fields for the most recent
      `TRIP_EVENT` (sequence, reason, uptime, safety TC, deciding threshold,
      three current channels, relay-recent mask, context age at trip), plus
      a `trip_event_age_ms` companion computed the same way the link's own
      `age_ms` is. `SafetyLinkClass` gained one new field,
      `trip_event_tick`, to support that age computation (`cached_tick`
      only moves on `GET_STATUS`, so a second tick was needed rather than
      reusing it).
- [x] **`safety_link.c`**: `safety_apply_diag()` and
      `safety_apply_trip_event()`, both following `safety_apply_power()`'s
      exact shape — length/subcommand check first (a mismatch counts a frame
      error and logs a warning, same as every other frame here), then a
      locked field-by-field copy, byte offsets matching
      `kilnlink_diag_decode()`/`kilnlink_trip_decode()`. `safety_drain_inbox()`
      now dispatches `SAFETY_CMD_DIAG`/`SAFETY_CMD_TRIP_EVENT` to them
      alongside the existing `GET_STATUS`/`FW_VERSION`/`UPDATE_STATUS`/`POWER`
      cases. `TRIP_EVENT` is idempotent per `LINK_PROTOCOL.md`'s own
      description ("the ESP dedups on `trip_seq`") — every copy received
      still updates the cache and `trip_event_tick` (so a resend burst keeps
      the reported age accurate), but only a `trip_seq` that actually
      changed logs `"safety processor TRIPPED"` at `ESP_LOGW`, so three
      repeats of the same event don't read as three separate trips.
      `safety_link_start()` initializes the trip/DIAG float fields to `NaN`
      and `diag_context_age_100ms` to `SAFETY_LINK_DIAG_CONTEXT_AGE_NEVER`
      (255), same "NaN/sentinel, not 0, until a real reading arrives"
      convention as the temperature/power fields — belt-and-suspenders,
      since every field here is also gated behind
      `diag_ever_received`/`trip_event_ever_received`.
- [x] **`dashboard_http.h`/`.c`**: `dashboard_status_t` gained the
      `diag_ever_received`/`diag_*` and `trip_event_ever_received`/`trip_*`
      fields as a straight passthrough of `safety_link_status_t`'s new
      fields (same `TODO.md` 10.1a shared-backend rule as `safety_temp_c`/
      `power_w`). `GET /api/status` emits them only when their `*_ever_received`
      flag is true (the DIAG/TRIP_EVENT keys are simply absent from the JSON
      object otherwise, rather than emitting a run of `null`s for a dozen
      fields — a different null-convention than `safety_temp_c`'s "always
      present, sometimes `null`," chosen because DIAG/TRIP_EVENT are
      logically one wide record each, not independent scalars). The
      `status_get_handler` JSON buffer grew `896` → `1400` bytes to hold the
      worst case with every optional block populated.
- [x] **LCD (`ui_page_safety.c`)**: one new row, "Last trip: reason 0xNN,
      Ns ago" (or "---" before the first `TRIP_EVENT` arrives) — chosen
      because `LINK_PROTOCOL.md` sec 7 calls the trip reason "the answer to
      'why did the kiln stop'" and it was the only field that fit this
      page's documented ~264px no-scroll budget (`ui_page_home.c`'s header
      comment derives that number; this page already used ~200px of it for
      its existing four rows plus the back button, per the arithmetic
      checked before adding anything). `DIAG`'s warn/trip masks and
      context-frame health counters are cached and HTTP-exposed
      (`GET /api/status` above) but were deliberately **not** added here —
      squeezing a fifth and sixth row into the remaining ~36-64px of slack
      would leave the page uncomfortably close to its ceiling for diagnostic
      data an operator doesn't need mid-firing; a dedicated diagnostics page
      (the `ui_page_board_health.c` precedent, TODO.md 10.7) is the better
      home for it if/when it's added, not this page.
- [x] `idf.py -C firmware/KilnFW build` (ninja, incremental against an
      already-configured `build/` — see this repo's
      `project_kilnfw_idf_build_invocation` memory for why `idf.py build`
      itself needs the PowerShell profile script sourced first): clean, no
      new warnings, under `-Werror`. `KilnCtrl.bin` 0x157800 bytes, 8% of the
      `factory` partition free (was 9% before this pass — the new fields/
      functions cost about 1% of the partition).
- [ ] **Not hardware-verified, and cannot be from this environment.** No
      ESP32-S3/Pico is attached, and ROADMAP.md M0 already established the
      isolated link doesn't pass a byte end-to-end on real hardware — so
      `safety_apply_diag()`/`safety_apply_trip_event()` have never decoded a
      frame that actually crossed the wire, only a clean cross-compile.
      Real verification needs both M0 (link fixed) and a `SaftyFW` build
      that actually sends Frame B/Frame D, which does not exist yet (see
      this section's opening paragraph) — nothing here can be exercised
      against live safety telemetry until then.
- [ ] **Not done this pass**: mirroring `DIAG`/`TRIP_EVENT` onto the
      PC-link `SAFETY` task (`uart_bridge.c`) so `pc_tools`/the MCP server
      see the same data without Wi-Fi — `LINK_PROTOCOL.md` sec 7's "Mirror
      all of it on the PC-link SAFETY task as well" and `LINK_PROTOCOL.md`
      sec 9 item 0.9 both call for this, but the task this pass was scoped
      to (`dashboard_http.c`/`ui_page_safety.c`, the web/LCD half of 0.9)
      didn't extend to `uart_bridge.c`. A real follow-up, not forgotten.

### 10.14 Command queue between every control surface and the tasks that own state

**See `firmware/KilnFW/docs/ARCHITECTURE.md` for the resulting task inventory, ownership doctrine, and honest verification status.**

**2026-08-19, filed from a bench bug, not speculative.** `ui_page_network.c`
froze the *entire* display (not just that page — every page's refresh timer,
touch input, and flush all stopped) whenever the LCD's Scan button was tapped,
because `scan_btn_cb()` called `wifi_prov_scan()` straight from
`lvgl_port_task` — a blocking full-channel radio scan running on the one and
only task that's allowed to call an `lv_*` function
(`lvgl_port.c`/`kiln_ui.h`'s header comments). Same session, same file:
`mode_home_btn_cb()`/`mode_ap_btn_cb()` (`wifi_prov_set_mode()`) and
`connect_submit_cb()` (`wifi_prov_add_network()`, which can itself trigger a
blocking `wifi_prov_scan()` internally via its auto-join tie-break) were the
same bug at two more call sites — found while scoping this section, not
separately reported.

**Research pass (three parallel Explore agents, 2026-08-19) before deciding
scope:**
- `wifi_prov.c`'s module state (`s_wifi`) has **zero locking**. Every caller —
  the Wi-Fi driver's default-event-loop task, `lvgl_port_task`, the esp_timer
  service task, and the HTTP server's task via `wifi_provision_http.c` —
  reads/writes it directly. Not observed to corrupt anything, not proven safe
  either (already flagged in `ui_page_network.c`'s header comment before this
  research pass; confirmed, not newly found).
- `firmware/SaftyFW/src/tasks/relay_owner.c` is the pattern to mirror if/when
  a real owning-task-per-module design happens: a `<owner>_cmd_type_t` enum +
  tagged struct, a small `xQueueCreate`d queue (depth 4, "a backlog here means
  something is wrong upstream"), one `<owner>_command_<verb>()` post-and-return
  function per command (fast-refuse synchronously if cheaply knowable, else
  `xQueueSend(queue, &cmd, 0)` — zero-tick, non-blocking, drop-and-report on a
  full queue), and the owning task drains with a **bounded** `xQueueReceive`
  timeout (200ms, not `portMAX_DELAY`) so its own periodic duties (here,
  watchdog checkin and a state-machine tick) aren't starved by an idle queue.
- `uart_bridge.c` has one FreeRTOS task per subsystem (THERMO/IO/DISPLAY/
  TOUCH/SAFETY/SYSTEM/INFO) and every one of them runs its command dispatch
  `switch` inline — no queue anywhere in this file. The risky ones (call
  straight into blocking SPI/I2C/flash) are **THERMO** (every `MAX31856_*`
  subcommand), **IO** (`kiln_io_*`/`SX1509_*` relay and I/O-expander writes —
  the safety-relevant ones), and **DISPLAY** (every `ILI9488_*` subcommand,
  some full-frame blits). SAFETY (the isolated-link commands) and SYSTEM
  (`FACTORY_RESET` does a flash erase!) are secondary.
- `firmware/KilnFW/App/drivers/*_http.c` handlers all run on esp_http_server's
  **one shared worker task** (`wifi_provision_http_start()` is the only
  `httpd_start()` call; every other `*_http_start()` just registers URIs
  against it). The clearly worst blocking handlers are `ota_http.c`'s ESP and
  Pico transfer handlers (long streamed flash/UART writes) and
  `wifi_provision_http.c`'s `provision_post_handler()` (can trigger a radio
  mode switch) — while any of those runs, no other HTTP client (including the
  dashboard's own polling) gets served.
- `profile_executor.c` already has two dedicated FreeRTOS tasks
  (`executor_task_entry`, `watchdog_task_entry`) but **no queue** — HTTP
  handlers call `profile_executor_run()`/`_halt()`/`_pause()`/`_resume()`
  directly, synchronized some other way, not via posted commands.
  `relay_authority.c` has neither a task nor a queue — plain synchronous
  static-array bookkeeping.

**Scope decision for this pass, given the above:** a full command-queue
rewrite of `wifi_prov.c`'s internals (routing the Wi-Fi driver's own event
handlers through the same queue as every caller, for genuine single-writer
state) is exactly the kind of deep, hardware-unverifiable change this
environment cannot responsibly ship blind — no board is attached here (see
this repo's `ROADMAP.md` M0), and a broken Wi-Fi stack change costs an
operator their only remote-monitoring link. That rewrite, and the web/UART
surfaces, stay **design-only, not started** — see the checklist below. What
*is* done this pass is the mechanically safe, directly-diagnosed slice: every
`ui_page_network.c` call site that was blocking `lvgl_port_task` on a
`wifi_prov_*()` call now runs it off that task instead, using the same
worker-task + mutex-guarded job-struct shape the Scan fix already
established (not a generic reusable abstraction — three small, near-identical
blocks, deliberately, matching this codebase's "three similar lines beats a
premature abstraction" convention).

- [x] **LCD side, `ui_page_network.c` only (2026-08-19).** `scan_job_t`
      (Scan button, landed slightly earlier the same session), `mode_job_t`
      (`mode_home_btn_cb()`/`mode_ap_btn_cb()` → `request_mode_change()`,
      fire-and-forget since `refresh_cb()` already polls
      `wifi_prov_get_mode()` every tick regardless of how the change
      happened), and `connect_job_t` (`connect_submit_cb()`, needs a real
      done/err result back to close the modal or show an error) — three
      small worker-task-per-action structs, each with its own
      `xSemaphoreCreateMutex()`-guarded handoff, each polled from
      `refresh_cb()`. All three mutexes are created once up front in
      `ui_page_network_build()`, not lazily on first tap — an earlier
      version of this same pass created `scan_job_t.lock` lazily inside
      `scan_btn_cb()`, which meant `refresh_cb()`'s very first tick (called
      immediately at page build, before any button has ever been tapped)
      called `xSemaphoreTake()` on a still-NULL handle. Found and fixed in
      the same session, before it reached a build, let alone hardware —
      every job-entry point also has a defensive NULL-lock guard (no-op with
      a log/status message) in case mutex allocation itself ever fails,
      matching this codebase's existing convention elsewhere (`autotune_engine.c`,
      `MAX31856.c`, `run_state.c`, etc. — grep `xSemaphoreCreateMutex` for the
      pattern).
      **Deliberately left synchronous**: `forget_confirm_yes_cb()`'s
      `wifi_prov_forget_network()` call — NVS-only, no radio, no internal
      scan, lowest risk of the four call sites on this page; not worth the
      same treatment this pass.
      Build: `idf.py -C firmware/KilnFW build` (ninja, incremental) clean
      under `-Werror`. **Not hardware-verified** — no ILI9488/board attached
      in this environment, so none of this has actually been tapped on real
      glass; this closes "no longer calls blocking Wi-Fi operations from
      lvgl_port_task," not "confirmed not to freeze on the bench."
- [x] **`wifi_prov.c`'s own owning task**, so `s_wifi` gets a genuine single
      writer (was: zero locking, see the research summary above). Landed
      2026-08-19 as Phase 4 — see that entry at the end of this section for
      what was built, what stayed direct, and how far verification actually
      got. The "do not attempt without hardware to test against" caveat this
      line used to carry was satisfied in the weak sense (a board is now on
      the bench and the new build boots on it) and NOT in the strong sense
      (the UART link that would report Wi-Fi state is physically broken) —
      read the Phase 4 entry's verification paragraph before treating this
      as settled.
- [ ] Web side: `dashboard_http.c`/`zones_http.c`/`profiles_http.c`/
      `rules_http.c`/`ota_http.c`/`wifi_provision_http.c` action-taking (POST)
      handlers post commands instead of running inline on esp_http_server's
      one shared worker task. `ota_http.c`'s transfer handlers and
      `wifi_provision_http.c`'s `provision_post_handler()` are the highest-value
      targets per the research pass above (longest/most blocking).
- [ ] Debug/PC-link UART side (user's explicit ask, 2026-08-19):
      `uart_bridge.c`'s per-subsystem tasks (THERMO/IO/DISPLAY especially,
      per the research pass above) post commands to an owning task instead of
      running the dispatch `switch` body inline, mirroring
      `relay_owner.c`'s shape (queue + bounded-timeout drain + one
      `<owner>_command_<verb>()` per command) rather than reinventing it.
- [ ] `profile_executor.c` (has two tasks, no queue) and `relay_authority.c`
      (has neither) are candidates to gain the same `relay_owner.c`-style
      queue once the web/LCD callers that drive them are migrated — not
      urgent today since their current call pattern hasn't been observed to
      freeze anything, but worth the same audit once this section's other
      items land.
- [ ] Once a real design exists for the non-LCD surfaces,
      `ui_page_network.c`'s three job structs from this pass are candidates
      to migrate onto it rather than staying page-local one-offs — flagged
      here so they aren't mistaken for "already fully solved, don't revisit."

**2026-08-19, long-term plan approved (user: "ignoring the size of the
change what would be best" → "lets plan to do it then as a long term fix").**
Three parallel research passes (`wifi_prov.c`'s ownership, the HTTP handler
layer, `uart_bridge.c` + `relay_owner.c`'s existing queue pattern) fed a full
plan, approved by the user; full text kept at
`C:\Users\budar\.claude\plans\moonlit-wishing-brook.md` for this machine.
Summary here so the plan survives independent of that local file:

**The pattern**, copied from `firmware/SaftyFW/src/tasks/relay_owner.c:47-211`
(already proven on the RP2040 side): per state-owning domain, a
`<owner>_cmd_type_t` enum + tagged `<owner>_cmd_t` struct, a small
`xQueueCreate`d queue, one `<owner>_command_<verb>()` producer function per
command (`xQueueSend(queue, &cmd, 0)`, 0 ticks — never blocks the caller,
drops and reports on a full queue), and an owner task that drains with a
**bounded** `xQueueReceive` timeout (not `portMAX_DELAY`) so its own
periodic/housekeeping duties aren't starved. Two producer shapes: fire-and-
forget (`bool` return, relay_owner.c's exact shape) where the caller doesn't
need data back, and request/response (command carries a pointer to a
caller-owned result struct + a `SemaphoreHandle_t` the caller creates,
posts, and waits on with a bounded timeout) where it does.

**Ownership map:**

| Owner | Owns | Replaces direct calls from |
|---|---|---|
| `kiln_io_owner` (new file) | relay + `SX1509` I/O-expander writes/reads (**not** `kiln_io_lcd_dc()`/`kiln_io_lcd_reset()` — those stay direct from `ILI9488.c`'s hot path, called once per display command from `lvgl_port_task`; queuing them would add latency to every draw for no correctness benefit, and `SX1509.c`'s own internal I2C-bus mutex already makes them safe to interleave with) | `uart_bridge.c`'s `io_bridge_task` `IO_CMD_*`/`SX_*` handlers, `dashboard_http.c`'s `dashboard_set_relay()` (itself called from both `relay_post_handler` and `ui_page_temperature.c`'s LCD button) |
| `thermo_owner` (new file) | `MAX31856_*` SPI access | `uart_bridge.c`'s `thermo_bridge_task` `THERMO_CMD_*` handlers |
| `wifi_prov` (add a task inside `wifi_prov.c`) | `s_wifi`, all `esp_wifi_*`/NVS calls, **including the driver's own `on_wifi_event`/`on_ip_event`** — the only way to get a genuine single writer | `ui_page_network.c`'s three job structs (this session's stopgap), `wifi_provision_http.c` |
| `profile_executor` (extend its two existing tasks) | fire-profile run/pause/halt/resume | `dashboard_http.c`'s `profile_exec_*_post_handler`s (direct calls today, no queue despite the tasks already existing) |

Explicitly out of scope: **DISPLAY** (`uart_bridge.c`'s `display_bridge_task`
is dead code — confirmed this pass, `main.c:771-773`'s own comment says LVGL
replaced it and it's never started; delete rather than migrate, separately).
**OTA** (`ota_http.c`'s transfer handlers legitimately need to hold a
streaming HTTP body open across the whole transfer — staying on the HTTP
worker is correct, not a gap). **SAFETY/SYSTEM** UART bridge tasks (lower
risk — cache reads, fire-and-forget to the isolated link, or a
`FACTORY_RESET` that reboots immediately after).

**Build order**, each phase independently shippable/buildable:
1. `kiln_io_owner` (highest safety-relevant value — relay writes race across
   three callers today with zero coordination, and two independent copies of
   the ownership/safety-fault check exist and can drift)
2. `thermo_owner`
3. `profile_executor` queue (retrofit onto tasks that already exist)
4. `wifi_prov` owning task, done *last* and separately — the only phase
   requiring the Wi-Fi driver's own event handlers to be rerouted too, a
   materially riskier change than "add a queue in front of an existing
   task," not to be attempted without real hardware to test a Wi-Fi-stack
   change against. `ui_page_network.c`'s three job structs migrate onto it
   once it lands.
5. HTTP handlers move in lockstep with whichever owner (1-4) they call into,
   not as a separate final phase.

**Design correction found while starting Phase 1, worth recording so it
isn't relitigated**: `io_bridge_task` is already a dedicated task draining a
queue set (UART inbox + expander IRQ semaphore, `xQueueSelectFromSet`) — it
is not a naive "call blocks its own task" handler the way the `wifi_prov`
call sites were. The actual race Phase 1 closes is that `io_bridge_task`
(UART), `dashboard_set_relay()` (HTTP +, via `ui_page_temperature.c`, LCD)
all write relay state today with **no coordination between them**, and the
`relay_authority_manual_blocked_by_owner()`/`relay_authority_on_blocked()`
safety gate is duplicated independently in both `io_bridge_task`'s
switch-case and `dashboard_set_relay()`. User confirmed (2026-08-19,
`AskUserQuestion`): build the new `kiln_io_owner.c` file per the plan above
rather than the smaller alternative (extending `io_bridge_task`'s existing
queue set in place) — full migration, not a patch.

- [x] **Phase 1: `kiln_io_owner.c`/`.h` (2026-08-19).** Scope grew twice while
      implementing, both times surfaced to the user before proceeding rather
      than decided silently:
      1. Research (`AskUserQuestion`) found `io_bridge_task` already runs as
         a dedicated task, not a naive blocking handler — the real bug was
         three uncoordinated writers (UART/HTTP/LCD) and a duplicated
         ownership/safety check. User chose the full `kiln_io_owner.c` file
         over the smaller in-place patch.
      2. Mid-implementation, found `profile_executor.c` (the PID/time-
         proportioning control loop itself, every tick) and
         `autotune_engine.c` also write relay state directly — a real
         lost-update race on `kiln_io_set_relay_mask()`'s read-modify-write,
         missed by the original plan's ownership table. User chose to
         include them in Phase 1 rather than defer to Phase 3.
      **What landed**: `kiln_io_owner.c`/`.h` (new), a single task + bounded
      queue (depth 8) owning every relay/digital-IO/raw-SX1509 write and
      read. Two producer families: MANUAL
      (`kiln_io_owner_command_set_relay[_mask]()`, applies the ownership +
      safety-fault gate, used by `uart_bridge.c`'s `io_bridge_task` and
      `dashboard_http.c`'s `dashboard_set_relay()` — itself called from both
      the HTTP handler and, via `ui_page_temperature.c`, the LCD) and
      AUTHORIZED (`kiln_io_owner_command_set_relay_mask_authorized()`, no
      ownership check since the caller already applied its own zone-level
      gate via `relay_authority_zone_blocked()` — used by
      `profile_executor.c`'s `apply_relay()`/`force_relay_mask_off()`/
      `sweep_unowned_relays()` and `autotune_engine.c`'s `apply_relay()`).
      `main.c` calls `kiln_io_owner_start()` right after `safety_link_start()`,
      before anything that can issue a relay/IO command.
      **Deliberately NOT routed through the owner**, each with its own doc
      comment explaining why: `kiln_io_lcd_dc()`/`kiln_io_lcd_reset()`
      (`ILI9488.c`'s hot path, called once per display command from
      `lvgl_port_task` — SX1509.c's own internal I2C-transaction mutex
      already makes these safe to interleave with everything else, and
      queuing them would add latency to every single display draw for no
      correctness benefit); and every DIRECT `kiln_io_all_relays_off()` call
      that exists as a last-resort fail-safe independent of everything else
      — `uart_bridge.c`'s link-loss watchdog, `profile_executor.c`'s own
      `watchdog_task_entry()` (guard 9 / the 30s safety-link-silence abort),
      and `main.c`'s `kiln_enter_safe_state()`. Routing any of those through
      the owner's queue would make them depend on the owner task NOT being
      the thing that's wedged — backwards for code whose purpose is acting
      when something else already is; `kiln_io_all_relays_off()` is
      unconditional and only ever turns things OFF, so a race between one of
      these and the owner mid-write is benign in the failure direction.
      **Build**: `idf.py -C firmware/KilnFW build` (ninja, incremental)
      clean under `-Werror`, after adding `kiln_io_owner.c` to
      `App/drivers/CMakeLists.txt`'s `SRCS` list (new file, not
      glob-discovered). Two real bugs caught by the build itself before
      this ever reached hardware: a `*/` inside a `/* ... */` comment
      (`kiln_io_*/SX1509_*` in a doc comment closed the comment early and
      the following text parsed as code) and the same pattern in
      `kiln_io_owner.h`.
      **Not hardware-verified.** No board is attached in this environment
      (`ROADMAP.md` M0) — this closes "single writer, compiles clean, gate
      logic centralized," not "confirmed safe on a real kiln." Real
      verification needs bench time: exercising SET_RELAY from all three
      surfaces (UART/HTTP/LCD) concurrently, confirming the ownership/
      safety-fault refusals still fire correctly now that they're centralized,
      and confirming a firing's relay switching timing is unaffected by the
      added queue hop (expected to be small — a bounded 200ms producer
      timeout against I2C transactions that take low milliseconds — but
      unmeasured on real hardware).
      **Not done this pass, explicitly out of scope**: `dashboard_http.c`'s
      own `kiln_io_read()` call (a status-building read, not a write — left
      direct since SX1509's bus-level mutex already makes it I2C-transaction-safe
      and it carries no safety implication, unlike every write case above).
- [x] **Phase 2: `thermo_owner.c`/`.h` (2026-08-19).** Same shape as Phase 1,
      deliberately, and a materially different motivation, stated up front in
      `thermo_owner.h`'s top comment rather than left implicit:
      `MAX31856.c`'s `ch->lock` (`max31856_lock()`/`max31856_unlock()`,
      around line 77) already guards each channel's ENTIRE multi-transfer
      sequence, not just one SPI transaction the way SX1509's mutex only
      covered one I2C transaction before Phase 1 — so there is no equivalent
      lost-update race here today. This phase is architectural consistency
      (one owning task per hardware subsystem, matching Phase 1 and
      `relay_owner.c`) and lays a single choke point for Phase 6's later
      system-mode gate, not a bug fix. Confirmed accurate rather than
      invented before writing the header, per explicit instruction this pass.
      **What landed**: `thermo_owner.c`/`.h` (new), a single task + bounded
      queue (depth 8) owning `MAX31856_config_channel`/`_set_thresholds`/
      `_set_cj_offset`/`_trigger_one_shot`/`_read`/`_read_all`/`_read_faults`/
      `_clear_faults`/`_read_reg`/`_write_reg`. One producer family (no
      ownership/safety gate applies to a thermocouple read or config write
      the way it does to a relay) — every `thermo_owner_command_*()` is
      post-and-wait, identical shape to `kiln_io_owner.c`'s. A bad or
      never-came-up channel index answers `ESP_ERR_NOT_FOUND` from inside the
      owner task's own `MAX31856_bus_channel()` lookup, same contract the
      direct calls had. `main.c` calls `thermo_owner_start(&thermo_bus)`
      right after the `~DRDY` provider wiring, before the UART THERMO bridge
      task or `safety_link_start()` — unconditionally, not gated on
      `thermo_bus.initialized`, since the thermocouple daughterboard is not
      physically attached in this environment and every per-channel producer
      already fails closed (`ESP_ERR_NOT_FOUND`/`ESP_ERR_TIMEOUT`) rather than
      assuming a channel exists.
      **Migrated**: `uart_bridge.c`'s `thermo_bridge_task` (`THERMO_CMD_*`
      switch, all nine subcommands that touch the driver — every wire-format
      guard/log line wording kept unchanged, only the call underneath moved),
      and `safety_link.c`'s `safety_build_and_send_context()` (its
      `MAX31856_read_all()` call, via the new `thermo_owner_command_read_all()`
      producer mirroring `MAX31856_read_all()`'s own out-array/max/count
      shape).
      **Deliberately left direct, each with its own doc comment**:
      `safety_link.c`'s `MAX31856_get_config()` call — reads
      `ch->cr0_shadow`/`ch->cr1_shadow` under `ch->lock` with no SPI transfer
      at all (confirmed by reading the implementation before deciding, not
      assumed), so a task hop would add latency for zero correctness benefit,
      same reasoning Phase 1 gave for `kiln_io_lcd_dc()`/`kiln_io_lcd_reset()`.
      Also left direct: every OTHER `MAX31856_read_all()` caller in this
      codebase (`profile_executor.c`'s control loop — off-limits this pass
      per explicit scope, along with `dashboard_http.c` — `autotune_engine.c`,
      `board_temps.c`, `ota_http.c`) — a research grep of the full
      `MAX31856_*` API surface found no OTHER caller of the
      config/write/one-shot/read/fault functions outside `uart_bridge.c`, so
      those five files' `read_all()` calls were the only thing left
      unmigrated on purpose: read-only, already serialized per-channel by
      `ch->lock`, and not part of the diagnosed "every writer/config path
      needs one owner" scope.
      **Build**: `idf.py -C firmware/KilnFW build` (ninja, incremental) clean
      under `-Werror`, after adding `thermo_owner.c` to
      `App/drivers/CMakeLists.txt`'s `SRCS` list (same "new file, not
      glob-discovered" gotcha Phase 1 hit).
      **Not hardware-verified, and cannot be bench-verified in this
      environment even later this pass**: no board is attached
      (`ROADMAP.md` M0), and additionally the thermocouple daughterboard
      itself is not physically connected to the bench board that IS present,
      so every `MAX31856_*` channel is expected to fail its own bring-up at
      boot regardless of this change — verifying `thermo_owner`'s actual SPI
      behavior (not just that it fails closed on a missing channel, which
      compiles and can be reasoned about but not observed working) is blocked
      on the daughterboard being connected, a separate hardware readiness gap
      from the no-board-at-all case Phase 1 hit.
- [~] **Phase 3: `profile_executor` command queue — REVIEWED AND
      DELIBERATELY SKIPPED (2026-08-19).** Marked `[~]`, not `[x]`: nothing
      was built, and this is a decision to record, not work to come back to
      unless the premise changes. The plan's build order put a queue in
      front of `profile_executor_run()`/`_halt()`/`_pause()`/`_resume()`
      because the ownership research pass had noted "two tasks already, but
      **no queue** — HTTP handlers call these directly, synchronized some
      other way." Reading the code closed that "some other way": all four
      wrap their entire bodies in `s_exec.lock`
      (`profile_executor.c:1707`, `:1989`, `:2039`, `:2067`, each an
      `xSemaphoreTake(s_exec.lock, portMAX_DELAY)` taken before the first
      state check and released on every return path). That is a correct
      mutex-guarded API, not an uncoordinated-writer bug — there is no
      Phase-1-style lost update here and no duplicated safety gate to
      centralize. Converting a *safety-critical* state machine (the thing
      that decides whether a kiln is firing) to a command queue would be
      pure regression risk against zero safety gain, and it would trade a
      lock whose failure mode is "the caller waits" for a queue whose
      failure mode is "the command is dropped." Revisit only if Phase 6's
      system-mode gate turns out to genuinely need a choke point here that
      the existing lock can't provide.
- [x] **Phase 4: `wifi_prov.c`'s owning task (2026-08-19).** The riskiest
      phase, done last per the plan, and the one that is structurally
      unlike Phases 1 and 2: no new file. `s_wifi` is module-private with
      nothing to wrap, so the task lives *inside* `wifi_prov.c` per the
      plan's own ownership map.
      **The bug being closed is real and was the largest one in this
      section**: `s_wifi` had ZERO locking and four independent writers —
      the Wi-Fi driver's default-event-loop task
      (`on_wifi_event`/`on_ip_event`), the esp_timer service task
      (`ap_fallback_timer_cb`/`rescan_timer_cb`), `lvgl_port_task` via
      `ui_page_network.c`, and esp_http_server's worker via
      `wifi_provision_http.c` (plus `uart_bridge_ext.c`'s wifi bridge task).
      Unlike Phase 2, this is not consistency work.
      **What landed**: one task (`wifi_prov_owner`, 4096 stack, prio 5) +
      bounded queue (depth 6) inside `wifi_prov.c`. Every public entry
      point's body moved into a `do_*()` static that only ever runs on that
      task; the public function became a thin producer (build command, post,
      bounded wait). **Every `wifi_prov_*()` signature is unchanged** — that
      was the deliberate blast-radius control, and the result is that none of
      the seven caller modules (`ui_page_network.c`,
      `wifi_provision_http.c`, `uart_bridge_ext.c`, `wifi_status_ui.c`,
      `readiness_http.c`, `ota_http.c`, `main.c`) needed a single edit.
      Migrated through the queue: `add_network`/`set_credentials`,
      `forget_network`, `get_saved_networks`, `set_mode`, `set_ap_ssid`,
      `set_ap_password`, `get_sta_ip`, `scan`.
      **The part that made this phase different from "add a queue in front
      of an existing task"**: all four of the driver's/timer's own callbacks
      were rerouted through the same queue. `on_wifi_event`, `on_ip_event`,
      `ap_fallback_timer_cb` and `rescan_timer_cb` are now each a single
      `post_event()` call (fire-and-forget, `xQueueSend` with 0 ticks — an
      event loop must never block on this module's queue); their real bodies
      became `do_ev_sta_start()`/`do_ev_sta_disconnected()`/`do_ev_got_ip()`/
      `do_ap_fallback_tick()`/`do_rescan_tick()` on the owner task. A full
      queue drops the event with a log line, which is safe **because every
      event this module consumes self-heals** — the driver re-emits
      disconnects while the link is down, both timers fire again, and a
      dropped GOT_IP leaves state at CONNECTING one beat too long (the AP
      stays up: the safe direction, never "reports connected when it
      isn't"). That reasoning is written out in full in `wifi_prov.c`'s
      owner-task comment rather than only here.
      **Deadlock rule, and the one call site that violated it**:
      `select_and_apply_join_candidate()` called the public
      `wifi_prov_scan()`. Post-conversion that would have had the owner task
      post to its own queue and wait for itself forever. It now calls
      `do_scan()` directly, and a forward declaration of `do_scan()` carries
      the comment explaining why. Every internal helper works on raw state;
      only the public boundary posts.
      **Two timeouts, not one, and sized from the caller side**:
      `WIFI_OWNER_SCAN_WAIT_MS` 15s for `scan`/`add_network`/`set_mode` (the
      three that can run a blocking `esp_wifi_scan_start(block=true)`, the
      latter two via `start_sta_join()`), `WIFI_OWNER_WAIT_MS` 12s for the
      rest. The 12s is NOT sized by the command's own millisecond-scale work
      — it is sized by the worst case *ahead of it in the queue*, which is a
      scan. A `thermo_owner`-style 200ms would spuriously fail any status
      read that landed behind an operator tapping Scan. Serializing scan
      against every other Wi-Fi command is the intended behavior (one
      radio), not a side effect.
      **Deliberately left as DIRECT reads, documented in `wifi_prov.h` with
      the same convention `thermo_owner.h` used for `MAX31856_get_config()`**:
      `wifi_prov_get_state()`, `wifi_prov_get_mode()`,
      `wifi_prov_is_sta_connected()`, `wifi_prov_get_sta_rssi()` — each
      reads one naturally-aligned word the owner writes with a single store,
      with no read-modify-write and no multi-field invariant to catch
      half-applied; a racing reader sees the value from just before or just
      after a transition, which is what a queue would give it too, one
      scheduling delay later. Also direct, and *unfixable* by a queue in
      principle: `wifi_prov_get_saved_ssid()`/`get_ap_ssid()`/
      `get_ap_password()`, which return POINTERS into module storage the
      caller dereferences after any lock would have been dropped
      (pre-existing, unchanged). And `wifi_prov_get_ap_client_count()`,
      which touches no `s_wifi` field beyond `started` — it is an
      `esp_wifi_ap_get_sta_list()` call into a driver API that is itself
      thread-safe. Everything reading compound state goes through the queue.
      **Where the task starts**: at the end of `wifi_prov_start()` but
      specifically *before* `esp_wifi_start()`, not after. `esp_wifi_start()`
      is what makes the driver begin emitting `WIFI_EVENT_STA_START`;
      creating the task after it would leave a window where the first event
      of every boot finds a NULL queue and is dropped, costing the boot's
      first join attempt. `wifi_prov_start()` itself deliberately stays a
      plain synchronous init (it cannot be a command — the task can't
      predate the state it owns). The overlap this leaves is the owner task
      draining real events while `wifi_prov_start()` finishes
      `esp_wifi_start()`/`wifi_provision_http_start()`; neither writes
      `s_wifi`, and the only remaining write is the single `started` bool
      every producer checks.
      **Build**: `idf.py -C firmware/KilnFW build` clean under `-Werror`,
      first try (no new file, so none of Phases 1/2's "forgot the
      CMakeLists `SRCS` entry" or `*/`-inside-a-doc-comment gotchas). App
      0x158f40 bytes, 8% of the partition free.
      **Verification actually reached — build-verified plus a partial boot
      smoke test, and the gap matters**: flashed over JTAG with OpenOCD
      (`flash_firmware`, per the repo rule — never esptool/`idf.py flash`);
      bootloader + partition table + app all programmed and **verified OK**,
      board reset and running. Liveness confirmed two ways: a subsequent
      OpenOCD resume attempt reported `[esp32s3.cpu0] not halted`, i.e. the
      core is running freely rather than sitting in a panic halt, and the
      one boot log line that did arrive (`temperature_sensor: ... Out of
      testing range`, at t=1332ms — a pre-existing, unrelated IDF warning)
      never repeated, which a crash/reboot loop would have made it do.
      **What could NOT be observed, stated plainly rather than softened**:
      the board's UART link to the PC is physically broken in this
      environment (a known pre-existing bench fault, same one that blocks
      `get_fw_version`), and that link is exactly what carries both the
      console log (`uart_log_bridge.c`) and `wifi_get_status`. So the boot
      log is one line, not thirty seconds of it, and **no Wi-Fi behavior was
      observed at all** — not the owner task announcing itself, not AP
      bring-up, not a station join attempt. "Boots clean and keeps running
      with the new Wi-Fi ownership in place" is what this establishes.
      Everything the phase is actually about — AP fallback, the rescan
      cadence, a real join, concurrent commands from LCD + HTTP + UART
      contending on the one owner — remains **unverified** and needs a bench
      session with a working UART link before this goes near a kiln that
      depends on remote monitoring.
      **Named remaining work, not done this pass**: `ui_page_network.c`'s
      three ad hoc job structs (`scan_job_t`/`mode_job_t`/`connect_job_t`)
      are still in place and still correct — the producers they call now
      queue instead of touching state directly, but they are still blocking
      calls that must stay off `lvgl_port_task`, so the worker tasks are
      still doing real work. Deleting the page-local duplication in favor of
      a shared async shape is a separate cleanup pass; leaving them was a
      deliberate blast-radius choice (this phase changed zero caller files),
      not an oversight. Also still open, unchanged by this phase: the web
      and UART surfaces' own queue items above.
- [ ] Phase 3: `profile_executor.c` command queue
- [ ] Phase 4: `wifi_prov.c` owning task (needs real Wi-Fi hardware to trust
      before shipping — do not rush this one)
- [ ] Phase 5: HTTP handler migration, per-domain, alongside 1-4
- [ ] Phase 6 (added mid-Phase-1, user request): a system-mode command gate,
      distinct from the owner-task pattern above. The owners answer "can two
      writers race on this piece of state"; this answers "is this *class* of
      command allowed at all given what the system is doing right now" —
      e.g. while a profile is firing, stop/pause/modify-this-run is fine,
      but starting a *different* profile, running a PID autotune, or a raw
      GPIO/SX1509 write through the debug/UART path should be refused
      outright. Can't live inside `kiln_io_owner`/`thermo_owner` — needs to
      see `profile_executor`'s and `autotune_engine`'s state, which those
      modules have no business knowing about. A policy layer *above* the
      owners, consulted by every producer-facing entry point (HTTP, LCD,
      UART) before a command is even built. Sketch and a first-pass policy
      table kept in the full plan file
      (`C:\Users\budar\.claude\plans\moonlit-wishing-brook.md` on this
      machine) — needs its own design pass before it's built; do not
      implement ahead of Phases 1-5 landing.
