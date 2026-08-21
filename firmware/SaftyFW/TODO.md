# TODO — Safety Processor Firmware

> **Status:** planning · **Last reviewed:** 2026-08-21
> **Keep this file current.** Tick items as they land, and keep "built" and
> "verified on hardware" distinct — `firmware/KilnFW/docs/PROJECT_STATUS.md` is the model
> for that discipline. Finished items are removed from this file (moved into
> the relevant `docs/*.md` if the reasoning is worth keeping, deleted if it was
> only working notes) rather than left here as a completed marker — see this
> repo's plan-doc convention.

Top-level ordering across both processors lives in [`../../ROADMAP.md`](../../ROADMAP.md);
this file owns the safety-processor detail. Phases here map to roadmap milestones
M0 and M2–M6.

Cross-references `docs/SAFETY_MODEL.md` (what trips and why),
`../CommonFW/docs/LINK_PROTOCOL.md` (the wire, both ends), `docs/HARDWARE.md` (the traced
board), `docs/CURRENT_SENSE.md` (the analog front end),
`docs/THERMOCOUPLE.md` (the sensor and its type), `docs/CONFIG_REFERENCE.md`
(every tunable) and `docs/GUARD_TEST_MATRIX.md` (how each guard is proven)
throughout.

**Hardware-gated, repo-wide**: the ESP32<->RP2040 isolated link has never been
proven end to end — `link_status` reports `frames_received: 0`, the Pico
console emits zero bytes, and the TCMT1109 optocouplers are suspect. Every item
below whose real verification depends on that link is open no matter how
complete the code and host tests are, and is marked accordingly.

---

## Phase 0 — Blockers (do these first, in this order)

- [ ] **0.0 Run the Tier 0 pin test** (`docs/HARDWARE.md` §1): both GPIO4 and
      GPIO5 as inputs with internal **pull-downs**, read them. The pin that
      reads HIGH is the one carrying R15 and is therefore the ESP's **RX**.
      Needs no safety-domain power, no Pico, no probe. ⚠️ **Driving one pin and
      reading the other proves nothing** — they are not connected to each other.
      The coordinated two-board rig to run this exists
      (`tools/PcTools/TODO.md` §1c) but has not resolved the electrical half —
      see that file for the live `write()` bug blocking it.
- [ ] **0.5a Fit a 3-pin DEBUG header and bring GP16/GP17 out** before A1 is
      soldered down — both are far cheaper at build time than after.
      `docs/HARDWARE.md` §7b.
- [ ] **0.5b Consider a RUN (pin 30) reset wire** to the probe or a button while
      the board is still open. RUN is currently unconnected; OpenOCD's SWD reset
      is sufficient without it.
- [ ] **0.6 Redefine `SAFETY_FAULT_SRC_SAFETY_LINK`** as "no telemetry frame
      within 1.5 s", feeding `relay_authority_on_blocked()`, plus a 30 s
      firing-abort. This is what makes *the safety processor must be alive to
      heat* true. `../CommonFW/docs/LINK_PROTOCOL.md` §8. The ESP side today
      still reuses the older link-staleness check (`safety_link_up_locked()`),
      not this redefinition. ⚠️ It also stops a main board with no Pico fitted
      from heating at all — bench work will need
      `safety_link_fault_on_link_loss(link, false)`.
- [ ] **0.6b Add the ESP-side `GET_FW_VERSION` request, with retry**, and the
      dashboard/GUI surface for build identity + config CRC. (Receiving an
      unsolicited `FW_VERSION` push is already handled — see Phase 7b — this
      item is the explicit request-with-retry half plus the GUI surface,
      still both open.)
- [ ] **0.7 Confirm the K4 → line-contactor interlock topology** and which J10
      pin is NO vs NC. `docs/HARDWARE.md` §3. **This is a system-wiring decision,
      not a firmware one, and it must be settled before any bench trip test.**
- [ ] **0.8 Confirm the E-stop is wired normally-closed**, or fit a deliberate
      jumper to `GND_Safty`. `docs/HARDWARE.md` §5.

## Phase 1 — `CommonFW`, before either firmware uses it

