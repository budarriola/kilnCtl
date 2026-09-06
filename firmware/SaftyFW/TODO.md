# TODO — Safety Processor Firmware

> **Status:** planning · **Last reviewed:** 2026-08-22
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

---

## 2026-08-28 -- the elements are connected, and S1 is not armed

**THE KILN'S HEATING ELEMENTS ARE NOW PHYSICALLY CONNECTED to the main board.**
This processor's guards stopped being a bench exercise today.

**S1 is disabled on the live board.** `abs_max_temp_c` reads
`set: true, value: 0` and 0 there means never trip, so the only temperature
ceiling in force is `KilnFW`'s guard 5 -- running on the processor that
commands the heat, which is the arrangement this whole firmware exists to not
depend on. `commissioned` reports `false`. Note the reporting trap that hid
this from the coordinator for several hours: `KilnFW`'s `/api/readiness` says
"all 58 safety parameters have values", and a zero IS a value. Setting a
Pico-side ceiling (90 C was proposed, deliberately above the fixture's 80 C so
the primary acts first) is offered and awaiting the owner's number.

**The 80 C fixture cap is a TEST THRESHOLD.** Phase 9's "confirm no test
threshold was left in place" now has a concrete instance to check: `max_temp_c`
= 80 on all three KilnFW zones, and whatever `abs_max_temp_c` ends up as here.
Both are properties of the bench fixture, not of any kiln.

**Consequence of today's S9 change, restated because the bench changed under
it:** S9's unclearable `trip_ineffective` escalation now requires commissioned
current sensing, because an uncommissioned CT chain reads the op-amp offset
floor as permanent current and would brick the board. No CT is fitted, so **a
genuinely welded contactor raises a warning and nothing more.** That was an
acceptable trade while nothing could get warm. It is a real gap now, and
commissioning the CT channels is what closes it.

**UPDATE 2026-09-04, read live from the board (`GET /api/safety/commissioning`
while `fuzzy_ab_20260904d` was firing):** `abs_max_temp_c` is now `set: true,
value: 80`, and `commissioned` reports `true` — S1 is armed at the fixture's
80 C cap. The Pico-side ceiling this section says was "awaiting the owner's
number" has been supplied and committed; the "S1 is disabled"/"only
KilnFW's guard 5 is in force" state described above is no longer current. CT
channels (`ct_installed`, `ct_cal[*].calibrated`) remain unset/false, so the
S9 welded-contactor gap described in the paragraph above is still real. No
`fields_set`/sentinel-value contradiction remains for `abs_max_temp_c` either
(80 is not the 0-sentinel), so the "Secondary finding" further below in this
file is also resolved and can be treated as historical.

---

## Audit 2026-08-27 — open items

A six-agent read-only audit of both firmwares. Fixed and committed already:
the dead `s_guard_cfg` (S1 unreachable at any temperature), the
`tc_placement_valid` gate (S2/S10 armed on uncommissioned boards).

**Swept 2026-09-02: every item below except the last was already fixed by the
2026-08-27 audit pass and this checklist had simply never been ticked** — each
was re-verified against the current source before being closed here, not
assumed from the commit log:

- [x] ~~`safety_guards_try_clear()` grants a full re-arm window against a
      still-present hazard.~~ Fixed. `guard_condition_still_immediate()`
      (`src/safety_guards.c:149`) now covers S2/S3/S5/S11/S12/S13, each
      recomputed from the current tick's `in`/`state` rather than trusting an
      accumulator `safety_guards_clear()` is about to zero.
- [x] ~~`TRIP_INEFFECTIVE` (S9, welded contactor) is clearable.~~ Fixed on both
      sides: `safety_guards_try_clear()` refuses unconditionally whenever
      `state->trip_ineffective` is set, before either of the checks above run
      (`safety_guards.c:228`), and `link_frame_decide_clear_trip()`
      (`src/tasks/link_frame.c:223`) refuses `SAFETY_TRIP_INEFFECTIVE` at the
      wire layer too, so the refusal is visible with a reason before the pure
      guard function is even reached.
- [x] ~~A trip during GRACE, once cleared, jumps straight to ARMED.~~ Fixed:
      `relay_clear_trip_transition()` (`src/tasks/relay_grace.c:23`) returns to
      GRACE when `elapsed_ticks < grace_ticks`, matching `relay_grace_tick()`'s
      own boundary, and only reaches ARMED when the grace window had already
      elapsed by the same instant a normal boot would have crossed it.
- [x] ~~A dropped `relay_owner_command_trip()` silently fails to open K4.~~
      Fixed: `relay_trip_command_still_owed()` /
      `relay_clear_command_still_owed()` (`src/tasks/relay_grace.c:40/48`) are
      wired into `safety_core.c`'s queue-drain loop (lines ~1176/1311) and
      retry the send until it succeeds, with the trip direction always winning
      a race against a superseding clear.
- [x] ~~Guard windows count nominal ticks, not wall-clock.~~ Fixed:
      `tick_dt_compute_s()` (`src/tasks/tick_timing.h`) measures real elapsed time
      from successive `now_ms` deltas, clamped, and falls back to the nominal
      constant only when `clock_health` reports the clock stalled
      (`src/tasks/safety_core.c:685`).
- [x] ~~Consumers do not age-check `thermo`/`current` snapshots.~~ Fixed:
      `safety_core.c:737` and `:899` gate both `tc_valid`/`current_valid` on
      `snapshot_is_fresh(now_ms, ...timestamp_ms, ..._MAX_AGE_MS)` in addition
      to the clock-health check, same audit pass as the `dt_s` fix above.
