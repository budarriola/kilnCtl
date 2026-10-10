# Review: SaftyFW + CommonFW since v1.0.0-pre.2 (2026-10-10)

Review only, no fixes. Scope: `git diff v1.0.0-pre.2..origin/dev -- firmware/SaftyFW firmware/CommonFW`
(3466-line diff, read in full against origin/dev `942cefb97`; origin/dev `fbb9ed468` at
write time differs only in SaftyFW host tests and `check_link_impl_isolation.ps1`).
Purpose: find interaction bugs between batches that were reviewed piecemeal
(kilnlink 17 / M4 trip_seq binding, SET_PARAM edit-list staging, F2 heat-grant drop,
volatile-install gate, L3 no-op skip, F5/F6/F7 guard changes, S9/S12 changes,
tc_offset and ct_cal bounds, trip-event seqlock, liveness filter).

## Verdict

No HIGH or MEDIUM finding. No guard is disabled or has its threshold loosened by
this range. Watchdog feeding is untouched (no hunk touches watchdog or feed code).
Two LOW interaction findings and five INFO notes below.

## Findings

### LOW-1: uncommissioned fitted CT blocks every volatile install of a trip-relevant field

`link_task.c` heat probe (~1775-1828) treats `current_task_any_current_present()`
as "heat possible". That value is masked to fitted channels
(`config_store_mask_current_present_to_fitted()`), but on a board with a CT chain
marked fitted and not yet calibrated, the counts-domain fallback reads the op-amp
offset floor as "current present" permanently. Result: `config_store_heat_possible()`
stays true, so `APPLY_CONFIG_VOLATILE` refuses any change that `would_loosen()`
does not prove a tightening, including `k_ct_v_per_a` and an abs_max raise
(kiln_cfg swap). COMMIT is separately refused while ARMED (pre-existing). The path
fails closed and the bench CT is commissioned, so there is no bench impact, but
commissioning a CT through the volatile path can deadlock until the CT is
calibrated by COMMIT while disarmed. Suggest: gate the current term on
`current_sensing_commissioned`, or document the required order.

**Resolution (dad356035): documented, check unchanged pending owner.** The required order (commission CT params while disarmed, via COMMIT, before arming) is in `firmware/CommonFW/docs/LINK_PROTOCOL.md` next to the `APPLY_CONFIG_VOLATILE` gate text. `scenario_heat_probe_current_floor` (`test_link_task_fuzz.c`) pins the fail-closed probe (present current => heat possible); gating on `current_sensing_commissioned` remains an owner decision.

### LOW-2: lost ANNOUNCE after ESP reboot silently downgrades M4 clear binding

`link_staging_apply_context_session()` zeroes `s_peer_announce.version` on an ESP
boot_id change unless the ANNOUNCE came under the new boot_id. If that ANNOUNCE is
lost, the Pico treats the peer as version 0: it sends the 30-byte DIAG (no
trip_seq) and accepts an unbound 3-byte CLEAR_TRIP. The ESP re-announces at most
3 times, 2 s apart (`safety_diag_reannounce_consider_locked`), then logs
"no trip_seq binding until reboot or link-down" and stays unbound. Not less safe
than the pre-17 baseline, but the M4 guarantee is weaker than the protocol doc
implies under link loss. Suggest an unbounded slow re-announce (for example every
30 s) while `diag_trip_seq_known` is false.

### INFO

- **trip_seq restarts at 1 on Pico reboot** (reset-one-side class). The ESP clears
  `diag_trip_seq_known` and re-arms the announce budget on a Pico boot_id change
  (`safety_link_frames.c`), so a stale seq cannot match. Covered.
- **S12 cannot be cleared while the cold junction reads invalid/NaN.** Conservative
  and intentional; noted because it can look like a stuck latch on a bad sensor.
- **INJECT_TC refused while ARMED or energized.** ARMED is the standing state, so
  injection is effectively disarmed-only. Usability only.
- **S9 no longer needs `context_valid`.** Still needs the debounce streak and
  `current_sensing_commissioned` (otherwise a non-latching warn). Residual: a
  commissioned board with a persistent CT mis-read can hold an unclearable S9.
- **Trip-event reader seqlock is bounded at 64 attempts** and returns the last
  (possibly torn) snapshot after that, as documented. At worst one torn DIAG read;
  the writer never emits seq 0 (255 wraps to 1).

## Checked and clean

