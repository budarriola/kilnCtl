# SaftyFW guard audit, 2026-10-14

Scope: `firmware/SaftyFW` (RP2040 safety processor) on `origin/dev` at
`153c12cf5`. Covered: guards S1..S16 (reachability, latch semantics,
`trip_mask = 1 << (reason - 1)`), the admin TEST_TRIP command, the
benign-reboot heat auto-resume (owner decision 2026-10-10), config store A/B
atomicity, MAX31856 fault handling, watchdog coverage, ISR/task and
cross-core shared state, timer and uptime wrap, and the CT alarm (H9) and
heat-rise (H5) checks from the owner decisions of 2026-10-04.

Read-only review. No board access, no code changed. Every MED was traced
against the code by the author; LOW and INFO items marked "(reported)" come
from parallel sub-reviews whose cited lines were spot-checked but not fully
re-traced.

Not repeated: findings owned by `SAFTYFW_GUARD_REVIEW_2026-10-09`,
`REVIEW_SAFTYFW_GUARD_FIXES_2026-10-09` (and FIX2/FIX3),
`REVIEW_SAFTYFW_TRIP_PATH_2026-10-10` (T1-T4), `REVIEW_SAFTYFX_2026-10-11`,
`REVIEW_SAFTYFX3_2026-10-11` and `REVIEW_TRIPWP2`. Where a finding here
extends one of those it says so.

Owner rule respected: no recommendation disables or blinds a guard. Every fix
below adds a refusal, a freshness gate or a bound.

## Verdict

HIGH 0, MED 5, LOW 11, INFO 16.

| ID | Sev | Finding |
|----|-----|---------|
| A1 | MED | REBOOT/ROLLBACK and `POST /api/sw_reset` are accepted while a trip is latched; a dual reset clears any latch, including S9 TRIP_INEFFECTIVE, with none of CLEAR_TRIP's checks. |
| A2 | MED | Thermo and current snapshot freshness compare a FreeRTOS tick timestamp against the hardware-timer clock; tick loss during flash erases or a debug halt accumulates, eventually reads every snapshot stale. |
| A3 | MED | STATUS sends TC and per-channel amps to the ESP with no age check; a frozen current snapshot (degraded windows) feeds the ESP's H9 CT leak alarm as live data. |
| A4 | MED | CLEAR_TRIP does not consult in-progress S9 evidence (K4 de-energized, current still present); a clear can land inside the S9 streak window and re-arm a welded K4. |
| A5 | MED | Persistent COMMIT_CONFIG accepts unbounded trip thresholds (`link_dead_hard_s`, `blind_grace_s`, `trip_verify_s`, `cj_max_c`, margins); one ESP-side bug can blind S5/S6b/S9/S12 from the next boot. |
| A6 | LOW | An all-0x00 MAX31856 burst is accepted as a valid 0 C reading until the ~30 s live CR0/CR1 check. |
| A7 | LOW | The S16 RAM-integrity check reads its own period from the live RAM record it is checking. |
| A8-A16 | LOW | See section "LOW". |

## MED

### A1: a latched trip is cleared by a reboot, with no CLEAR_TRIP checks

Files:
- `firmware/SaftyFW/src/tasks/update_task_reboot_policy.c:7-38`:
  `update_task_reboot_policy_decide(relay_energized, transfer_active, ...)`
  refuses only when ARMED or a transfer is active. It never looks at the trip
  latch. Called from `tasks/update_task.c:1610`.
- `firmware/SaftyFW/src/tasks/link_task.c:2486-2560` (`handle_reboot`) and
  `:2391` (`handle_rollback`) act on that decision.
- `firmware/SaftyFW/src/main.c:333-348`: at boot `boot_reason` is read,
  discarded (`(void)boot_reason;`, "TODO (Phase 8)") and
  `boot_reason_clear_trip()` runs. The Pico comes up untripped.
