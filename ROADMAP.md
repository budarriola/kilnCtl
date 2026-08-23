# kilnCtl Roadmap — both processors

> **Status:** planning · **Last reviewed:** 2026-08-21
> **Keep this file current.** This is the top-level dispatch board: the place to
> start a task from when you do not already know which plan owns it. It holds
> *ordering and cross-processor dependencies only* — the detail lives in the
> per-area plans linked below, and duplicating their content here guarantees the
> two will drift. When a milestone lands, tick it here **and** in the owning
> plan. When the shape of the work changes, edit this file rather than letting it
> describe a project that no longer exists.

The system is two firmwares that must agree with each other:

- **`KilnFW`** — ESP32-S3 main controller. Thermocouples, SSR heater outputs, PID,
  profiles, Wi-Fi, web GUI. Partly built and partly verified on hardware.
- **`SaftyFW`** — RP2040 safety processor (A1). Independent overheat and fault
  detection, owns the mechanical pilot relay K4. **Nothing built yet.**

They talk over an opto-isolated UART. That link, and the rule that **the safety
processor must be alive for the main processor to heat**, is what makes this one
project rather than two.

---

## Where each kind of task is planned

| Plan | Owns |
|---|---|
| [`ROADMAP.md`](ROADMAP.md) (this file) | Milestone order, cross-processor dependencies |
| [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) | Main firmware: web UI, profiles, PID, thermal protection, storage |
| [`firmware/KilnFW/docs/PROJECT_STATUS.md`](firmware/KilnFW/docs/PROJECT_STATUS.md) | What in `KilnFW` is built vs. verified — the honest ledger |
| [`firmware/KilnFW/docs/UI_PLAN.md`](firmware/KilnFW/docs/UI_PLAN.md) | LCD + web UI usability/cleanup plan — no-scroll LCD audit, phone/tablet web audit, prioritized fix queue |
| [`firmware/KilnFW/docs/ARCHITECTURE.md`](firmware/KilnFW/docs/ARCHITECTURE.md) | Tasks, priorities, owner-task queues, single-writer ownership doctrine |
| [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) | Safety firmware, phases 0–10 |
| [`firmware/SaftyFW/docs/SAFETY_MODEL.md`](firmware/SaftyFW/docs/SAFETY_MODEL.md) | What trips, why, and the anti-nuisance doctrine |
| [`firmware/SaftyFW/docs/ARCHITECTURE.md`](firmware/SaftyFW/docs/ARCHITECTURE.md) | Tasks, priorities, core affinity, logging transports |
| [`firmware/SaftyFW/docs/HARDWARE.md`](firmware/SaftyFW/docs/HARDWARE.md) | The traced board, pin map, bench connections |
| [`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`](firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md) | Guard-by-guard test coverage and real-input reachability |
| [`firmware/SaftyFW/docs/CONFIG_REFERENCE.md`](firmware/SaftyFW/docs/CONFIG_REFERENCE.md) | Every safety tunable, its default, and whether getting it wrong is dangerous |
| [`firmware/SaftyFW/docs/COMMISSIONING.md`](firmware/SaftyFW/docs/COMMISSIONING.md) | How those values get set from the web GUI and persist on the safety processor across OTA |
| [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md) | Shared `kilnlink` code, used by both firmwares |
| [`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md) | The wire, both ends — the contract neither side may break alone |
| [`firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](firmware/CommonFW/docs/UPDATE_PROTOCOL.md) | Field updates for both processors: interlocks, one-password auth, ESP OTA partitioning |
| [`firmware/SaftyFW/docs/BOOTLOADER.md`](firmware/SaftyFW/docs/BOOTLOADER.md) | The RP2040 bootloader, flash layout and recovery mode |
| [`firmware/SimFW/docs/PLAN.md`](firmware/SimFW/docs/PLAN.md) | Kiln simulator / unit-test fixture (a *third* firmware, a second Pico on the bench): MAX31856 emulation, CT waveforms, relay sensing, thermal model, fault injection, its own MCP/CLI/GUI (`kilnsim`); also owns the `UnitTestFw` decommission. **Software complete, hardware-gated**: no fixture has ever been built or connected. A software-only cross-check (`tools/virtual_simfw` + `tools/virtual_dut`) runs real `SaftyFW` guard code against the simulator on a PC — see M9 |
| [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md) | GUI, MCP, GPIO probe, debug and logging for **both** processors |
| [`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md) | The hardware/software reorganisation and its blockers |
| [`docs/SETUP.md`](docs/SETUP.md) | Fresh-clone setup: what is machine-specific, and how `tools/setup.ps1` handles it |

---

## The dependency spine

Four facts set the order. Everything else can be shuffled.

1. **The safety UART pins are swapped in `KilnFW`.** Until that is fixed the link
   cannot pass a byte in either direction, so every link-dependent task is
   blocked behind it.
2. **The wire contract is shared code.** Writing it twice guarantees the two
   copies diverge, so `CommonFW` comes before either firmware consumes it.
3. **The relay is the last thing to wire up.** Guards get built and host-tested
   against synthetic inputs before anything can physically open a contactor.
4. **The liveness rule cuts both ways.** Once the ESP refuses to heat without
   safety telemetry, a main board with no Pico fitted cannot heat either — so
   that switch is thrown deliberately, with a documented bench escape hatch.

---

## M0 — Unblock the link · *the only milestone with no alternatives*

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 0.

- [x] **Link confirmed working end-to-end (2026-08-23).** The bench
      measurement found the byte actually died on baud mismatch, not idle
      level, framing or opto polarity: the TCMT1109 optocouplers cannot switch
      fast enough for 115200. Walking the rate down settled on 9600, now
      hardcoded on both sides, and `safety_get_status()` returns live
      telemetry. See `firmware/KilnFW/docs/SAFETY_LINK.md` "Transport" and
      `firmware/SaftyFW/docs/HARDWARE.md` §1.
- [ ] Tier 0 pin test settles ESP TX/RX by measurement (`HARDWARE.md` §1) — still
      needs the Pico physically attached
- [x] ESP TX/RX GPIO assignment and pull-up fixed in code, docs corrected,
      `UART_PROTO_MSG_BROADCAST` added, stale netlist removed, bench path
      decided (Debug Probe SWD + UART bridge on GP16/GP17) — all 2026-08-16
- [ ] DEBUG header and GP16/GP17 access provided before A1 is soldered down

System-wiring decisions gating any bench trip test (not firmware work) are
recorded in the decisions table below, with reasoning in
[`firmware/SaftyFW/docs/HARDWARE.md`](firmware/SaftyFW/docs/HARDWARE.md) §3/§5.

## M1 — Tooling that makes everything after it cheaper

Owned by [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md). Worth doing early precisely
because it is what turns later hardware questions into a script instead of a
soldering session.

- [x] `pc_tools` moved to `tools/PcTools/`; GPIO probes built for both chips
      (ESP: deny-list incl. GPIO6; Pico: over SWD, GPIO6 read-only). **Pico
      probe bench-tested 2026-08-19, PASS.** ESP probe bench-tested 2026-08-19
      but blocked: the PC↔ESP command UART (COM9) is unresponsive independent
      of the M0 isolated link — see bench state below
- [x] Coordinated two-board test script (`tools/PcTools/scripts/
      coordinated_gpio_test.py`) — reaches each board by a path independent of
      the link under test; run 2026-08-19 confirmed the Pico half works and
      the ESP half hits the same COM9 fault above
- [x] OpenOCD wrapper covering both chips (program/reset/halt/read/write) —
      2026-08-17, `kilnctrl.debug_probe`
- [x] Per-processor console capture + interleaved log file
      (`kilnctrl-console-capture`) — host-verified only; the SAFETY log-relay
      wire path is still unimplemented in firmware
- [ ] **HW change: LCD backlight control.** No GPIO/PWM path exists yet
      (dim/off on idle, touch-driven wake)
- [ ] **HW change: relay status LEDs** for K1–K4, S9
- [ ] **HW change: distinct connector types** for the thermocouple daughterboards
      vs. main-board connectors

**Bench state (2026-08-20):** ILI9488 LCD, ESP32-S3 JTAG, and Pico SWD all
verified working. Three MAX31856 ICs + thermocouples now fitted on the ESP32-S3
board (channels 0/1/2 reading correctly, `thermo_owner.c` unblocked); the
safety processor (RP2040) still has none fitted. Still broken: the Pi↔ESP
isolated UART link (M0, the top blocker) and the separate PC↔ESP command UART
(COM9, blocks console/wifi-status/MCP tooling but not the isolated link work).

## M2 — `CommonFW`, before either firmware depends on it · *done, 2026-08-19*

Owned by [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md). `kilnlink` builds
under both pico-sdk and ESP-IDF, both firmwares consume the same codecs for
every `LINK_PROTOCOL.md` sec 4/6 command, `pc_tools` cross-checks the same
byte vectors, and a CI grep (`tools/check_no_duplicate_crc.ps1`) keeps a
second CRC/framing implementation from reappearing outside `CommonFW`. Detail
in `firmware/CommonFW/README.md` and `docs/LINK_PROTOCOL.md`.

## M3 — Safety processor to first trustworthy reading

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 2–4. Independent of the
link, so it can run in parallel with M1 and M2 once M0 is out of the way.

- [x] FreeRTOS SMP skeleton, GPIO6 driven low first, watchdog with latched trip
      reason — all done 2026-08-16, build-verified
- [ ] **MAX31856 thermocouple ICs not yet connected on the bench.** Blocks all
      real-reading work below it
- [~] MAX31856 driver + config plumbing (tc_type via flash-backed
      `config_store`, commissioned over `SAFETY_CMD_SET_CONFIG`) built and
      wired end-to-end in code (2026-08-19) — **still open**: the part itself
      is not physically populated on the bench, and there is no LCD/web
      commissioning surface yet
- [x] 12 of 13 guards (`SAFETY_MODEL.md` §4) implemented as pure functions and
      host-tested against synthetic inputs (320+/320+ checks) — S8
      (rate-of-rise) intentionally ships disabled until a real kiln's ramp
      rate is measured. **Qualified by M9's `virtual_dut` cross-check**: only
      4 of these 12 (S5, S6b, S7, S12) are reachable by real code today —
      `safety_core_build_input()` doesn't yet populate the fields the other 8
      need. Guard logic being correct says nothing about its inputs being
      wired; see `GUARD_TEST_MATRIX.md`'s reachability section for the
      guard-by-guard detail and M4/M5 below for the specific gaps
- [x] CI grep: `safety_core.c` never includes the link header — 2026-08-16,
      `firmware/SaftyFW/tools/check_isolation.ps1`

## M4 — Relay authority

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 5. First milestone that can
physically stop a kiln, and the first that can nuisance-trip one.

- [x] Relay owner task is the sole writer of GPIO6 — confirmed by whole-tree grep
- [x] Trip latches and requires an explicit `CLEAR_TRIP` to clear, refused if
      the tripping condition re-fires on a one-tick retest
      (`safety_guards_try_clear()`); wired end to end PC→ESP→Pico and
      ESP-side send (`safety_link_send_clear_trip()`) plus web/UART surfaces —
      all built and host-tested (2026-08-19). **Known scope limit** (documented,
      not a bug): a graduated/windowed guard's elapsed-time accumulator resets
      on clear, so it re-trips on its own timescale rather than instantly.
      **Not hardware-verified**: the physical link has never carried a real
      CLEAR_TRIP frame (M0)
- [ ] **K4 is never energized anywhere in the current tree** (found via M9's
      `virtual_dut` cross-check, 2026-08-20). `relay_owner_command_energize()`
      is implemented and host-tested but has zero callers — that caller is
      Phase 7's link_task/GUI integration, not yet built. Not a regression;
      just means K4 reads open from every boot today regardless of guards
- [x] Rule engine drives relays through the existing owner arbitration —
      `rules_task.c` claims `RELAY_OWNER_RULE` and never writes the SX1509
      directly, so precedence is PROFILE/AUTOTUNE > RULE > MANUAL. Fails safe
      on safety fault, down link, OTA in progress, and on its own stale-tick
      watchdog. **Rules may never command a zone-assigned (PID/thermocouple)
      relay** — owner's scope rule, enforced in the evaluator, the task and
      the POST handler; heater relays stay readable as rule conditions.
      2026-08-22, verified on hardware before the bench was disassembled
- [x] Dashboard/readiness no longer report the safety link as up merely
      because the driver object exists — both call sites now consult the real
      staleness-gated `link_up`. This was a live false positive: the board
      reported "safety=up" with the UART unplugged. 2026-08-22
- [ ] S9 trip-ineffective escalation proven with a deliberately welded contactor
      (hardware-gated)
- [ ] Every guard exercised per
      [`GUARD_TEST_MATRIX.md`](firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md) —
      §2's host-provocation table is fully audited (452+/452+ checks pass);
      §3's hardware-trip rows remain open (no bench hardware attached)

## M5 — The link carrying real traffic

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 6–8, contract in
[`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md).

