# kilnCtl Roadmap — both processors

> **Status:** planning · **Last reviewed:** 2026-08-24
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
  detection, owns the mechanical pilot relay K4. **Built and running on real
  silicon**; every guard input is now produced, and what remains is
  commissioning values plus the hardware-gated trip proofs.

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
| [`firmware/SimFW/docs/PLAN.md`](firmware/SimFW/docs/PLAN.md) | Kiln simulator / unit-test fixture (a *third* firmware, a second Pico on the bench): MAX31856 emulation, CT waveforms, relay sensing, thermal model, fault injection, its own MCP/CLI/GUI (`kilnsim`); also owns the `UnitTestFw` decommission. **Running on real silicon since 2026-08-24** — the fixture Pico boots, every command group is hardware-verified, fault injection reaches the emulated MAX31856 registers, and two I/O expanders are attached. Still gated on the SPI-timing proof (M-A, `spi_txn_count` is 0) and on the relay/E-stop/DUT-power harness. `kilnsim testmgr` is the one-command regression entry point; `firmware/SimFW/docs/TEST_MANAGER.md` documents it — see M9 |
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
- [x] Tier 0 pin test settles ESP TX/RX by measurement (`HARDWARE.md` §1) —
      moot as a separate step: the Pico is attached and the link carries live
      telemetry both ways (`safety_get_status()` returns fresh frames,
      `tx_dropped 0`), which settles TX/RX by observation rather than by
      probing pins
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
safety processor (RP2040) still has none fitted.

**Bench state (2026-08-24):** both UARTs work — the two blockers named above
are cleared. The isolated Pi↔ESP link is up and carrying telemetry at 9600
(M0), and the PC↔ESP command UART on COM9 is responsive. Both firmwares were
flashed over JTAG/SWD today and verified running. The safety processor's own
MAX31856 is still not populated, so `safety TC invalid` is the expected steady
state there; the E-stop net measures low (a contact IS fitted, contrary to
`HARDWARE.md` §5's older note), so S7 correctly stays quiet.

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
      rate is measured. **Input wiring now complete (2026-08-24):**
      `safety_core_build_input()` populates every field the guards read —
      `context_valid`, `any_current_present`, `relay_commanded_recently`/
      `_continuously`, `zone_count`, the setpoint/measured reductions,
      `sample_counter_advancing`, both discretes and `reboot_grace_active`.
      The older "only S5/S6b/S7/S12 are reachable, the other 8 have no
      producer" finding is superseded: what still holds a guard dormant is a
      missing **commissioning value** (S1's `abs_max_temp_c` defaults to 0 =
      never trip; S13 needs `tc_source`/`borrowed_zone_index`), which is a
      different kind of gap from a missing producer. Two real producer bugs
      were found and fixed on 2026-08-24 — the E-stop polarity was inverted
      (S7 could not fire) and `current_sense_set_cal()` was never called, so
      `any_current_present` was permanently false (S3/S9/S11 and S6b's
      current-gated trip could not fire). **The reachability count in
      `GUARD_TEST_MATRIX.md` predates both fixes; re-run M9's `virtual_dut`
      to re-establish it rather than trusting the old number.**
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
      **Hardware-verified 2026-08-23/24**: the physical link has carried real
      CLEAR_TRIP frames. Refused correctly against a latched S5 with the
      safety thermocouple genuinely absent, accepted once the tripping
      condition cleared, and the board never rebooted in either case — which
      it originally did, via a `log_task` stack overflow on the refusal path
- [x] **K4 now has a real energize path.** The "zero callers" finding of
      2026-08-20 is stale: `safety_core_request_enable()` calls
      `relay_owner_command_energize()` (`safety_core.c`), reached from
      `link_task`'s enable handler, and it refuses ON when the safety
      thermocouple is declared absent or a trip is latched. **Still not
      proven on hardware**: K4 has never been observed closing on a real
      board, and the bench cannot demonstrate a genuine heat-enable until the
      safety thermocouple is populated (S5 latches without it)
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
      over/under-current trip) built and wired onto `SAFETY_CMD_POWER`.
      **2026-08-24: the calibration was never actually loaded** —
      `current_sense_set_cal()` had no caller anywhere, so `amps[]` was
      permanently 0 and `any_current_present` permanently false, silently
      disabling S3/S9/S11 and S6b's current-gated trip while S4 warned
      forever. Now loaded at boot and on every `COMMIT_CONFIG`, and presence
      detection is decoupled from `k_ct_v_per_a` (deciding whether current
      flows never needed a volts-per-amp scale). `CURRENT_SENSE.md` §5 and
      `CONFIG_REFERENCE.md` both claimed a wrong `k_ct_v_per_a` could not
      affect guard behaviour; that was false and is corrected. `context_valid`
      is also computed now — see M3's guard bullet. **Still open**: the
      per-channel CT-to-jack commissioning check on real hardware, and a bench
      measurement of the ADC noise floor to confirm the uncommissioned
      presence fallback margin (25 counts) sits above it