The link contract is shared code, implemented once. See
[`../CommonFW/README.md`](../CommonFW/README.md) for the full checklist; the
gating items are:

- [ ] Shared ids split out of `uart_task_ids.h`; PC-link ids left behind
- [x] `kilnlink` codecs — all pure and bounds-checked, host-tested, consumed
      by `pc_tools` as the third implementation. Every wire command through
      `0x1A` (`SET_CT_CAL`/`GET_CT_CAL`/`CT_CAL`) now has a codec in
      `firmware/CommonFW/src/`, including `kilnlink_ceiling.c` (SET_FIRING_CEILING)
      and `kilnlink_set_clock.c` (SET_CLOCK) — **this file previously claimed
      those two were "not yet coded"; that was stale.** What is genuinely still
      open is the SaftyFW-side *consumer* wiring for both (Phase 7, below) —
      the codecs exist and are host-tested, but `link_task.c` does not call
      them yet.
- [ ] `KilnFW`'s `uart_protocol.c` delegating framing/CRC, proven byte-identical
      to the pre-refactor output **before** the old code is deleted
- [ ] CI grep: no CRC or byte-stuffing implementation outside `CommonFW`

## Phase 2 — Skeleton

Built and build-verified under the real toolchain (arm-none-eabi-gcc 14.2.1 /
pico-sdk 2.1.1 / FreeRTOS-Kernel RP2040 SMP, Ninja). **Never flashed or run on
real hardware** — no RP2040 attached to any machine this has been built on.

- [ ] `flash_safe_execute()` for every config write — still not started for
      `config_store`'s writes specifically (Phase 9's store exists now but
      this item is about the write path's flash-safety wrapper).
- [ ] Blink-equivalent proof of life over SWD/RTT, and the physical heartbeat
      LED's toggle logic — **neither verified**; no RP2040/debug probe attached
      to any build machine.
- [ ] Runtime log-level command over the link — `log_task_set_level()`/
      `_get_level()` exist and are used internally, but `link_task.c` has no
      RX handler wiring a wire command to them yet.
- [ ] RTT as `SaftyFW`'s secondary log transport — needs the debug probe wiring
      on the bench.

## Phase 3 — Thermocouple

- [~] **Choose `tc_type` deliberately, per sensor** — type K is marginal above
      ~1150 °C and green-rots *low* in reduction; type S/R suits a
      chamber-mounted sensor on a cone-10 kiln (`docs/THERMOCOUPLE.md` §2).
      `tc_type` is a runtime parameter sourced from `config_store`, with a wire
      command (`SAFETY_CMD_SET_CONFIG`) able to set it — but nothing has ever
      written a non-default record, so every board still resolves to type K
      via the safe-default path. **Still not decided**; not a claim that K is
      correct for this kiln.
- [ ] **Per-type plausibility ranges**, driven from the configured type — a
      range hard-coded to type K misfires on every other type. Needs a
      commissioned `tc_type` first.
- [ ] Bench: read ambient with the thermocouple attached; confirm open-circuit
      reports `THERMO_FAULT_OPEN` rather than a plausible number. Phase 9
      commissioning work; no hardware attached to do it yet.

## Phase 4 — Guards, host-tested, no relay yet

`safety_guards.c` implements 12 of `SAFETY_MODEL.md` §4's 13 guards (S1–S7,
S9–S13) as a pure function, host-tested including nuisance-rejection cases.
S8 (implausible rate of rise) remains unimplemented **by design** —
`SAFETY_MODEL.md` §4 says it ships disabled until a real full-power ramp is
logged (Phase 9); there is no defensible threshold to build yet.

- [ ] Reuse `firmware/KilnFW/App/test/sim_plant.c` for realistic thermal traces
      in the guard tests (current tests use synthetic step/ramp sequences,
      which were enough to prove each guard's boundary, but a closed-loop
      trace is still useful for S2/S8 tuning later).

## Phase 5 — Relay authority

- [ ] **Bench-verify the safe state four ways**: power-on, watchdog reset,
      brownout, and firmware halted at a breakpoint. K4 must be de-energized in
      all four. Hardware-gated — needs a real RP2040 on the bench.