- [~] Current sensing: load-active detection and a power estimate (not an
      over/under-current trip) built and wired onto `SAFETY_CMD_POWER` — **open**:
      guards S3/S4 are deliberately not wired to `any_current_present` until
      the per-channel CT-to-jack commissioning check passes on real hardware
      (`SaftyFW/docs/CURRENT_SENSE.md` §5). More broadly, `safety_core_build_
      input()` never sets `context_valid` at all, which independently keeps
      S2/S3/S4/S10/S13 inactive — see M3's guard bullet
- [x] ESP → Pico context frames (`SAFETY_CMD_PUSH_CONTEXT`, incl.
      `relay_recent_mask`) built from live board state — 2026-08-18. Not
      hardware-verified: no Pico on this bench to confirm it decodes correctly
- [~] Pico → ESP telemetry (status, diagnostics, firmware version, trip events,
      power) — all five frame types have working codecs, send paths, and
      `KilnFW`-side decode/dispatch, plus PC-facing `GET_DIAG`/`GET_TRIP_EVENT`
      subcommands (2026-08-18–19). **Not hardware-verified, and cannot be from
      this environment**: M0's dead link means no frame here has ever crossed
      a real wire — host-tested codec vectors and clean builds under each real
      toolchain only
- [x] Pico never blocks on the link — all five no-wait rules audited clean
      2026-08-18 (no ACK/retry, bounded non-blocking TX, `safety_core.c` never
      calls into the link, correct task priority/core affinity)
