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
- [ ] **0.1 Fix the swapped safety-UART pins in `KilnFW`.** `KILNCTL_SAFETY_TX_IO`
      must become **4** and `KILNCTL_SAFETY_RX_IO` must become **5**
      (`firmware/KilnFW/App/drivers/Kconfig:189-200`, plus `sdkconfig`). Move the internal
      pull-up to GPIO5. **The link cannot work in either direction until this is
      done.** Full trace and the independent R15 confirmation: `docs/HARDWARE.md` §1.
- [ ] **0.2 Add `UART_PROTO_MSG_BROADCAST = 0x04`** to
      `firmware/KilnFW/App/drivers/espInterfaces/uart_protocol.{c,h}` — send without
      waiting for an ACK, receive without sending one. Required because the
      safety processor never transmits an ACK; without it every ESP push costs
      10 retries × 50 ms. See `../CommonFW/docs/LINK_PROTOCOL.md` §1.
- [ ] **0.3 Correct `firmware/KilnFW/docs/SAFETY_LINK.md` and `firmware/KilnFW/docs/HARDWARE.md`.**
      Both describe the optocoupler data directions backwards (U3 is drawn
      mirrored relative to U1/U2). Do this *with* 0.1, or the next person will
      "fix" 0.1 back.
- [ ] **0.4 Regenerate or delete `hardware/mainBoard/kiln.net`.** Dated 2026-07-19,
      sources a pre-move path, and disagrees with the current schematic in at
      least three places. It is the likely origin of the errors in 0.3.
      See `docs/HARDWARE.md` §10.
- [ ] **0.4b Move `tools/PcTools/` → `PcTools/`** and add the **GPIO probe**
      (`../../tools/PcTools/TODO.md` §1). The probe is what makes 0.0 runnable without
      building a one-off firmware, and it is reusable for every future
      "is this net where the schematic says" question. Default off, hard
      deny-list including **GPIO6**.
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

- [ ] `firmware/CommonFW/` created; `kilnlink` CMake target consumable by pico-sdk
- [ ] ESP-IDF component wrapper in `firmware/KilnFW/components/kilnlink/`
- [ ] `kilnlink_version.h` owns `KILNLINK_PROTOCOL_VERSION`; `KilnFW`'s
      `UART_PROTOCOL_VERSION` becomes an alias, not a second number
- [ ] Shared ids split out of `uart_task_ids.h`; PC-link ids left behind
- [ ] `kilnlink_frame` / `_context` / `_status` codecs, all pure and bounds-checked
- [ ] Host tests + `test/vectors/`, including hostile inputs
- [ ] `pc_tools` consuming the same vectors — it is the **third** implementation
- [ ] `KilnFW`'s `uart_protocol.c` delegating framing/CRC, proven byte-identical
      to the pre-refactor output **before** the old code is deleted
- [ ] CI grep: no CRC or byte-stuffing implementation outside `CommonFW`

## Phase 2 — Skeleton

- [ ] pico-sdk + FreeRTOS-Kernel (SMP) CMake project, `-Wall -Wextra -Werror`.
- [ ] `main()` drives **GPIO6 low as its first statement**, before any init.
- [ ] Hardware watchdog, 1 s, fed by `watchdog_task` only when all tasks check in.
      **`pause_on_debug` true in dev builds, false in release.**
- [ ] `boot_reason` captured and latched at startup; **latch the trip reason in
      the watchdog scratch registers** so a watchdog reset does not lose why.
- [ ] `flash_safe_execute()` for every config write, with the multicore lockout,
      and `__not_in_flash_func` on any ISR that can fire during one.
      `docs/ARCHITECTURE.md` §8.
- [ ] **Assert `configUSE_CORE_AFFINITY` at build time** — without it
      `vTaskCoreAffinitySet()` silently does nothing and the core isolation
      quietly evaporates.
- [ ] Task skeletons at the priorities and core affinities in
      `docs/ARCHITECTURE.md` §4 — link work on core 0, everything that can trip
      on core 1.