- [ ] Verify **de-energized K4 opens the contactor**, on the real interlock
      wiring from 0.7. Getting this backwards passes every bench test and fails
      dangerous. Hardware-gated, and blocked on 0.7's wiring decision.

Everything else in this phase (latching trip semantics, the `CLEAR_TRIP`
clear-refusal logic and its documented scope limit against graduated guards,
the GRACE→ARMED state machine, and the guard-test-matrix audit) is built and
host-tested; see `docs/GUARD_TEST_MATRIX.md` for the coverage record. **None
of it has been exercised against a real relay/contactor** — the two bullets
above are what remains.

## Phase 6 — Current sensing

Sampling, peak-envelope conversion, clip detection and the power estimate are
built and build-verified. **Not hardware-verified** — no RP2040/CT hardware
attached to any build machine, so nothing here has ever produced a real
current reading.

- [~] `zero_counts` re-measurement — the mechanism
      (`current_sense_recalibrate_zero()`) exists but nothing calls it; the
      ">= 5 min idle" precondition and drift report are commissioning-flow
      work (Phase 9) and current-sense calibration constants, while the
      config-store record now has a `ct_cal[3]` field to hold them
      (Phase 9), still need a commissioning caller.
- [ ] **Run the full commissioning check in `docs/CURRENT_SENSE.md` §5** — in
      particular step 2, one relay at a time, confirming each CT maps to the
      channel you think it does. Needs real hardware; not done.
- [ ] Implement **S9** (trip ineffective / contactor welded) consumer wiring
      against real current data — the guard logic exists (Phase 4); this is
      about feeding it real `link_task` relay context (Phase 7).
- [ ] ⚠️ **S3 and S4 stay disabled until the CT channel mapping is confirmed.**
      A correlation guard fed by a mis-mapped CT trips on healthy firings and
      stays quiet on the failure it exists to catch.

## Phase 7 — The link

Framing (`0x7E`/stuffing/CRC16), the no-wait rules (never ACKs, never
retransmits, never blocks on TX), `PUSH_CONTEXT` parsing, and `CLEAR_TRIP`
handling are built, host-tested, and audited clean against
`../CommonFW/docs/LINK_PROTOCOL.md` §2's five rules. **None of it has crossed
real wire** — the pi↔ESP UART link is currently dead on the bench
(`link_status`: `frames_received: 0`).

- [ ] Handle `SET_FIRING_CEILING` (0x09) → S1's `effective_ceiling`.
      **Clamp with `min()`** — the ESP may only ever tighten it. The codec
      exists in `CommonFW` (Phase 1); `link_task.c` does not call it yet.
- [ ] Handle `SET_CLOCK` (0x0C), diagnostic only. **No guard may read it.**
      Same status — codec exists, not consumed.
- [~] `boot_id` change resets every correlation window — the change is
      *detected* (`link_task.c` tracks `s_last_context_boot_id`), but there is
      no correlation guard yet for it to reset (see S2/S6/S10 below), so the
      detection is currently a documented no-op.
- [~] **Honour the `SIM_PLANT` flag**: disable S2/S3/S4 and warn persistently —
      the seen-tracking/DIAG warning bit is real, but S2/S3/S4 don't exist yet
      to be disabled.
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
      is indistinguishable from a kiln holding a soak. (Note: all guard
      *inputs* for S13 now exist end to end; S13/S1 stay dormant until
      commissioning sets a real `tc_source`/`tc_placement_mode` — this is
      expected, not a gap.)
- [ ] `SAFETY_FLAG_BORROWED` in every status frame when borrowing, and the GUI
      labels the temperature accordingly.
- [ ] Compare the borrowed channel's reported `tc_type` against
      `borrowed_type_expected`; warn on a change.
- [ ] Enable **S3** / **S4** once phase 5's mapping check has passed.
- [ ] Develop the parser against a **PC-side stub emitting context frames**
      before `KilnFW` can send any — the reverse of the stub already described
      in `firmware/KilnFW/docs/SAFETY_LINK.md`. (Note: `tools/PcTools`'s
      `fake_peer.py` now provides exactly this stub in both directions —
      confirm it covers this item before reopening work here.)

## Phase 7b — Mutual version compatibility