- [x] Mutual version handshake: `ANNOUNCE_VERSION` both ways, `min_compatible`
      checked in both directions, both firmwares on the shared codec
- [x] TX ring reserves capacity for telemetry; log frames dropped above a 50%
      watermark and the drops counted
- [x] Borrowed-thermocouple staleness split correctly across S11/S13/S6 —
      audit-confirmed 2026-08-18, no code fix needed
- [x] `SAFETY_CMD_COMMIT_CONFIG_REJECTED` (0x20) — a refused commissioning
      commit used to be indistinguishable from an accepted one. The Pico now
      names the offending `param_id` and a reason code (RANGE /
      CONTRADICTION), and `safety_cfg_http.c` surfaces it in the page's error
      text instead of "sent, awaiting confirmation". 2026-08-22, host-tested
      both ends; not hardware-verified (M0)
- [x] `SET_LOG_LEVEL` (0x1B) reachable from the ESP — the codec and the Pico
      consumer existed with no caller, so the feature was dead. Now
      `POST /api/safety/log_level`, deliberately API-only (a bench knob, not
      an operator control). 2026-08-22
- [x] LCD stopped showing a raw `reason 0x%02X` where the web showed decoded
      words — the two same-language copies are one shared table
      (`safety_trip_words.h`). 2026-08-22

