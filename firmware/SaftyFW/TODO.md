# TODO — Safety Processor Firmware

> **Status:** planning · **Last reviewed:** 2026-08-16
> **Keep this file current.** Tick items as they land, and keep "built" and
> "verified on hardware" distinct — `firmware/KilnFW/docs/PROJECT_STATUS.md` is the model
> for that discipline. If a phase changes shape, edit it here rather than
> letting the plan and the work drift apart.

Top-level ordering across both processors lives in [`../../ROADMAP.md`](../../ROADMAP.md);
this file owns the safety-processor detail. Phases here map to roadmap milestones
M0 and M2–M6.

Planning doc. **Nothing here is built yet** — `firmware/SaftyFW/` contains only these
documents. Written before any code so the sequencing, the thresholds and the
protocol can be argued about while they are still cheap to change.

Cross-references `docs/SAFETY_MODEL.md` (what trips and why),
`../CommonFW/docs/LINK_PROTOCOL.md` (the wire, both ends), `docs/HARDWARE.md` (the traced
board), `docs/CURRENT_SENSE.md` (the analog front end),
`docs/THERMOCOUPLE.md` (the sensor and its type), `docs/CONFIG_REFERENCE.md`
(every tunable) and `docs/GUARD_TEST_MATRIX.md` (how each guard is proven)
throughout.

Neither firmware is complete, so phase 0 contains `KilnFW` items as well.

---

## Phase 0 — Blockers (do these first, in this order)

Nothing downstream works until 0.1 and 0.2 land.

- [ ] **0.0 Run the Tier 0 pin test** (`docs/HARDWARE.md` §1): both GPIO4 and
      GPIO5 as inputs with internal **pull-downs**, read them. The pin that
      reads HIGH is the one carrying R15 and is therefore the ESP's **RX**.
      Needs no safety-domain power, no Pico, no probe. ⚠️ **Driving one pin and
      reading the other proves nothing** — they are not connected to each other.
      Three lines of schematic evidence already agree; this is the measurement.
- [x] **0.0b Build the coordinated two-board GPIO test rig** — a GPIO probe on
      *both* processors plus the PC script that drives them, reaching each by a
      path that is **not** the link under test (ESP over USB serial, Pico over
      SWD). `../../tools/PcTools/TODO.md` §1/§1b/§1c. Pico-side probe and the
      coordinated script both done 2026-08-18 (`tools/PcTools/src/kilnctrl/
      pico_gpio_probe.py`, `tools/PcTools/scripts/coordinated_gpio_test.py`);
      **not yet run against real hardware** this session.
- [x] **0.1 Fix the swapped safety-UART pins in `KilnFW`.** `KILNCTL_SAFETY_TX_IO`
      must become **4** and `KILNCTL_SAFETY_RX_IO` must become **5**
      (`firmware/KilnFW/App/drivers/Kconfig:189-200`, plus `sdkconfig`). Move the internal
      pull-up to GPIO5. **The link cannot work in either direction until this is
      done.** Full trace and the independent R15 confirmation: `docs/HARDWARE.md` §1.
      Done 2026-08-16, `idf.py build` verified clean. Still needs the §1 bench
      measurement — this was the code-side fix, not the hardware proof.
- [x] **0.2 Add `UART_PROTO_MSG_BROADCAST = 0x04`** to
      `firmware/KilnFW/App/drivers/espInterfaces/uart_protocol.{c,h}` — send without
      waiting for an ACK, receive without sending one. Required because the
      safety processor never transmits an ACK; without it every ESP push costs
      10 retries × 50 ms. See `../CommonFW/docs/LINK_PROTOCOL.md` §1.
      Done 2026-08-16: `uart_protocol_send_broadcast()` added, RX path delivers
      BROADCAST frames without ACK/dedup. `UART_PROTOCOL_VERSION` left at 4 —
      not bumped, since nothing consumes BROADCAST yet (no `SaftyFW` peer
      exists); revisit when M5 actually wires this frame type to a consumer.
- [x] **0.3 Correct `firmware/KilnFW/docs/SAFETY_LINK.md` and `firmware/KilnFW/docs/HARDWARE.md`.**
      Both describe the optocoupler data directions backwards (U3 is drawn
      mirrored relative to U1/U2). Do this *with* 0.1, or the next person will
      "fix" 0.1 back. Done 2026-08-16.
- [x] **0.4 Regenerate or delete `hardware/mainBoard/kiln.net`.** Dated 2026-07-19,
      sources a pre-move path, and disagrees with the current schematic in at
      least three places. It is the likely origin of the errors in 0.3.
      See `docs/HARDWARE.md` §10. Already deleted from the tree.
- [x] **0.4b Move `tools/PcTools/` → `PcTools/`** and add the **GPIO probe**
      (`../../tools/PcTools/TODO.md` §1). The probe is what makes 0.0 runnable without
      building a one-off firmware, and it is reusable for every future
      "is this net where the schematic says" question. Default off, hard
      deny-list including **GPIO6**.
      Move was already done (`tools/PcTools/`, `docs/REPO_LAYOUT.md`). GPIO
      probe done 2026-08-16 (`App/drivers/gpio_probe.{c,h}`,
      `CONFIG_KILNCTL_ENABLE_GPIO_PROBE` default off, `tools/PcTools`'s
      `probe.py` + MCP tools). **0.0 itself still needs the Pico side** (§1b
      below) before the coordinated two-board test in 0.0 can actually run.
- [x] **0.5 Flashing and console path — DECIDED (2026-08-16): Raspberry Pi Debug
      Probe over SWD, plus the probe's UART bridge on GP16/GP17. The Pico's own
      USB is not used.** One cable covers programming, reset, halt/step, memory
      access *and* the console, so USB adds a second cable and TinyUSB in the
      safety processor without replacing anything. `docs/HARDWARE.md` §7b has the
      wiring; `docs/ARCHITECTURE.md` §1 has the firmware reasoning.
- [ ] **0.5a Fit a 3-pin DEBUG header and bring GP16/GP17 out** before A1 is
      soldered down — both are far cheaper at build time than after.
      `docs/HARDWARE.md` §7b.
- [ ] **0.5b Consider a RUN (pin 30) reset wire** to the probe or a button while
      the board is still open. RUN is currently unconnected; OpenOCD's SWD reset
      is sufficient without it.
- [ ] **0.6 Redefine `SAFETY_FAULT_SRC_SAFETY_LINK`** as "no telemetry frame
      within 1.5 s", feeding `relay_authority_on_blocked()`, plus a 30 s
      firing-abort. This is what makes *the safety processor must be alive to
      heat* true. `../CommonFW/docs/LINK_PROTOCOL.md` §8. ⚠️ It also stops a main board with
      no Pico fitted from heating at all — bench work will need
      `safety_link_fault_on_link_loss(link, false)`.
- [ ] **0.6b Add the ESP-side boot version request**, with retry, and the
      dashboard/GUI surface for build identity + config CRC.
- [ ] **0.7 Confirm the K4 → line-contactor interlock topology** and which J10
      pin is NO vs NC. `docs/HARDWARE.md` §3. **This is a system-wiring decision,
      not a firmware one, and it must be settled before any bench trip test.**
- [ ] **0.8 Confirm the E-stop is wired normally-closed**, or fit a deliberate
      jumper to `GND_Safty`. `docs/HARDWARE.md` §5.

---

## Phase 1 — `CommonFW`, before either firmware uses it

The link contract is shared code, implemented once. See
[`../CommonFW/README.md`](../CommonFW/README.md) for the full checklist; the
gating items are:

- [x] `firmware/CommonFW/` created; `kilnlink` CMake target consumable by pico-sdk
      (2026-08-16 — target builds under MSVC; not yet actually pulled into a
      pico-sdk project, since `SaftyFW` has no CMake project at all yet)
- [x] ESP-IDF component wrapper in `firmware/KilnFW/components/kilnlink/`
      (2026-08-16, `idf.py build` verified clean, linked-but-unused so far)
- [x] `kilnlink_version.h` owns `KILNLINK_PROTOCOL_VERSION` (2026-08-16);
      `KilnFW`'s `UART_PROTOCOL_VERSION` **not yet** switched to alias it
- [ ] Shared ids split out of `uart_task_ids.h`; PC-link ids left behind
- [~] `kilnlink_frame` / `_context` / `_status` codecs, all pure and
      bounds-checked — **`kilnlink_frame` done** 2026-08-16 (delimiter,
      stuffing, CRC16/CCITT-FALSE); `_context`/`_status` not started, their
      payload layout is still being decided in `../CommonFW/docs/LINK_PROTOCOL.md`
