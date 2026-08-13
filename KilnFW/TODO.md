# TODO — Web UI / Kiln Control Front End

Planning doc; sections 1 and 4 (Wi-Fi provisioning/network settings) are
implemented and hardware-verified as of 2026-08-10, everything else below is
still unbuilt. Only the bare ESP32-S3 board is wired up so far (no
thermocouple daughterboard, relays, display, or safety RP2040), which is why
Wi-Fi — the one piece needing no other hardware — went first. Captures the
feature request as itemized work so it can be scoped and sequenced before any
code is written. Cross-references `docs/SAFETY_MODEL.md` and
`docs/PROJECT_STATUS.md` throughout, since the profile-execution engine this
introduces is a **new actor that commands relays** and must go through the
same safety-wins gate as the existing PC/MCP link — not a parallel path
that bypasses it.

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
- [ ] **Who drives relays for a running profile?** A new on-device
      profile-executor task, distinct from the UART bridge's `SET_RELAY`
      path but subject to the *same* gate (now `relay_authority_on_blocked()`,
      see below) — a profile must not be able to
      energize a relay that a safety fault is blocking. Needs a shared
      "who's allowed to turn this relay on right now" chokepoint rather
      than two independent callers of `kiln_io_set_relay`. The chokepoint
      itself now exists (see the sub-item below); the profile executor that
      would be its second caller is still unbuilt — needs the relay/expander
      hardware, not present yet.
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
wired into `main_page.html` at `/`. Profile controls and the graph are
explicitly deferred, not stubbed: no profile-execution engine exists yet
(section 6) to drive either one, and building non-functional buttons for
them would be exactly the kind of half-finished UI the project avoids
elsewhere. `GET /api/status` and `POST /api/relay` are the new endpoints;
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
- [ ] Temperature graph: desired profile curve overlaid with actual
      measured temperature over time, updating live while a profile runs.
      Deferred — needs the history-ring-buffer writer (designed in section
      0, not yet implemented) and a profile engine (section 6) to produce
      the "desired" curve.
- [ ] Start / Stop / Pause controls for the running profile.
      Deferred — needs the profile executor (section 6).
- [ ] List of saved profiles with a way to select one to run.
      Deferred — needs profile storage/CRUD (section 5).
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

- [ ] Thermocouple calibration: per-channel offset (and possibly gain),
      stored persistently, applied on top of `MAX31856_read()`'s raw value
      — decide whether calibration lives in firmware (applied before the
      value ever reaches a client) or is a display-layer adjustment; doing
      it in firmware means every consumer (web UI, PC GUI, MCP) sees the
      same corrected number.
      **Half-settled/half-built (2026-08-11)**: storage side is done —
      `zone_cfg_t.cal_offset_c` (`App/drivers/zones_http.c`), persisted in
      NVS, editable on the page. The "applied in firmware" decision itself
      is also made (per the header comment in `zones_http.h`: firmware-side,
      so every consumer sees the same corrected number), but not yet wired
      up — `cal_offset_c` is "stored only; not yet applied to a live
      reading path" (`zones_http.c` line ~33). Wiring it into
      `MAX31856_read()`'s consumers (`dashboard_http.c`, the UART bridge)
      is explicitly out of scope for the page itself and still open. Left
      unchecked because the value has no effect on any reading yet.
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