## M6 — Throw the liveness switch

The point at which the two processors become one system. Deliberately separate,
because it changes what a bare main board will do.

- [x] Link staleness → fault at a fixed 1.5 s ceiling, 30 s silence aborts a
      running firing, boot-time `FW_VERSION` request retried until answered,
      and a documented bench escape hatch (`safety_link_fault_on_link_loss`) —
      all built 2026-08-18/19. Code-verified and flashed; not hardware-timing
      verified (no Pico on this bench)
- [~] GUI (web + LCD) surfaces safety temperature, enclosure temperature, and
      power — built and wired to the same status cache the wire frames land
      in; reads null/"---" today because no Pico has ever sent the frames on
      this bench (M0), not because of a code gap
- [ ] **AP-fallback fix unverified end to end** — needs a router with both
      correct and deliberately-wrong static config; see `KilnFW/TODO.md` Wi-Fi

**2026-08-20: a large batch of UI, Wi-Fi, and boot-stability bugs were found
and fixed during a full hardware test pass** — profile/readiness reporting,
the LCD no-scroll rewrite, captive-portal DNS hijack, gzip content
negotiation, the Digital Fire built-in schedules, a thermocouple-fault page,
web DHCP/static-IP toggle, and others. Full list, and the ongoing ledger, is
in `firmware/KilnFW/TODO.md` and `docs/UI_PLAN.md`; two hazards worth reuse
were promoted to the decisions table below (internal-SRAM exhaustion at task
creation, and the UART owner's per-transfer heap churn).

## M7 — Repo reorganisation · *done 2026-08-16, two items open*

Owned by [`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md). Tree split into
`hardware/`/`firmware/`/`tools/`/`docs/`, library tables and submodule paths
fixed, fresh-clone `mainBoard` open confirmed 2026-08-19.

- [ ] `mykicadMcp/` and `pdfMcp/` moved under `tools/` — blocked by running
      processes holding the directories open, plus `.mcp.json` hardcoded paths
      that need updating first
- [ ] `hardware/UnitTestFixture` KiCad project still unopened (the other three
      projects were confirmed clean 2026-08-16)

## M8 — Field updates

Owned by [`firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](firmware/CommonFW/docs/UPDATE_PROTOCOL.md)
and [`firmware/SaftyFW/docs/BOOTLOADER.md`](firmware/SaftyFW/docs/BOOTLOADER.md);
tracked as `KilnFW/TODO.md` section 9 and `SaftyFW/TODO.md` phase 10.

Last, and genuinely last: a bootloader is new code in the component with nothing
behind it, and it is only defensible because SWD sits underneath as the recovery
path. Two facts set the shape of this milestone:

- **The RP2040 mask ROM has no UART bootloader.** Updating the Pico over the
  isolated link means writing one. It is written once over SWD and never
  updates itself.
- **You cannot OTA your way into being OTA-capable.** The ESP needs a new
  partition table with two app slots, and a partition table can only be written
  over a cable.

- [x] **Measure the isolated link's real error rate — done, and 115200 does
      not work at all.** The optocouplers cap the link at 9600 (see M0); the
      update transfer's error rate at that baud, over a sustained
      multi-megabyte run, is still unmeasured. Retry cost is still 200 ms × up to 10
- [x] Real flash size established (N16R8, 16 MB/8 MB PSRAM) and declared in
      `sdkconfig` — 2026-08-17. `partitions.csv` still maps only the first 2 MB
      (single factory slot); the two-app-slot OTA table is the next item.
      Bootloader still needs reflashing on the physical board for this to
      take effect there
- [x] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, app confirms itself only after
      NVS + safety link + web server are up — host-build-verified, not yet
      hardware-flashed
- [~] Pico flash layout/metadata frozen (reserved `signature[64]`, `sig_required`,
      pubkey region) and implemented in `metadata.c`/`flash_layout.h` — design
      and code done 2026-08-19; not flashed or hardware-verified (no RP2040
      attached)
- [~] Pico bootloader: GPIO6 low first, per-boot CRC, `boot_attempts` fallback,
      and a real recovery-mode UART1 receiver (not beacon-only) — all built
      and host-build-verified 2026-08-19; not flashed or exercised over a live
      UART1 link
- [x] Mutual protocol-version check landing with M5 — both firmwares on the
      shared `kilnlink_announce` codec, GUI shows both sides' versions and
      names which is older. Not verified against real mismatched hardware
- [~] Compatibility floor (frame ids `0x00`–`0x0F` reserved, never gated on
      `peer_version_compatible`) — frozen and implemented; not verified
      end-to-end against a live mismatch
- [x] Image header validated before the first erase (2026-08-17)
- [x] Challenge–response on the AP password, never crosses the wire, 3-failure
      lockout (2026-08-17)
- [x] Both update paths refused unless idle and cool, with the specific
      blocker named (2026-08-17)
- [ ] Link-loss heating block **not** bypassed during a Pico update — correct
      by code inspection on both sides (`relay_authority.c` untouched since
      2026-08-13; `update_task.c` only reads output/thermo status to gate
      `UPDATE_BEGIN`, never writes relay/GPIO state), but not yet exercised on
      real hardware
- [x] Four MCP tools for OTA (challenge, ESP update, Pico update, status),
      plus explicit ESP and Pico rollback and a web `/ota` page — built and
      unit-tested against mocked HTTP (2026-08-18–19); not yet exercised
      against a physical board
- [x] `SAFETY_CMD_ANNOUNCE_REBOOT` sent before a routine ESP reboot so a
      firmware update doesn't trip S6(b) — 20 s grace window, host-tested;
      not hardware-verified (no board attached to confirm a real reboot
      suppresses the trip)

## M9 — SimFW: the bench fixture that finally unblocks hardware verification

Owned by [`firmware/SimFW/docs/PLAN.md`](firmware/SimFW/docs/PLAN.md). A *third*
firmware — a second Pico that plugs into the main board's connectors in place
of the real thermocouple daughterboard and the rest of the kiln, so guards,
faults, and control loops in both other firmwares can be exercised repeatably
from a PC script instead of a real kiln. Not on the `KilnFW`↔`SaftyFW`
dependency spine, but it directly gates `GUARD_TEST_MATRIX.md` §3's
hardware-trip rows, which is why it earns a milestone here.

- [x] **Software complete: every task, every `src/sim/` module, every driver,
      the 19-scenario test library, and the `kilnsim` PC toolset (CLI/GUI/MCP)
      — no stubs remain** (2026-08-20, commit `c891b72`). Also extracted
      `UnitTestFw`'s UART protocol into `firmware/CommonFW` as `benchproto`.
      Detail and per-module verification numbers in `docs/PLAN.md`
- [x] `docs/HARDWARE.md` written — fixture pin map reconciled across four
      driver files, zero GPIO collisions. Surfaced two real open questions,
      tracked in `docs/PLAN.md` §11:
      - `KilnFW`'s and `SaftyFW`'s own `HARDWARE.md` docs disagree on whether
        J7 pin 1 is no-connect or `3.3v_Safty` — needs a continuity check
        before the fixture's isolated-side power feed is wired
      - the DUT-power relay (a single MCP23017 bit) can only brown out one of
        the main board's two independent 12 V inputs unless the bench
        operator deliberately commons them downstream of that relay
- [ ] **Hardware-gated, nothing below has ever touched real silicon** — no
      fixture (breadboard or PCB) has ever been built or connected. In
      particular: M-A's SPI-timing exit criterion (Saleae capture, ≥10k
      transactions, zero underruns) is unmet and is the single biggest
      unproven risk; CT amplitude calibration is still an identity placeholder
      pending the real sweep-fit-store procedure (M-D); the `UnitTestFw`
      decommission is only half done (protocol extracted, hardware proof-out
      not done, old tree still present); none of the 19 scenarios has run
      against a real `KilnFW`+`SaftyFW` pair
