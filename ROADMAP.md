# kilnCtl Roadmap — both processors

> **Status:** planning · **Last reviewed:** 2026-08-19
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
| [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) | Safety firmware, phases 0–10 |
| [`firmware/SaftyFW/docs/SAFETY_MODEL.md`](firmware/SaftyFW/docs/SAFETY_MODEL.md) | What trips, why, and the anti-nuisance doctrine |
| [`firmware/SaftyFW/docs/ARCHITECTURE.md`](firmware/SaftyFW/docs/ARCHITECTURE.md) | Tasks, priorities, core affinity, logging transports |
| [`firmware/SaftyFW/docs/HARDWARE.md`](firmware/SaftyFW/docs/HARDWARE.md) | The traced board, pin map, bench connections |
| [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md) | Shared `kilnlink` code, used by both firmwares |
| [`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md) | The wire, both ends — the contract neither side may break alone |
| [`firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](firmware/CommonFW/docs/UPDATE_PROTOCOL.md) | Field updates for both processors: interlocks, one-password auth, ESP OTA partitioning |
| [`firmware/SaftyFW/docs/BOOTLOADER.md`](firmware/SaftyFW/docs/BOOTLOADER.md) | The RP2040 bootloader, flash layout and recovery mode |
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

- [ ] **Link confirmed not working end-to-end (2026-08-18). Needs bench
      measurement, not more code guessing.** Scope pins/logic analyzer TX and RX
      lines at both opto boundaries during a coordinated GPIO test (`tools/PcTools`
      coordinated test, M1 below) to find where the byte actually dies — idle
      level, framing, baud mismatch, or opto polarity. Do not touch pin config
      again until the trace shows what's wrong
- [ ] Tier 0 pin test settles ESP TX/RX by measurement (`HARDWARE.md` §1) — still
      needs the Pico physically attached; the code-side fix below is not this
- [x] `KILNCTL_SAFETY_TX_IO` = 4, `RX_IO` = 5, pull-up moved to GPIO5 (2026-08-16,
      `Kconfig` + `sdkconfig`; `idf.py build` clean)
- [x] `firmware/KilnFW/docs/SAFETY_LINK.md` and `HARDWARE.md` corrected in the same change (2026-08-16)
- [x] `UART_PROTO_MSG_BROADCAST = 0x04` added to `uart_protocol.{c,h}` (2026-08-16,
      send/receive implemented; version not bumped, nothing consumes it yet)
- [x] `hardware/mainBoard/kiln.net` regenerated or deleted — was already gone; confirmed 2026-08-16
- [x] Bench path decided: Debug Probe SWD + probe UART bridge on GP16/GP17; no Pico USB
- [ ] DEBUG header and GP16/GP17 access provided before A1 is soldered down

System-wiring decisions that gate any bench trip test, and are not firmware work:

- [x] K4 → line-contactor interlock topology identified from the schematic
      (2026-08-16): J10 pin 1 = NO, pin 2 = COM, pin 3 = NC — wire the contactor
      coil to pins 1+2. Read from the K4 symbol's drawn rest position, not a
      silkscreen label, so **still wants a continuity check against the
      physical part** before final wiring (`firmware/SaftyFW/docs/HARDWARE.md` §3)
- [x] E-stop circuit confirmed normally-closed by design (2026-08-16) — no jumper
      is fitted anywhere on the `estop` net today, so an as-built board reads
      permanent STOP until a switch or a deliberate jumper is physically added
      (`firmware/SaftyFW/docs/HARDWARE.md` §5)

## M1 — Tooling that makes everything after it cheaper

Owned by [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md). Worth doing early precisely
because it is what turns later hardware questions into a script instead of a
soldering session.

- [x] `KilnFW/pc_tools/` → `tools/PcTools/`, package still `kilnctrl`
- [x] GPIO probe on the ESP, default off, deny-list including GPIO6 (2026-08-16,
      `firmware/KilnFW/App/drivers/gpio_probe.{c,h}` + `tools/PcTools`; not yet
      bench-tested, see `tools/PcTools/TODO.md`)
- [x] GPIO probe on the Pico over SWD, GPIO6 never writable (2026-08-18,
      `tools/PcTools/src/kilnctrl/pico_gpio_probe.py`, no firmware agent
      needed -- RP2040 GPIO is memory-mapped, poked via the existing
      `debug_probe.py`/OpenOCD SWD connection; GPIO6 refused unconditionally
      for SET_MODE/WRITE, READ allowed; not yet bench-tested, see
      `tools/PcTools/TODO.md` §1b)
- [x] Coordinated two-board test script, reaching each side by a path that is
      **not** the link under test (`tools/PcTools/scripts/
      coordinated_gpio_test.py`; not yet run against real hardware)
- [x] OpenOCD wrapper covering both chips: program, reset, halt, read/write memory
      — done 2026-08-17, `kilnctrl.debug_probe` + `openocd_util` (`tools/PcTools/TODO.md`
      "Debug and programming"); flashed and verified `firmware/SaftyFW/build/SaftyFW.elf`
      to the Pico over SWD as the first real use