- [ ] Profile editor: named profile, ordered list of segments, each with
      target temperature, ramp rate (°/hour or °/min — decide units and
      whether it's configurable), and dwell/soak time.
- [ ] Save / load / delete profiles (persisted per the storage decision).
- [ ] Validate profile parameters at creation time (ramp rate and target
      within sane/safe bounds — tie into the safety model rather than
      trusting client input, same principle as every other untrusted-input
      boundary in this project).
- [ ] **Feasibility check against each zone's actual capability** (derived
      from the PID tuning on the settings page, per section 3):
  - [ ] A requested ramp-up or ramp-down rate the kiln cannot physically
        achieve must be flagged as infeasible before the profile can be
        saved/run, not discovered mid-firing as the real temperature falls
        behind the desired curve.
  - [ ] **Warn (not block) at 20% margin** — a segment within 20% of the
        zone's estimated ramp-rate ceiling gets a visible warning, distinct
        from the hard error for something outright impossible.
  - [ ] This check has to run both at profile-creation time (client-side
        preview is fine for UX, but the authoritative check must be
        on-device against the real per-zone ceiling) and probably again at
        profile-*start* time, since a profile could be created for one
        kiln/zone configuration and later run after settings changed.

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
      Half-done: `pid.c`'s output is clamped to [0,1] (implemented), but
      nothing reports "cannot follow, cooling-limited" — no such diagnostic
      exists on `/api/profile_exec` or elsewhere. Left unchecked because the
      bullet's reporting requirement isn't met, only the clamp.
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
- [ ] **Bumpless transfer is required at every discontinuity**, not just that
      one: mode change, tuning change from the web UI mid-firing, profile
      resume after pause, autotune finishing. The rule: whenever the loop
      restarts, set `I = (u_desired - P - D) / Ki`.
      Partial: `pid_seed_bumpless()` (`pid.c`) is called for the
      functional-range re-entry above, for profile resume-after-pause
      (`profile_executor.c`'s `profile_executor_resume()`), and — since
      2026-08-12 — for a **tuning change mid-firing**, from
      `reload_zone_config()` on the 6A.7 config-reload path, seeded off the
      duty the zone last commanded. A mode change deliberately does *not*
      use it: there is no state to carry between a duty fraction and a
      hysteresis latch, so that path forces the zone off and restarts the
      controller cold instead. Autotune finishing still has no call site
      (6A.4's acceptance flow). 3 of the 4 named discontinuities covered, so
      still unchecked.
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
- [ ] **Default policy on a single-zone trip: abort the whole firing.**
      Configurable, but this is the default and the rationale should be in the
      docs: in a multi-zone kiln the remaining zones keep dumping heat into a
      chamber whose temperature is now partly unmeasured, and the ware is
      already ruined. "Continue with the other zones" is the option that needs
      justifying, not the abort.
      Not meaningfully applicable yet — the executor only ever runs one
      zone at a time (6A.5's concurrent multi-zone execution is unbuilt), so
      "abort the whole firing" and "abort this zone" are the same thing
      today and this bullet's actual multi-zone policy question hasn't been
      exercised. Left unchecked rather than claiming it as tested.
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
- [ ] **Every threshold above is config, not a constant.** Kconfig supplies
      the compile-time defaults (matching how the rest of this firmware does
      hardware config), `zone_cfg_t` holds the per-zone overrides, and the
      Settings → Thermocouples & Zones page edits them. The request explicitly
      deferred picking the sanity rate ("I will determine later"), and every
      other number above is a first guess that a real firing will correct.
      Partial: only `max_temp_c`, `min_temp_c`, and the pre-existing
      `sanity_rate_c_per_min` made it onto `zone_cfg_t`/the settings page
      this pass. Every other guard threshold (wrong-dir rate/window,
      off-settle, runaway rate/margin, drift period, sensor debounce count,
      frozen window) is still a firmware-wide constant in `thermal_guard.c`.
      Left unchecked because most thresholds are still constants, not
      per-zone config.
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
- [ ] **Move the feedforward subtraction into `pid_seed_bumpless()`.**
      It solves `integral = (u_desired - P)/Ki`, which is only exact when the
      caller has already subtracted the feedforward term. Both call sites in
      `profile_executor.c` do that correctly (2026-08-12), but the correct
      home for it is `pid.c` — the function should take `ff_u` and subtract
      it itself, so the next caller cannot get it wrong. `pid.h`'s doc
      comment also still says the executor "always passes 0.0f" for
      feedforward, which stopped being true the same day.
- [ ] **`esp_wifi_set_config(AP) failed: ESP_ERR_WIFI_MODE` at boot.**
      Observed on every boot 2026-08-12, on two separate flashes. Not
      blocking — the station join succeeds and the fallback AP comes up when
      it is actually needed — but the AP config is being applied while the
      driver is in the wrong mode, so it is either a bring-up ordering bug or
      a dead call. Not diagnosed.
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
- [ ] **Manual relay control is not blocked during a firing.** Confirmed from
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
- [ ] **Re-check `max_ramp_c_per_hr` against a running profile.** It is a
      run-start feasibility gate (section 5) and the reload path deliberately
      does not consume it, so an operator can now lower it mid-firing below
      what the running profile demands and nothing notices. Pre-existing, but
      config reload makes it reachable without stopping the run. Either
      re-run the feasibility check on reload and warn, or state in the UI
      that this field only binds at start.

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

- [ ] `docs/WEB_UI.md` — page inventory, API/WebSocket message shapes,
      how it relates to the existing UART protocol (same device data,
      different transport).
- [ ] `docs/WIFI_PROVISIONING.md` — AP fallback vs. local-only mode, the
      Kconfig default password and why a default exists, resilience
      guarantees.
- [ ] `docs/PROFILES.md` — profile format, the execution engine, relay
      rule-engine syntax.
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
      `docs/GUARD_TEST_MATRIX.md`. `PID_CONTROL.md` itself has not been
      re-read or updated to match — it still says both are owed, so it is
      the doc to fix next in this section.
- [ ] Update `docs/SAFETY_MODEL.md`'s summary table with the new relay
      caller (profile executor / web UI) and confirm every row still
      holds once it's a real third caller instead of a hypothetical.
- [ ] Update `docs/PROJECT_STATUS.md` once any of this actually lands.

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
      `KilnFW/build` (per the documented idf.py python-env workaround). **Not
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
`ninja -j 24` in `KilnFW/build`, per the documented idf.py python-env
workaround). Not yet flashed/tested on hardware — in particular, confirm on
a board with real saved zone/rules/profile/relay-cycle data that the
one-time migration off the old default `nvs` partition actually carries it
forward into `kiln_nvs`/`profiles_nvs` rather than starting fresh, and that
`zones_config_valid` reads `true` after that migration and `false` on a
genuinely first-boot/unconfigured board (no hardware attached to this
session to check either).

### 8.3 Config wizard page: what is set up, what is not

- [ ] **A page that answers "is this kiln ready to fire?"** as a checklist
      with per-item status and a link to the page that fixes each one.
      Candidates, all derivable from state that already exists: network
      configured; thermocouple count and zone mapping set; relays assigned
      to zones; control mode chosen per zone; guard limits (`max_temp_c`
      especially) set; calibration entered; at least one profile saved;
      autotune run per zone (or gains entered by hand); hardware present and
      answering (`io_ready` / `thermo_ready` / `safety_ready` from
      `/api/status`); every storage section compatible (8.2's report).
- [ ] **Distinguish "not done" from "cannot be done yet."** Assigning
      relays to zones is meaningless before the thermocouple count is set,
      and autotune cannot run without a zone. Order and gate the items
      rather than presenting a flat list of red crosses.
- [ ] **Distinguish "unset" from "deliberately off",** which this firmware
      already cares about: `max_temp_c == 0` means no ceiling, and
      `cross_zone_max_delta_c == 0` disables guard 8. Both are legitimate
      choices and neither should nag forever — but "I chose this" has to be
      recordable, or the wizard becomes noise that gets ignored, which is
      worse than no wizard at all.
- [ ] **It is a status page first and a wizard second.** The value is
      answering "what is missing" on a board someone else set up six months
      ago, not walking a first-time user through screens in order.

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