- [x] ESP → Pico context frames (`SAFETY_CMD_PUSH_CONTEXT`, incl.
      `relay_recent_mask`) built from live board state — 2026-08-18. Not
      hardware-verified: no Pico on this bench to confirm it decodes correctly
- [~] Pico → ESP telemetry (status, diagnostics, firmware version, trip events,
      power) — all five frame types have working codecs, send paths, and
      `KilnFW`-side decode/dispatch, plus PC-facing `GET_DIAG`/`GET_TRIP_EVENT`
      subcommands (2026-08-18–19). **Now hardware-verified (2026-08-23/24)**:
      M0 is cleared, status/diag/power frames cross the real wire, and the
      diag/power applied counters were observed climbing at the expected
      0.5/s over a 90 s soak. Getting there needed a UART TX self-start fix —
      an edge-triggered TX interrupt that never re-armed stranded a frame in
      the ring silently, with no counter tripped
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
      `sdkconfig` — 2026-08-17. **Both follow-ups are now done**: the
      two-app-slot table exists (`otadata`/`ota_0`/`ota_1`/`factory`/
      `pico_img`/`coredump`, and `factory` moved to 0x810000 on 2026-08-21),
      and the bootloader + partition table were flashed and verified on the
      physical board over JTAG on 2026-08-24. **Consequence worth knowing:**
      the sanctioned JTAG path writes the app into `factory`, so a
      bench-flashed build always boots `factory` and the rollback/boot-confirm
      machinery below never executes — it can only be exercised by a real OTA
      into `ota_0`/`ota_1`
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
- [~] **First silicon: the fixture Pico now runs on the bench** (2026-08-24).
      `SimFW` boots, enumerates, and every command group (SYS/MODEL/TC/CT/
      RELAY/IO/FAULT) is verified against the real board. Fault injection
      works end to end — a PC-side `TC_DISCONNECTED` flips the emulated
      MAX31856's open-circuit flag and clears on cancel. Two MCP23017
      expanders are attached and pass six selftest checks. Link soaked at 960
      commands over six reconnects with zero anomalies. `kilnsim testmgr`
      gives a one-command tiered regression verdict; `kilnsim tasks` reports
      per-task stack high-water marks, and `kilnsim selftest` now fails below
      a 2x margin.

      Getting there took four real firmware defects, all fixed: a
      `sim_engine` stack overflow that stopped the board booting at all; a
      cross-core-halt design that would have panicked at every scheduler
      start; a msg_index/dedup lifetime mismatch that silently returned a
      *stale reply from a different command* after every reconnect; and a
      `pio_claim_unused_sm(..., true)` whose own error guard was unreachable
      and which would have halted only core 1, leaving core 0 answering as if
      healthy.
- [ ] **Still hardware-gated** — M-A's SPI-timing exit criterion (Saleae
      capture, ≥10k transactions, zero underruns) remains unmet and is still
      the single biggest unproven risk: `spi_txn_count` is 0, so the PIO
      MAX31856 emulation has never been clocked by any master. It needs a
      second Pico as reference master (`tools/spi_test_master/` builds clean,
      but its checked-in `build/` is stale — rebuild before flashing). Most of
      M-A is achievable without a Saleae; the analyzer is only needed for the
      electrical timing margin, not functional correctness. CT amplitude
      calibration is still an identity placeholder (M-D). Relay sense, E-stop,
      fault line and DUT power are unwired, so no scenario has yet run against
      a real `KilnFW`+`SaftyFW` pair.