- Liveness filter: only ESP(0) -> SAFETY(2) refreshes S6b. Every KilnFW send goes
  through `uart_protocol_send_broadcast(UART_PROTO_DEVICE_SAFETY)`, so no legitimate
  frame is newly ignored.
- F6 bad TC read: S2/S10 hold (no reset, no accumulate); S3/S4/S13/S14/S15 still
  run via `goto context_guards`; the S5 trip still returns before the goto.
- F2 heat-grant drop: decided from `CONTEXT_FLAG_HEAT_OWNER_ACTIVE`, which the ESP
  sets for profile RUNNING/PAUSED, profile/autotune claimants (covers CT sweep and
  relay-ID) and danger mode. The context gap is computed before
  `s_last_context_rx_tick` is updated, and bad frames return before touching it.
- SET_PARAM edit list: candidate = cached full record (including a volatile
  install) plus edits; same id replaces in place; reset on COMMIT/APPLY success
  and on a new ESP session; SET_CONFIG and SET_CT_CAL drop superseded edits.
- Volatile gate allowlist: only provenance/telemetry fields; `i_present_a` value is
  compared (only the manual flag is allowlisted); `k_ct_v_per_a` is not allowlisted.
  Tightenings require a finite, enabled new value.
- L3 no-op skip: runs after the write decision, compares packed bytes against the
  real flash slot, and resets `s_persisted_record`, which correctly drops the
  volatile-dirty state.
- tc_offset bounded at SET_PARAM and commit; clamped (and reported) at load, never
  rejected, so an old slot cannot drop abs_max to defaults.
- **Owner rule (Pico abs_max never tighter than ESP zone maxima):** the range adds
  no new path that leaves the Pico tighter. A volatile abs_max raise is refused only
  while heat is possible (accepted at idle); COMMIT refusal while ARMED is
  pre-existing, and `safety_ceiling_sync` retries with backoff. The one exception is
  LOW-1, which can block a volatile raise on an uncommissioned-CT board.

## Negative tests (tools\negtest.ps1, saftyfw-host)

All baselines passed; real tree unchanged in every run.

| Mutation | Result |
|---|---|
| `link_frame.c` liveness filter returns true | CAUGHT, `test_link_frame.c:420-424` |
| `link_staging_session_drops_heat_grant` returns false | CAUGHT, `test_link_staging.c:309-323` |
| `link_task.c` call site skips `safety_core_request_enable(false)` | CAUGHT, `test_link_staging.c:334` (source-text assertion, not behavioral) |
| S2 accumulator reset instead of held on bad read | CAUGHT, `test_safety_guards.c:4324` |
| `clear_trip_occurrence_matches` returns true | CAUGHT, `test_safety_guards.c:3662-3666` |
| `safety_core.c` call site passes `bound=false` | CAUGHT, `test_safety_core_host.c:564-565` |
| `would_loosen` treats abs_max raise as tightening | CAUGHT, `test_config_store_flash.c:627-648` |

The first run of three mutations was caught only by C4100 unused-parameter build
errors (vacuous); they were re-run with `(void)` casts and caught by real
assertions, as listed. The F2 call-site mutation is caught only by a test that
greps `link_task.c` source; a behavioral test through `push_context` would be stronger.

**Resolved (dad356035):** `scenario_push_context` in `test_link_task_fuzz.c` already drives the real `link_task.c` push-context path (new boot_id and context gap without HEAT_OWNER_ACTIVE => `safety_core_request_enable(false)`; heat owner or same session => no drop). Re-run negtest (`-Preset saftyfw-host -RequireAssertion`), call site skipped: CAUGHT by `link_task_fuzz_tests.exe` (behavioural) as well as the source-text test. Probe mutation (`in.any_current_present = false`): CAUGHT by `scenario_heat_probe_current_floor`.

## Stack

A fresh SaftyFW target build is outside this review's rules (no full target
builds). The main-tree ELF (`3de9542b`, 2026-10-07) is stale and the checker
refuses it. `check_saftyfw_task_stack_budgets.ps1` was run against
`C:\wt\devbatch1\firmware\SaftyFW\build\SaftyFW.elf` (built clean from `2f8192dd2`,
2026-10-10; differs from origin/dev only in host tests and a check script, so the
target image is equivalent): all 9 tasks `[ok]`. Largest: `link_task` 5080 B of
10240 B (ceiling 9472), `update_task` 2352 B of 6144 B (ceiling 2560, 208 B
headroom to the ceiling), `safety_core` 2208 B of 6144 B. Seven tasks are
INDETERMINATE (lower bound), as before this range.