- [x] Host tests + `test/vectors/`, including hostile inputs — for
      `kilnlink_frame`: `test/test_frame.c` (MSVC+CMake+Ninja+CTest, all
      passing) plus `test/vectors/frame_vectors.json` (3 valid + 6 hostile).
      Context/status vectors don't exist yet, same reason as above
- [x] `pc_tools` consuming the same vectors — it is the **third**
      implementation (2026-08-16, `tools/PcTools/selfcheck.py`)
- [ ] `KilnFW`'s `uart_protocol.c` delegating framing/CRC, proven byte-identical
      to the pre-refactor output **before** the old code is deleted
- [ ] CI grep: no CRC or byte-stuffing implementation outside `CommonFW`

## Phase 2 — Skeleton

Done 2026-08-16, and **build-verified under the real toolchain** (arm-none-eabi-gcc
14.2.1 / pico-sdk 2.1.1 / FreeRTOS-Kernel's RP2040 SMP port, Ninja) — a clean
`cmake --build` produces `build/SaftyFW.elf` with zero warnings under
`-Wall -Wextra -Werror`, from a from-scratch reconfigure. **Never flashed or run
on real hardware** — no RP2040 is attached to the machine this was built on; see
`docs/ARCHITECTURE.md`'s completion checklist for the same caveat spelled out
per item. Toolchain paths and the exact commands are in `CMakeLists.txt`'s
header comment, so the build is reproducible elsewhere.

- [x] pico-sdk + FreeRTOS-Kernel (SMP) CMake project, `-Wall -Wextra -Werror`.
      `CMakeLists.txt`, `FreeRTOSConfig.h`. `kilnlink` linked in (unused so
      far, same "linked but not yet called" state as `KilnFW`'s own component
      wrapper).
- [x] `main()` drives **GPIO6 low as its first statement**, before any init.
      `src/main.c`.
- [x] Hardware watchdog, 1 s, fed by `watchdog_task` only when all tasks check
      in — real bitmask-of-registered-tasks gate, not a stub.
      `src/tasks/watchdog_task.c`. **`pause_on_debug` hardcoded true** — there
      is no release/debug build distinction in this CMake project yet; TODO
      left in both `main.c` and `watchdog_task.c` to add one and flip it for
      release, per `docs/ARCHITECTURE.md` §8.
- [x] `boot_reason` read and cleared at startup, via two watchdog scratch
      registers (deliberately not scratch[4], which pico-sdk's own
      `watchdog_enable()`/`watchdog_enable_caused_reboot()` already use),
      magic-word validated. `src/boot_reason.{c,h}`. **Latching itself is not
      yet wired to a real trip** — `boot_reason_latch_trip()` exists and is
      called by nothing, because nothing in this phase evaluates a guard
      (that integration is Phase 4/5's job, deliberately left undone per this
      phase's brief).
- [ ] `flash_safe_execute()` for every config write — **not started**; there
      is no config store yet (Phase 9).
- [x] **`configUSE_CORE_AFFINITY` and `configNUMBER_OF_CORES` asserted at
      build time** — `#error` in `src/task_priorities.h`, and the build above
      only succeeds because both are set correctly in `FreeRTOSConfig.h`.
- [x] Task skeletons at the priorities and core affinities in
      `docs/ARCHITECTURE.md` §4 — link work (`link_task`, `log_task`) on core
      0, everything that can trip (`relay_owner`, `watchdog_task`,
      `safety_core`, `discrete_task`, `thermo_task`, `current_task`) on core
      1. `src/tasks/*.c`. Every task's real work (thermocouple reads, ADC
      sampling, framing, guard evaluation) is a TODO comment for its own
      later phase — this phase proves the task/priority/affinity/watchdog-
      checkin shell compiles and links, not that any of them do anything yet.
- [ ] Blink-equivalent proof of life over SWD/RTT — **not verified**; no
      RP2040 hardware and no debug probe attached to the machine this was
      built on, so nothing here has ever run.
- [x] **Heartbeat LED, physical, on the safety processor itself** (2026-08-17)
      — requested this session, built same day. `watchdog_task.c` toggles
      `SAFTYFW_PIN_HEARTBEAT_LED` (GPIO25) in the exact same
      `mask == WATCHDOG_CHECKIN_ALL_MASK` branch that feeds the real
      hardware watchdog, so the LED cannot physically drift from what the
      watchdog itself is deciding: every task checked in -> feed + toggle
      (steady 500 ms-period blink); any task missed its window -> neither
      runs, so the LED freezes at its last level instead of the code
      choosing a "fault" level for it (this is a side effect of the
      existing feed decision, not a second liveness check — see the file
      header comment). GPIO25 is a real pin, not a placeholder: A1 is
      confirmed `PICO_BOARD=pico` (docs/HARDWARE.md section 2, stock
      Raspberry Pi Pico module), and GPIO25 is that module's own onboard
      LED, wired module-internally — it has no A1 schematic net, unlike
      every other constant in `board_pins.h`, which is why it wasn't in
      that file before. **Not verified** — same as the SWD/RTT item above,
      no RP2040 hardware and no debug probe attached to the machine this
      was built on; the toggle logic has never actually driven a real LED.
- [x] **Log transport**: `kilnlink` LOG frames as primary (2026-08-17) —
      `log_task.c` owns a bounded FreeRTOS queue (16 entries, 96 bytes each,
      allocated once at `log_task_start()`), drains it, and hands entries to
      `link_task_send_log()` (task id 5, same as `KilnFW`'s existing `LOG`
      task) as kilnlink BROADCAST frames. RTT secondary transport **not
      built** — needs the debug probe wiring, no hardware to develop against.
- [x] `SAFTYFW_ENABLE_USB_STDIO` compile flag (2026-08-17) — a CMake
      `option()`, default OFF, gating a `stdio_usb` mirror in `log_task.c`
      behind `#ifdef`. Build-verified both ways; never exercised on hardware
      (`docs/ARCHITECTURE.md` section 1's 3V3 back-feed caveat still applies
      and is not this code's problem to solve).
- [x] Reserve TX ring capacity for telemetry (2026-08-17) — `log_task.c`
      checks `link_task_get_tx_ring_fill_fraction()` before enqueueing a log
      frame and drops (counted) rather than sends once the ring is at/above
      50% full, so a log burst can never be the thing that displaces a Frame
      A/B send; Frame A/B's own TX calls never consult this at all.
- [x] Dropped-log-frame counter (2026-08-17) — `log_task_get_dropped()`,
      cross-core-safe (`taskENTER_CRITICAL`-guarded increment, since
      `log_task_log()` is called from any task on either core). Not yet
      folded into a wire frame's spare field — exposed but unconsumed.
- [ ] Runtime log-level command over the link — **not started**.
      `log_task_set_level()`/`_get_level()` exist and are used internally
      (`LOG_LEVEL_WARN` default per `docs/ARCHITECTURE.md` section 1), but
      `link_task.c` has no RX handler wiring a wire command to them yet —
      deliberately deferred (lower priority than Frame B, per this pass's
      own scoping) rather than rushed.
- [x] **CI grep check: `safety_core.c` must not include the link header, and
      `link_task.c` must not reference GPIO6.** `tools/check_isolation.ps1`,
      comment-stripping so the rule can be documented in prose inside those
      same files without tripping its own check. Verified to both pass on the
      real files and fail loudly when a violation is deliberately introduced.
      Not yet wired into an actual CI pipeline (this repo doesn't have one
      yet) — it is a script to be run, not an automated gate.

## Phase 3 — Thermocouple

- [x] Port `firmware/KilnFW/App/drivers/MAX31856.c`. **Port it, do not rewrite it** — same
      part, same registers, and `firmware/KilnFW/docs/MAX31856.md` already documents the
      traps. `src/max31856.{c,h}`, 2026-08-16 — single channel, no bus-sharing
      machinery (this board has one device, THERMOCOUPLE.md §1), same
      register map / fixed-point conversions / comparator-fault-mode logic /
      failure-honesty discipline as the original.
- [x] `spi_owner`; `CS0` driven as a plain GPIO. `src/spi_owner.{c,h}` — a
      mutex-guarded direct-call module, deliberately **not** a request-queue
      task: THERMOCOUPLE.md §1 says this bus has exactly one device and will
      never contend, and ARCHITECTURE.md §4's task table has no separate
      spi_owner row, only `thermo_task`. See `spi_owner.h`'s header comment
      for the full reasoning. Judgement call — revisit if a real second
      device ever lands on this bus.
- [x] `~DRDY` (GPIO12) as a real **interrupt** — unlike the main board, this is a
      direct Pico GPIO, so do not poll it. `src/tasks/thermo_task.c`:
      `gpio_set_irq_enabled_with_callback()`, falling edge,
      `vTaskNotifyGiveFromISR()` + `portYIELD_FROM_ISR()`.
- [x] `thermo_task` publishing `thermo_snapshot_t`; **NaN, never 0, when invalid**.
      `src/snapshots.h` (shared with `current_snapshot_t`), published under a
      mutex, read via `thermo_task_get_snapshot()`. `src/tasks/safety_core.c`
      now pulls the real snapshot instead of the Phase 2 hardcoded
      `tc_valid = false` stub.
- [~] **Choose `tc_type` deliberately, per sensor** — type K is marginal above
      ~1150 °C and green-rots *low* in reduction. Type S/R for a chamber-mounted
      sensor on a cone-10 kiln. `docs/THERMOCOUPLE.md` §2. **Not decided this
      phase** — `tc_type` is correctly a runtime parameter of
      `max31856_configure()` (not a compile-time constant), but `main.c`
      currently passes `MAX31856_TC_TYPE_PLACEHOLDER` (type K) because no
      `config_store` exists yet (Phase 9) to source the real per-installation
      decision from. Explicitly not a claim that K is correct for this kiln.
- [ ] **Per-type plausibility ranges**, driven from the configured type — a
      range hard-coded to type K misfires on every other type. **Not started**
      — needs a real commissioned `tc_type` (above) to be meaningful.
- [x] Set the `MASK` register explicitly — **reset default `FFh` masks every
      fault**, leaving `~FAULT` permanently inactive. `max31856_configure()`
      writes `MAX31856_DEFAULT_FAULT_MASK` (0xFC) every time.
- [x] **`~DRDY` silence detection** → S5. This is the stopped-converting failure
      `KilnFW` structurally cannot see. `thermo_task.c` waits on the DRDY
      notification with a timeout of 2× `max31856_conversion_time_ms()`; a
      timeout publishes `thermo_snapshot_t.valid = false` directly, without
      attempting a stale burst read, which `safety_core.c` maps onto
      `tc_valid = false` for S5. **Build-verified only, not hardware-verified**
      — no MAX31856 is attached to the build machine.
- [ ] Bench: read ambient with the thermocouple attached; confirm open-circuit
      reports `THERMO_FAULT_OPEN` rather than a plausible number. **Not
      possible this phase** — no hardware attached to the build machine; this
      is Phase 9 commissioning work, per this file's own task brief.

## Phase 4 — Guards, host-tested, no relay yet

- [x] `safety_guards.c` as a **pure function** — no RTOS, no SDK, no I/O,
      `dt_s` passed in. `src/safety_guards.{c,h}`, 2026-08-16. No `#include`
      of anything RTOS/SDK/link-shaped; `safety_guard_input_t` carries no
      link-derived field (verified by the independence-invariant test).
- [x] Implement **S1** (absolute over-temp), **S5** (sensor invalid, graduated),
      **S7** (E-stop), **S11** (frozen TC), **S12** (cold junction / enclosure).
      These need no link and no current calibration, and S1 alone justifies the
      board. S11 and S12 are free — both read data S1 already fetches.
      2026-08-16. S11's "heat commanded" qualifier has no real source yet
      (no current sense, no link context) — `heat_commanded` is a plain bool
      the caller supplies, defaulting to false, which keeps S11 correctly
      dormant on an idle kiln until Phase 6/7 wire a real signal into it.
- [x] Implement **S2** (sustained over-setpoint), **S3** (load stuck on),
      **S4** (load inactive, WARN only), **S6** (main controller unhealthy,
      both signals), **S9** (trip-ineffective escalation), **S10** (safety TC
      vs zone TC disagreement, WARN), **S13** (borrowed channel stale) —
      2026-08-18, `src/safety_guards.c`/`.h`. Same pure-function discipline as
      the first five: no RTOS, no SDK, no I/O, `dt_s` passed in. Rather than
      pulling in `link_task`'s real `context_snapshot_t` or `current_task`'s
      `current_snapshot_t` (neither exists as a real producer yet — Phase 6/7),
      the specific scalar facts each of these seven guards needs (max active
      zone setpoint, nearest zone measurement, current-presence booleans,
      relay-recent/continuous booleans, a per-zone sample-counter-advancing
      flag, mainFault, link-up, post-trip relay-deenergized) are flattened
      directly into `safety_guard_input_t` with a single `context_valid` gate
      collapsing "never received / stale / DEGRADED_NO_CONTEXT" per
      ARCHITECTURE.md section 9's rule that these guards go inactive, never
      pessimistic, on unusable context. S9 required restructuring
      `safety_guards_tick()`'s early-return-when-latched path, since it is the
      one guard that must keep evaluating (whether the relay actually
      de-energized and current is still present) after every other guard has
      already stopped mattering — it escalates `state->trip_ineffective`
      independently of `is_tripped` and overwrites the reported reason to
      `SAFETY_TRIP_INEFFECTIVE`, matching ARCHITECTURE.md section 9's trip-code
      comment calling S9 "escalation, not a cause". S8 (implausible rate of
      rise) remains unimplemented by design — `SAFETY_MODEL.md` section 4 says
      it ships disabled until a real full-power ramp is logged (Phase 9); there
      is no defensible threshold to build yet. Runtime configuration integrity
      (the periodic CRC background check) is also out of scope here — it needs
      `config_store` (Phase 9), which doesn't exist. This module now implements
      12 of `SAFETY_MODEL.md` section 4's 13 guards.
- [x] `discrete_task`: debounce E-stop (50 ms) and `mainFault` (200 ms).
      Built in the relay-authority pass, 2026-08-16: a standard
      consecutive-sample debounce in `src/tasks/discrete_task.c`, sample
      count derived from `SAFTYFW_PERIOD_DISCRETE_TASK_MS` (ceiling divide)
      rather than hardcoded, so the window stays 50 ms / 200 ms regardless
      of the task's actual period. `safety_guards.c` receives
      `estop_pressed` already debounced, as designed. Build- and
      host-test-verified, not hardware-verified.
- [x] Host test harness (MSVC, no SDK), mirroring `firmware/KilnFW/App/test/`.
      `test/test_common.h` (copied verbatim), `test/test_main.c`,
      `test/test_safety_guards.c`, `test/build_host_tests.ps1`. Builds and
      passes clean under `/W4 /WX`, 2026-08-16 (80/80 checks then; now
      320/320 across the whole host-test suite as of 2026-08-18's S2/S3/S4/
      S6/S9/S10/S13 pass, 48 of those checks new this pass). Also
      build-verified 2026-08-18 under the real arm-none-eabi-gcc/pico-sdk
      toolchain: `safety_guards.c` and `safety_core.c` compile clean with
      zero warnings under `-Wall -Wextra -Werror` as part of a `cmake --build`
      — the full link fails, but on a pre-existing, unrelated cause: an
      **untracked, mid-work `firmware/CommonFW/src/kilnlink_status.c`**
      (declared functions whose parameter types don't match
      `kilnlink_status.h`, e.g. `kilnlink_status_get_status_t` vs
      `kilnlink_status_status_t`) left uncommitted in the tree from a
      different, unfinished pass. Not touched or fixed here — out of this
      pass's scope (guard logic, not the status codec), and not something
      this session introduced (confirmed via `git status`: those
      `CommonFW` files are `??` untracked, not modified by this change).
- [x] **Write the nuisance-rejection tests before the trip tests.** A 900 ms
      sensor dropout must *not* trip S5; a single noisy SPI read must not either.
      Both cases are explicit tests in `test/test_safety_guards.c`, ahead of
      every guard's trip case.
- [ ] Reuse `firmware/KilnFW/App/test/sim_plant.c` for realistic thermal traces.
      Not done — this phase's tests use synthetic step/ramp sequences
      instead, which were enough to prove each guard's boundary; wiring
      `sim_plant.c` in is left for whenever a later phase needs a full
      closed-loop trace (e.g. S2/S8 tuning).

## Phase 5 — Relay authority

- [x] `relay_owner` — the **only** code in the build that writes GPIO6. True
      since Phase 2; unchanged by this pass.
- [~] Latching trip semantics; clear refused while the condition still holds.
      Latching itself is built and wired, 2026-08-16:
      `relay_owner_command_trip()` de-energizes GPIO6 and enters a latched
      `TRIPPED` state in one step; `relay_owner_command_energize()` is
      refused (returns `false`, no-op) while `TRIPPED`; `safety_core.c`
      calls `relay_owner_command_trip()` then `boot_reason_latch_trip()` on
      a new `safety_guards_tick()` trip, matching `SAFETY_MODEL.md` section
      6's 4-step order. **"Clear refused while the condition still holds"
      is NOT built** — `relay_owner_clear_trip()` exists and unconditionally
      transitions `TRIPPED` -> `ARMED` if called, but nothing calls it yet
      (that enforcement needs a caller with guard state to re-check against,
      e.g. `safety_core.c` re-running `safety_guards_tick()` before honouring
      a clear request — Phase 7's link_task/GUI clear-command job). Marked
      partial rather than checked for that reason.
- [x] GRACE → ARMED state machine, `startup_grace_s` = 60 s. Built
      2026-08-16 in `src/tasks/relay_owner.c`: the GRACE timer starts the
      instant `relay_owner_task()` itself begins running (a judgement call —
      see that file's header comment for why this was chosen over a
      separate "enter grace" call from `main.c`), and transitions to `ARMED`
      automatically once `SAFTYFW_STARTUP_GRACE_MS` (60000) has elapsed,
      checked every loop iteration. While `GRACE`, `relay_owner_command_energize()`
      is accepted and tracked but GPIO6 is never driven high. Build- and
      host-test-verified; **not hardware-verified** — no RP2040 attached.
- [ ] **Bench-verify the safe state four ways**: power-on, watchdog reset,
      brownout, and firmware halted at a breakpoint. K4 must be de-energized in
      all four.
- [ ] Verify **de-energized K4 opens the contactor**, on the real interlock
      wiring from 0.7. Getting this backwards passes every bench test and fails
      dangerous.

## Phase 6 — Current sensing

Sampling/conversion/snapshot-publishing built and build-verified 2026-08-16
(arm-none-eabi-gcc 14.2.1, pico-sdk 2.1.1, FreeRTOS-Kernel RP2040 SMP,
Ninja) — a from-scratch `cmake --build` produces `SaftyFW.elf` with zero
warnings under `-Wall -Wextra -Werror`. **Not hardware-verified** — no
RP2040/CT hardware attached to the build machine, so nothing below claims a
real current reading. The guards that would consume this output (S3, S4,
S9) are explicitly out of scope for this pass — they need `link_task`'s
`relay_recent_mask` context, which is Phase 7.

- [x] `adc_owner`: round-robin ADC0/1/2, 16× oversample, 20 Hz/channel
      (`src/current_sense.c`, called from `src/tasks/current_task.c` at
      `SAFTYFW_PERIOD_CURRENT_TASK_MS` = 50 ms). Manual `adc_select_input()` +
      single-shot `adc_read()` polling rather than hardware round-robin/FIFO
      capture — a deliberate deviation from `ARCHITECTURE.md` §8's general
      round-robin guidance, documented in `current_sense.c`'s header comment,
      because `CURRENT_SENSE.md` §4's more specific "no free-running capture"
      rules out `adc_run(true)`. First conversion after each mux switch is
      discarded per §4/§8.
- [x] Peak-envelope conversion per `docs/CURRENT_SENSE.md` §2, exact formula
      (`cs_counts_to_amps()`). **No RMS accumulator, no DMA capture.**
- [x] Clip detection — within ~50 mV of the rail, compared in the native
      12-bit count domain. Reported as `current_snapshot_t.clipped[n]`, a
      state, never folded into the amps number. **Not a trip condition** —
      still counts as load-active.
- [~] `zero_counts` re-measurement — the **mechanism** exists
      (`current_sense_recalibrate_zero()`), but it has no visibility into
      relay state by design (module isolation) and nothing calls it yet; the
      ">= 5 min idle, no relay commanded on" precondition and the drift
      report are Phase 9's `config_store`/commissioning-flow job.
- [ ] **Run the full commissioning check in `docs/CURRENT_SENSE.md` §5** — in
      particular step 2, one relay at a time, confirming each CT maps to the
      channel you think it does. Needs real hardware; not done.
- [x] **No over-current / under-current guard**, by design — these channels
      are a load-active detector and a power estimator. Fuses and breakers
      own over-current (`docs/SAFETY_MODEL.md` §3). Nothing in this pass adds
      one.
- [x] Power estimate: `i_conducting_a`, `conduction_fraction` over a 120 s
      rolling window, and `p_avg_w` (`NAN` unless `mains_voltage_v` is
      configured) — `current_sense_power_t`, kept in a struct deliberately
      separate from `current_snapshot_t` so no guard can end up consuming the
      filtered value. **No guard reads any of it** — nothing calls
      `current_task_get_power()`/`current_task_get_snapshot()` yet.
- [ ] Implement **S9** (trip ineffective / contactor welded). Out of scope for
      this pass — needs `link_task`'s relay context (Phase 7).
- [ ] ⚠️ **S3 and S4 stay disabled until the CT channel mapping is confirmed.**
      A correlation guard fed by a mis-mapped CT trips on healthy firings and
      stays quiet on the failure it exists to catch. Not implemented in this
      pass (Phase 7).

## Phase 7 — The link

- [x] `0x7E` framing, `0x7D`/`^0x20` stuffing, CRC16/CCITT-FALSE now actually
      wired to real traffic: `link_task.c` calls the existing, host-tested
      `kilnlink_frame_encode_raw()`/`kilnlink_stuff()`/`kilnlink_unstuff()`/
      `kilnlink_frame_decode()` (`CommonFW/src/kilnlink_frame.c`) rather than
      a separate `uart_frame.c` — that library already **is** this item,
      "linked but unused" until this pass. Byte-compatible with
      `uart_protocol.c` by construction (same library backs both).
- [x] **Plain hardware UART, no inversion, no PIO.** `uart_owner.c`: plain
      `hardware/uart.h`, `uart_set_hw_flow(..., false, false)`, no PIO.
- [x] Receiver hardening: resync on `0x7E` from any state, bounded buffers,
      break tolerated as "peer not up", **no allocation**. `link_task.c`'s
      `link_task_rx_process_byte()`/`link_task_handle_raw_frame()`.
- [x] Parse `SAFETY_CMD_PUSH_CONTEXT` (0x07) → `context_snapshot_t` (2026-08-17,
      `link_task.c`): `link_task_handle_push_context()` calls the (separately
      landed) `link_frame_unpack_context()`, stamps `timestamp_ms` locally
      (the unpacker deliberately never touches it), publishes under a new
      mutex-guarded `s_context_lock`/`s_context_snapshot` pair matching
      `thermo_task.c`'s publish/get pattern exactly, and exposes it via the
      new `link_task_get_context_snapshot()`. A rejected/malformed payload
      only increments `s_context_frames_bad` and never overwrites the last
      good snapshot. `link_task_send_diag()`'s `context_age_100ms`/
      `context_frames_ok`/`context_frames_bad` are real now instead of the
      old hardcoded 255/0/0.
- [~] `boot_id` change resets every correlation window (2026-08-17,
      `link_task.c`): the **tracking** exists --
      `link_task_handle_push_context()` detects `snap.boot_id !=
      s_last_context_boot_id` and updates it every frame -- but there is
      genuinely no correlation guard anywhere in this codebase yet for a
      detected change to reset (S2/S6/S10 below are still unchecked), so the
      detection is currently a documented no-op. Left partial rather than
      checked off outright, matching how `zero_counts` and the latching-trip
      entries elsewhere in this file mark "mechanism exists, not yet wired
      to a consumer."
- [~] **Honour the `SIM_PLANT` flag**: disable S2/S3/S4 and warn persistently
      (2026-08-17, `link_task.c`): the **seen-tracking and warning** are real
      -- `s_context_sim_seen` latches true on the first successfully-parsed
      frame with `CONTEXT_FLAG_SIM_PLANT` set and never clears, and DIAG's
      `sim_context_seen` flag bit reports it persistently. S2/S3/S4
      themselves do not exist yet to be disabled (see this phase's own S2/S6/
      S10 and S3/S4-enable items below, still unchecked) -- so this is
      honestly a partial, not a completed feature.
- [ ] Handle `SET_FIRING_CEILING` (0x09) → S1's `effective_ceiling`.
      **Clamp with `min()`** — the ESP may only ever tighten it.
- [ ] Handle `CLEAR_TRIP` (0x0A), refused while the condition holds and
      refused on a `trip_mask` mismatch.
- [ ] Handle `SET_CLOCK` (0x0C), diagnostic only. **No guard may read it.**
- [ ] **`tc_placement_mode` commissioning field** (`CHAMBER_AGREED` /
      `EXTERNAL_OVERHEAT`). Gates S1's firing-ceiling tightening, S2 and S10.
      **No default** — until set, S2/S10 stay off and S1 uses the fixed limit
      (`docs/SAFETY_MODEL.md` §3).
- [ ] Implement **S2** (sustained over-setpoint), **S6** (main controller
      unhealthy — the two independent signals, kept independent) and **S10**
      (safety TC vs zone TC disagreement, WARN by default).
- [ ] **`tc_source` commissioning field**: `OWN_J7` / `BORROWED_ZONE` / `BOTH`.
      `tc_placement_mode` forced to `CHAMBER_AGREED` when borrowing, and a
      contradictory config **rejected, not reconciled**.
- [ ] Implement **S13** (borrowed channel not updating) against the context
      frame's per-zone `sample_counter`. Without it a frozen main-board channel
      is indistinguishable from a kiln holding a soak.
- [ ] `SAFETY_FLAG_BORROWED` in every status frame when borrowing, and the GUI
      labels the temperature accordingly.
- [ ] Compare the borrowed channel's reported `tc_type` against
      `borrowed_type_expected`; warn on a change.
- [ ] Enable **S3** / **S4** once phase 5's mapping check has passed.
- [ ] Develop the parser against a **PC-side stub emitting context frames**
      before `KilnFW` can send any — the reverse of the stub already described
      in `firmware/KilnFW/docs/SAFETY_LINK.md`.

## Phase 7b — Mutual version compatibility

Both processors must check each other. Design:
[`../CommonFW/docs/LINK_PROTOCOL.md`](../CommonFW/docs/LINK_PROTOCOL.md),
`ANNOUNCE_VERSION`. This lands with the link, not with updates — it is what makes
every later frame safe to parse.

- [x] 7b.1 `KILNLINK_PROTOCOL_VERSION` and `KILNLINK_MIN_COMPATIBLE` in
      `CommonFW`, one definition, both firmwares including it -- **done
      2026-08-17**: `KilnFW` now includes it too.
      `firmware/KilnFW/App/drivers/uart_task_ids.h` includes
      `kilnlink/kilnlink_version.h` and `UART_PROTOCOL_VERSION` is now
      `((uint16_t)KILNLINK_PROTOCOL_VERSION)`, an alias rather than a second
      number -- exactly what that header's own doc comment already asked
      for. The `kilnlink` ESP-IDF component wrapper
      (`firmware/KilnFW/components/kilnlink/CMakeLists.txt`) and
      `App/drivers/CMakeLists.txt`'s `REQUIRES ... kilnlink` were already in
      place from an earlier pass (M2); this pass only needed the include and
      the alias. `idf.py build` verified clean.
- [x] 7b.2 `ANNOUNCE_VERSION` = `0x0F` sent by the ESP unprompted at boot, on
      retry, and on every `boot_id` change -- **done 2026-08-17**,
      `firmware/KilnFW/App/drivers/safety_link.c`:
      `safety_link_send_announce_version_burst()` sends
      `SAFETY_CMD_ANNOUNCE_VERSION` (0x0F) as a `uart_protocol_send_broadcast()`
      four times, 250ms apart (~750ms, matching Frame D's "repeated a few
      times over the next second" spirit), called once from
      `safety_poll_task()` before its steady loop (the boot push) and again
      from `safety_apply_fw_version()` whenever a Pico `FW_VERSION` (0x0B)
      frame reports a `boot_id` different from the last one seen. Build
      identity is real, not a stub: `safety_build_announce_version_payload()`
      reads `FW_GIT_COMMIT`/`FW_GIT_DIRTY`/`FW_BUILD_DATE`/`FW_BUILD_TIME`
      from `build_info.h` (generated fresh every build by
      `App/drivers/gen_build_info.cmake`, already existed before this pass
      and already fed the PC-link's own `GET_FW_VERSION` reply in
      `uart_bridge.c`) -- unlike `SaftyFW`, `KilnFW` did not need an honest
      stub here. `esp_boot_id` is generated once at `safety_link_start()` via
      `esp_random()` (the ESP's hardware RNG, simpler than `SaftyFW`'s
      time-derived pseudo-random fallback). Receiving and parsing `FW_VERSION`
      itself needed building too (Phase 0.6b was unstarted): `safety_parse_fw_version()`
      reads bytes 1-4 before anything else per the floor rule, and
      `safety_drain_inbox()` now dispatches incoming frames by subcommand
      byte instead of assuming every inbox message is a status reply. This is
      a minimal slice of 0.6b (parsing an unsolicited `FW_VERSION` push), not
      all of it -- there is still no explicit ESP-side `GET_FW_VERSION`
      *request* with retry, and no GUI/dashboard surface for build identity +
      config CRC; both remain open under 0.6b.
- [x] 7b.3 `min_compatible` added to `FW_VERSION` at a fixed offset; version
      fields read and compared **before** anything after them is parsed --
      `link_frame_pack_fw_version()` (bytes 3..4) and
      `link_task_handle_announce_version()` (reads bytes 1..4 before anything
      else, bails on `frame->length < 5`)
- [x] 7b.4 Compatibility evaluated in **both** directions --
      `link_frame_versions_compatible()`, host-sanity-checked against five
      combinations (equal, newer self, self-raised-floor, peer-below-floor,
      self-below-peer's-floor)
- [~] 7b.5 Mismatch sets `DEGRADED_NO_CONTEXT`: context frames discarded
      unparsed, context-free guards still commanding the relay, context-dependent
      guards reported disabled, **no trip latched** -- **partial, both sides**:
      the Pico half is as previously documented (`link_task_get_degraded_no_context()`
      set/cleared correctly, never touches `relay_owner`/`boot_reason`,
      mechanism exists but nothing to discard/disable yet). The ESP half is
      new this pass (2026-08-17), `firmware/KilnFW/App/drivers/safety_link.c`:
      "The ESP: treats it exactly like a dead link -- `SAFETY_FAULT_SRC_SAFETY_LINK`
      asserts, every heater-on is blocked, a running firing aborts"
      (`LINK_PROTOCOL.md` sec 4) is now real -- `safety_apply_fw_version()`
      computes `link_frame_versions_compatible()`'s exact formula (ported into
      `safety_link_versions_compatible()`, since `kilnlink` today carries only
      framing/CRC, not frame-payload logic, per `CommonFW/README.md`'s
      Integration section) against the Pico's announced protocol/min_compatible,
      and `safety_update_health()` folds a known mismatch into the *same*
      `SAFETY_FAULT_SRC_SAFETY_LINK` bit link-staleness already uses
      (`assert = !up || version_mismatch`), gated by the same
      `fault_on_link_loss` policy switch the existing dead-link path already
      had -- no new mechanism invented, `SAFETY_FAULT_SRC_SAFETY_LINK` and
      `safety_link_set_fault_source()` both already existed. Marked `[~]`
      rather than `[x]` for two honest reasons: (1) the Pico side is still
      partial as above -- this checkbox covers both sides' behaviour and only
      one half is more complete now; (2) Phase 0.6's full "no telemetry within
      1.5s, feeding `relay_authority_on_blocked()`, plus a 30s firing-abort"
      redefinition is still unbuilt on the ESP side -- this pass reused the
      *existing* link-staleness check (`safety_link_up_locked()`,
      `SAFETY_LINK_UP_PERIODS` poll periods) rather than building 0.6 as a
      prerequisite, since 0.6 is its own listed item and out of this pass's
      scope. "Every heater-on is blocked, a running firing aborts" is
      therefore only as true today as the existing link-loss fault path
      already made it -- this pass did not change that scope.
- [x] 7b.6 Telemetry keeps flowing during a mismatch — it is the only way the ESP
      can display the problem or push the fix. `link_task`'s TX cadence
      (status + `FW_VERSION`) never checks `s_degraded_no_context`.
- [x] 7b.7 **Compatibility floor**: framing, `ANNOUNCE_VERSION`, `FW_VERSION` and
      the `UPDATE_*` frames work regardless of version. Ids `0x00`–`0x0F`
      reserved; those layouts may be appended to, never reordered or resized.
      Without this a mismatch makes the field-update path unusable and every fix
      needs a debug probe -- **ESP side audited and confirmed 2026-08-17**:
      `safety_link.c`'s RX dispatch (`safety_drain_inbox()`) never consulted
      `peer_version_compatible` before this pass either, so there was no
      gating bug to fix -- `GET_STATUS` and (new) `FW_VERSION` are routed by
      subcommand byte alone, unconditionally. A one-line comment was added at
      the dispatch switch stating the floor constraint explicitly (mirroring
      how `link_task.c` documents its own isolation rule in comments), and
      the default case for an unrecognised subcommand was changed to silently
      discard rather than log a frame error, matching
      `LINK_PROTOCOL.md`'s own additive-compatibility principle ("a peer that
      has never heard of it discards it") -- previously any non-`GET_STATUS`
      frame (which did not exist on the wire yet, so this never fired in
      practice) would have been logged as an "unexpected frame" wire error.
      `SaftyFW`-side floor compliance is that project's own concern and not
      re-audited here.
- [ ] 7b.8 Host test: every combination of older/newer/equal on both sides,
      including a peer that announces a `min_compatible` above its own version

## Phase 8 — Telemetry (required)

The ESP will not permit heating without this. See `../CommonFW/docs/LINK_PROTOCOL.md` §8.

- [x] Non-blocking TX ring. **Drop on full, increment a counter, never block.**
      `uart_owner.c`: `uart_owner_send()` drops the whole frame and increments
      `s_tx_dropped` (`uart_owner_get_tx_dropped()`) if the ring lacks room;
      never blocks, never waits on the ISR.
- [x] Emit the **existing 23-byte** status frame unchanged, every 500 ms — this
      is what lets the Pico be validated against an unmodified `KilnFW`.
      `link_task_send_status()`, byte layout cross-checked by hand against
      `firmware/KilnFW/App/drivers/safety_link.h` and by a standalone host
      sanity check (round-tripped through the real `kilnlink_frame` codec).
- [x] Emit `SAFETY_CMD_DIAG` (0x08), additive (2026-08-17) —
      `link_task_send_diag()`, 2s cadence (deliberately slower than Frame A's
      500ms; nothing on the ESP side gates on this frame yet). `trip_reason`
      and `state` are real (via a new `safety_core_get_diag_status()`
      channel, same isolation-respecting pattern as the output-status one);
      `warn_mask`/`trip_mask` are honestly degraded to a single bit each —
      `safety_guards.c` tracks one `reason` for the whole module, not a
      13-guard bitmask, so a fuller mask has no data source yet.
      `context_age_100ms` is always 255 (never received — no context-frame
      parsing exists), `context_frames_ok`/`bad` always 0 (same reason),
      `tx_frames_dropped` is real. `flags` bit1 `calibration_missing` is
      always 1 (true — no `config_store` yet, Phase 9); bit0
      `sim_context_seen` and bit2 `estop_unwired_suspect` are always 0, no
      detection heuristic exists for either.
- [ ] `build_info.h` generated on every build (git commit, dirty, timestamp).
      **Unknown must map to `dirty = 1`** — an uncommitted build must never
      report itself clean. **Not built this pass**: `link_task_send_fw_version()`
      currently hardcodes `dirty = 1` and empty commit/datetime fields, which
      is honest (unknown maps to dirty, per the rule) but not the real
      generated build identity.
- [x] Emit `SAFETY_CMD_FW_VERSION` (0x0B) on request **and unsolicited at boot**;
      include `boot_id`, `config_version` and the **active config CRC**.
      `boot_id` is a real (pseudo-random, time-derived) per-boot value;
      `config_version`/`config_crc` are 0 — honest, since `config_store`
      (Phase 9) doesn't exist yet and the spec documents 0 as exactly that
      case ("running on compiled-in defaults that were never commissioned").
- [ ] Emit `SAFETY_CMD_TRIP_EVENT` (0x0D) **immediately on trip**, repeated a
      few times, carrying the deciding values at the moment of the trip. Half a
      second later that evidence is gone.
- [ ] Emit `SAFETY_CMD_POWER` (0x0E): per-channel conducting amps, conduction
      fraction, watts, plus totals and accumulated Wh.

## Phase 8b — ESP web GUI surface (`KilnFW` work)

Everything here is already on the wire; this is `dashboard_http.c` presenting it.
See `../CommonFW/docs/LINK_PROTOCOL.md` §7 for the full panel spec and presentation rules.

- [ ] **Safety Processor panel**, always visible: safety temperature, enclosure
      (cold-junction) temperature, power total + per channel, energy this firing,
      link state/age, armed/tripped, trip reason.
- [ ] **Label the safety temperature with its placement mode.** In
      `EXTERNAL_OVERHEAT` it will not track the zone temperatures and should not.
- [ ] Colour the enclosure temperature against S12's 60 °C / 85 °C thresholds.
- [ ] **Invalid readings render as `—`, never as a number.** NaN on the wire
      must not become 0 on screen.
- [ ] Mark power as an estimate; show `—` when `mains_voltage_v` is unconfigured
      rather than assuming a default.
- [ ] **`TRIP_INEFFECTIVE` gets its own visual treatment** — it means "go to the
      breaker", not "investigate the kiln".
- [ ] Safety processor build identity + config CRC on the diagnostics page.
- [ ] Panel must render with the link down: last-known values plus an explicit
      age, never a blank or a spinner.
- [ ] Mirror the same data on the PC-link `SAFETY` task so `pc_tools` and MCP
      see it without Wi-Fi.
- [ ] **Assert in the host tests that guard verdicts are bit-identical with the
      TX path stubbed out.** No verdict may depend on anyone listening.

## Phase 9 — Commissioning and the honest gaps

- [ ] Config store: versioned, CRC'd, safe defaults, `calibration_missing` flag.
- [ ] Config writes **refused while ARMED**.
- [ ] Log a full-power ramp, measure the real maximum °C/min, **then** set and
      enable **S8** at ~2× it. Do not guess this number.
- [ ] Work `docs/GUARD_TEST_MATRIX.md` end to end and record every result
      (date, commit, config CRC, outcome).
- [ ] **Confirm no test threshold was left in place** — re-read the config CRC
      from telemetry after the trip tests.
- [ ] Commissioning check that the fitted thermocouple matches `tc_type`, at a
      known soak against a reference instrument. A mismatch is otherwise silent.
- [ ] **Provoke each enabled guard on real hardware and record the result.**
      A guard that has only ever passed a host test is not commissioned.
- [ ] Board-change proposals for the next revision, written up rather than
      forgotten:
  - [ ] **Contactor mirror/feedback contact** into a spare Pico GPIO — the only
        way to detect a welded contactor, currently an unclosable gap
        (`docs/SAFETY_MODEL.md` §7).
  - [ ] **Second, independent safety thermocouple.** One sensor means no
        redundancy and no way to detect a plausible-but-wrong reading.
  - [ ] **Fix the Pico power feed**: VSYS through a Schottky rather than
        back-feeding 3V3, so USB is safe to connect (`docs/HARDWARE.md` §7).
  - [ ] **A Pico → ESP hardware fault line**, the mirror of U1. Today a safety
        trip is invisible to the main board except as elements going cold.
  - [ ] Populate or explicitly remove `R72` and document the required CT type on
        the silkscreen — a current-output CT fitted here reads garbage and
        nothing detects it.

## Phase 10 — Field updates over the isolated link

Last, deliberately. A bootloader is new code in the one component with nothing
behind it, and it is only defensible because SWD sits underneath as the recovery
path. Full design: [`docs/BOOTLOADER.md`](docs/BOOTLOADER.md), wire contract and
interlocks: [`../CommonFW/docs/UPDATE_PROTOCOL.md`](../CommonFW/docs/UPDATE_PROTOCOL.md).

**Decide the flash layout before programming the first board** — the bootloader
is written once over SWD and never updated in the field, so its region, the
metadata format and the slot boundaries are effectively permanent.

- [ ] **10.0 Measure the isolated link's error rate at 115200** over a sustained
      multi-megabyte transfer. The TCMT1109 optocouplers are the bandwidth limit
      and nobody has characterised them. A 5 % frame loss turns a 35 s update
      into minutes, because retries cost 200 ms each up to ten times.
- [x] 10.1 Confirm the module's actual flash size on real hardware. **2026-08-17:**
      2MB, from `PICO_BOARD=pico`'s pico-sdk board definition
      (`PICO_FLASH_SIZE_BYTES = 2 * 1024 * 1024`, `boards/pico.h`) — not a
      bench measurement. A1 is a stock Raspberry Pi Pico
      (`firmware/SaftyFW/CMakeLists.txt`, confirmed `PICO_BOARD=pico`, not
      `pico_w`/`pico2`/a custom board file), so its onboard flash size is a
      fixed hardware fact of that board, not a sourcing ambiguity like the
      ESP32-S3 third-party module's flash was. **Distinct from 10.0** (measuring
      the isolated link's *error rate* over UART), which still requires
      physical bench hardware and remains unstarted.
- [x] 10.2 Freeze the flash layout and the metadata format, with a
      `format_version` that refuses the unrecognised. Reserve the signature
      field and public-key space now even though signing ships off —
      `BOOTLOADER.md` §6. **2026-08-17**: `bootloader/flash_layout.h` and
      `bootloader/metadata.h`/`.c` frozen and host-tested (204/204,
      `test/test_bootloader_metadata.c`) earlier this session; `bootloader/main.c`
      now implements against that frozen layout/format rather than just
      declaring it. Signature/public-key space reservation itself is
      unchanged from before this pass — still a §6 TODO, not newly addressed
      here.
- [~] 10.3 Bootloader: GPIO6 low as the first statement; double-buffered CRC'd
      metadata; active-slot CRC on **every** boot; `boot_attempts` fallback.
      Never writes its own region or the config partition. **2026-08-17,
      `bootloader/main.c`**: GPIO6 low as the literal first statement;
      metadata read via XIP and validated with the frozen
      `bootloader_metadata_find_latest()`; `bootloader_decide_boot()`/
      `bootloader_decide_after_crc_fail()` drive the `boot_attempts`-fallback
      and CRC-retry logic; `persist_metadata()` writes through the frozen
      log-append API, `seq` incremented exactly once outside the pure
      decision functions; never touches anything but
      `BOOTLOADER_METADATA_FLASH_OFFSET`/`_SIZE`. Build-verified
      (arm-none-eabi-gcc 14.2.1 / pico-sdk 2.1.1, bare Ninja build, zero
      warnings under `-Wall -Wextra -Werror`), **not hardware-verified** — no
      RP2040/probe attached to the build machine. Marked `[~]` rather than
      `[x]` because recovery mode (10.4) is beacon-only, not the full frame
      subset this item's own wording implies is complete end to end.
- [~] 10.4 Recovery mode: UART1 only, GPIO6 low, minimal frame subset, no
      timeout out of it. This is what makes a failed update recoverable without
      a probe. **2026-08-17, `bootloader/main.c`'s `enter_recovery()`**: UART1
      brought up at 115200 8N1 (plain `hardware/uart.h`, no PIO/inversion,
      mirroring `src/tasks/uart_owner.c`'s init pattern), GPIO6 left low and
      never re-touched, loops forever with no timeout out. **Beacon only** —
      sends a raw distinctive marker byte sequence every ~1s, not a framed
      `UPDATE_STATUS`. Does **not** implement `UPDATE_BEGIN`/`UPDATE_DATA`/
      `UPDATE_END`/`UPDATE_ABORT` frame handling (receiving/writing image data
      into flash) — deliberately out of scope this pass per the coordinator's
      scope decision; that is real streaming-flash-write logic for its own
      dedicated pass. Marked `[~]`, not `[x]`, for exactly that reason.
- [~] 10.5 Bootloader-only build, flashed and verified over SWD independently of
      any application. **Build only** — **2026-08-17**: `cmake -G Ninja -B build`
      + `cmake --build build` from `firmware/SaftyFW/bootloader/` succeeds
      clean (arm-none-eabi-gcc 14.2.1, pico-sdk 2.1.1, Ninja), producing
      `saftyfw_bootloader.elf` / `.bin` — **9764 bytes** of flash `text`
      against the ~64K (`BOOTLOADER_FLASH_SIZE` = 65280 B) budget, ~15% used.
      `pico_add_extra_outputs()` left disabled in `bootloader/CMakeLists.txt`
      for the same reason `../CMakeLists.txt` disables it (no host C/C++
      compiler available to build `picotool` from source in this
      environment) — the plain `.elf` is what OpenOCD flashes anyway.
      **Not flashed or verified over SWD** — no debug probe or RP2040
      hardware attached to the machine this was built on. Checked off for the
      build half only; the "flashed and verified over SWD" half of this
      item's own wording is still open.
- [x] 10.6 Application side: staged writes to the inactive slot, flash routines
      and interruptible ISRs in RAM, core 1 parked, watchdog handled across
      multi-hundred-millisecond erases. **Link/build prerequisite only —
      2026-08-17**: the application can now be BUILT and LINKED to run from
      either slot at all, which nothing produced before this pass (the only
      prior application build, `SaftyFW`, links at pico-sdk's stock address,
      which collides with the bootloader's own 0x10000100). Added
      `firmware/SaftyFW/bootloader/app_slot.ld.in`, a custom linker script
      template derived from pico-sdk 2.x's own
      `pico_crt0/rp2040/memmap_default.ld` with the `.boot2` output section
      removed (`.boot2` input sections explicitly `/DISCARD/`ed rather than
      left as an orphan-section hazard — there is exactly one boot2 in the
      whole image, already part of the bootloader's own build at flash
      offset 0) and `FLASH`'s `ORIGIN` parameterized via a
      `configure_file()` `@SAFTYFW_APP_FLASH_ORIGIN@` placeholder;
      `firmware/SaftyFW/CMakeLists.txt` extended with a
      `saftyfw_add_slot_executable()` function (factoring the application's
      source list into `SAFTYFW_APP_SOURCES`, shared by all three targets)
      producing two new targets, `SaftyFW_slotA` (origin `0x10011000` =
      `XIP_BASE` + `BOOTLOADER_SLOT_A_FLASH_OFFSET`) and `SaftyFW_slotB`
      (origin `0x100E1000` = `XIP_BASE` + `BOOTLOADER_SLOT_B_FLASH_OFFSET`),
      linking the exact same sources/libraries as the default `SaftyFW`
      target, which is unchanged. Build-verified from scratch (arm-none-eabi-
      gcc 14.2.1 / pico-sdk 2.1.1, Ninja, zero warnings under
      `-Wall -Wextra -Werror`) and confirmed by reading the linked ELFs
      (`readelf -l`/`-S`, `nm`): `SaftyFW_slotA.elf`'s `__VECTOR_TABLE` /
      `__flash_binary_start` sit at exactly `0x10011000`,
      `SaftyFW_slotB.elf`'s at exactly `0x100e1000`, both with no `.boot2`
      section present anywhere in the output. Each slot image is ~46.6 KB of
      flash (`arm-none-eabi-size`: 47752 B `text`), well under the 832 KB
      (`BOOTLOADER_SLOT_FLASH_SIZE`) budget (~5.6% used).
      **`*** MANUAL SYNC HAZARD ***`**: `app_slot.ld.in`'s `FLASH` `LENGTH`
      (hardcoded `0xD0000`) and the two `saftyfw_add_slot_executable()` call
      sites' origin literals in `CMakeLists.txt` are hand-copied from
      `bootloader/flash_layout.h`'s `BOOTLOADER_SLOT_FLASH_SIZE` /
      `_SLOT_A_FLASH_OFFSET` / `_SLOT_B_FLASH_OFFSET` — a linker script
      cannot `#include` a C header, so nothing enforces these three stay in
      sync today. A CI check that parses `flash_layout.h`'s macros and
      asserts the `.ld.in` and `CMakeLists.txt` literals agree would close
      this gap; not built this pass, flagged for a human. **Still missing,
      real future work**: the actual runtime flash-write staging logic
      itself — nothing calls `flash_range_erase()`/`flash_range_program()`
      from the application yet, no RAM-resident flash routines, no core-1
      parking, no watchdog-feeding across the erase. This pass is purely a
      build/link-time capability, not the staging mechanism. `docs/
      BOOTLOADER.md` section 5's "Writing flash while running from flash"
      subsection was re-read and left unchanged — it is honestly still all
      open (it is about the write-time mechanics, not the link-time
      capability this pass adds).
      **2026-08-17, follow-on session — the runtime staging half is now
      built:** `src/tasks/update_task.{h,c}` (new), wired into
      `SAFTYFW_APP_SOURCES` so all three targets (`SaftyFW`, `SaftyFW_slotA`,
      `SaftyFW_slotB`) build-verified from scratch, zero warnings under
      `-Wall -Wextra -Werror` (arm-none-eabi-gcc 14.2.1 / pico-sdk 2.1.1 /
      Ninja). Real `flash_safe_execute()`-wrapped `flash_range_erase()` in
      `FLASH_BLOCK_SIZE` (64K) units with a `watchdog_task_checkin()`
      immediately before and after each block (never during — nothing can
      run while flash is mid-erase); real `flash_range_program()` for
      `UPDATE_DATA` chunks via a read-modify-write page buffer (see below);
      staged writes go ONLY to `update_receiver_handle_begin()`'s own
      `target_slot` — never the slot this image is itself running from.
      **The 248-byte chunk / 256-byte flash page mismatch, resolved:**
      `UPDATE_CHUNK_LEN` (248) is not a multiple of `FLASH_PAGE_SIZE` (256),
      and successive chunks are 248 bytes apart, so a chunk essentially never
      lands on a page boundary and can span two pages —
      `update_task_program_chunk()` reads back the one or two full pages a
      chunk touches from the XIP alias (valid because the whole target slot
      was already erased before any `UPDATE_DATA` is accepted), overlays the
      chunk's bytes at the right sub-page offset, and reprograms the whole
      page range — never flips a flash bit 0→1 without an erase, since
      previously-landed neighbour bytes are re-written with their own
      unchanged value and only genuinely-erased (0xFF) bytes go 1→0 for the
      first time. See `update_task.c`'s header comment for the full
      reasoning, including why this did NOT need `__not_in_flash_func()` on
      the erase/program callbacks themselves — verified against the real
      vendored `pico-sdk` source (`src/rp2_common/pico_flash/flash.c`) this
      session: `flash_safe_execute()`'s FreeRTOS-SMP helper disables
      interrupts on BOTH cores for the whole callback duration (a stronger
      guarantee than "the ISRs are RAM-resident"), and `hardware/flash.h`'s
      own erase/program functions carry their own SRAM XIP-reentry
      trampoline, so ordinary flash-resident calling code is safe by
      construction. `link_task.c` gained a small non-blocking FreeRTOS queue
      (`update_task_handle_begin/_data/_end/_abort()`, `xQueueSend` with a
      zero timeout, drops silently on a full queue) so `link_task`'s own RX
      loop is never blocked by update_task's flash I/O — see `update_task.h`'s
      header comment for the full design.
- [x] 10.7 Whole-slot CRC verified by reading **back from flash** — the only
      check that catches a write that reported success and did not land.
      **2026-08-17:** `update_task_process_end()` reads the target slot back
      via the XIP-mapped alias and computes `bootloader_crc32()` over
      `[0, length)`, compared against the BEGIN header's own `crc32` (not
      against anything the receive-side bitmap merely believes arrived).
- [~] 10.8 `PENDING_VERIFY` cleared only after config CRC, a plausible
      thermocouple reading, ADC sampling, every task checked in, and one
      acknowledged telemetry frame. **Not at the end of `main()`** — an image
      that boots but cannot read its thermocouple is worse than the old one.
      **2026-08-17:** the gate is fully wired —
      `update_task_confirm_tick()` (a periodic tick, not an end-of-`main()`
      check) gathers real evidence for four of the five items
      (`thermo_task_get_snapshot()`'s `valid`; a freshness proxy on
      `current_task_get_snapshot()`'s timestamp, since `current_task.h` has
      no direct "is sampling running" boolean; the new
      `watchdog_task_all_checked_in_since_boot()`, a cumulative-since-boot
      bitmask added this session distinct from `watchdog_task`'s own
      periodic feed-window mask; and `link_task_get_status_tx_ok_count()`,
      also added this session, standing in for "acknowledged" — this link's
      design has no ACK for the Pico to wait on at all, per `confirm.h`'s own
      header comment) and, once `update_confirm_missing()` returns 0, writes
      the running slot's metadata to `BOOTLOADER_SLOT_VALID` exactly once via
      the same `flash_safe_execute()`-wrapped persist pattern `UPDATE_END`
      uses. **Marked `[~]`, not `[x]`, because `config_crc_ok` is
      permanently `false`** — no `config_store` exists yet (Phase 9), and
      `confirm.h`'s own discipline forbids a caller from ever passing `true`
      for a check it cannot actually perform ("unknown must never read as
      confirmed-good"). This means `update_confirm_missing()` can never
      reach 0 and no slot can be marked `VALID` in this build — an honest
      consequence of wiring the gate against a config store that does not
      exist yet, not a bug to paper over with a fake CRC check. The gate
      starts working the moment Phase 9 lands, with no further change needed
      in `update_task.c`.
- [x] 10.8b Image header validated **before the first erase**: magic, target,
      header version, protocol version, `min_compatible`, length, CRC32. Without
      it, a `KilnFW` image uploaded to the Pico endpoint erases the staging slot
      before the mistake is noticed.
      **2026-08-17:** now validated against real incoming `UPDATE_BEGIN`
      frames, not just in isolation — `update_task_process_begin()` calls the
      frozen `update_image_header_unpack()`/`update_receiver_handle_begin()`
      (which itself checks preconditions, THEN the header, per its own
      documented order) before `update_task_erase_slot()` is ever reached.
- [~] 10.8c `UPDATE_DATA` unacknowledged, with a received-range bitmap and a gap
      report every 500 ms — stop-and-wait leaves the wire idle most of every
      round trip and turns a lossy link into a retry storm.
      **2026-08-17:** `UPDATE_DATA` frames are handled exactly as received —
      no ACK is ever sent — and `update_task_periodic_status()` emits a
      `UPDATE_STATUS` (wire layout invented this session, since
      `UPDATE_PROTOCOL.md` names the frame but never specifies its payload —
      see `update_task.c`'s header comment for the chosen bytes) every
      500 ms while a transfer is active, carrying a window of up to 32
      missing-chunk indices from `update_received_ranges_find_gaps()`,
      advancing a cursor across calls per that function's own documented
      scheme. The retransmission-round cap (`update_retransmit_should_continue()`)
      is wired: a full gap-report cursor pass that still finds a gap counts
      as one round, and exceeding the cap aborts the transfer (reverts the
      target slot's metadata to `EMPTY`) and reports it distinctly
      (`UPDATE_STATUS_ERR_RETRANSMIT_CAP`) rather than looping forever.
      Marked `[~]` rather than `[x]` for one honest caveat: because a full
      gap-report pass can take many status-frame periods to cycle through a
      large image's chunk count, one "round" in this implementation can take
      significantly longer than the ~2 s cadence `UPDATE_PROTOCOL.md`'s
      throughput section seems to assume when it talks about retries — the
      cap (10 rounds) is still a genuine, working backstop, just a slower
      one than a literal reading of the doc might suggest. Not measured
      against a real link (item 10.0 is still open).
- [x] 10.9 The Pico independently enforces its own preconditions: relay open, no
      trip pending, temperature below the ceiling. It does not take the ESP's
      word for any of them.
      **2026-08-17:** `update_task_gather_preconditions()` pulls real evidence
      — `safety_core_get_output_status()`'s `!relay_energized`,
      `safety_core_get_diag_status()`'s `trip_reason == SAFETY_TRIP_NONE`,
      and `thermo_task_get_snapshot()`'s `valid && !isnan(tc_c) && tc_c <
      UPDATE_TASK_TEMP_CEILING_C` (100 °C, `UPDATE_PROTOCOL.md` section 1's
      documented default; no `config_store` yet to source a real
      per-installation ceiling from, Phase 9) — and
      `update_task_process_begin()` refuses (with the specific unmet
      precondition named in the `UPDATE_STATUS` reply, never a generic
      failure) before the image header is even inspected, matching
      `update_receiver_handle_begin()`'s own documented check order.
- [ ] 10.10 Verification: power cut during erase, during streaming, and during
      the metadata write; corrupt slot rejected; bad-but-booting image rolled
      back; both slots invalidated and recovered over the link with no probe.
      **Explicitly out of scope for the 2026-08-17 follow-on session that
      built 10.6/10.7/10.8/10.8b/10.8c/10.9's runtime code** — this needs a
      physical RP2040 board and a debug probe, neither of which exists on
      the machine that session ran on. Left unchecked deliberately rather
      than simulated or faked.

---

## Deliberately not doing

Recorded so these do not get re-proposed as oversights.

- **No PID, no profiles, no control of any kind.** That is `KilnFW`'s job. Every
  feature added here is a feature that can fail in the one place with no backup.
- **No filesystem, no network, no display, no USB console.**
- **The bootloader never updates itself.** It is the recovery path; a
  recovery path that the thing it recovers from can overwrite is not one.
  Changing it is a bench operation over SWD.
- **No dynamic allocation after init.**
- **No auto-recovery from a latched trip.** "The temperature came back down" is
  evidence the trip worked, not evidence the fault is gone.
- **No I2C.** GPIO7/8 have pull-ups and reach J7, but nothing answers. Left
  uninitialised rather than half-initialised.
- **No RP2040 ADC INL correction.** Real, documented, and irrelevant against
  thresholds with tens-of-percent margins — correcting it would imply a
  precision this channel does not have (`docs/CURRENT_SENSE.md` §4).
- **No attempt to reconstruct duty cycle from the current signal.** With a 1 s
  peak-hold in front of the ADC the information is not recoverable; that is what
  `relay_recent_mask` is for.