Both processors must check each other. Design:
[`../CommonFW/docs/LINK_PROTOCOL.md`](../CommonFW/docs/LINK_PROTOCOL.md),
`ANNOUNCE_VERSION`.

`KILNLINK_PROTOCOL_VERSION`/`_MIN_COMPATIBLE` (shared), `ANNOUNCE_VERSION`
send/receive on both sides, the `FW_VERSION` version-fields-first parse,
bidirectional compatibility evaluation, telemetry-keeps-flowing-on-mismatch,
and the `0x00`–`0x0F` compatibility floor are all built and host/build-verified.

- [ ] 7b.5, remaining half: **on a version mismatch, the Pico side has the
      mechanism (`DEGRADED_NO_CONTEXT` tracked correctly) but nothing yet to
      discard/disable** — no context-dependent guard exists yet for it to
      disable (same dependency as Phase 7's S2/S6/S10). Also, Phase 0.6's full
      redefinition (dead-link fault, not just version mismatch) is still
      unbuilt on the ESP side — this phase reused the pre-existing
      link-staleness check instead.
- [ ] 7b.8 Host test: every combination of older/newer/equal on both sides,
      including a peer that announces a `min_compatible` above its own version.

## Phase 8 — Telemetry (required)

The ESP will not permit heating without this. See `../CommonFW/docs/LINK_PROTOCOL.md` §8.

Frame A (status), Frame B (DIAG), Frame D (TRIP_EVENT) and Frame E (POWER) are
all built, host-tested, and build-verified against the real toolchain. **None
has ever carried a real reading or a real trip across actual wire** — no
RP2040/CT hardware attached to any build machine, and the link is bench-dead.

- [ ] `build_info.h`-equivalent identity for `SaftyFW` itself (git commit,
      dirty, timestamp) is not generated on every build yet.
      `link_task_send_fw_version()` currently hardcodes `dirty = 1` and empty
      commit/datetime fields — honest (unknown maps to dirty), but not the
      real build identity.
- [ ] `config_version`/`config_crc` in `FW_VERSION` and DIAG's
      `calibration_missing` bit are not yet wired to the real `config_store`
      record (Phase 9) — `link_task.c`'s send paths still use 0/0 or a
      hard-coded 1 rather than reading the store.

## Phase 8b — ESP web GUI surface (`KilnFW` work)

See `../CommonFW/docs/LINK_PROTOCOL.md` §7 for the full panel spec.
**Correction found this pass**: this section previously listed every bullet
below as `[ ]` open; `firmware/KilnFW/App/drivers/safety_page.html` already
implements the core panel — link status, protocol-version compatibility,
safety + enclosure temperature, power draw, the DIAG card, the trip card, and
the "Clear latched trip" flow with confirmation. What is still genuinely
missing, confirmed by reading that file:

- [ ] **Label the safety temperature with its placement mode.** In
      `EXTERNAL_OVERHEAT` it will not track the zone temperatures and should not.
- [ ] Colour the enclosure temperature against S12's 60 °C / 85 °C thresholds.
- [ ] Mark power as an estimate; show `—` when `mains_voltage_v` is unconfigured
      rather than assuming a default.
- [ ] **`TRIP_INEFFECTIVE` gets its own visual treatment** — it means "go to the
      breaker", not "investigate the kiln".
- [ ] Safety processor build identity + config CRC on the diagnostics page.
- [ ] Mirror the same data on the PC-link `SAFETY` task so `pc_tools` and MCP
      see it without Wi-Fi — not confirmed present in `tools/PcTools/src/kilnctrl/safety.py`.
- [ ] **Assert in the host tests that guard verdicts are bit-identical with the
      TX path stubbed out.** No verdict may depend on anyone listening.

## Phase 9 — Commissioning and the honest gaps

Config store (versioned, CRC'd, safe defaults), `SET_CONFIG` (tc_type), CT
calibration storage + wire commands, and `SAFETY_CMD_ROLLBACK` are all built,
host-tested, and build-verified. Config writes are refused while ARMED
(host-tested; no real ARMED write attempted on hardware). **None of this has
been exercised against real RP2040 hardware.**

- [ ] Log a full-power ramp, measure the real maximum °C/min, **then** set and
      enable **S8** at ~2× it. Do not guess this number. Hardware-gated.
- [ ] Work `docs/GUARD_TEST_MATRIX.md` end to end and record every result
      (date, commit, config CRC, outcome). Hardware-gated.
- [ ] **Confirm no test threshold was left in place** — re-read the config CRC
      from telemetry after the trip tests. Hardware-gated.
- [ ] Commissioning check that the fitted thermocouple matches `tc_type`, at a
      known soak against a reference instrument. Hardware-gated.
- [ ] **Provoke each enabled guard on real hardware and record the result.**
      A guard that has only ever passed a host test is not commissioned.
      Hardware-gated.
- [ ] No PC-side sender yet for `SET_CT_CAL`/`GET_CT_CAL` — `tools/PcTools`
      has no MCP tool calling these frames; `firmware/SimFW/tools/ct_calibration/`
      still only writes a local JSON file.
- [ ] Board-change proposals for the next revision:
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
- [ ] `update_task.c`'s `PENDING_VERIFY`-clear gate (Phase 10.8) still cannot
      compute `config_crc_ok` for real — it's permanently `false` since nothing
      reads the now-existing `config_store`'s CRC yet, so no slot can be
      marked `VALID` in any build today. See Phase 10 below.

## Phase 10 — Field updates over the isolated link

Last, deliberately. A bootloader is new code in the one component with nothing
behind it, and it is only defensible because SWD sits underneath as the recovery
path. Full design: [`docs/BOOTLOADER.md`](docs/BOOTLOADER.md), wire contract and
interlocks: [`../CommonFW/docs/UPDATE_PROTOCOL.md`](../CommonFW/docs/UPDATE_PROTOCOL.md).

The flash layout, metadata format, bootloader (including recovery mode's full
minimal frame subset), application-side staged writes with read-modify-write
flash programming, whole-slot CRC-verify-from-flash, and image-header
validation before the first erase are all built and build-verified against the
real toolchain. **Nothing in this phase has been exercised against real
RP2040 hardware** — no probe or board attached to any build machine, so no
bootloader has ever actually been flashed over SWD, no application has ever
run from either slot, and no update has ever crossed real wire.

- [ ] **10.0 Measure the isolated link's error rate at 115200** over a sustained
      multi-megabyte transfer. The TCMT1109 optocouplers are the bandwidth limit
      and nobody has characterised them. A 5 % frame loss turns a 35 s update
      into minutes, because retries cost 200 ms each up to ten times.
      Hardware-gated.
- [ ] Signature/public-key reservation (`BOOTLOADER.md` §6) is designed
      (768 B pubkey slot, `slot[2].signature[64]`, `sig_required`) but not yet
      in code — `flash_layout.h`/`metadata.h` have neither field. Signature
      algorithm and key-rotation support are both still undecided.
- [ ] **`*** MANUAL SYNC HAZARD ***`**: `app_slot.ld.in`'s `FLASH` `LENGTH` and
      the two `saftyfw_add_slot_executable()` origin literals in
      `CMakeLists.txt` are hand-copied from `bootloader/flash_layout.h` and
      nothing enforces they stay in sync. A CI check parsing the header and
      asserting agreement would close this; not built.
- [ ] 10.8, remaining half: `update_task_confirm_tick()`'s `PENDING_VERIFY`→
      `VALID` gate cannot compute `config_crc_ok` — nothing reads the now-existing
      `config_store`'s CRC yet, so `update_confirm_missing()` can never reach 0
      and no slot can be marked `VALID` in this build. Wiring it to a real
      `config_store` read is the next step.
- [ ] 10.8c, remaining caveat: the retransmit-round cap (10 rounds) is a real
      backstop, but one "round" can take far longer in practice than
      `UPDATE_PROTOCOL.md`'s throughput section seems to assume — not measured
      against a real link (blocked on 10.0).
- [ ] 10.10 Verification: power cut during erase, during streaming, and during
      the metadata write; corrupt slot rejected; bad-but-booting image rolled
      back; both slots invalidated and recovered over the link with no probe.
      Hardware-gated, entirely unstarted.

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