- [ ] **Dependency this milestone exists to unblock**: `GUARD_TEST_MATRIX.md`
      §3's hardware-trip rows (safe-state power-on, sensor open-circuit,
      current-mapping commissioning, every enabled guard's real trip) stay
      blocked on "no bench hardware" until the fixture physically exists.
      Each scenario already declares which guard(s) it exercises, so the
      moment hardware exists there is a concrete script to run instead of
      nothing
- [x] **`virtual_simfw`/`virtual_dut`: a software-only cross-check that
      compiles `SimFW`'s and `SaftyFW`'s own unmodified guard code for the
      host and ticks real guard logic against simulated data** (2026-08-20).
      Not hardware verification and doesn't claim to be — no real SPI/relay/
      link/FreeRTOS jitter — but it empirically established that **only S5,
      S6b, S7, S12 can structurally fire in today's shipping `SaftyFW`**; the
      other nine are blocked on specific unpopulated inputs (see M3/M4/M5
      above for the per-guard detail, `GUARD_TEST_MATRIX.md`'s reachability
      section for the full table). Re-runnable: re-run after Phase 6/7 lands
      to see which guards newly become reachable, without needing bench
      hardware

---

## Future work — KilnFW PC-link command acknowledgement

Not on the dependency spine and not owned by any per-area plan yet — filed
here until someone picks it up, at which point it should move into
`firmware/KilnFW/TODO.md` and this becomes a one-line pointer per the upkeep
rule below. Scoped to the **PC↔ESP link** (`uart_protocol.c`, `uart_bridge.c`,
`uart_bridge_ext.c`, `tools/PcTools/src/kilnctrl`) — unrelated to the
ESP↔Pico safety link's `LINK_PROTOCOL.md`, which has its own, already-correct,
no-ACK doctrine (see below).