- [~] **Guard assertions in the scenario suite were vacuous** (found
      2026-08-24, half fixed the same day by the SimFW-owning session).
      `kilnsim`'s `EventType` defines `GUARD_TRIP`/`GUARD_WARN`/`LINK_UP`/
      `TRIP_INEFFECTIVE_LATCHED`; `runner.py` synthesized none of them, so the
      clause in 25 of 27 scenarios could not fail and the suite's guard
      coverage was nominal rather than real. `runner.py` now synthesizes them
      (`6f1cbbf`, recorded in `686a8d0`), and guard-typed clauses come back
      **BLOCKED** rather than vacuously PASS — the honest outcome. **Still
      open, owned by SimFW:** `testmgr` attaches no observer on its own runs,
      and the `SafetyClient` polling shape is unconfirmed against a real
      board.
- [x] **`UnitTestFw` deleted** (2026-08-23) — the old ESP32-S3 instrument
      bench tree (App, pc_tools, docs, embedded KiCad files) removed from the
      repo along with every stale reference, ahead of the M-B hardware-proof
      gate `docs/PLAN.md` originally called for, by explicit decision
      (SimFW is the replacement; not re-litigated). Detail in
      `firmware/SimFW/docs/DESIGN_NOTES.md` §12
- [x] **A hardware-free CI path that is worth trusting** (2026-08-24,
      `2bfce93`/`925cdee`). `kilnsim --virtual testmgr` runs the whole tiered
      suite against `virtual_simfw` — the real SimFW simulation code compiled
      for the host — instead of `MockSimLink`, which proves close to nothing.
      A virtual run reports SaftyFW and ESP absent with a `--virtual`-specific
      reason and caps at tier 0, so it cannot claim hardware tiers it does not
      have, and every saved report now stamps `link_kind` (serial/virtual/
      mock), which it never did before: a mock run's JSON used to be
      indistinguishable from a real-hardware one after the fact. Exit code 0
      against a fresh `virtual_simfw` with `command_groups_reachable` passing.
      **This does not move any hardware-gated milestone below** — it is the
      cheapest layer of `PLAN.md` §13's four, not a substitute for the ones
      that name silicon.
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

**Moved out of this file, 2026-08-24.** Per the upkeep rule at the top, the
detail now lives in [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md)
section 11, which owns it. In brief: the core defect is fixed — a truncated
payload, an out-of-range index and each relay refusal reason (owned / safety /
updating) now produce a reply the host can tell apart from success and from
each other, and `pc_tools` surfaces the reason instead of decoding it and
discarding it, which is what it used to do. The rule worth carrying forward is
recorded in [`firmware/KilnFW/docs/UART_PROTOCOL.md`](firmware/KilnFW/docs/UART_PROTOCOL.md):
on this hop an ACK means *queued*, not *done*, so a handler that rejects must
reply for itself. Three narrower instances of the same shape remain open and
are listed in TODO.md section 11.

This does NOT apply to the ESP↔Pico safety link, whose no-ACK doctrine is
deliberate and correct: `LINK_PROTOCOL.md` sections 1–2 forbid obliging the
Pico to reply, and telemetry already carries `config_crc`, the trip mask and
`boot_id` every 500 ms, so "poll the next frame" is both available and
sufficient there. The two files carry similar-looking comments that mean
different things; keep them distinct.

---

## Decisions taken, so they are not re-litigated