- [x] **S13's producer does not exist.** ~~`sample_counter_advancing` is
      hardcoded false.~~ Fixed: `context_borrowed_sample_counter_advancing()`
      (`src/snapshots.h`, host-tested in `test/test_snapshots.c`) compares the
      commissioned `borrowed_zone_index` zone's context-frame `sample_counter`
      against the last tick's, called from `safety_core_build_input()`
      (`src/tasks/safety_core.c`). `tc_source` may now be commissioned to
      BORROWED_ZONE/BOTH — see `SAFETY_MODEL.md`'s guard table.
- [x] ~~Config record field ranges are validated on commit but not on load.~~
      Fixed: `config_store_unpack_ex()` (`src/config_store.c:556`) calls
      `config_params_validate_ranges()` at load time too, and a structurally
      valid but range-refused record falls back to
      `config_store_default()` with `calibration_missing`/
      `config_store_is_config_rejected()` reporting why, not just booting
      unvalidated onto the guards.
- [x] ~~`config_store_get_config_version()` truncates `seq` to 8 bits.~~ Fixed:
      `config_store_seq_to_version()` (`src/config_store.c:942`) maps every
      `seq >= 1` into `[1, 255]` via `(seq - 1u) % 255u + 1u`, deliberately
      excluding 0 from the wrapped range (0 is reserved for the one real
      "never committed" case) instead of the old `seq & 0xFF` that let seq 256
      alias seq 0 and read as "no valid config".
- [x] ~~`config_store_write()` erases the whole sector then programs.~~ Fixed:
      `config_store_flash.c` already ping-pongs across the two slots in the
      sector (`s_cached_slot`, `config_store_next_write_needs_erase()`,
      `needs_erase` only true when both slots are full) rather than erasing on
      every commit.
- [x] ~~Only one trip reason is ever retained; `ARCHITECTURE.md` §9 promises a
      bitmask.~~ Doc corrected, not code changed — `ARCHITECTURE.md:539` now
      states plainly that `trip_mask` is a single latched reason, wire-encoded
      as one bit for backward compatibility, and that `trip_reason` is the
      authoritative field. Re-verified 2026-09-02: still accurate.