**The failure mode.** `uart_protocol.c`'s RX task ACKs a `DATA` frame the
instant it is enqueued into the destination bridge task's inbox — before that
task's `switch (subcmd)` ever runs. Commit `c91ed50` closed the narrowest gap
(all 11 `default:` branches now reply `{subcmd, ok=0}`), and `CONTROL`/
`PROFILES`/`WIFI`/`AUTOTUNE` already reply ok/fail for their *recognized*
mutating subcommands via `bx_reply_ok_err`. But `bridge_args_ok`/`bx_args_ok`
(truncated payload) and `bridge_range_ok` (out-of-range index) in
`uart_bridge.c` both do `rejected = true; break;` with **no reply at all** —
comment at `uart_bridge.c:753-755` names the shape explicitly: "rejected ->
no reply". Same for `IO_CMD_SET_RELAY`/`SET_RELAY_MASK`'s
`KILN_IO_OWNER_RELAY_ERR_OWNED`/`ERR_SAFETY`/`ERR_UPDATING` refusals
(`uart_bridge.c:763-846`) — logged on the device, invisible on the wire. A
caller that only checks the transport ACK cannot distinguish "relay energized"
from "refused because a profile owns it" from "refused because safety is
faulted" from "dropped because the frame was truncated." For a relay command,
that is the worst available shape: success and safety-refusal look identical.

**Why the status quo isn't just an oversight.** `uart_bridge.c` and
`safety_link.c` both say, deliberately, "the PC observes the outcome via the
next status/diag poll, not an ACK from here" — for `SAFETY_CMD_SET_CONFIG`/
`SET_CT_CAL`/`ROLLBACK`, which ride the *fire-and-forget broadcast* half of
this codebase, that's correct: those commands cross onto the Pico, which
`LINK_PROTOCOL.md` §1–2 forbids from ever being obliged to reply, so "poll the
next telemetry frame" is the only mechanism physically available, and it works
because telemetry already carries the relevant state (`config_crc`, trip
mask, boot_id) every 500 ms regardless. **That reasoning does not transfer to
the PC↔ESP hop**: this is a stop-and-wait `DATA`/`ACK` link with retries
already built in (`uart_protocol_send_limited`), not a one-way broadcast from
a component that must never block. Reusing the Pico doctrine here is
borrowing a constraint (no round trip) that doesn't apply, at the cost of the
one thing a request/response link is good for.

**Options.**

| # | Scheme | Cost | kilnctrl changes? | Wire-compatible? |
|---|---|---|---|---|
| a | Status-quo-plus-poll: leave transport ACK as-is, PC always re-queries state after a mutating command | Cheapest to ship (nothing to build) but weakest: nothing on the wire tells a caller *when* to poll, doubles round trips for every write, and a caller that doesn't poll is back to today's blind spot | None required, but only closes the hole if every caller adopts the habit | Yes — no frame change |
| b | Explicit `{subcmd, ok, [msg]}` reply on **every** mutating subcommand — extend `bx_reply_ok_err`'s existing convention to the paths that currently `break` silently | Touches ~25-30 `rejected = true; break;`/early-return sites across `uart_bridge.c`'s THERMO/IO/DISPLAY/SAFETY tasks (`CONTROL`/`PROFILES`/`WIFI`/`AUTOTUNE` already mostly there) | Yes — `io_expander.py` and siblings currently discard everything but the transport `SendResult`; they'd need to read the reply payload for calls that matter | Yes — appends a reply where none existed, or extends an existing one; no reorder/resize of anything a v5 client already parses |
| c | Defer the transport ACK itself until after the handler dispatches, so ACK proves execution not delivery | Reaches into `uart_protocol.c`'s core RX/dedup/ACK path; conflates "malformed frame" with "slow but valid command" (SX_SCAN's PC-side timeout is already 4 s); requires either the transport layer to block on the destination task's dispatch (the coupling `LINK_PROTOCOL.md`'s no-block doctrine explicitly targets, even though this is a different link) or a redesigned per-subcommand timeout budget | Yes — full retry/timeout assumption rewrite | No — changes what an ACK has always meant to every existing caller |
| d | Sequence/receipt: every mutating command gets a seq, PC correlates against a per-task "last applied seq" queryable state, mirroring the Safety link's `seq`/`trip_seq` dedup | New per-task state (`dedup_record`-shaped ring) plus a new query subcommand per task, or a shared new one | Yes — new correlation logic | Yes, but adds real wire surface for a benefit the Safety link needed (surviving a lost reply on a no-ACK broadcast) and this link already has for free (retried `DATA`/`ACK`) |