- [x] Per-processor console windows and log files, plus an interleaved file —
      `tools/PcTools/src/kilnctrl/console_capture.py` (`kilnctrl-console-capture`
      CLI, not a Tk window: reuses the existing ESP `LogClient`/`get_device_log`
      plumbing, adds a raw-serial reader for the Pico's probe UART console),
      writing `esp_*.log` / `safety_*.log` / `interleaved_*.log` under
      `tools/PcTools/logs/console/`. Host-verified: the interleave/merge and
      file-writing logic, with synthetic events (no hardware attached this
      session). Not verified against real ESP or Pico console traffic; the
      wire-protocol LOG-relay path for SAFETY (task 5, device SAFETY) is still
      unimplemented in firmware, so today's SAFETY capture only covers the
      probe-UART bench path — see `tools/PcTools/TODO.md` "Logging and consoles"

**Bench hardware now present, confirmed 2026-08-17:**
- [x] Raspberry Pi Debug Probe connected to A1 (Pico), SWD + UART bridge —
      programmed and verified `SaftyFW.elf` over it this session
      (`firmware/SaftyFW/docs/HARDWARE.md` §7b bench path)
- [x] Main board LCD (ILI9488, `DISPLAY` task) connected, boots, and shows
      "kilnCtl Ready"
- [ ] **HW change needed: backlight control.** LCD backlight currently has no
      GPIO/PWM control path from the board — add one (dim/off on idle,
      touch-driven wake). Owns: schematic net + pin assignment in
      `hardware/mainBoard/`, then `firmware/KilnFW/App/drivers/screen_idle.{c,h}`
      wiring once the pin exists
- [ ] **HW change: relay status LEDs.** Add indicator LED for each relay (K1–K4,
      S9), driven from the GPIO that controls the relay coil. Schematic + layout in
      `hardware/mainBoard/`
- [ ] **HW change: thermocouple board connectors.** Use different connector types
      for thermocouple boards (`ThermocoupleBoard`, `SafyThermocoupleBoard`) to
      distinguish from main board connectors. Schematic + footprints in respective
      board projects

## M2 — `CommonFW`, before either firmware depends on it

Owned by [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md), gating items repeated in
[`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 1.

- [~] `kilnlink` target consumable by both pico-sdk and ESP-IDF — builds
      clean under MSVC and, as of 2026-08-16, under `KilnFW`'s real
      xtensa-gcc build too (`components/kilnlink/CMakeLists.txt` wrapper
      around `CommonFW`, `idf_component_register`-based). **Now also
      verified under pico-sdk/arm-none-eabi-gcc**: `firmware/SaftyFW`
      has a real CMake project (`firmware/SaftyFW/CMakeLists.txt:47`,
      `add_subdirectory(../CommonFW kilnlink)`) and links `kilnlink` into
      all three of its executables (`CMakeLists.txt:130`/`215`,
      `SaftyFW`/`SaftyFW_slotA`/`SaftyFW_slotB`); `src/tasks/link_task.c`
      actually calls into it (`#include "kilnlink/kilnlink_*.h"` for
      `clear_trip`/`diag`/`frame`/`power`/`trip`/`version`, plus
      `kilnlink_frame_encode_raw`/`_decode`, `kilnlink_stuff`/`_unstuff`,
      and the `diag`/`trip`/`power`/`clear_trip` codecs — see e.g.
      `link_task.c:249,255,423-438,452-456,480-508,637-641,673-680`),
      most recently building clean at commit `62ce6bf`. `KilnFW`'s side
      is also no longer "linked-but-unused": `App/drivers/espInterfaces/
      uart_protocol.c` now calls `kilnlink_crc16_ccitt_false`/
      `kilnlink_stuff` (lines 37, 43) instead of its own copies (see the
      "`KilnFW` delegating framing and CRC" bullet below). **What's still
      genuinely open:** the ESP-IDF `components/kilnlink/CMakeLists.txt`
      wrapper compiles only a subset of codecs (`kilnlink_crc.c`,
      `kilnlink_frame.c`, `kilnlink_context.c` — not the
      `announce`/`diag`/`trip`/`power`/`clear_trip`/`ceiling`/
      `get_fw_version`/`set_clock` codecs `SaftyFW` already links), and
      `KilnFW`'s `safety_link.c` still hand-rolls the `ANNOUNCE_VERSION`
      frame inline rather than calling `kilnlink_announce` (tracked in the
      "Codecs pure and bounds-checked" bullet below) — so this stays `[~]`
      until the ESP-IDF wrapper's codec set and `KilnFW`'s remaining
      hand-rolled parsing are brought up to parity with `SaftyFW`'s usage
- [x] `KILNLINK_PROTOCOL_VERSION` the single source (2026-08-16); `KilnFW`'s
      `UART_PROTOCOL_VERSION` (`App/drivers/uart_task_ids.h`) is now
      `((uint16_t)KILNLINK_PROTOCOL_VERSION)`, a real alias rather than a
      second number, and both `KILNLINK_PROTOCOL_VERSION` and
      `KILNLINK_MIN_COMPATIBLE` are 5 in `kilnlink_version.h`
- [x] Codecs pure and bounds-checked; host tests and `test/vectors/` — **every
      `docs/LINK_PROTOCOL.md` sec 4/6 command that has a concrete byte layout
      now has a codec** (2026-08-19), on top of the framing layer
      (`kilnlink_frame`/`kilnlink_crc`). ESP→Pico (sec 4): `kilnlink_context`
      (`SAFETY_CMD_PUSH_CONTEXT` 0x07, including `relay_recent_mask`),
      `kilnlink_announce` (`SAFETY_CMD_ANNOUNCE_VERSION` 0x0F, the
      compatibility-floor layout shared with Frame C, 2026-08-18),
      `kilnlink_ceiling` (`SAFETY_CMD_SET_FIRING_CEILING` 0x09),
      `kilnlink_clear_trip` (`SAFETY_CMD_CLEAR_TRIP` 0x0A),
      `kilnlink_get_fw_version` (`SAFETY_CMD_GET_FW_VERSION` 0x0B, one byte,
      no fields), `kilnlink_set_clock` (`SAFETY_CMD_SET_CLOCK` 0x0C,
      optional) -- the last four added 2026-08-19. Pico→ESP (sec 6):
      `kilnlink_status` (Frame A, `SAFETY_CMD_GET_STATUS` 0x01, the existing
      23-byte layout), `kilnlink_diag` (Frame B, `SAFETY_CMD_DIAG` 0x08),
      `kilnlink_trip` (Frame D, `SAFETY_CMD_TRIP_EVENT` 0x0D),
      `kilnlink_power` (Frame E, `SAFETY_CMD_POWER` 0x0E) -- all four already
      done as of 2026-08-18. Every one of the ten payload codecs has its own
      `test_<name>.c` and byte-exact `test/vectors/<name>_vectors.json`; all
      13 host test binaries (framing + fuzz + uart_protocol_delegate + the
      ten payload codecs) pass under MSVC+CMake+Ninja as of 2026-08-19.
      **Note:** `KilnFW`'s `safety_link.c` already builds/parses the
      `ANNOUNCE_VERSION`-shaped frame inline (see `KilnFW/TODO.md` 9.0 below)
      and was not migrated onto this codec -- the two implementations are
      independently correct (same field layout, both host- and
      xtensa-gcc-verified) but not yet unified; that consolidation is a
      follow-on, not a functional gap. **Not wired into either firmware's
      real send/receive dispatch** (`uart_protocol.c` on `KilnFW`,
      `link_task.c` on `SaftyFW`) -- that migration is a separate item below,
      matching how `kilnlink_diag`/`kilnlink_trip` already landed codec-only
      before their own wiring passes
- [x] `pc_tools` consuming the same vectors as the third implementation --
      **now every payload-codec manifest, not just the framing layer**
      (2026-08-19). `selfcheck.py`'s existing `commonfw_vector_checks()`
      still covers `frame_vectors.json` (2026-08-16); a new
      `commonfw_payload_vector_checks()` was added alongside it, driving a
      new pure-Python `tools/PcTools/src/kilnctrl/kilnlink_codec.py` (encode-
      only -- nothing in `pc_tools` decodes these frames yet, see that
      module's docstring) against `context_vectors.json`,
      `status_vectors.json`, `announce_vectors.json`, `diag_vectors.json`,
      `trip_vectors.json`, `power_vectors.json`, and the four new
      `ceiling`/`clear_trip`/`get_fw_version`/`set_clock` manifests -- ten
      files, proving Python produces byte-identical output to the C encoders
      for all of them. Found and fixed a real pre-existing bug while wiring
      this up: `status_vectors.json`'s `too_short` hostile vector had a
      Python slice expression (`"..."[:-2]`) left inline in the JSON literal
      instead of the already-sliced string, which is invalid JSON and made
      `json.loads()` on that file raise -- fixed to the sliced value
- [x] `KilnFW` delegating framing and CRC, proven byte-identical **before** the
      old code is deleted (2026-08-18). `App/drivers/espInterfaces/uart_protocol.c`
      now calls `kilnlink_crc16_ccitt_false`/`kilnlink_stuff` instead of its own
      copies; byte-identical output vs. the old local implementation proven by
      `firmware/CommonFW/test/test_uart_protocol_delegate.c` (known vectors +
      528 fuzz cases, incl. delimiter/escape-saturated buffers), and
      `idf.py -C firmware/KilnFW build` builds clean under xtensa-gcc.
      `firmware/UnitTestFw/UnitTest/App/drivers/espInterfaces/uart_protocol.c`
      is a stale fork (not a literal mirror) and was **not** migrated in this
      pass -- stays on `check_no_duplicate_crc.ps1`'s allowlist
- [x] CI grep: no CRC or byte-stuffing implementation outside `CommonFW` --
      `tools/check_no_duplicate_crc.ps1` (2026-08-18), matching
      `firmware/SaftyFW/tools/check_isolation.ps1`'s conventions. Greps for
      the kilnlink CRC-16/CCITT-FALSE polynomial (`0x1021`) outside
      `firmware/CommonFW`. Not registered in any CI pipeline (none exists
      yet for this check to join). Scoped to catch *new* untracked
      duplicates only: the 4 known pre-migration copies (`KilnFW`'s
      `uart_protocol.c` and its `UnitTestFw` mirror, `pc_tools`'
      `protocol.py` and its mirror) are an explicit, documented allowlist
      tied to the two items above -- deleting an allowlist entry is part of
      finishing each migration, so it can't go stale silently

## M3 — Safety processor to first trustworthy reading

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 2–4. Independent of the
link, so it can run in parallel with M1 and M2 once M0 is out of the way.

- [x] FreeRTOS SMP skeleton, tasks at the planned priorities and core affinities —
      done 2026-08-16, build-verified clean under the real arm-none-eabi-gcc/pico-sdk
      toolchain (task bodies are still TODO shells; Phases 3/6/7 fill them in)
- [x] `main()` drives GPIO6 low as its first statement — done 2026-08-16
- [x] Watchdog, fed only when every task checks in, trip reason latched in
      scratch registers — done 2026-08-16; the latch call itself is unwired
      until a guard exists to trigger it (Phase 4/5)
- [ ] **MAX31856 thermocouple ICs not yet connected on the bench (2026-08-18).**
      Blocks all real-reading work below it — SPI bus verify, per-channel type
      config, and fault-read all need the parts populated/wired first, not just
      the schematic net
- [ ] MAX31856 on J7, with per-thermocouple type configuration
- [x] Guards implemented and **host-tested against synthetic inputs**, no relay
      yet — 2026-08-18, `firmware/SaftyFW/src/safety_guards.c`. 12 of the 13
      guards in `SAFETY_MODEL.md` section 4 are built as pure functions and
      host-tested (320/320 checks, MSVC, `/W4 /WX`): S1/S5/S7/S11/S12 landed
      2026-08-16; this pass adds S2/S3/S4/S6/S9/S10/S13, fed by synthetic
      context/current-presence/link-liveness facts rather than real
      `link_task`/`current_task` producers (neither exists yet — Phase 6/7).
      S8 (rate-of-rise) is the one guard intentionally not built: it ships
      disabled per `SAFETY_MODEL.md` until a real kiln's ramp rate is
      measured on the bench, and building a placeholder threshold now is the
      mistake that section explicitly refuses to make. Build-verified under
      the real arm-none-eabi-gcc/pico-sdk toolchain too (`safety_guards.c`/
      `safety_core.c` compile clean, zero warnings under
      `-Wall -Wextra -Werror`) — the full `SaftyFW.elf` link currently fails
      on an unrelated, pre-existing, uncommitted bug in
      `firmware/CommonFW/src/kilnlink_status.c` (untracked in git, not
      touched by this pass; see `firmware/SaftyFW/TODO.md` Phase 4 for
      detail). No relay/GPIO6 wiring touched — guards remain evaluated but
      not commanding hardware, matching this milestone's scope
- [x] CI grep: `safety_core.c` never includes the link header — done 2026-08-16,
      `firmware/SaftyFW/tools/check_isolation.ps1`

## M4 — Relay authority

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 5. First milestone that can
physically stop a kiln, and the first that can nuisance-trip one.

- [x] Relay owner task is the only writer of GPIO6 (already true, ROADMAP just
      hadn't been ticked — confirmed 2026-08-19 by grepping the whole tree:
      `firmware/SaftyFW/src/tasks/relay_owner.c`'s three `gpio_put` call
      sites and `main.c`'s one-time pre-scheduler safe-state drive are the
      only two writers anywhere; `safety_core.c` only ever calls
      `relay_owner_command_trip()`, never touches the pin)
- [~] Trip latches; clearing requires an explicit command (2026-08-19). The
      latch itself already worked (`relay_owner_command_trip()` de-energizes
      + latches TRIPPED atomically, refuses `energize` while tripped). What
      was missing: `relay_owner_clear_trip()` existed but cleared
      **unconditionally** and had zero callers anywhere — "refused while the
      tripping condition is still true" (`SAFETY_MODEL.md` sec 6) was
      unenforced. Added `safety_guards_try_clear()` (pure function,
      `safety_guards.c`/`.h`): resets guard state, immediately re-evaluates
      one tick against fresh input, refuses (leaves state re-tripped) if
      that retick re-trips. **Honest scope limit, stated in its own doc
      comment and exercised by a host test**: this reliably catches
      unwindowed guards (S7 estop, S6a mainFault, S6b's hard backstop) still
      active at clear time, but a graduated/windowed guard (S1/S2/S3/S5/S9/
      S11/S12/S13) has its elapsed-time accumulator reset by the same clear,
      so a single retick will not necessarily catch a still-present
      condition — it re-trips on its own normal timescale instead, which is
      not a safety hole, just not an *instant* refusal for those guards.
      `safety_core.c` gained `safety_core_request_clear_trip()` wiring this
      into relay_owner. Host-tested: 391/391 checks pass
      (`test/build_host_tests.ps1`), including new cases for the estop
      refuse/succeed paths and the documented S1 scope-limit behavior. Real
      `cmake --build` for the RP2040 target (`SaftyFW`/`_slotA`/`_slotB`)
      succeeds clean. **2026-08-19, later pass — the caller now exists**:
      `link_task.c` decodes inbound `SAFETY_CMD_CLEAR_TRIP` (0x0A) via
      CommonFW's `kilnlink_clear_trip_decode()` (the codec's decode side
      already existed, host-tested, no changes needed there), checks
      "nothing currently tripped" and the wire `trip_mask` against the
      currently-latched one (`link_frame_trip_mask_for_reason()`, new pure
      helper in `link_frame.c`/`.h`, factored out of Frame B/DIAG's own
      trip_mask synthesis so both use the identical single-bit-per-reason
      mapping) before calling `safety_core_request_clear_trip()`, and logs
      the accepted/refused outcome via `log_task_log()`. Never ACKs on the
      wire — matches `LINK_PROTOCOL.md`'s silence on a CLEAR_TRIP reply; the
      ESP observes the outcome via the tripped bit in the next status/DIAG
      frame. Host-tested: 427/427 checks pass (new
      `test_trip_mask_for_reason()` in `test_link_frame.c`, 5 cases). Real
      `cmake --build` for `SaftyFW`/`_slotA`/`_slotB` succeeds clean, zero
      warnings. `tools/check_isolation.ps1` still passes (link_task.c still
      never references GPIO6/relay; it only calls INTO safety_core, same
      direction as the existing status/diag/trip-event pulls). **Honestly
      not hardware-verified end-to-end**: the physical pi↔ESP UART link is
      currently broken on the bench, so the CLEAR_TRIP frame itself has
      never been exercised over real wire — this is build- and
      host-test-verified only. This closes the milestone's remaining gap;
      see `firmware/SaftyFW/TODO.md` Phase 7 for the item this checks off.
- [ ] S9 trip-ineffective escalation proven with a deliberately welded contactor
- [ ] Every guard exercised per [`GUARD_TEST_MATRIX.md`](firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md)

## M5 — The link carrying real traffic

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 6–8, contract in
[`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md).

- [~] Current sensing: load-active detection and a power estimate — **not** an
      over/under-current trip. **2026-08-18**: the sampling/conversion driver
      (`current_sense.c`/`.h`, `current_task.c`/`.h`) was already built and
      build-verified in an earlier pass. This pass wired its output onto the
      wire: `link_task.c` gained `link_task_send_power()`, sending a real
      `SAFETY_CMD_POWER` (Frame E) every 2 s via `kilnlink_power_encode()`,
      and `current_sense_power_t` gained `mains_voltage_v`/`calibrated`/
      `any_clipped`/`p_total_w`/`energy_wh` so the frame can be built without
      the module leaking calibration-struct visibility across the file
      boundary. `KilnFW` already decodes Frame E (`firmware/KilnFW/TODO.md`
      10.10/10.11), so the GUI power readout is now fed end to end in
      firmware. **Still open, and gating this from becoming `[x]`:** guards
      S3/S4 are deliberately NOT wired to `any_current_present` yet — SaftyFW's
      `docs/CURRENT_SENSE.md` section 5 requires the per-channel CT-to-jack
      commissioning check ("command one relay, confirm exactly one channel
      responds") to pass on real hardware first, and no RP2040/CT hardware is
      attached to any build machine in this environment. Host tests
      (`test/test_kilnlink_power.c`) and a real arm-none-eabi-gcc/pico-sdk
      build both pass; nothing here has run against actual current-sense
      hardware.
- [x] ESP → Pico context frames, including `relay_recent_mask`. **2026-08-18**:
      `KilnFW`'s `safety_link.c` now builds a real `SAFETY_CMD_PUSH_CONTEXT`
      (0x07) from live board state -- `kiln_io_get_relay_shadow()` for
      `relay_now_mask`, a poll-task-owned rolling window
      (`SAFETY_LINK_CONTEXT_RECENT_WINDOW_S` = 180 s) for `relay_recent_mask`,
      raw per-channel `MAX31856_read_all()` readings and configured `tc_type`
      for the per-zone blocks (`sample_counter` incremented only when
      `MAX31856Reading::stale` is false, i.e. a fresh conversion was actually
      consumed), and `profile_executor_get_status()` for
      setpoint/active/relay-on/guard-tripped/PROFILE_RUNNING -- encoded
      through `kilnlink_context_encode()` (the `kilnlink_context.c` codec
      landed in the pass above is now an actual build dependency of
      `KilnFW`, not just linked-but-unused) and sent as a broadcast every poll
      period, same cadence and same call site
      (`uart_protocol_send_broadcast`) `ANNOUNCE_VERSION` already uses. Wired
      via a new `safety_link_set_context_sources()` setter called from
      `app_main` alongside the other `dashboard_http_start()`-style hardware
      wiring. Builds clean (`idf.py build`, xtensa-gcc). **Not verified
      end-to-end**: no Pico exists on this bench (ROADMAP.md M0's
      bench-confirmed dead link) to confirm the frame decodes correctly on
      the receiving end -- this closes "the ESP builds and sends the frame",
      not "a real Pico received and parsed it correctly".
- [~] Pico → ESP telemetry: status, diagnostics, firmware version, trip events, power.
      **2026-08-18**: status, firmware version, and power (`SAFETY_CMD_POWER`,
      commit `b1fdf46`) are now built and sent by `SaftyFW`'s `link_task.c`
      and decoded by `KilnFW`'s `safety_link.c`. **Also 2026-08-18**: codecs
      for `SAFETY_CMD_DIAG` (Frame B, `0x08`, 26 bytes) and
      `SAFETY_CMD_TRIP_EVENT` (Frame D, `0x0D`, 29 bytes) landed in
      `firmware/CommonFW` — `kilnlink_diag.{c,h}`, `kilnlink_trip.{c,h}`,
      host-tested (9/9 CommonFW test suites pass, including the two new
      ones). **2026-08-19: `KilnFW`'s decode/dispatch half now exists too.**
      `safety_link.c` gained `safety_apply_diag()` and
      `safety_apply_trip_event()`, hand-parsing the same 26/29-byte layouts
      the CommonFW codecs define (this ESP-IDF component still compiles only
      `kilnlink_crc.c`/`kilnlink_frame.c`/`kilnlink_context.c` — see
      `firmware/KilnFW/components/kilnlink/CMakeLists.txt` — so DIAG/
      TRIP_EVENT are hand-parsed the same way GET_STATUS and POWER already
      are, not by linking the new codecs directly), and
      `safety_drain_inbox()` now dispatches `SAFETY_CMD_DIAG` (`0x08`) and
      `SAFETY_CMD_TRIP_EVENT` (`0x0D`, both newly defined in
      `uart_task_ids.h`) to them. `safety_link_status_t` gained twelve
      `diag_*` fields (trip/warn masks, boot reason, context-frame health
      counters, armed/warn/tripped state) and nine `trip_*` fields for the
      most recent trip event, including a dedicated `trip_event_age_ms`
      computed the same way the link's own `age_ms` is. `TRIP_EVENT`
      dedups on `trip_seq` for *logging* purposes only (a resend burst logs
      once, not three times) but every copy received still refreshes the
      cache and its age, per this file's own idempotency note in sec 6.
      `GET /api/status` (`dashboard_http.c`) now surfaces both frames'
      fields (null until each has actually arrived, same convention as
      `safety_temp_c`/`power_w`). The LCD gained exactly one new row —
      `ui_page_safety.c`'s "Last trip" line — chosen because sec 7 calls
      trip reason "the answer to 'why did the kiln stop'" and it was the
      only field that fit the page's documented ~264px no-scroll budget;
      DIAG's warn/trip masks and context-health counters are cached and
      HTTP-exposed but deliberately left off the LCD (see `TODO.md`'s entry
      for the reasoning and the diagnostics-page follow-up this leaves).
      `idf.py -C firmware/KilnFW build` (via ninja, incremental after a full
      configure) is clean, no new warnings, under `-Werror`.
      **2026-08-19, same day: `SaftyFW`'s send half now exists too, closing
      the gap this bullet used to describe as "neither frame has a send
      path."** Frame B (`SAFETY_CMD_DIAG`): `link_task.c`'s
      `link_task_send_diag()` was already sending it every 2s since
      2026-08-17 via a hand-rolled local packer
      (`link_frame_pack_diag()`) — this pass migrated that send path onto
      the shared `kilnlink_diag_encode()` codec instead (no field or timing
      change) and deleted the now-duplicate local packer from
      `link_frame.{c,h}`, rather than leaving two independent
      implementations of the identical 26-byte layout in the tree. Frame D
      (`SAFETY_CMD_TRIP_EVENT`) is genuinely new: the cross-task
      notification path this bullet flagged as "not investigated" is now
      built, respecting the isolation direction `check_isolation.ps1`
      enforces the same way `safety_core_get_output_status()`/
      `_get_diag_status()` already did — a new
      `safety_core_get_trip_event()` getter (`safety_core.{c,h}`) captures
      `trip_seq`/`trip_reason`/`uptime_ms`/`safety_tc_c`/`deciding_threshold`
      the same tick a trip latches (right after `relay_owner_command_trip()`),
      lock-free single-writer/plain-read, same pattern as the existing
      getters. `link_task.c` polls it every ~100ms poll cycle
      (`link_task_poll_trip_event()`) and, on a new `trip_seq`, fires a
      4-copy/250ms-apart burst via `kilnlink_trip_encode()`, mirroring the
      exact burst shape `KilnFW`'s own
      `safety_link_send_announce_version_burst()` uses for
      `ANNOUNCE_VERSION` (a scheduled unconditional repeat, not the
      retransmission-on-failure LINK_PROTOCOL.md sec 2 rule 2 forbids).
      `deciding_threshold` is real for 6 of 12 implemented guards (a new
      pure `safety_guards_deciding_threshold_c()`, host-tested,
      `test_deciding_threshold()`) and honestly `NaN` for the rest, since
      those guards (S5/S6a/S6b/S7/S9) have no single meaningful magnitude to
      report. `current_a[]`/`relay_recent_mask`/`context_age_100ms` are
      pulled by `link_task` from `current_task`/its own context snapshot at
      trip-*detection* time (≤~1 poll period, ~100ms, after the real trip)
      rather than the guard-tick instant, since `safety_core`'s guard input
      carries no raw current or ESP-context data to capture more precisely
      — see `firmware/SaftyFW/TODO.md` Phase 8 for the full breakdown of
      which fields are exact vs. approximated. Host-tested where the logic
      is pure (`test/build_host_tests.ps1`, 422/422, 10 new checks) and a
      real `cmake --build` (arm-none-eabi-gcc 14.2.1/pico-sdk 2.1.1/Ninja)
      succeeds clean, zero warnings under `-Wall -Wextra -Werror`, for
      `SaftyFW`/`_slotA`/`_slotB`. `tools/check_isolation.ps1` re-run clean.
      **Still open**: sec 9 item 0.9's other half — mirroring DIAG/TRIP_EVENT
      onto the PC-link `SAFETY` task in `uart_bridge.c` so `pc_tools`/MCP see
      the same data without Wi-Fi — is not done (not touched by either
      pass). **Not hardware-verified, and cannot be from this environment**:
      no ESP32-S3/Pico is attached, and M0 already established the isolated
      link doesn't pass a byte end-to-end on real hardware, so neither
      `SaftyFW`'s new send path nor `KilnFW`'s `safety_apply_diag()`/
      `safety_apply_trip_event()` have ever exchanged a frame that actually
      crossed the wire — only host-tested codec vectors on both sides and
      clean builds under each real toolchain. Nothing here has crossed a
      real link (M0's bench-confirmed dead link) regardless.
- [x] Pico never blocks on the link — all five no-wait rules honoured.
      **Audited 2026-08-18, no violations found** (`firmware/SaftyFW/src/
      tasks/link_task.c`, `uart_owner.c`): (1) never ACKs/expects one —
      `link_task_handle_raw_frame()` (`link_task.c:463-465`) discards
      anything but `KILNLINK_MSG_BROADCAST`, no ACK path exists; (2) never
      retransmits — no retry logic anywhere in `link_task.c`, a failed
      `uart_owner_send()` is just discarded; (3) never blocks on TX —
      `uart_owner_send()` (`uart_owner.c:110-142`) is a bounded
      `save_and_disable_interrupts()` critical section that drops the whole
      frame and counts it (`s_tx_dropped`) when the ring lacks room, never
      waits on the ISR; (4) `safety_core.c` never calls into the link —
      confirmed both by inspection (no call to any `link_task_*` function in
      `safety_core.c`; `context_snapshot_t` consumption is still a TODO
      comment at `safety_core.c:114-116`) and by `tools/check_isolation.ps1`
      (re-run clean this pass: no link/uart `#include` in `safety_core.{c,h}`,
      no GPIO6/relay reference in `link_task.{c,h}`); (5) priority/affinity —
      `task_priorities.h:49` puts `link_task` at priority 2 (second-lowest,
      above only `log_task`/`update_task` at 1) and `SAFTYFW_CORE_LINK_PATH`
      (core 0), disjoint from `SAFTYFW_CORE_TRIP_PATH` (core 1) that
      `relay_owner`/`safety_core`/guard tasks run on — matches
      `docs/ARCHITECTURE.md` §4 exactly. `PUSH_CONTEXT` (0x07) receive-side
      already exists (`link_task_handle_push_context()`, wired since an
      earlier pass) and was reaudited here rather than found missing: its
      only lock wait (`s_context_lock`, `link_task.c:370`) is a bounded 50 ms
      `xSemaphoreTake`, and downstream `update_task` dispatch uses a
      zero-timeout `xQueueSend` (`update_task.c:880`). No code changes were
      needed. Full `SaftyFW` build verified clean (`cmake --build .`,
      arm-none-eabi-gcc/pico-sdk, zero warnings under `-Wall -Wextra -Werror`)
      and all 320 host tests pass (`test/build_host_tests.ps1`).
- [x] Mutual version handshake: `ANNOUNCE_VERSION` both ways, `min_compatible`
      checked in both directions, compatibility floor reserved at ids `0x00`–`0x0F`.
      `KilnFW`'s `safety_link.c` sends `ANNOUNCE_VERSION` unprompted at boot
      (a burst, loss-tolerant) and on every Pico `boot_id` change, and folds a
      known-incompatible peer into `SAFETY_FAULT_SRC_SAFETY_LINK` exactly like
      a dead link (`safety_update_health()`); `SaftyFW`'s `link_task.c` parses
      inbound `ANNOUNCE_VERSION`/`GET_FW_VERSION`, all inside the link task,
      never touching `safety_core.c` (`check_isolation.ps1` clean). Shared
      `kilnlink_announce` codec added this pass (see M2); `KilnFW`/`SaftyFW`
      still each hand-roll this frame rather than calling into it (M2 note)
- [x] TX ring reserves capacity for telemetry; log frames dropped above the
      watermark and the drops counted (`log_task.c`'s 50% `LOG_TX_RESERVE_FRACTION`,
      landed 2026-08-17; the pure admit/drop decision was pulled out into
      host-testable `firmware/SaftyFW/src/tasks/tx_watermark.c` this pass)
- [x] Borrowed-thermocouple staleness split correctly across S11 / S13 / S6
      (audit-confirmed 2026-08-18, no code fix needed: `safety_guards.c`'s S6b
      block reads only `in->link_up`, S13's block reads only `in->context_valid`
      / `in->sample_counter_advancing`, and S11 reads neither -- three disjoint
      input facts, no cross-reads, matching `SAFETY_MODEL.md`'s S13 table
      verbatim; added `test_s6_s13_split()` in `test/test_safety_guards.c` to
      exercise the three split scenarios end-to-end, 378/378 host checks pass)

## M6 — Throw the liveness switch

**2026-08-18: LCD pages rewritten to never require scrolling** (explicit user
requirement). `ui_page_home.c` was stacking AP-QR + zones + the safety card +
profile card + chart + nav onto one scrollable column — the code's own prior
comment admitted "scrolling is the safe fallback rather than guessing
pixel-perfect fixed heights." Rewritten to a hard budget against this
codebase's actual runtime canvas, 480x320 landscape (the ILI9488 panel is
natively 320x480, but Kconfig's default startup rotation is landscape, and
`ui_theme.h`'s own spacing constants were already budgeted against 480x320
landscape) =~ 264px of real content height after the status bar and outer
padding: home page trimmed to zones + a compact run-state summary +
Start/Stop/Menu; the AP-QR card and profile picker were deleted outright
(duplicate of `ui_page_network.c`'s own QR; picker replaced by the existing
fallback-profile logic); the safety-processor card moved to a new
`ui_page_safety.c`, the chart to a new `ui_page_history.c`, both reachable
via `ui_page_config.c`'s nav hub, which itself grew from 4 to 7
destinations and switched to a compact 2-column grid to fit the same
budget. `ui_page_board_health.c`/`ui_page_temperature.c` compacted
(smaller padding, `ui_page_temperature.c`'s relay buttons 72px→36px).
`LV_OBJ_FLAG_SCROLLABLE` explicitly cleared on every page's outer
containers so a future overflow clips visibly instead of silently becoming
scrollable again. Real `idf.py -C firmware/KilnFW build` verified clean.
**One known gap, not silently claimed solved:** `ui_page_temperature.c`'s
fit depends on how many relays are configured per zone at runtime, which
this pass couldn't bound at compile time. `ui_page_network.c`'s overflow
was fixed in a 2026-08-18 follow-up: Scan/Saved made mutually exclusive (one
70px list visible at a time via a toggle) and, once connected, the whole
list block hides behind a "Change network" button in favor of the QR row —
worst case now ~268px against the ~264px budget, computed not measured
(see `firmware/KilnFW/TODO.md`'s "ui_page_network.c follow-up pass" note).
Explicit user clarification, same date: page-level touch-drag scrolling
must never happen, but a fixed-height list widget (like the scan/saved
lists) scrolling internally via normal touch-drag is fine and is the
sanctioned way to show a variable-length list — that's not new pagination,
it's how `lv_list` already worked. **Not verified on real hardware**: no
ILI9488 panel attached in this environment, so pixel-exact fit is still
unconfirmed for every page — this closes "budgeted and scroll-disabled in
code, with a real computed margin," not "confirmed to fit on the physical
screen."

The point at which the two processors become one system. Deliberately separate,
because it changes what a bare main board will do.

- [x] `SAFETY_FAULT_SRC_SAFETY_LINK` redefined as "no telemetry within 1.5 s"
      (2026-08-18). It already worked out to 1.5 s at the default 500 ms
      poll period (`SAFETY_LINK_UP_PERIODS=3`), but that was period-relative,
      not the fixed ceiling LINK_PROTOCOL.md sec 8 specifies. Added
      `SAFETY_LINK_STALE_MS` (1500) and a pure `safety_link_is_stale(age_ms,
      threshold_ms)` in `safety_link.h`, OR'd into `safety_update_health()`'s
      `up` computation in `safety_link.c` so a reconfigured poll period
      (`SET_POLL_PERIOD`) can only make the fault fire *sooner*, never later.
      No hardware attached in this environment, so the real 1.5 s firing
      point is unverified against a live Pico — this closes the code gap,
      not the hardware-timing-verified gap.
- [x] 30 s silence aborts a firing (2026-08-18). New
      `SAFETY_LINK_FIRING_ABORT_SILENCE_MS` (30000) in `safety_link.h`.
      `profile_executor.c`'s existing guard-9 watchdog task (`watchdog_task_
      entry`, already polling every 2 s for a stuck control task) now also
      reads `safety_link_get_status()`'s `age_ms` each tick and, if silence
      exceeds 30 s while a firing is `RUNNING`/`PAUSED`, faults the run
      (`PROFILE_EXEC_FAULTED`, `fault_reason` set) and forces relays off via
      `kiln_io_all_relays_off()` — same fail-toward-off call guard 9 uses for
      a stuck control task — then keeps retrying that relay-off write every
      tick the link stays silent, per sec 8's "dropped and retried until the
      write succeeds". Reuses the existing fault/abort machinery rather than
      a parallel path; the separate 1.5 s check above already asserts
      `SAFETY_FAULT_SRC_SAFETY_LINK` well before 30 s, so no second assertion
      is needed here. Build clean (`idf.py -C firmware/KilnFW build`). Not
      hardware-timing-verified — no Pico/link attached here.
- [x] Boot-time version request with retry, surfaced in the GUI (2026-08-19).
      Distinct from the existing `ANNOUNCE_VERSION` broadcast (that's the ESP
      telling the Pico who it is, unprompted) — this is the ESP explicitly
      asking the Pico who *it* is. A Pico already running before this ESP
      boots has no reason to volunteer `FW_VERSION` on its own
      (`LINK_PROTOCOL.md` sec 4: unsolicited only "at Pico boot and on
      request"), so without an explicit request its version would never be
      learned this boot cycle. `safety_link.c`'s `safety_poll_task()` now
      sends the `SAFETY_CMD_FW_VERSION` (0x0B) request every poll period
      (~500 ms) for as long as `peer_version_known` stays false, reusing
      `safety_exchange()`'s existing ACK/drain machinery
      (`expect_status=false`, same call shape `REQUEST_ENABLE`-style calls
      already use) rather than adding a second retry/backoff scheme — the
      poll loop's own cadence *is* the retry. GUI surfacing was already done
      in M8 (`ui_page_safety.c`'s "Safety Processor" card, "---" until
      `link_version_known`); this closes the missing request itself. Build
      clean (`idf.py -C firmware/KilnFW build`, xtensa-gcc), flashed to the
      bench ESP32-S3. **Not hardware-verified**: no Pico exists on this
      bench (M0's bench-confirmed dead link) to confirm a real `FW_VERSION`
      reply actually lands and stops the retry — this closes the code gap,
      not the live-Pico-verified gap.
- [x] Bench escape hatch documented: `safety_link_fault_on_link_loss(link, false)`
      (already implemented — `firmware/KilnFW/App/drivers/safety_link.{h,c}`,
      default `true`/fail-safe, header comment explains the bring-up
      use case; ROADMAP just hadn't been ticked)
- [~] GUI shows safety temperature, enclosure temperature and power
      (2026-08-18). `GET /api/status` (`dashboard_http.c`) gained
      `safety_temp_c`/`enclosure_temp_c`/`power_w`, null when invalid — read
      straight from `safety_link_get_status()`'s cache (`tc_temp_c`/
      `cj_temp_c`, `LINK_PROTOCOL.md` sec 6 Frame A), not a second frame
      parse. The LCD home page (`ui_page_home.c`) shows the same three
      fields in a new "Safety Processor" card, via the same
      `dashboard_get_status()` call the page already makes (TODO.md 10.1a).
      **2026-08-18 update: power now has a real codec and dispatch path**,
      closing the gap this item originally called out. `SAFETY_CMD_POWER`
      (Frame E, 0x0E) is decoded by a new `kilnlink_power.{c,h}` in
      `firmware/CommonFW` (host-tested, mirrors `kilnlink_status.c`'s
      conventions) and hand-parsed the same way in `safety_link.c`'s new
      `safety_apply_power()`, dispatched from `safety_drain_inbox()` and
      cached in `safety_link_status_t`. `dashboard_get_status()` now reads
      `power_w`/`power_valid` from that cache instead of hard-coding
      `null`/`NaN`. Build clean (`idf.py -C firmware/KilnFW build`); host
      `ctest` in `firmware/CommonFW/build` 7/7 green, including the new
      `test_power`. See `KilnFW/TODO.md` 10.10 for detail.
      **Still not hardware-verified, and still reads `null`/"---" on any
      real board today**: no Pico is attached in this environment, the link
      itself is bench-confirmed dead (M0), and `SaftyFW` does not build or
      send Frame E yet (M5's "Pico → ESP telemetry ... power" bullet, still
      open on the Pico side) — so `power_ever_received` never goes true
      outside the host tests. That is the designed-for state until M0/M5
      land on the Pico side too, not a defect in this pass.

## M7 — Repo reorganisation · *done 2026-08-16, three items open*

Owned by [`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md). Done **before** `SaftyFW`
phase 1, which is the timing blocker B6 preferred: the paths in the new
documents are correct from the start rather than being rewritten later.

- [x] Tree split into `hardware/`, `firmware/`, `tools/`, `docs/` — 380 files,
      all byte-identical renames
- [x] B1 library tables, B2 TFT35-SPI submodule, B3 `CLAUDE.md`, B5 clean tree
- [x] Everything else the move broke: `.mcp.json`, both `.vscode`
      `compile-commands-dir` settings, two `PcTools` modules that derived paths
      from `__file__`, 3D model paths in 17 footprints, twelve broken doc links
- [x] B4 — the stale `mainBoard/kiln.net` deleted
- [ ] `mykicadMcp/` and `pdfMcp/` moved under `tools/` — blocked at the time by
      running MCP server processes holding the directories open
- [x] **Three of four KiCad projects opened, no missing libraries (2026-08-16).**
      Includes `mainBoard`, which is the one B1 applied to, so the relative
      library path is confirmed working. `UnitTestFixture` still unopened
- [ ] Fresh `git clone` into a scratch directory opens `mainBoard` — the only
      test that catches the absolute-path breakage for someone who is not this
      user on this machine

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

- [ ] **Measure the isolated link's real error rate at 115200 first.** Nobody has
      characterised the optocouplers; retry cost is 200 ms × up to 10
- [x] **Establish the real flash size, then switch to it (2026-08-17).** Confirmed
      against the LonelyBinary product page for the board in hand
      (variant 43784065712285): it is an **N16R8 — 16 MB flash, 8 MB PSRAM**,
      resolving the three-way disagreement (buy lists said N8R8/8 MB, the 3D
      model said N8R2/8 MB). `firmware/KilnFW/sdkconfig` now declares
      `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` / `CONFIG_ESPTOOLPY_FLASHSIZE="16MB"`
      (was wrongly `_2MB`/`"2MB"`). **Scope of this pass: the sdkconfig
      flash-size declaration only.** `partitions.csv` is UNCHANGED — it still
      maps only the first 2 MB (single `factory` app slot, no OTA), with a
      header comment now noting the remaining ~14 MB is unmapped headroom for
      the item below. The real
      image is 1167 KB, so two copies do not fit in the 1500 KB app region. The
      new two-app-slot OTA partition table this milestone describes is still a
      separate, unstarted item — see the next checklist entries. The bootloader
      must still be reflashed for the corrected header flash-size to take
      effect on the physical board; this pass is config/doc-only and has not
      been flashed or build-verified against real hardware
- [x] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, and the app confirms itself only
      after NVS, safety link and web server are up (2026-08-17, already on
      `main` before this pass — `partitions.csv` two-app-slot table +
      `ota_rollback_confirm_task()` in `App/main.c` gating on
      `nvs_ok && web_ok && link_up`; host-build-verified, not yet
      hardware-flashed)
- [ ] Pico flash layout and metadata format frozen before the first board is
      programmed; signature field and key space reserved even though signing is off
- [ ] Pico bootloader: GPIO6 low first, active slot CRC'd every boot, recovery
      mode over UART1 with no timeout out of it
- [x] **Mutual protocol-version check** (lands with M5, gates this): each side
      verifies the other, a mismatch blocks heating on the ESP
      (`safety_link.c`'s `safety_update_health()` asserts
      `SAFETY_FAULT_SRC_SAFETY_LINK` on `version_mismatch`, same as a dead
      link) and puts the Pico in `DEGRADED_NO_CONTEXT` without latching a trip
      (`link_task.c`'s `link_task_handle_announce_version()`). `ANNOUNCE_VERSION`
      built this pass at the codec layer (`kilnlink_announce.{c,h}`,
      `KilnFW/TODO.md` 9.0); both firmwares' hand-rolled encode/parse of this
      frame predate the codec and were not migrated onto it. **2026-08-18,
      later same day:** the GUI half of this item is done too -- the LCD's
      "Safety Processor" card now shows both sides' protocol versions, marks
      compatibility, names which side is older on a mismatch, and always
      appends "Update ESP first" (static text per `UPDATE_PROTOCOL.md`'s
      unconditional rule, not computed from which side is older) -- see
      `KilnFW/TODO.md` 9.0 for the file-level detail. Not verified against
      real mismatched hardware, only by code inspection and a no-Pico bench
      build
- [~] **Compatibility floor** frozen so a version mismatch can never disable the
      update path itself — otherwise every mismatch needs a debug probe.
      Frame ids `0x00`-`0x0F` (incl. `ANNOUNCE_VERSION`, `FW_VERSION`) are
      reserved per `LINK_PROTOCOL.md` sec 4 and neither side's dispatch gates
      those frames on `peer_version_compatible`; **not verified end-to-end
      against a live mismatch this pass** — no hardware bring-up with two
      deliberately-mismatched builds was run
- [x] Image header validated before the first erase, so a wrong-target upload
      cannot erase a slot (2026-08-17, already on `main` — `ota_esp_do_transfer()`
      checks `esp_image_header_t` magic + chip ID before `esp_ota_begin()`)
- [x] Challenge–response on the AP password, so it never crosses the wire;
      lockout after 3 failures (2026-08-17, already on `main` —
      `GET /api/ota/challenge`, PSA Crypto HMAC, `App/drivers/ota_http.c`)
- [x] Both paths refused unless the kiln is idle and cool, with the specific
      blocker named (2026-08-17, already on `main` — `ota_interlock_check()`
      names the zone/reason, e.g. `"zone 2 is at 340 C"`)
- [ ] Link-loss heating block **not** bypassed during a Pico update — alarm text
      suppressed, never the block. **Correctly untouched, not yet verifiable**:
      `relay_authority.c` is unmodified (right, per design) but there is no
      Pico-update transfer path wired up yet to exercise this end-to-end
      (`KilnFW/TODO.md` 9.4)
- [x] **Four MCP tools (2026-08-18).** `tools/PcTools/src/kilnctrl/ota_http_client.py`
      (pure HTTP client: challenge fetch, HMAC derivation, streamed push,
      status poll) plus four `mcp__kilnctrl__` wrappers in `mcp_server.py`:
      `ota_get_challenge(host)`, `ota_update_esp(image_path, password, host)`,
      `ota_update_pico(image_path, password, host)`, `ota_status(host)`.
      Board discovery mirrors `gui.py`'s `_wifi_default_host()` (UART
      `wifi_get_status()`'s station IP, falling back to the AP default
      `192.168.4.1`; an explicit `host` always wins). Request construction /
      HMAC signing / response parsing unit-tested against mocked HTTP
      (`tools/PcTools/tests/test_ota_http_client.py`, 15 tests, no live
      board). **Real gap, surfaced honestly rather than papered over**:
      `ota_status()` can only report the Pico relay's progress
      (`GET /api/ota/pico/status`) — there is no HTTP route for the ESP
      self-update's own progress counter or the persisted `ota_record` "last
      update" blob, both of which exist only as in-process C getters today.
      **Not yet exercised against a physical board** — no hardware attached
      in this pass's environment; live-board verification (real interlock
      refusals, real lockout, a real Pico relay) is still outstanding.

---

## Decisions taken, so they are not re-litigated

| Decision | Date | Where the reasoning lives |
|---|---|---|
| Pico bench path is the Debug Probe: SWD plus its UART bridge on GP16/GP17. **The Pico's own USB is not used.** | 2026-08-16 | `firmware/SaftyFW/docs/ARCHITECTURE.md` §1 |
| **PSRAM stays disabled** on the ESP32-S3. Not an oversight — nothing needs it, and it costs determinism, a boot failure mode and a DMA audit | 2026-08-16 | `firmware/KilnFW/TODO.md` §9.1a |
| Library paths use `${KIPRJMOD}/../lib`, not a KiCad path variable | 2026-08-16 | `docs/REPO_LAYOUT.md` B1 |
| OTA authentication is challenge–response on the AP password, never a form POST | 2026-08-16 | `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` §2 |
| Update frames and the version handshake are a **frozen compatibility floor** | 2026-08-16 | `firmware/CommonFW/docs/LINK_PROTOCOL.md` |

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