- [ ] Blink-equivalent proof of life over SWD/RTT.
- [ ] **Log transport**: `kilnlink` LOG frames (task 5) as primary, RTT over SWD
      as secondary. `docs/ARCHITECTURE.md` §1.
- [ ] `SAFTYFW_ENABLE_USB_STDIO` compile flag, **default off, debug builds
      only** — it is a build option, not a runtime GUI toggle.
- [ ] **Reserve TX ring capacity for telemetry**; log frames may only use what
      remains, and are dropped at enqueue above the watermark. A verbose log
      must never be able to displace telemetry and stop a firing.
- [ ] Dropped-log-frame counter in the diagnostic frame.
- [ ] Runtime log-level command over the link, **default warnings+errors only**.
- [ ] **CI grep check: `safety_core.c` must not include the link header, and
      `link_task.c` must not reference GPIO6.** Cheap, and it keeps the isolation
      property from eroding.

## Phase 3 — Thermocouple

- [ ] Port `firmware/KilnFW/App/drivers/MAX31856.c`. **Port it, do not rewrite it** — same
      part, same registers, and `firmware/KilnFW/docs/MAX31856.md` already documents the
      traps.
- [ ] `spi_owner` request-queue task; `CS0` driven as a plain GPIO.
- [ ] `~DRDY` (GPIO12) as a real **interrupt** — unlike the main board, this is a
      direct Pico GPIO, so do not poll it.
- [ ] `thermo_task` publishing `thermo_snapshot_t`; **NaN, never 0, when invalid**.
- [ ] **Choose `tc_type` deliberately, per sensor** — type K is marginal above
      ~1150 °C and green-rots *low* in reduction. Type S/R for a chamber-mounted
      sensor on a cone-10 kiln. `docs/THERMOCOUPLE.md` §2.
- [ ] **Per-type plausibility ranges**, driven from the configured type — a
      range hard-coded to type K misfires on every other type.
- [ ] Set the `MASK` register explicitly — **reset default `FFh` masks every
      fault**, leaving `~FAULT` permanently inactive.
- [ ] **`~DRDY` silence detection** → S5. This is the stopped-converting failure
      `KilnFW` structurally cannot see.
- [ ] Bench: read ambient with the thermocouple attached; confirm open-circuit
      reports `THERMO_FAULT_OPEN` rather than a plausible number.

## Phase 4 — Guards, host-tested, no relay yet

- [ ] `safety_guards.c` as a **pure function** — no RTOS, no SDK, no I/O,
      `dt_s` passed in.
- [ ] Implement **S1** (absolute over-temp), **S5** (sensor invalid, graduated),
      **S7** (E-stop), **S11** (frozen TC), **S12** (cold junction / enclosure).
      These need no link and no current calibration, and S1 alone justifies the
      board. S11 and S12 are free — both read data S1 already fetches.
- [ ] `discrete_task`: debounce E-stop (50 ms) and `mainFault` (200 ms).
- [ ] Host test harness (MSVC, no SDK), mirroring `firmware/KilnFW/App/test/`.
- [ ] **Write the nuisance-rejection tests before the trip tests.** A 900 ms
      sensor dropout must *not* trip S5; a single noisy SPI read must not either.
- [ ] Reuse `firmware/KilnFW/App/test/sim_plant.c` for realistic thermal traces.

## Phase 5 — Relay authority

- [ ] `relay_owner` — the **only** code in the build that writes GPIO6.
- [ ] Latching trip semantics; clear refused while the condition still holds.
- [ ] GRACE → ARMED state machine, `startup_grace_s` = 60 s.
- [ ] **Bench-verify the safe state four ways**: power-on, watchdog reset,
      brownout, and firmware halted at a breakpoint. K4 must be de-energized in
      all four.
- [ ] Verify **de-energized K4 opens the contactor**, on the real interlock
      wiring from 0.7. Getting this backwards passes every bench test and fails
      dangerous.

## Phase 6 — Current sensing

- [ ] `adc_owner`: round-robin ADC0/1/2, 16× oversample, 20 Hz/channel.
- [ ] Peak-envelope conversion per `docs/CURRENT_SENSE.md` §2. **No RMS
      accumulator, no DMA capture** — the front end has already demodulated.
