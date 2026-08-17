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
- [ ] **0.0b Build the coordinated two-board GPIO test rig** — a GPIO probe on
      *both* processors plus the PC script that drives them, reaching each by a
      path that is **not** the link under test (ESP over USB serial, Pico over
      SWD). `../../tools/PcTools/TODO.md` §1/§1b/§1c.
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
      passes clean under `/W4 /WX`, 80/80 checks, 2026-08-16.
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
- [ ] Parse `SAFETY_CMD_PUSH_CONTEXT` (0x07) → `context_snapshot_t`.
- [ ] `boot_id` change resets every correlation window.
- [ ] **Honour the `SIM_PLANT` flag**: disable S2/S3/S4 and warn persistently.
      A safety processor correlating against fabricated temperatures is worse
      than one with no context at all.
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

- [ ] 7b.1 `KILNLINK_PROTOCOL_VERSION` and `KILNLINK_MIN_COMPATIBLE` in
      `CommonFW`, one definition, both firmwares including it -- **half done**:
      `KILNLINK_MIN_COMPATIBLE` added to `CommonFW/include/kilnlink/kilnlink_version.h`
      and `SaftyFW` includes/uses both; `KilnFW` does not include it yet
      (out of scope this pass, `firmware/KilnFW/` untouched)
- [ ] 7b.2 `ANNOUNCE_VERSION` = `0x0F` sent by the ESP unprompted at boot, on
      retry, and on every `boot_id` change (`KilnFW` work -- not this pass)
- [x] 7b.3 `min_compatible` added to `FW_VERSION` at a fixed offset; version
      fields read and compared **before** anything after them is parsed --
      `link_frame_pack_fw_version()` (bytes 3..4) and
      `link_task_handle_announce_version()` (reads bytes 1..4 before anything
      else, bails on `frame->length < 5`)
- [x] 7b.4 Compatibility evaluated in **both** directions --
      `link_frame_versions_compatible()`, host-sanity-checked against five
      combinations (equal, newer self, self-raised-floor, peer-below-floor,
      self-below-peer's-floor)
- [ ] 7b.5 Mismatch sets `DEGRADED_NO_CONTEXT`: context frames discarded
      unparsed, context-free guards still commanding the relay, context-dependent
      guards reported disabled, **no trip latched** -- **partial**: the flag
      (`link_task_get_degraded_no_context()`) is set/cleared correctly and
      never touches `relay_owner`/`boot_reason` (verified: no such call
      exists in `link_task.c`, and `tools/check_isolation.ps1` passes). Left
      unchecked because there is nothing yet to discard (`PUSH_CONTEXT`
      unparsed) or disable (no context-dependent guard exists — Phase 4 only
      built S1/S5/S7/S11/S12) — the mechanism exists, the wiring it feeds
      does not yet.
- [x] 7b.6 Telemetry keeps flowing during a mismatch — it is the only way the ESP
      can display the problem or push the fix. `link_task`'s TX cadence
      (status + `FW_VERSION`) never checks `s_degraded_no_context`.
- [ ] 7b.7 **Compatibility floor**: framing, `ANNOUNCE_VERSION`, `FW_VERSION` and
      the `UPDATE_*` frames work regardless of version. Ids `0x00`–`0x0F`
      reserved; those layouts may be appended to, never reordered or resized.
      Without this a mismatch makes the field-update path unusable and every fix
      needs a debug probe
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
- [ ] 10.1 Confirm the module's actual flash size on real hardware.
- [ ] 10.2 Freeze the flash layout and the metadata format, with a
      `format_version` that refuses the unrecognised. Reserve the signature
      field and public-key space now even though signing ships off —
      `BOOTLOADER.md` §6.
- [ ] 10.3 Bootloader: GPIO6 low as the first statement; double-buffered CRC'd
      metadata; active-slot CRC on **every** boot; `boot_attempts` fallback.
      Never writes its own region or the config partition.
- [ ] 10.4 Recovery mode: UART1 only, GPIO6 low, minimal frame subset, no
      timeout out of it. This is what makes a failed update recoverable without
      a probe.
- [ ] 10.5 Bootloader-only build, flashed and verified over SWD independently of
      any application.
- [ ] 10.6 Application side: staged writes to the inactive slot, flash routines
      and interruptible ISRs in RAM, core 1 parked, watchdog handled across
      multi-hundred-millisecond erases.
- [ ] 10.7 Whole-slot CRC verified by reading **back from flash** — the only
      check that catches a write that reported success and did not land.
- [ ] 10.8 `PENDING_VERIFY` cleared only after config CRC, a plausible
      thermocouple reading, ADC sampling, every task checked in, and one
      acknowledged telemetry frame. **Not at the end of `main()`** — an image
      that boots but cannot read its thermocouple is worse than the old one.
- [ ] 10.8b Image header validated **before the first erase**: magic, target,
      header version, protocol version, `min_compatible`, length, CRC32. Without
      it, a `KilnFW` image uploaded to the Pico endpoint erases the staging slot
      before the mistake is noticed.
- [ ] 10.8c `UPDATE_DATA` unacknowledged, with a received-range bitmap and a gap
      report every 500 ms — stop-and-wait leaves the wire idle most of every
      round trip and turns a lossy link into a retry storm.
- [ ] 10.9 The Pico independently enforces its own preconditions: relay open, no
      trip pending, temperature below the ceiling. It does not take the ESP's
      word for any of them.
- [ ] 10.10 Verification: power cut during erase, during streaming, and during
      the metadata write; corrupt slot rejected; bad-but-booting image rolled
      back; both slots invalidated and recovered over the link with no probe.

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