| Decision | Date | Where the reasoning lives |
|---|---|---|
| Pico bench path is the Debug Probe: SWD plus its UART bridge on GP16/GP17. **The Pico's own USB is not used.** | 2026-08-16 | `firmware/SaftyFW/docs/ARCHITECTURE.md` §1 |
| **On-board relays K1-K4 are for galvanic isolation and switch other relays only — never element current.** They are EE2-12NUH signal relays (2 A, 125 VA max), so element switching was never physically possible. This retires the duty-window contact-life worry; the downstream device's own life and coil inrush now set the limit. | 2026-08-24 | `firmware/KilnFW/TODO.md` §6A.0, `hardware/datasheets/mainBoard_Relay/EE2-12NUH.pdf` p7 |
| ~~**PSRAM stays disabled** on the ESP32-S3~~ — **reversed 2026-08-17: PSRAM is ENABLED** (octal, 8 MB) and used for LVGL draw buffers + heap + several task stacks | 2026-08-16, reversed 2026-08-17 | `firmware/KilnFW/TODO.md` §9.1a |
| Library paths use `${KIPRJMOD}/../lib`, not a KiCad path variable | 2026-08-16 | `docs/REPO_LAYOUT.md` B1 |
| OTA authentication is challenge–response on the AP password, never a form POST | 2026-08-16 | `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` §2 |
| Update frames and the version handshake are a **frozen compatibility floor** | 2026-08-16 | `firmware/CommonFW/docs/LINK_PROTOCOL.md` |
| **A request and its reply may never share a command id.** Telling them apart by payload length structurally blocks a short refusal reply, which is why SAFETY/DISPLAY/TOUCH's driver-error paths stayed silent. `GET_CT_CAL`/`GET_PARAM`/`GET_CONFIG_PAGE` moved to `0x22`/`0x23`/`0x24`; `GET_FW_VERSION` keeps its shared `0x0B` as the documented exception, being inside the frozen floor where a refusal is never needed. | 2026-08-24 | `firmware/CommonFW/docs/LINK_PROTOCOL.md` |
| **The two links version independently.** `UART_PROTOCOL_VERSION` (PC↔ESP) was an alias of `KILNLINK_PROTOCOL_VERSION` (ESP↔Pico) behind a hard-equality gate, so an isolated-link bump refused every PC command until pc_tools moved. Bitten three times before being split. | 2026-08-24 | `firmware/KilnFW/App/drivers/uart_task_ids.h` |
| K4 → line-contactor interlock: J10 pin 1 = NO, pin 2 = COM, pin 3 = NC (read from the K4 symbol's rest position, not silkscreen) — still wants a continuity check against the physical part | 2026-08-16 | `firmware/SaftyFW/docs/HARDWARE.md` §3 |
| E-stop circuit is normally-closed by design; no jumper fitted on the `estop` net today, so an as-built board reads permanent STOP until one is added | 2026-08-16 | `firmware/SaftyFW/docs/HARDWARE.md` §5 |
| ESP32-S3 boot-loop (repeating stack overflow in the main task, right after LVGL's boot banner) fixed by raising `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 3584→8192 | 2026-08-19 | `firmware/KilnFW/App/main.c`, `sdkconfig.defaults` |
| Internal SRAM exhaustion: `xTaskCreatePinnedToCore()` always takes TCB+stack from internal SRAM, and Wi-Fi/lwIP + LVGL had claimed nearly all of it by the time later tasks tried to start (caused the AUTOTUNE/WIFI UART-task registration failures). Fixed at the source — moved LVGL's allocator and the Wi-Fi/lwIP pools to PSRAM — not by shrinking the tasks that were failing | 2026-08-20 | `firmware/KilnFW/TODO.md` §1 |
| `uart_owner_transfer()` called `xSemaphoreCreateBinary()` (a heap alloc) on every single UART transfer; under real interactive load this exhausted internal SRAM (`ESP_ERR_NO_MEM` bursts every ~40s). Fixed with a static, stack-resident semaphore | 2026-08-18 | `firmware/KilnFW/App/drivers/espInterfaces/uart_owner.c` |
| SimFW's PIO SPI-slave engine originally sampled/shifted on the wrong clock edges (mode 0, despite being labeled mode 1); corrected to match the MAX31856 datasheet's Table 5 (CPOL=0) and both real masters' actual config | 2026-08-20 | `firmware/SimFW/src/drivers/max31856_spi_slave.pio`, `docs/DESIGN_NOTES.md` §3.2.1 |
| LVGL hit-testing cannot escape a parent that doesn't contain the touch point, and a non-`LV_OBJ_FLAG_FLOATING` child of a flex column silently joins the flow and eats the page's content budget | 2026-08-20 | `firmware/KilnFW/App/drivers/ui_topbar.h` |
| SimFW would not boot at all: `sim_engine`'s 2048-byte stack against a measured 2328-byte chain, caused by one non-static local (`fault_event_t[64]`, 1536 B). Third instance of this species after `telemetry` and `wave_owner`. Note the chain crosses a module boundary — `fault_sched`'s per-tick work runs on `sim_engine`'s stack while its own task idles — so neither file looks wrong read alone | 2026-08-24 | `firmware/SimFW/src/tasks/sim_engine.c` |
| SimFW's cross-core halt used the SIO inter-core FIFO, which is FreeRTOS's own SMP doorbell — it would have `hard_assert`ed at every `vTaskStartScheduler()`, and sharing the handler would not have helped since FreeRTOS drains that FIFO without inspecting it. Moved to a dedicated hardware timer alarm | 2026-08-23 | `firmware/SimFW/src/drivers/simfw_fatal.c` |
| benchproto returned a **stale reply from a different command** after every PC reconnect: the host's `msg_index` restarts at 0 per connection while the firmware's dedup ring and per-task ACK cache live for the MCU's boot lifetime. `FAULT_LIST` reported "0 faults" while 8 were armed — a clean-decoding wrong answer, diagnosed by reply *length* (3 bytes is `FAULT_SCHEDULE`'s shape, not an empty list's 2). Fixed with a `SYS_SESSION_RESET` handshake that must itself bypass dedup | 2026-08-23 | `firmware/CommonFW/src/benchproto_link.c`, `firmware/SimFW/src/tasks/usb_owner.c` |
| `pio_claim_unused_sm(pio, true)` panics internally and never returns, making the `if (sm < 0)` guard after it dead code — and a bare `panic()` halts only the calling core, while `spi_emu_a/b` are pinned to core 1, so core 0 would have kept serving USB against a fixture whose SPI emulation never started. Fourth structurally-unfailable check shipped in this repo | 2026-08-24 | `firmware/SimFW/src/drivers/max31856_pio_engine.c` |
| A 0-byte `i2c_write_blocking()` reports **every** address as present: pico-sdk guards `len == 0` with `invalid_params_if`, a no-op in release builds, so the transfer loop never runs and the address phase is never driven. Caught while writing a bus scanner, before it produced a wrong answer | 2026-08-24 | `firmware/SimFW/src/tasks/i2c_owner.c` |
| SimFW's relay-sense inputs were configured with pull-ups **off** while the sense lines are active-low, and the read path did not invert. Each defect masked the other, so the code looked self-consistent | 2026-08-24 | `firmware/SimFW/src/tasks/i2c_owner.c` |
| `RESET_SIM` / `LOAD_PRESET` zeroed the event ring's producer sequence but not the consumer cursor, which is only initialised at boot. The cursor was stranded *above* the producer and the drain returned 0 **forever**. Every health counter stayed clean (`evt_seq_gap_count: 0`, `evt_send_drop_count: 0`) precisely because nothing downstream of the stuck drain ever ran — the clean counters were evidence *of* the bug | 2026-08-24 | `firmware/SimFW/src/tasks/sim_engine.c` |
| `SET_SEED` stored the seed but never reseeded the PRNG — `fault_engine_init(&s_engine, 0)` runs once at boot with a hardcoded 0. Every scenario's `seed:` was cosmetic: stored, echoed in telemetry, returned by `sim_engine_get_seed()`, and ignored by the actual randomness, so the fixture's own replayability contract (`DESIGN_NOTES.md` §7.2) was not held. Hidden by the event-ring bug above, which had starved `determinism_spot_check` into a permanent SKIP — fixing one unmasked the next | 2026-08-24 | `firmware/SimFW/src/sim/fault_engine.c`, `src/tasks/fault_sched.c` |

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