- `firmware/KilnFW/App/drivers/http/sw_reset_http.c:374-390`: the only gate
  is `ota_http_check_interlocks` (firing running or heater on). A latched
  Pico trip is not a refusal, despite the comment at `:50-66` ("does not
  clear a latched safety trip"). `:496` then calls `safety_link_send_reboot`.

Scenario: the Pico latches any trip. While tripped K4 is open, so "heater
on" reads false and no firing is running. An operator (or a tool) calls
`POST /api/sw_reset`, which resets both processors. The Pico boots
untripped; the ESP's T3 mitigation (`reboot_was_tripped`, in ESP RAM) is
lost because the ESP rebooted too.

Consequence: the latch is gone without CLEAR_TRIP's boot_id/seq binding,
without `guard_condition_still_immediate()`, and without the unconditional
S9 refusal (`safety_guards.c:383-399`, `link_frame_decide_clear_trip()`).
S9 means "K4 welded, go to the breaker"; a soft reset is now a way out of
it. The same applies to a TEST trip still inside its trip_verify_s hold and
to any trip whose condition still holds (graduated guards then need to
re-accumulate from zero). This extends T3 in
`REVIEW_SAFTYFW_TRIP_PATH_2026-10-10.md:113-137` with a concrete,
operator-reachable path.

Fix (refusals only):
1. `update_task_reboot_policy_decide()` refuses REBOOT/ROLLBACK while
   `is_tripped`, and always while `trip_ineffective`.
2. `sw_reset_http.c` and the OTA interlocks refuse while the Pico reports a
   latch (DIAG/STATUS trip_reason != 0), with a 409 naming the reason.
3. Implement the Phase 8 TODO: keep the scratch trip reason across a
   commanded reboot and re-latch it (at least S9) at boot, or report it in
   DIAG so the ESP treats the boot as fatal.

### A2: snapshot freshness mixes two clocks

Files:
- Stamped with `xTaskGetTickCount() * portTICK_PERIOD_MS`:
  `firmware/SaftyFW/src/tasks/thermo_task.c:571` (and `:346`, injection),
  `firmware/SaftyFW/src/current_sense.c:367`.
- Compared against `now_ms = to_ms_since_boot(get_absolute_time())`
  (`tasks/safety_core.c:810`) in `snapshot_is_fresh()` at
  `safety_core.c:879` (THERMO_MAX_AGE_MS) and `:1051` (CURRENT_MAX_AGE_MS).
- `configTICK_RATE_HZ` 1000, tickless idle off. The context snapshot already
  uses the hardware timer (`link_task.c:1373`), so the mix is local to these
  two producers.

Scenario: SysTick does not advance while interrupts are masked for
`flash_safe_execute()` erases (each config commit; a Pico OTA slot erase is
about 9 s) or while the core is halted by a debug probe (DBGPAUSE is cleared
at `main.c:275`, so the hardware timer keeps running). Each event leaves the
tick clock permanently behind `to_ms_since_boot()`. The lag is cumulative
and never re-synchronised.

Consequence: once the lag passes 2000 ms every fresh sample reads stale.
Thermo stale drives S5 to a trip that cannot be cleared until reboot
(fail-safe, but a nuisance trip with a misleading reason). Current stale
makes `any_current_present` false, which blinds S3, S9 and the soft S6b
current term (fail-dangerous). The drift has not been measured on the
bench; the mechanism is in the code.

Fix: stamp both snapshots with `to_ms_since_boot(get_absolute_time())`, the
clock the consumer uses. Add a host test that advances one clock without the
other.

### A3: STATUS sends stale TC and amps to the ESP as live

Files:
- `firmware/SaftyFW/src/tasks/link_task.c:868`: `temp_valid = th_present &&
  th.valid`, no age test.
- `link_task.c:880-881` and `:949`: `cur.amps[]` from
  `current_task_get_snapshot()` is encoded with no freshness test.
- `firmware/SaftyFW/src/current_sense.c:354-363`: a degraded window
  (`count == 0` or `timeouts > 0`) holds the old snapshot and its timestamp
  and only bumps `s_degraded_window_count`; the comment relies on
  `safety_core.c`'s staleness gate. `current_sense_get_degraded_window_count()`
  (`:472`) has no caller, so the condition is never reported.
- ESP side: `firmware/KilnFW/App/drivers/control/ct_leak_alarm_service.c:92`
  gates only on link staleness (`safety_link_is_stale`), not on sample age.

Scenario: the CT ADC stops completing windows (DMA/IRQ fault, ADC timeout)
while the link stays up. The Pico's own guards age the snapshot out after
CURRENT_MAX_AGE_MS, but STATUS keeps sending the last good amps every frame.

Consequence: H9 (owner decision 2026-10-04) evaluates frozen amps as live.
A frozen low value masks a real leak; a frozen high value holds a false
alarm. Same for the TC shown on the ESP, though `th.valid` limits that case.

Fix: in `link_task_send_status()` mark TC and amps invalid (NaN plus a flag
bit, the `LINK_FLAG2_CJ_VALID` pattern) when the snapshot is older than the
same max age `safety_core.c` uses; have the ESP's H9 treat invalid amps as
"unknown", never as zero. Report the degraded-window count in DIAG.

### A4: a clear can land inside the S9 verification streak

Files: `firmware/SaftyFW/src/safety_guards.c:296-314` (TEST case of
`guard_condition_still_immediate()`), `:330` (S3 immediate), `:383-417`
(`safety_guards_try_clear()`), `:620-660` (S9 streak, runs only in the
tripped path).

Scenario (TEST): the hold releases as soon as `s9_verify_elapsed_s >=
trip_verify_s` (`:307`). S9 then needs `S9_CURRENT_PRESENT_STREAK_TO_TRIP`
consecutive current-present ticks after that point (`:620-645`) before it
latches TRIP_INEFFECTIVE. `test_trip_masks_real_condition()` checks S3's
immediate condition, but that is false while the ESP still reports
`relay_commanded_recently` (`:330`, `safety_core.c:1058-1061`), which is the
normal state when a test trip is fired during a run. A clear arriving in
those few ticks (a tool that polls and clears at hold expiry would) is
granted with K4 welded.

Scenario (other reasons): a clear granted before `trip_verify_s` has elapsed
leaves the tripped path, so S9 never finishes evaluating the open command.

Consequence: a welded K4 detected by S9's own evidence is re-armed, and the
unclearable alarm is never raised. While the ESP commands heat, S3 cannot
see it either.

Fix (refusal only): in `safety_guards_try_clear()`, refuse any clear while
`s9_verify_active` and `any_current_present` (commissioned chain) or
`s9_current_present_streak > 0`, independent of the latched reason.

### A5: persistent thresholds have no upper bound

Files: `firmware/SaftyFW/src/config_params.c:448-612` (`config_params_set`)
and `:636-808` (`config_params_validate_ranges`), applied by
`config_params_validate_ex()` (`:809-941`) at COMMIT_CONFIG. Fields:
`link_dead_hard_s` 0x0403, `blind_grace_s` 0x0206, `trip_verify_s` 0x0307
(U16, no range), `cj_max_c` 0x020C, `overshoot_margin_c` 0x0202,
`tc_disagreement_c` 0x0208 (F32, finite-only, `:757`, `:778`, `:785`
region).

Scenario: COMMIT_CONFIG is refused while ARMED, but a disarmed commit of
`link_dead_hard_s = 65535`, `blind_grace_s = 65535` or `trip_verify_s =
65535` is accepted and persisted. A UI or MCP bug on the ESP side is enough.

Consequence: from the next load S6b effectively never fires, S5 stays at
WARN for about 18 h (S1/S8/S11/S12 skip every bad read), and S9 waits 18 h.
`REVIEW_SAFTYFW_GUARD_FIXES_2026-10-09` HIGH-1 bounded the volatile path
only; `REVIEW_SAFTYFX3_2026-10-11.md:227` left the `rate_window_s` bound
open pending an owner decision. This is the same class over more fields.

Fix: add documented upper bounds (for example the ceilings in
`CONFIG_REFERENCE.md`) at `config_params_set()` time, which refuses the
write without invalidating stored records. Load-time bounds need the same
owner decision as the `rate_window_s` item.

## LOW

- **A6** `firmware/SaftyFW/src/max31856.c:370-396`: no test for an
  all-0x00 burst (MISO stuck low). It decodes as 0 C, faults clear, and is
  accepted until the CR0/CR1 live check (`tasks/thermo_task.c:517-561`,
  about 30 s). All-0xFF sets OPEN and S5 catches it. Fix: treat an all-zero
  burst (including CR0/CR1 bytes) as a bad read.
- **A7** `firmware/SaftyFW/src/tasks/link_task.c:2048-2057`: the S16 check
  period comes from `config_store_get_full_record()`, the live RAM record
  under test. A corrupted `config_check_period_s` of 0 disables the check; a
  large value defers it. Fix: read the period from the persisted copy, clamp
  it, and refuse 0 for 0x0504 if the owner does not need the "disabled"
  setting.
- **A8** `safety_guards.c:325-328`, `:364` (reported): S2 and S11 clears are
  granted while `tc_valid` is false. S1 already refuses that case
  (`:320-321`, saftyfx6 F4). Fix: refuse on unknown reading.
- **A9** `tasks/safety_core.c:1903-1995` (reported): `request_enable` does
  not check `s_guard_state.is_tripped`. Relay_owner still refuses; defense
  in depth only.
- **A10** `safety_guards.c:416-417`, `safety_core.c` near 1463-1530
  (reported): if the post-clear retest re-trips with a different reason,
  trip_seq, TRIP_EVENT and the scratch boot reason are not updated, so the
  ESP shows the old reason.
- **A11** `safety_guards.c:842` (reported): the bad-read `goto` skips S8 dt
  accumulation, so intermittent bad reads shorten the effective window and
  inflate the rate (fail-safe nuisance trip).
- **A12** `config_store.c:1196-1200`, `:1259-1276` (reported): a newer A/B
  record that fails validation is skipped silently and the next write's seq
  can regress below it.
- **A13** `config_store_flash.c` near 1500-1520 (reported): a no-op write
  moves the RAM seq backwards relative to flash.
- **A14** `current_sense.c` (`cs_counts_to_amps`, reported): returns 0 A when
  `k_ct <= 0`, which reads as "no current" rather than unknown.
- **A15** `current_sense.c` (reported): `set_cal`/`set_ct_cal` copy `s_cal`
  across cores without a lock; a torn gain/offset pair is possible for one
  window.
- **A16** `current_sense.c` (reported): auto-zero folds in a stale
  `counts_avg` during degraded windows.

## INFO

1. `main.c:333`: `watchdog_enable()` runs before `boot_reason_read()`, so
   `watchdog_enable_caused_reboot()` is unreliable (see
   `saftyfw_watchdog_caused_reboot_ordering_2026-09-24.md`).
2. Cross-core plain statics, all benign today: `s_relay_on_since_tick`
   (`link_task.c:1461`, `:3757`), `reboot_announce.c:6-10`, the link_up pair
   (`link_task.c:3747-3755`), the u64 wall clock (`:3780-3791`).
3. The unbound legacy CLEAR_TRIP fallback after 30 s is by design.
4. `safety_core.c:1482`: the scratch boot reason is not updated on an S9
   escalation.
5. A commanded reboot is labelled WATCHDOG; the ESP treats it as fatal,
   which is fail-safe.
6. `uart_owner.c:178-196`: the RX ISR advances `s_rx_tail` on ring-full;
   CRC catches the damage, cost is liveness only.
7. `link_task.c:1614`: the TEST_TRIP 10 s rate limit uses tick ms that wrap
   at 49.7 days; harmless.
8. `relay_owner.c:142-144`: tick wrap grants one extra grace period;
   fail-safe.
9. `safety_guards.c:755-756`: the S7 "Checked first" comment is wrong; the
   order is S16, S6a, S6b, S7.
10. A failed seqlock config read uses defaults for one tick (T2, known).
11. A stale current snapshot blinds S9 by design; the watchdog covers a dead
    current_task. A2 is the case where it is not dead.
12. The S16 fallback double buffer is not covered by its CRC.
13. SET_CT_CAL and SET_CONFIG do not run `config_params_validate_ex()`.
14. H5 (heat rise, `thermal_guard` guard 1) and H9 (`ct_leak_alarm.c`,
    `ct_leak_alarm_service.c`) are ESP-only; accepted SAFETY_CASE gap (d).
    A3 is the Pico-side input they depend on.
15. Config `seq` is u32; wrap is not reachable.
16. ESP benign-reboot resume (`heat_enable.c`) treats WATCHDOG, BROWNOUT,
    STACK, MALLOC, ASSERT and "last DIAG said TRIPPED" as fatal; correct
    except for the dual-reset path in A1.

## Checked and clean

- `link_frame_trip_mask_for_reason()` (`link_frame.c:253`) is `1u <<
  (reason - 1)` for every reason; TEST = 4, mask 0x0008.
- TEST_TRIP gate order: magic, peer version >= 18, boot_id, duplicate,
  update busy, already tripped, 10 s rate limit. Admin tier is enforced on
  the ESP route (`POST /api/safety/test_trip`).
- TEST is evaluated last, never overwrites a real reason, and is held until
  `trip_verify_s` plus `test_trip_masks_real_condition()`.
- CLEAR_TRIP: boot_id and seq binding; S9 refused at both layers.
- Trip-event seqlock.
- Watchdog: all 8 tasks check in; the 1000 ms hardware watchdog is fed only
  when every task is on time.
- Config A/B: only the inactive sector is erased, with read-back verify, CRC
  and magic; running-image erase guard; volatile-install loosen check is
  fail-closed.
- MAX31856 fault-bit mapping, injection gating, `discrete_task`.
- Wrap-safe arithmetic in `tick_timing.c` and `clock_health.c`.

## Resolution (2026-10-14)

MED findings A1-A5 fixed; all with host tests and negative tests. No kilnlink
protocol bump, no wire-format change.

- **A1** (`0cc88612f`, `ff41e2c23`): the Pico refuses REBOOT and ROLLBACK while
  S9 TRIP_INEFFECTIVE is latched (`update_task_reboot_policy_decide()` and the
  new `update_task_rollback_gate_decide()`; new reply reasons
  `KILNLINK_REBOOT_RESULT_REASON_TRIP_INEFFECTIVE = 4` and
  `KILNLINK_ROLLBACK_RESULT_REASON_TRIP_INEFFECTIVE = 5`, request-triggered
  replies, so an older ESP reads them as UNKNOWN). The ESP's `POST
  /api/sw_reset` refuses with 409 when the last fresh DIAG reports trip reason
  10 (`sw_reset_s9_refusal_needed()`); an unknown or stale DIAG does not block.
  Other trip reasons are unchanged (benign reboot still resumes).
- **A2** (`8bfbddf99`): `thermo_task.c` and `current_sense.c` stamp snapshots with
  `to_ms_since_boot(get_absolute_time())`, the clock `safety_core.c` ages them
  with. New host stub `test/stubs/pico_time_clock` lets tests move the hardware
  clock independently of the FreeRTOS tick.
- **A3** (`217ca9659`): `link_task_send_status` sends NaN amps for a stale or
  never-published current snapshot (`status_amps_for_wire()`), and marks TC
  invalid for a stale thermo snapshot, using the shared
  `SNAPSHOT_THERMO_MAX_AGE_MS` / `SNAPSHOT_CURRENT_MAX_AGE_MS` (2000 ms, now in
  `tick_timing.h`, used by `safety_core.c` too). ESP side: the parser already
  carries NaN; `ct_leak_alarm_tick()` now treats an all-unknown sample as
  neither quiet nor current, so stale amps can neither raise nor clear an H9
  alarm (previously a NaN sample counted as a quiet one).
- **A4**: `safety_guards_try_clear()` refuses CLEAR_TRIP while S9 evidence is
  accumulating (verify window active with current present, or a non-zero
  current-present streak).
- **A5**: SET_PARAM upper bounds in `config_params.c`:
  `link_dead_hard_s <= 600`, `blind_grace_s <= 300`, `trip_verify_s <= 60`,
  `cj_max_c <= 125`, `overshoot_margin_c <= 300`, `tc_disagreement_c <= 500`
  (`CONFIG_PARAMS_*_MAX_*` in `config_params.h`). NOTE: the 2026-10-09 HIGH-1
  volatile path contains no numeric bounds (it refuses any change while heat is
  possible), so these ceilings are new, about 5x the compiled defaults; owner
  may adjust. Set-time only: stored records still load, and `rate_window_s` is
  left alone pending the owner decision. abs_max is not touched.

Negative tests: every fix has a mutation that the new tests catch (see the
landing report). `update_task.c`'s two call sites of `update_task_s9_latched()`
are pico-only and not host-reachable; the pure gates they call are tested.

## Resolution (lowfx, 2026-10-15): LOW findings A6-A16

No protocol change. Each code fix has a host test; negtest results are in the landing report.

- **A6** fixed: `max31856_read()` returns false when all six burst bytes OR to 0 (MISO stuck low), leaving the output in the spi_failed/NaN state. `test_max31856_hal_spi.c` rewritten for the unscripted-read case.
- **A7** partly fixed: `config_check_period_effective_s()` clamps the S16 period to `CONFIG_CHECK_PERIOD_MAX_S` (3600 s), so a corrupted large value no longer defers the check indefinitely. A corrupted 0 still reads as "disabled" because 0 is the documented disable setting; reading the period from the persisted copy was not done (owner has not said the disable setting can go).
- **A8** fixed: S2 and S11 clears are refused on `!tc_valid` (as S1 does). Test in `test_safety_guards.c` (`test_try_clear`).
- **A9** fixed: `safety_core_request_enable(true)` refuses while `s_guard_state.is_tripped`.
- **A10** fixed: when a post-clear retest re-trips with a different reason, `boot_reason_latch_trip` and the seqlock trip-event capture (new `safety_core_capture_trip_event()`) run again, so trip_seq/TRIP_EVENT carry the new reason. Test: `test_clear_retrip_with_different_reason_republishes`.
- **A11** fixed: the bad-read branch accumulates S8 `dt_s` into an open window, so intermittent bad reads no longer shorten the effective window.
- **A12, A13** not changed (accepted): both only matter if a rejected (too-new or range-rejected) record later becomes valid, for example after a firmware change that accepts it; today such a record stays rejected and the lower-seq winner is the correct record. A fix touches the persistence write path (seq floor from rejected records, no-op write seq bookkeeping) and needs its own review and flash-level test; recorded here rather than changed blind.
- **A14** not changed (accepted): returning NaN for an uncalibrated channel would poison the reporting filter and `snap.amps`, and every caller already gates on `calibrated`/commissioned. Presence detection does not use this value (`current_presence_is_flowing()` has its own uncalibrated path).
- **A15** fixed: `current_sense_set_cal()`/`set_ct_cal()` write under a critical section and `current_sense_sample()` copies `s_cal` into a local under one, so a gain/offset pair cannot tear. The call sites are Pico-only; no host test reaches them.
- **A16** fixed: auto-zero accumulates only when the snapshot is new (`snapshot_is_new()`, tested), never folding a stale `counts_avg` during degraded windows. The call site in `current_task.c` is Pico-only; the helper is tested.

- **A7** (lowfin, 2026-10-15) fully fixed in the part not blocked on the owner: the S16 check period is now read from the persisted record (`config_store_get_persisted_check_period_s()`), so a corrupted live RAM value can neither disable nor defer the check. The persisted 0 still means "disabled" (owner has not retired that setting). Test: `test_config_store_flash.c` A7 section; negtest `A7_live_period` CAUGHT.