- [ ] Clip detection → `CURRENT_FLAG_CLIPPED`, reported as a state, never as a
      number.
- [ ] `zero_counts` measured at runtime after ≥ 5 min with no relay commanded on;
      drift reported as a diagnostic.
- [ ] **Run the full commissioning check in `docs/CURRENT_SENSE.md` §5** — in
      particular step 2, one relay at a time, confirming each CT maps to the
      channel you think it does.
- [ ] **No over-current / under-current guard**, by design — these channels are
      a load-active detector and a power estimator. Fuses and breakers own
      over-current (`docs/SAFETY_MODEL.md` §3).
- [ ] Power estimate: `i_conducting_a`, `conduction_fraction` over a 120 s
      window, and `p_avg_w` when `mains_voltage_v` is configured. **No guard may
      read any of it** (`docs/CURRENT_SENSE.md` §3b).
- [ ] Implement **S9** (trip ineffective / contactor welded). Needs only the
      current channels and the relay state, so it lands here — and it is the
      guard that catches an interlock wired to the wrong J10 contact, which
      passes every other test.
- [ ] ⚠️ **S3 and S4 stay disabled until the CT channel mapping is confirmed.**
      A correlation guard fed by a mis-mapped CT trips on healthy firings and
      stays quiet on the failure it exists to catch.

## Phase 7 — The link

- [ ] `uart_frame.c`: `0x7E` framing, `0x7D`/`^0x20` stuffing,
      CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF, MSB-first, no reflect/xorout).
      Byte-compatible with `uart_protocol.c`.
- [ ] **Plain hardware UART, no inversion, no PIO.** The ESP inverts; adding a
      second inversion here kills the link.
- [ ] Receiver hardening: resync on `0x7E` from any state, bounded buffers,
      break tolerated as "peer not up", **no allocation**.
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
      `CommonFW`, one definition, both firmwares including it
- [ ] 7b.2 `ANNOUNCE_VERSION` = `0x0F` sent by the ESP unprompted at boot, on
      retry, and on every `boot_id` change (`KilnFW` work)
- [ ] 7b.3 `min_compatible` added to `FW_VERSION` at a fixed offset; version
      fields read and compared **before** anything after them is parsed
- [ ] 7b.4 Compatibility evaluated in **both** directions
- [ ] 7b.5 Mismatch sets `DEGRADED_NO_CONTEXT`: context frames discarded
      unparsed, context-free guards still commanding the relay, context-dependent
      guards reported disabled, **no trip latched**
- [ ] 7b.6 Telemetry keeps flowing during a mismatch — it is the only way the ESP
      can display the problem or push the fix
- [ ] 7b.7 **Compatibility floor**: framing, `ANNOUNCE_VERSION`, `FW_VERSION` and
      the `UPDATE_*` frames work regardless of version. Ids `0x00`–`0x0F`
      reserved; those layouts may be appended to, never reordered or resized.
      Without this a mismatch makes the field-update path unusable and every fix
      needs a debug probe
- [ ] 7b.8 Host test: every combination of older/newer/equal on both sides,
      including a peer that announces a `min_compatible` above its own version

## Phase 8 — Telemetry (required)

The ESP will not permit heating without this. See `../CommonFW/docs/LINK_PROTOCOL.md` §8.

- [ ] Non-blocking TX ring. **Drop on full, increment a counter, never block.**
- [ ] Emit the **existing 23-byte** status frame unchanged, every 500 ms — this
      is what lets the Pico be validated against an unmodified `KilnFW`.
- [ ] Emit `SAFETY_CMD_DIAG` (0x08), additive; an old ESP ignores it.
- [ ] `build_info.h` generated on every build (git commit, dirty, timestamp).
      **Unknown must map to `dirty = 1`** — an uncommitted build must never
      report itself clean.
- [ ] Emit `SAFETY_CMD_FW_VERSION` (0x0B) on request **and unsolicited at boot**;
      include `boot_id`, `config_version` and the **active config CRC**.
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