- [x] ~~`SAFETY_MODEL.md` §6 lists an E-stop assert/release cycle as a clear
      path.~~ Doc corrected, not code changed — `SAFETY_MODEL.md:882-908` now
      states explicitly that no such path exists, that releasing E-stop is
      only a *precondition* S7's `CLEAR_TRIP` recheck must see satisfied, and
      that `CLEAR_TRIP` must still be sent afterward. Re-verified 2026-09-02:
      still accurate, and matches the code (`safety_guards_try_clear()`'s only
      caller is `link_task.c`'s `CLEAR_TRIP` handler).
- [ ] **`watchdog_enable(..., pause_on_debug = true)`** with a probe
      permanently attached means a core halted in a fault state is never reset,
      disabling the documented fail-safe reboot on every bench board. GPIO6
      stays low through a halt so this is not an immediate heat-on hazard.
      Verify on the bench, then decide deliberately.

**Hardware-gated, repo-wide, historical**: the ESP32<->RP2040 isolated link
went unproven for a long time — `link_status` reported `frames_received: 0`
and the Pico console emitted zero bytes. The cause turned out to be the
TCMT1109 optocoupler pair's bandwidth, not the pin map or firmware: at 115200
baud zero frames were ever received. Walking the baud rate down settled on
9600, which both firmwares then hardcoded, and the link has run end to end on
the bench (2026-08-23) — see `docs/HARDWARE.md` §1 for the measurement. That
optocoupler pair was replaced by a non-inverting digital isolator (U6, an
ADuM1201WT) on 2026-08-25; the 9600 figure was a property of the retired
parts, not of either firmware, and the baud sweep is now complete — see
`KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig` for the committed
value. Items below marked hardware-gated may still be open for their own
reasons; the link itself is no longer the blocker.

---

## Phase 0 — Blockers (do these first, in this order)

- [x] **0.0 Pin test — done 2026-08-23, electrically.** Coordinated two-board
      GPIO drive/read (`docs/HARDWARE.md` §1) settled it: **ESP GPIO5 = TX,
      ESP GPIO4 = RX**, both data directions and the fault line inverting as
      predicted, cross-checked at register level over JTAG/SWD. The pull-down
      variant of this test is no longer worth running: it reads R15's position,
      and R15 was itself wired to the wrong net until 2026-08-22 — which is how
      the schematic-derived map came out inverted twice. Drive and read across
      the barrier instead. What was blocking the electrical half was a
      `gpio_probe` task stack overflow in `KilnFW` (3072 bytes, overflowed on
      its first command), fixed the same day.
- [ ] **0.5a Fit a 3-pin DEBUG header and bring GP16/GP17 out** before A1 is
      soldered down — both are far cheaper at build time than after.
      `docs/HARDWARE.md` §7b.
- [ ] **0.5b Consider a RUN (pin 30) reset wire** to the probe or a button while
      the board is still open. RUN is currently unconnected; OpenOCD's SWD reset
      is sufficient without it.
- [ ] **0.7 Confirm the K4 → line-contactor interlock topology** and which J10
      pin is NO vs NC. `docs/HARDWARE.md` §3. **This is a system-wiring decision,
      not a firmware one, and it must be settled before any bench trip test.**
- [ ] **0.8 Confirm the E-stop is wired normally-closed**, or fit a deliberate
      jumper to `GND_Safty`. `docs/HARDWARE.md` §5.

## Phase 1 — `CommonFW`, before either firmware uses it

The link contract is shared code, implemented once. See
[`../CommonFW/README.md`](../CommonFW/README.md) for the full checklist; the
gating items are:

- [x] **Won't do** — Shared ids split out of `uart_task_ids.h` into a
      `kilnlink_ids.h`; PC-link ids left behind. Investigated 2026-08-24: the
      `SAFETY_CMD_*` values in `uart_task_ids.h` that match a `KILNLINK_*_CMD`
      are deliberate literal mirrors, not accidental drift — several double
      as real PC→ESP dispatch values in `uart_bridge.c`, and the rest exist
      only so `uart_task_ids.h` stays the one place every subcommand on this
      wire is enumerated (its own doc comments say so explicitly). This
      firmware's own `src/tasks/link_frame.h` makes the identical choice for
      the identical reason (`LINK_FRAME_CLEAR_TRIP_CMD` etc., redeclared
      "rather than pulling the kilnlink codec header into this file's own
      namespace," even though `link_task.c` already includes those codec
      headers directly) — an established, repo-wide convention this item
      would have reversed for no fixed drift. See
      `../CommonFW/README.md`'s matching Contract entry for the full
      evidence.
- [x] `kilnlink` codecs — all pure and bounds-checked, host-tested, consumed
      by `pc_tools` as the third implementation. Every wire command through
      `0x1A` (`SET_CT_CAL`/`GET_CT_CAL`/`CT_CAL`) now has a codec in
      `firmware/CommonFW/src/`, including `kilnlink_ceiling.c` (SET_FIRING_CEILING)
      and `kilnlink_set_clock.c` (SET_CLOCK) — **this file previously claimed
      those two were "not yet coded"; that was stale.** The SaftyFW-side
      *consumer* wiring this entry used to list as still-open is also done
      and was likewise stale: `link_task_handle_set_firing_ceiling()` and
      `link_task_handle_set_clock()` both exist and are dispatched, with the
      `min()` ceiling clamp living in `safety_guards.c` and covered by
      property tests sweeping `1e30`/`NaN`/`±Inf` (verified 2026-08-22).
- [x] `KilnFW`'s `uart_protocol.c` delegating framing/CRC, proven byte-identical
      to the pre-refactor output. **Re-swept 2026-09-03: already shipped.**
      `App/drivers/owners/uart_protocol.c`'s `frame_and_send()`
      (lines 97-113) calls `kilnlink_stuff()` directly, and the CRC sites
      (lines 165, 204, 740, 847) call `kilnlink_crc16_ccitt_false()`
      directly. The thin wrapper was deliberately removed 2026-08-27 so its
      own name would stop tripping the isolation grep, proven byte-identical
      in `App/test/test_uart_protocol_link_delegate.c` before removal.
- [x] CI grep: no CRC or byte-stuffing implementation outside `CommonFW` —
      2026-08-22, `tools/check_link_impl_isolation.ps1`. Standalone (matching
      `check_isolation.ps1`'s convention), not build-wired. **Re-run
      2026-09-03: now green.** Output: "Link implementation isolation check
      passed: no CRC/byte-stuffing implementation found outside
      firmware/CommonFW." The "real hits" this entry used to report were the
      pre-delegation `uart_protocol.c`, fixed by the item above — this is
      the check working, not a false positive resolving itself.

## Phase 2 — Skeleton

Built and build-verified under the real toolchain (arm-none-eabi-gcc 14.2.1 /
pico-sdk 2.1.1 / FreeRTOS-Kernel RP2040 SMP, Ninja). **Never flashed or run on
real hardware** — no RP2040 attached to any machine this has been built on.

- [ ] Blink-equivalent proof of life over SWD/RTT, and the physical heartbeat
      LED's toggle logic — **neither verified**; no RP2040/debug probe attached
      to any build machine.
- [x] Runtime log-level command over the link. ⚠️ **Blocked on a decision, not
      on code**: `log_task_set_level()`/`_get_level()` exist and work
      internally, but `CommonFW` allocates **no wire command id** for this —
      grepped end to end 2026-08-22 for `log_level`/`LOG_LEVEL`/`SET_LOG`,
      nothing. Minting one is a wire-contract change (`LINK_PROTOCOL.md` plus
      both firmwares plus `pc_tools`), so it is the owner's call rather than
      something `link_task.c` can decide unilaterally. Low risk when it
      happens — additive, and an older peer ignores an unknown id — with
      `0x1B` the next free id. DONE — `SAFETY_CMD_SET_LOG_LEVEL` (0x1B) is
      now a real wire command (`kilnlink_set_log_level.h`), handled by
      `link_task.c`'s `link_task_handle_set_log_level()`
      (`KILNLINK_SET_LOG_LEVEL_CMD` case, ~line 2179).
- [ ] RTT as `SaftyFW`'s secondary log transport — needs the debug probe wiring
      on the bench.

## Phase 3 — Thermocouple

- [~] **Choose `tc_type` deliberately, per sensor** — type K is marginal above
      ~1150 °C and green-rots *low* in reduction; type S/R suits a
      chamber-mounted sensor on a cone-10 kiln (`docs/THERMOCOUPLE.md` §2).
      Owner decision 2026-08-22: **this is not a compile-time choice at all.**
      It is commissioned from the ESP web GUI along with the rest of the
      safety parameter surface, and persists on the safety processor —
      see [`docs/COMMISSIONING.md`](docs/COMMISSIONING.md) for the mechanism.
      Until a real record is written every board still resolves to type K via
      the safe-default path, which is not a claim that K is correct here.
- [x] **Per-type plausibility ranges** — done 2026-08-24,
      `src/max31856_tc_range_policy.c`, wired in `thermo_task.c` (feeds S5's
      existing sensor-invalid path; no new trip). Ranges are datasheet Table 1
      (`firmware/KilnFW/Datasheets/MAX31856.pdf` p.12), inclusive both ends;
      host-tested for all 8 real types, both boundaries, NaN, and the
      unrecognised/voltage-mode-type case (`test/test_max31856_tc_range_policy.c`).
      Applied unconditionally, including on a never-commissioned (default
      Type K) board — deliberate, argued in the policy header and
      `docs/THERMOCOUPLE.md`'s §2 update, not gated on a "commissioned"
      flag (tc_type has none — see `config_store.h`'s own doc comment on
      that field, unchanged by this pass). Distinct from, and does not
      replace, the MAX31856's own unmaskable `TCRANGE` fault bit already
      folded into S5: this check is anchored to config_store's belief, so it
      additionally catches config_store and the part's actual CR1 register
      going out of sync (e.g. a failed `max31856_configure()` write). Does
      **not** solve `docs/THERMOCOUPLE.md`'s §2 wrong-sensor-physically-
      fitted case (a real Type-S junction read through Type-K's LUT still
      lands inside K's own range) — that section's existing "nothing in the
      electronics can detect this" stands, and still needs the commissioning
      soak-test check it already lists. Not yet hardware-verified.
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

- [~] `boot_id` change resets every correlation window — the change is
      *detected* (`link_task.c` tracks `s_last_context_boot_id`), but there is
      no correlation guard yet for it to reset (see S2/S6/S10 below), so the
      detection is currently a documented no-op.
- [~] **Honour the `SIM_PLANT` flag**: disable S2/S3/S4 and warn persistently —
      the seen-tracking/DIAG warning bit is real, but S3/S4 don't exist yet
      to be disabled (S2 does now — see below).
- [x] **`tc_placement_mode` commissioning field** (`CHAMBER_AGREED` /
      `EXTERNAL_OVERHEAT`). **Re-swept 2026-09-03: already landed, this
      checklist had simply never been ticked.** `config_store.h`/`.c` carry
      the field (`CONFIG_STORE_TC_PLACEMENT_*`), `tc_placement_valid` gates
      S2/S10 exactly as specified, and S1's firing-ceiling tightening reads
      it too (`safety_guards.c`).
- [x] Implement **S2** (sustained over-setpoint), **S6** (main controller
      unhealthy) and **S10** (safety TC vs zone TC disagreement, WARN by
      default). **Re-swept 2026-09-03: already landed.** `safety_guards.c`
      implements `SAFETY_TRIP_OVER_SETPOINT` (S2, ~line 684),
      `SAFETY_TRIP_MAIN_FAULT`/`SAFETY_TRIP_LINK_DEAD` (S6, ~line 423/451)
      and the TC-disagreement WARN path (S10, ~line 740), each host-tested.
- [x] **`tc_source` commissioning field**: `OWN_J7` / `BORROWED_ZONE` /
      `BOTH`. **Re-swept 2026-09-03: already landed** (`config_store.h`'s
      `tc_source`, gated by `CONFIG_STORE_SET_TC_SOURCE`; the BORROWED_ZONE
      -> forced-`CHAMBER_AGREED` rejection is in `config_params_validate()`,
      host-tested as "tc_placement_mode vs tc_source contradiction
      rejected").
- [x] Implement **S13** (borrowed channel not updating) against the context
      frame's per-zone `sample_counter`. **Confirmed already implemented**,
      not just its inputs: `safety_guards.c`'s `SAFETY_TRIP_BORROWED_STALE`
      path (~line 764) consumes `sample_counter_advancing`, WARN then TRIP,
      host-tested. S13/S1 do stay dormant until commissioning sets a real
      `tc_source`/`tc_placement_mode` — that part of the parenthetical is
      still accurate.
- [x] `SAFETY_FLAG_BORROWED` in every status frame when borrowing, and the GUI
      labels the temperature accordingly. **Landed 2026-09-03**, via a NEW
      byte, not bits 0x40/0x80 (those turned out to already be spent by
      `LINK_FLAG_TC_NOT_INSTALLED`/`LINK_FLAG_TC_INJECTED` — the "free bits"
      note above was written against a stale reading of `kilnlink_status.h`,
      not the real wire producer in `link_frame.c`). Frame A grew a V3 (26-
      byte) extension: byte 24 flags2 bit0 `LINK_FLAG2_BORROWED`
      (`link_frame.h`), byte 25 `borrowed_zone_index`
      (`LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN` = 0xFF sentinel), gated on
      `LINK_FRAME_STATUS_V3_MIN_PROTOCOL` (10) exactly like the V1->V2
      `tx_dropped_sat` precedent. `link_task_send_status()` derives
      `is_borrowed` from `tc_source` (`BORROWED_ZONE`/`BOTH`) and
      `borrowed_zone_index` from the same commissioned config record S13
      already reads. `KilnFW`'s `safety_apply_status()` reads an absent V3
      byte as UNKNOWN (`borrowed_known=false`), never a false "not borrowed",
      and `safety_page.html` renders "Reading source: ..." from
      `/api/safety/commissioning`. `CommonFW/docs/LINK_PROTOCOL.md`'s Frame A
      table has the byte layout. `LINK_FLAG_TC_NOT_INSTALLED`/
      `LINK_FLAG_TC_INJECTED` (existing bits 0x40/0x80) were ALSO given a
      `KilnFW`-side reader this same pass (`SAFETY_FLAG_TC_NOT_INSTALLED`/
      `SAFETY_FLAG_TC_INJECTED`, `uart_task_ids.h`) — both were being set on
      the wire with nothing on the ESP side ever naming them.
- [x] Compare the borrowed channel's reported `tc_type` against
      `borrowed_type_expected`; warn on a change. **Done 2026-09-03** —
      genuinely open until now (confirmed: `borrowed_type_expected` was a
      real, validated, round-tripped `config_store` field with zero
      consumers anywhere outside config code). `context_borrowed_type_
      mismatch()` (`src/snapshots.h`, pure/host-tested,
      `test/test_snapshots.c`) compares the borrowed zone's context-frame
      `tc_type` against it, called from `safety_core_build_input()`
      (`src/tasks/safety_core.c`), which logs a `LOG_LEVEL_WARN` once on the
      false->true transition. Not a `SAFETY_MODEL.md` guard (no S-number,
      no trip) — a diagnostic, same as the doc's own wording ("warns on a
      change"). No wire-format change: `tc_type` was already in every
      `PUSH_CONTEXT` zone block.
- [ ] Enable **S3** / **S4** once phase 5's mapping check has passed.
- [x] PC-side stub emitting context frames: done, `tools/PcTools/src/kilnctrl/fake_peer.py`
      (`FakeEspPeer.send_broadcast`/`request_with_retry` stands in for KilnFW emitting
      PUSH_CONTEXT-shaped frames; `FakeSaftyPeer.send_telemetry` covers the reverse
      direction), with fault injection via `FaultInjector`.

## Phase 7b — Mutual version compatibility

Both processors must check each other. Design:
[`../CommonFW/docs/LINK_PROTOCOL.md`](../CommonFW/docs/LINK_PROTOCOL.md),
`ANNOUNCE_VERSION`.

`KILNLINK_PROTOCOL_VERSION`/`_MIN_COMPATIBLE` (shared), `ANNOUNCE_VERSION`
send/receive on both sides, the `FW_VERSION` version-fields-first parse,
bidirectional compatibility evaluation, telemetry-keeps-flowing-on-mismatch,
and the `0x00`–`0x0F` compatibility floor are all built and host/build-verified.

- [x] 7b.5, remaining half: **on a version mismatch, the Pico side has the
      mechanism (`DEGRADED_NO_CONTEXT` tracked correctly) but nothing yet to
      discard/disable** — no context-dependent guard exists yet for it to
      DONE — `safety_core.c` now derives a single `context_valid` fact from
      `DEGRADED_NO_CONTEXT`/stale/never-received/clock-stalled and
      `safety_guards.c` uses it to disable the context-dependent guards
      (S6a/S6b, S11/S13, S14 all cite it, e.g. `safety_guards.c:804`).
      disable (same dependency as Phase 7's S2/S6/S10). Also, Phase 0.6's full
      redefinition (dead-link fault, not just version mismatch) is still
      unbuilt on the ESP side — this phase reused the pre-existing
      link-staleness check instead.
- [x] 7b.8 Host test — 2026-08-22, `test/test_link_frame_wire.c`. A 14-case
      named matrix (older/newer/equal on both the protocol and the
      `min_compatible` axis, both directions, inclusive/exclusive boundaries,
      plus the pathological peer announcing `min_compatible` above its own
      version) and a 6561-combination exhaustive sweep against an
      independently-written reference formula. Also asserts telemetry keeps
      flowing during a mismatch. `link_frame_versions_compatible()` had **no**
      test coverage at all before this.

## Phase 8 — Telemetry (required)

The ESP will not permit heating without this. See `../CommonFW/docs/LINK_PROTOCOL.md` §8.

Frame A (status), Frame B (DIAG), Frame D (TRIP_EVENT) and Frame E (POWER) are
all built, host-tested, and build-verified against the real toolchain. **None
has ever carried a real reading or a real trip across actual wire** — no
RP2040/CT hardware attached to any build machine, and the link is bench-dead.

- [x] `build_info.h`-equivalent identity — 2026-08-22.
      `tools/gen_build_info.cmake` generates `saftyfw_build_info.h` into the
      build dir on **every** build (an `add_custom_target`, not a
      dependency-tracked custom command, for the same reason KilnFW's copy
      is), and `link_task_send_fw_version()` now sends the real commit/dirty/
      datetime instead of hard-coded `dirty = 1` with empty strings. Verified
      by grepping the commit hash and build timestamp out of `SaftyFW.elf`
      itself, not just the generated header.
- [x] `config_version`/`config_crc` in `FW_VERSION` — wired to
      `config_store_get_config_version()`/`_get_config_crc()`. Still reports
      0/0 on a never-commissioned board, which is the documented meaning of
      "running on compiled-in defaults", not a stub.
- [x] DIAG's `calibration_missing` bit wired to the real `config_store`
      record — done 2026-08-24. It was a wiring fix, not a wire-format
      change: `KILNLINK_DIAG_FLAG_CALIBRATION_MISSING` already existed in
      `kilnlink_diag.h`, and `link_task_send_diag()` was setting it
      unconditionally to 1 every boot rather than reading
      `config_store_is_calibration_missing()`. The `flags`-byte assembly is
      now `src/link_diag_flags.c` (pure, host-tested,
      `test/test_link_diag_flags.c`, since `link_task.c` itself cannot be) —
      no `KILNLINK_PROTOCOL_VERSION` bump.

## Phase 8b — ESP web GUI surface (`KilnFW` work)

See `../CommonFW/docs/LINK_PROTOCOL.md` §7 for the full panel spec.
**Correction found this pass**: this section previously listed every bullet
below as `[ ]` open; `firmware/KilnFW/App/drivers/http/safety_page.html` already
implements the core panel — link status, protocol-version compatibility,
safety + enclosure temperature, power draw, the DIAG card, the trip card, and
the "Clear latched trip" flow with confirmation. What is still genuinely
missing, confirmed by reading that file:

- [x] **Label the safety temperature with its placement mode.** 2026-09-03.
      `tc_placement_mode` was already exposed over HTTP — it rides the
      existing `GET /api/safety/commissioning` per-param list
      (`safety_cfg_http.c`'s `build_commissioning_json()`, param id 259 /
      0x0103) that `safety_commissioning_page.html` already reads, so no
      wire or C change was needed. `safety_page.html` now polls that
      endpoint (own `.catch()`, alongside the existing `/api/status` poll)
      and renders one of four honest states instead of the old static
      "not reported" placeholder: `CHAMBER_AGREED` ("tracks the zone
      temperatures, can be compared against them"), `EXTERNAL_OVERHEAT`
      ("will NOT track the zone temperatures, do not compare"), "not
      commissioned" (field genuinely unset), or "unknown" (peer protocol
      version too old / not yet known to trust the wire's set-bit — same
      `unset_reporting_reliable` gate `safety_cfg_http.c`'s 2026-08-27 M1
      fix already added). No default is guessed in any of those last two
      cases.
All of the following landed 2026-08-22 and are listed here only because the
section above still names open work:

- [x] Enclosure temperature coloured against S12's real 60 °C / 85 °C
      (instantaneous, so it can flag slightly before S12's `cj_time_s = 60 s`
      debounce actually trips — an early warning, not a false one).
- [x] Power marked an estimate, rendering `—` when `mains_voltage_v` is
      unconfigured. No default mains voltage is assumed anywhere.
- [x] `TRIP_INEFFECTIVE` visual treatment — was already implemented (full
      red card plus a black "go to the breaker" banner), found on inspection,
      left alone.
- [x] Safety processor build identity + config CRC on the diagnostics page,
      rendering "not yet announced this boot" rather than a placeholder.
- [x] Mirrored on the PC-link `SAFETY` task. DIAG/TRIP_EVENT were already
      there; `FW_VERSION` needed both halves — `SafetyFwVersion` +
      `SafetyClient.get_fw_version()` on the PC side, and the missing
      `SAFETY_CMD_FW_VERSION` dispatch case plus
      `safety_link_build_fw_version_payload()` on the ESP side, without which
      the query fell through to `bridge_reply_unsupported()`. Verified live
      against the board.
- [x] Guard verdicts bit-identical with TX stubbed — asserted structurally,
      which is the honest form of this test. The real TX path is
      pico-sdk-only and cannot link into the host binary, and
      `safety_guards.c` has no TX field, pointer or include (enforced by
      `tools/check_isolation.ps1`), so there is no second code path to diff.
      `test_safety_guards.c` instead `memcmp`s the whole verdict struct
      across two instances every tick of a long mixed sequence.

(The placement-mode label bullet above closed 2026-09-03, once Phase 7's
`tc_placement_mode` commissioning field gave it something real to label
*with*.)

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
- [x] `update_task.c`'s `PENDING_VERIFY`-clear gate now reads a real config
      CRC (2026-08-22, `config_store_confirm_crc_ok()`). ⚠️ **Practical
      consequence, not a bug**: `config_store_get_config_version()` returns 0
      until a record has actually been *written*, and nothing has ever written
      a non-default record on any board — so no slot reaches `VALID` until
      commissioning writes one. That direction is deliberate: a slot stuck at
      `PENDING_VERIFY` is recoverable, one wrongly marked `VALID` is not.

## Phase 10 — Field updates over the isolated link

Last, deliberately. A bootloader is new code in the one component with nothing
behind it, and it is only defensible because SWD sits underneath as the recovery
path. Full design: [`docs/BOOTLOADER.md`](docs/BOOTLOADER.md), wire contract and
interlocks: [`../CommonFW/docs/UPDATE_PROTOCOL.md`](../CommonFW/docs/UPDATE_PROTOCOL.md).

The flash layout, metadata format, bootloader (including recovery mode's full
minimal frame subset), application-side staged writes with read-modify-write
flash programming, whole-slot CRC-verify-from-flash, and image-header
validation before the first erase are all built and build-verified against the
real toolchain. As of 2026-08-23 the bootloader has been exercised against
real RP2040 hardware over SWD: it reaches `main`, its flash-capacity sanity
check passes against the board's real JEDEC id (`bootloader/main.c`'s
`flash_capacity_at_least_expected()`), and it correctly falls into recovery
mode when no valid slot metadata exists. Flashing `SaftyFW.elf` directly over
SWD and running the application works fine. What is still unproven is booting
the application *through* the bootloader from a slot — see the open item
below — and a real end-to-end update crossing the isolated link.

**2026-09-06, first live attempt at the isolated-link half**
(`firmware/CommonFW/docs/UPDATE_PROTOCOL.md` "Hardware exercise 2026-09-05/06"):
`ota_update_pico()` staged a real `SaftyFW_slotA.elf`-derived `.bin`
(95,020 bytes, `arm-none-eabi-objcopy -O binary`, no hand-built header
needed) and started the relay, but the Pico refused `UPDATE_BEGIN` with "a
safety trip is pending" before a single byte reached flash — the ESP's own
cached safety status showed no trip at the same moment, so this is the
Pico's independent precondition check working, not a bug. The actual
UART1 transfer this phase's checklist items (10.0's error-rate measurement,
10.8c's round-cap timing) are about was not reached. Also relevant given
10.9 below: this bench Pico is still running `SaftyFW.elf` directly (not
booted through the bootloader), consistent with a `debug_read_memory` scan
of the metadata sector (`0x10010000`) reading back as ordinary code rather
than a `"KLN1"`-magic record — so even a transfer that clears the
precondition and passes CRC today would stage a slot the current boot
vector does not consult — a real prerequisite gap, but see 10.9 below: the
bootloader hand-off itself is not the blocker, only this bench Pico's
current image is.

- [x] **10.0 Measure the isolated link's error rate — done 2026-08-23, and
      it was not 115200 at the time.** The TCMT1109 optocoupler pair then
      fitted turned out to be the bandwidth limit: 115200 and 57600 delivered
      zero frames, ever; 38400 lost about 20%; 19200 looked clean over a short
      window but lost ~10% over a longer one; 9600 tracked sent-to-received
      one for one over minutes and was the committed, hardcoded value on both
      sides for as long as that optocoupler pair was fitted. That pair was
      replaced by a non-inverting digital isolator (U6) on 2026-08-25, so the
      9600 ceiling no longer applies and the sweep is now complete — see
      `KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig` for the
      committed value. What remains open is the *update-transfer* error rate at
      whatever the current committed baud is over a sustained multi-megabyte
      run — untested, and a lower baud with any nonzero frame loss turns a
      35 s update at 115200 into something much longer, because retries still
      cost 200 ms each up to ten times. Hardware-gated.
- [x] Signature/public-key reservation (`BOOTLOADER.md` §6). DONE (found
      shipped, eighth audit pass, 2026-09-04) — both fields exist now, as
      pure reservations with nothing reading or verifying them yet:
      the 768 B pubkey region (`flash_layout.h:46-54`) and per-slot
      `signature[64]`/`sig_required` (`metadata.h:101-116`). Signature
      algorithm and key-rotation support remain undecided, and no code
      packs, unpacks, or checks either field — signing itself is still off.
- [x] **`*** MANUAL SYNC HAZARD ***`** closed 2026-08-22:
      `tools/check_flash_layout_sync.cmake` parses `flash_layout.h` and
      asserts `app_slot.ld.in`'s `FLASH LENGTH` and `CMakeLists.txt`'s two
      slot-origin literals agree. Build-wired (not standalone), so a hand-edit
      to any of the three is caught on the very next build. Proven to fail by
      skewing the linker script's `LENGTH` — build stopped with both
      mismatched numbers named.
- [x] 10.8's remaining half — see Phase 9's `config_crc_ok` entry above.
- [ ] 10.8c, remaining caveat: the retransmit-round cap (10 rounds) is a real
      backstop, but one "round" can take far longer in practice than
      `UPDATE_PROTOCOL.md`'s throughput section seems to assume — not measured
      against a real link (blocked on 10.0).
- [x] **10.9 RESOLVED 2026-08-25 — bootloader hand-off was never broken; the
      slot-A image tested on 2026-08-23 was stale.** Re-tested with a
      freshly built slot-A image: 60 s window, ESP frames-deframed +195,
      received +36, zero crc/framing/length-mismatch/resync errors,
      `safety_get_status` link up; recovery mode verified separately at
      230400 baud (deframed +61 in 61 s). Root cause: the original image
      predated the isolated link's baud change (9600 → 230400) and the
      removal of both ends' TX line inversions, so the bootloader was
      faithfully booting an application built for a different wire
      configuration — a stale-artifact bug, not a hand-off defect. Always
      rebuild slot images in the same pass as any link-config change.
      **Remaining blocker for a bench Pico update is separate and still
      open**: this bench Pico is currently running the monolithic
      `SaftyFW.elf` directly, not booted through the bootloader from a slot
      (`debug_read_memory` at the metadata sector reads ordinary code, not a
      `"KLN1"` record — see the 2026-09-06 exercise note above). The
      prerequisite next step is flashing the bootloader + a freshly built
      `SaftyFW_slotA` image via `debug_program(peer="pico")` — not done as
      part of this pass.
- [ ] 10.10 Verification: power cut during erase, during streaming, and during
      the metadata write; corrupt slot rejected; bad-but-booting image rolled
      back; both slots invalidated and recovered over the link with no probe.
      Hardware-gated, entirely unstarted.
- [ ] Reconcile host-side vs ESP-side UPDATE_BEGIN image CRC before trusting a
      CRC match as proof of a correct transfer — the 2026-09-06 exercise saw
      host zlib CRC32 `0xc02711a8` against the ESP-reported `0x02F15704` for
      the same staged image, so the two sides are not computing the same
      CRC32 variant/parameters (`../CommonFW/docs/UPDATE_PROTOCOL.md`
      "Hardware exercise 2026-09-05/06").

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

---

## An uncommissioned safety processor grants heating enable — RESOLVED 2026-08-28

**Found 2026-08-24, on hardware, the first time this board ever granted enable.
Needed a decision, not a patch — it changes the safety contract, so it was
written down rather than quietly fixed.**

**The owner's answer was an unqualified NO: an uncommissioned safety processor
must refuse heating enable.** Shipped 2026-08-28. `src/commissioning_gate.c`
(pure, host-tested in `test/test_commissioning_gate.c`) defines commissioned as
BOTH `!rec->calibration_missing` AND `config_params_all_required_set(rec)`
recomputed from `fields_set` — the two must agree, and any disagreement in
either direction refuses. `safety_core_request_enable()` consults it on the ON
direction only, alongside the update interlock and the `safety_tc_installed`
refusal; de-energizing is never gated. No new fault source or wire field: the
refusal is already reported end to end as Frame B's
`KILNLINK_DIAG_FLAG_CALIBRATION_MISSING` → KilnFW's `commissioned:false` and
the "Safety processor commissioned" readiness item, plus a
`request_enable: refused: safety processor not commissioned` log line.

**The cost named below is real and was accepted**: a bench board that has never
been commissioned now cannot close K4 until a real `COMMIT_CONFIG` pass lands
the eight required fields. That is the intent — an uncommissioned board has
`abs_max_temp_c == 0`, so S1 can never trip, and heating under a ceiling that
can never fire is exactly the state this refusal makes unreachable.

The original write-up is kept below unchanged, because it is the reasoning the
decision was made against.

### What was observed

The safety MAX31856 and its thermocouple were fitted. After a Pico reset (its
SPI init runs at boot, so the IC has to be present before boot — see the
bring-up note below), the board reported:

```
link up; heating enable granted; safety thermocouple valid | 30.20 C (CJ 28.08 C)
```

Heating enable was granted while the board reports `commissioned: false`.

### Why that is not benign

Three facts, each verified in the source rather than inferred:

1. **`safety_core_request_enable()` never consults commissioning state.** It
   refuses the ON direction for exactly two reasons: an active update
   transfer, and `cfg_rec.safety_tc_installed == 0`. `calibration_missing` is
   not among them (`src/tasks/safety_core.c`).
2. **S1, the absolute over-temperature guard, is disabled when uncommissioned.**
   `safety_guards.c` is explicit: *"`abs_max_temp_c == 0` means 'not
   commissioned' -- never trip, and never accumulate a streak toward one"*.
   That is deliberate and documented in `SAFETY_MODEL.md` §4, and correct as
   an anti-nuisance rule in isolation.
3. **`calibration_missing` gates nothing.** Its only consumer in the whole tree
   is `KilnFW`'s `safety_cfg_http.c:191`, where it computes the `commissioned`
   field for a web page. No heat interlock on either processor reads it.

Together: the independent protection layer will permit heating with **no
absolute temperature ceiling in force**. S8 (rate-of-rise) also ships disabled
pending a measured ramp, so two of the temperature protections are inactive at
once. `KilnFW`'s own zone `max_temp_c` (1300 °C) still applies, but that is the
controller protecting against itself — precisely what the safety processor
exists to not rely on.

### The decision needed

Should `safety_core_request_enable()` refuse the ON direction while
`calibration_missing` is set?

**Arguments for:** it is the rule `calibration_missing` appears to have been
invented for; "uncommissioned means unsafe to heat" is the conservative
reading; and it closes the gap at the one point that gates everything else.

**Argument against, and it is a real cost:** it would make heating impossible
on this bench until the four no-default section-1 fields are commissioned, and
those need values only the kiln's owner can supply (element power, kiln and
thermocouple maximum ratings, physical zone arrangement). Every bench test that
needs enable would be blocked behind a commissioning pass. A `bench_preset`
exists precisely because that trade was already felt once.

**Not implemented either way.** Changing when a safety processor permits
heating is not a drive-by edit, and the right answer depends on whether the
bench must stay usable pre-commissioning.

### Secondary finding: contradictory bookkeeping on `abs_max_temp_c`

The commissioning API reports `abs_max_temp_c` as `set: true, value: 0`. The
guard treats `0` as "not commissioned" regardless of the `set` flag, so
behaviour is safe — but "set" and "holds the sentinel meaning unset" should not
both be true of one field. Either the `fields_set` bit should not be set for a
field still holding 0, or the API should not report a 0-valued
no-default field as set. As it stands, a reader trusting `set` concludes the
ceiling is commissioned when it is not.

### Bring-up note worth keeping

The safety MAX31856 must be present **before** the Pico boots: its SPI init
runs once at startup, so an IC fitted under power reads as `safety TC invalid`
until the Pico is reset, with no fault indication pointing at the real cause.
A `debug_reset` over SWD is enough. Also note `watchdog_enable(..., true)` sets
`pause_on_debug`, so an attached probe suspends the 1000 ms watchdog — a core
in a fault state will sit there indefinitely under the debugger instead of
being reset, which makes a debugger session look worse than the real
untethered behaviour. Both already flagged in `docs/ARCHITECTURE.md` §8.