**Recommendation: (b).** It is the direct generalization of the fix already
shipped in `c91ed50` and already proven out by `CONTROL`/`WIFI`/`PROFILES` —
no new frame type, no version bump, no timeout redesign, and it is honest
about *why* the Pico's no-reply contract doesn't apply here: that contract
exists to keep a safety processor from ever blocking on a dead peer, and the
PC↔ESP link already blocks-with-retry by design. The only real cost is
mechanical breadth — every silent refusal path needs one more line — and a
second, coordinated change in `kilnctrl` to actually look at what comes back
instead of trusting a bare transport ACK. (a) is the fallback if effort is
tightly bounded: it needs zero firmware changes and can be adopted piecemeal
in `kilnctrl` today, but it never closes the gap for a caller that doesn't
explicitly poll, which is the caller most likely to be a bug, a script, or an
impatient human.

**Migration path.** Land per-task, safety-adjacency first: `IO` (relay
commands) before `THERMO`/`DISPLAY`/`SAFETY`'s remaining silent paths. Every
new reply is `{subcmd, ok, [msg]}` appended after that task's existing echo
byte, so a `kilnctrl` build that doesn't yet read it keeps working exactly as
before (same "purely additive" property `c91ed50`'s commit message claims) —
no protocol version bump needed unless a specific reply changes the *shape*
of an already-non-empty response rather than adding one where there was none.
`kilnctrl` picks up each task's new replies independently, starting with
`io_expander.py`'s relay `send()` wrapper, so the two sides can land in
separate commits without either being broken by the other mid-migration.

**Scope estimate.** Two coordinated changes, not one: firmware side is
`firmware/KilnFW/App/drivers/uart_bridge.c` and `uart_bridge_ext.c`
(~25-30 call sites); PC side is `tools/PcTools/src/kilnctrl/io_expander.py`
and the handful of sibling task clients that currently discard the reply
payload for a mutating call, plus their MCP tool wrappers in
`mcp_server.py`. Each task can ship and be adopted independently — this is
not a single flag-day cutover.

---

## Decisions taken, so they are not re-litigated

| Decision | Date | Where the reasoning lives |
|---|---|---|
| Pico bench path is the Debug Probe: SWD plus its UART bridge on GP16/GP17. **The Pico's own USB is not used.** | 2026-08-16 | `firmware/SaftyFW/docs/ARCHITECTURE.md` §1 |
| ~~**PSRAM stays disabled** on the ESP32-S3~~ — **reversed 2026-08-17: PSRAM is ENABLED** (octal, 8 MB) and used for LVGL draw buffers + heap + several task stacks | 2026-08-16, reversed 2026-08-17 | `firmware/KilnFW/TODO.md` §9.1a |
| Library paths use `${KIPRJMOD}/../lib`, not a KiCad path variable | 2026-08-16 | `docs/REPO_LAYOUT.md` B1 |
| OTA authentication is challenge–response on the AP password, never a form POST | 2026-08-16 | `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` §2 |
| Update frames and the version handshake are a **frozen compatibility floor** | 2026-08-16 | `firmware/CommonFW/docs/LINK_PROTOCOL.md` |
| K4 → line-contactor interlock: J10 pin 1 = NO, pin 2 = COM, pin 3 = NC (read from the K4 symbol's rest position, not silkscreen) — still wants a continuity check against the physical part | 2026-08-16 | `firmware/SaftyFW/docs/HARDWARE.md` §3 |
| E-stop circuit is normally-closed by design; no jumper fitted on the `estop` net today, so an as-built board reads permanent STOP until one is added | 2026-08-16 | `firmware/SaftyFW/docs/HARDWARE.md` §5 |
| ESP32-S3 boot-loop (repeating stack overflow in the main task, right after LVGL's boot banner) fixed by raising `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 3584→8192 | 2026-08-19 | `firmware/KilnFW/App/main.c`, `sdkconfig.defaults` |
| Internal SRAM exhaustion: `xTaskCreatePinnedToCore()` always takes TCB+stack from internal SRAM, and Wi-Fi/lwIP + LVGL had claimed nearly all of it by the time later tasks tried to start (caused the AUTOTUNE/WIFI UART-task registration failures). Fixed at the source — moved LVGL's allocator and the Wi-Fi/lwIP pools to PSRAM — not by shrinking the tasks that were failing | 2026-08-20 | `firmware/KilnFW/TODO.md` §1 |
| `uart_owner_transfer()` called `xSemaphoreCreateBinary()` (a heap alloc) on every single UART transfer; under real interactive load this exhausted internal SRAM (`ESP_ERR_NO_MEM` bursts every ~40s). Fixed with a static, stack-resident semaphore | 2026-08-18 | `firmware/KilnFW/App/drivers/espInterfaces/uart_owner.c` |
| SimFW's PIO SPI-slave engine originally sampled/shifted on the wrong clock edges (mode 0, despite being labeled mode 1); corrected to match the MAX31856 datasheet's Table 5 (CPOL=0) and both real masters' actual config | 2026-08-20 | `firmware/SimFW/src/drivers/max31856_spi_slave.pio`, `docs/DESIGN_NOTES.md` §3.2.1 |
| LVGL hit-testing cannot escape a parent that doesn't contain the touch point, and a non-`LV_OBJ_FLAG_FLOATING` child of a flex column silently joins the flow and eats the page's content budget | 2026-08-20 | `firmware/KilnFW/App/drivers/ui_topbar.h` |

---

## How the work gets done — delegate to Sonnet subagents

Standing instruction from the repository owner, 2026-08-21. It applies to every
milestone below and to any new work filed against this roadmap.

**The coordinating session should not implement roadmap work itself.
Implementation is assigned to Sonnet subagents, and the coordinator oversees
them.** In practice that means:

- The coordinator reads enough of the code to write an accurate brief, splits
  the work into non-overlapping file scopes, dispatches Sonnet subagents, and
  then verifies what comes back. It does not sit down and write the feature.
- **Sonnet is the default worker model.** Opus and Haiku are available when a
  task genuinely calls for them; Fable coordinates and reviews only, and is
  never a worker.
- **Scopes must not overlap.** Two agents editing the same file collide
  silently and the loser's work is lost. Every brief names the files it owns
  and the files it must not touch.
- **Builds stay central.** Concurrent `idf.py` runs against one build
  directory clobber each other, so subagents write code and the coordinator
  compiles once. Briefs say "do not build" explicitly.
- **Verification is not delegated.** A subagent's report is a claim, not
  evidence. The coordinator builds, flashes, and exercises the change against
  the four levels in "What 'done' means" below before any item here is ticked.
- The narrow exception is shared scaffolding that encodes a hazard already paid
  for in a debugging session — the kind of thing a fresh agent reliably gets
  wrong. Writing that once, centrally, so every delegated task inherits the fix
  is cheaper than briefing the hazard into every agent.
  `firmware/KilnFW/App/drivers/ui_topbar.h` is the worked example: it exists
  because LVGL hit-testing cannot escape a parent that does not contain the
  touch point, and because a non-`LV_OBJ_FLAG_FLOATING` child of a flex column
  silently joins the flow and eats the page's content budget. Both cost a
  session before they were understood.

---

## Working from the repository root

Claude and the editor are opened at `kilnCtl/` from 2026-08-16 onward. This is
now a load-bearing assumption rather than a preference:

- **Open `kilnCtl.code-workspace`, not the folder.** The ESP-IDF extension needs
  a `CMakeLists.txt` in the workspace folder it is pointed at, and the repository
  root does not have one. The multi-root workspace gives it `firmware/KilnFW`
  while keeping the root open alongside.
- `.mcp.json` uses root-relative paths, including `-C firmware/KilnFW` for the
  ESP-IDF server.
- `idf.py` needs `-C firmware/KilnFW`; `uv` needs `--project tools/PcTools`.

What it changes, and what still needs doing, is
[`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md) "Working from the repository root".

---

## Cross-processor invariants

Any change that touches these needs both plans read, not one:

- The safety processor **never blocks on the link**. A frozen ESP core must not
  be able to stall it.
- The safety processor is **RX-only while firing**; the telemetry relaxation
  applies outside firing and to unacknowledged broadcast frames.
- **The safety processor must be alive to start or continue any heater-on event.**
- Current measurement is **load-active detection and a power estimate only**.
- Guards clear two independent bars — a magnitude correct operation cannot reach,
  and a duration a transient cannot sustain.
- All board relays are **pilot relays**, driving external contactors and SSRs.
- **Neither processor is updatable while the kiln can heat**, and the Pico
  enforces that itself rather than trusting the ESP.
- **Each processor checks the other's protocol version**, in both directions, and
  neither trusts the other's data until both agree. The frames that establish
  that, and the ones that carry an update, are frozen so a mismatch can never
  lock out the fix.

## What "done" means

Same four levels everywhere, and they are not interchangeable:

**planned** → **built** (compiles, `-Wall -Wextra -Werror`) → **host-tested**
(synthetic inputs, negative paths) → **hardware-verified** (observed on the real
board). [`firmware/KilnFW/docs/PROJECT_STATUS.md`](firmware/KilnFW/docs/PROJECT_STATUS.md) keeps
built and verified distinct; every plan here is expected to do the same.

## Roadmap upkeep

- [ ] Milestone ticks mirrored into the owning plan, not only here
- [ ] `Last reviewed` date bumped whenever a milestone changes state
- [ ] New work filed under a milestone, or a new milestone added with its owner
- [ ] **A finished item leaves this plan.** Either it moves to a
      reference/doc file (a decision, hazard, or reasoning someone will re-hit
      — a one-line pointer here is enough) or it is deleted. Ticked boxes and
      completion narratives do not accumulate here; a milestone that is fully
      done collapses to one line
