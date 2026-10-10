# SaftyFW guard and trip-logic review (2026-10-09)

Scope: RP2040 SaftyFW on `origin/dev` at `db1f4df5`, under
`firmware/SaftyFW/src`. Covered: `safety_guards.c` (S1..S16), `tasks/safety_core.c`
(input build, enable path), `config_store*` / `config_params.c` (validation, ARMED
write refusal, volatile install), `tasks/relay_owner.c`, the thermocouple read path
(`tasks/thermo_task.c`, `max31856.c`) and MAX31856 fault handling, CT presence,
and how the ESP-side H5 / H9 / guard 9 relate to the Pico guards.

Out of scope: the link protocol itself (see `SAFETY_LINK_REVIEW_2026-10-09.md`
and `KILNLINK_ROBUSTNESS_AUDIT_2026-10-09.md`). Findings already raised in those
reviews, or in `pico_volatile_install_and_unconfigured_ceiling_2026-09-14.md`,
are not repeated.

Method: source reading only, with negative tests run through
`tools\negtest.ps1 -Preset saftyfw-host`. There was no board access and no code
was changed.

Owner rules checked:

- No guard-disable path may exist. **F1 breaks this rule.**
- `abs_max_temp_c` must be the same as or looser than the ESP zone max, never
  tighter. Clean on the Pico side (see "Checked and clean").

## Findings

### F1 HIGH: the ESP can blind the Pico's own thermocouple while K4 stays ARMED

**Where:**

- `config_store_flash.c:1642-1670`: `config_store_volatile_would_loosen_safety()`
  checks only `abs_max_temp_c`, `max_rate_c_per_min` and `tc_type`.
- `config_store_flash.c:1690-1735`: `config_store_write_volatile()` applies every
  other field while ARMED.
- `tasks/link_task.c:2802-2870`: `link_task_handle_apply_config_volatile()`.
- `config_params.c:515`: `safety_tc_installed` (0x0211) is settable through
  SET_PARAM. SET_PARAM staging has no ARMED check of its own (`link_task.c:~2609`).
- `tasks/thermo_task.c:293-299` and `:316-325`: injection is gated only on
  `safety_tc_installed == 0`.
- `tasks/safety_core.c:1821-1912`: `safety_core_request_enable()` refuses only a
  *new* ON request when `safety_tc_installed == 0`. It never de-energizes a K4
  that is already ARMED.

**Failure scenario:** while K4 is ARMED and energized mid-firing, the ESP does
the following:

1. Sends `SET_PARAM 0x0211 = 0`.
2. Sends `APPLY_CONFIG_VOLATILE`. `would_loosen_safety()` does not look at
   `safety_tc_installed`, so the install is accepted, nothing is written to
   flash, and the relay stays ARMED.
3. Sends `INJECT_TC(valid=1, tc_c=25.0)` at least every 2 s, which keeps
   `thermo_fresh` true.

From then on, `thermo_task_get_snapshot()` returns the injected snapshot. S1,
S8, S11, S12 and S2/S10 all evaluate a fake 25 C while the real kiln keeps
heating. Nothing drops K4, because the relay is already ARMED.

The ESP is the component the Pico exists to be independent of. This chain needs
only a buggy kiln-package swap or a misbehaving ESP, with no physical access.
It is a guard-disable path, which breaks the owner rule.

The 2026-09-14 Finding A carve-out explicitly left "every other field"
loosenable. `safety_tc_installed` is not a PID-shaped parameter: it gates
injection.

Other trip-relevant fields are also loosenable through the same volatile path
while ARMED, each finite-checked only:

- `ct_installed` -> 0, or `ct_topology` / `zone_ct_channel`: silences S3, S9 and S14.
- `tc_offset_c`: see F2.
- `cj_max_c` / `cj_warn_c`: S12.
- `overcurrent_pct` / `overcurrent_time_s`.
- `firing_margin_c` / `overshoot_margin_c`.
- `tc_disagreement_c`.

**Suggested fix:**

- While ARMED, `config_store_volatile_would_loosen_safety()` should refuse any
  change to `safety_tc_installed`, at minimum 1 -> 0.
- Defense in depth: make `thermo_task_inject_reading()` refuse whenever
  `relay_owner_is_energized()` / ARMED. Injection is a bench-only facility.
- Then decide per field for the remaining trip thresholds listed above. The
  simplest rule is to refuse a change to any S-guard threshold while ARMED,
  leaving only PID/profile-shaped fields volatile.
- Add a host test: ARMED plus a volatile install of `safety_tc_installed=0` is
  refused.

### F2 MED: `tc_offset_c` is unbounded and shifts the reading S1 sees

**Where:**

- `config_params.c:484-489`: the value is only `CHECK_F32_FINITE()`.
- `tasks/thermo_task.c:626`: the offset is added to the hot-junction reading
  before every guard sees it.
- It is not in `config_store_volatile_would_loosen_safety()`.

**Failure scenario:** a commit of `tc_offset_c = -400` makes a real 1300 C
chamber read as 900 C. That value passes the per-type plausibility check, so S1
never trips against `abs_max_temp_c`. The commit can come through COMMIT_CONFIG
while idle or in GRACE, or through APPLY_CONFIG_VOLATILE even while ARMED.

The comment says no magnitude bound is "invented". However, `abs_max_temp_c` is
validated against `TC_MAX_C_BY_TYPE` while the offset that rescales it is not,
so the `abs_max` bound can be bypassed by one parameter. A calibration
correction for a type K/N/S thermocouple is a few degrees.

**Suggested fix:**

- Bound `|tc_offset_c|`, for example to 25 C or another owner-chosen limit,
  in `config_params_validate_ex()`.
- Treat any `tc_offset_c` change while ARMED as loosening, or at least a more
  negative value.

### F3 MED: S9 (welded contactor) is blind whenever ESP context is invalid

**Where:**

- `safety_guards.c:414`:
  `if (state->s9_verify_elapsed_s >= verify_th && in->context_valid && in->any_current_present)`.
- `context_valid` is false when any of these holds (`safety_core.c` build_input):
  - the clock is stalled
  - no CONTEXT has been published
  - DEGRADED_NO_CONTEXT
  - the context is older than 5 s

**Failure scenario:** K4 welds, S6b trips because the ESP went silent, or the
ESP reboots or hangs. The Pico has told K4 to open and the CT still sees current,
which is exactly S9's case. Because context is invalid, `s9_current_streak`
never advances, so the board never latches TRIP_INEFFECTIVE and never raises the
"go to the breaker" alarm. Escalation happens only if the ESP comes back. The
likeliest moment for the weld to matter, ESP dead with mains live, is the one
moment S9 is gated off.

The in-code rationale (`safety_guards.c:381-385`, from `c3542d97`) is "S3 gates
on context_valid; S9 must too". S3 needs context because it reasons about the
ESP's heat command. S9 reasons about the Pico's own de-energize command, which
it always knows. `SAFETY_MODEL.md:678` does not mark S9 as *needs context*, while
it does mark S2, S3, S4, S10 and S14. The current-trust concern is already
covered by `S9_CURRENT_PRESENT_STREAK_TO_TRIP` and the commissioned /
uncommissioned split.

**Host-test vacuity:** every S9 test sets `context_valid = true`. The negative
test removing the gate was MISSED (see below).

**Suggested fix:**

- Drop `in->context_valid` from the S9 condition.
- Add tests in which S9 escalates with `context_valid = false` after an S6b trip.

### F4 LOW-MED: S12 passes silently on a NaN cold-junction reading

**Where:**

- `max31856.c:385-395`: a CJRANGE fault sets only
  `cj_temperature_c = NAN` and leaves `tc` finite.
- `safety_guards.c:128-133`: `s5_bad_read_now()` checks
  OPEN/OVUV/TCRANGE/NaN-tc but not CJRANGE.
- `safety_guards.c:694`: `if (in->cj_c > cj_max)`. A NaN takes the else branch,
  which clears `s12` warn and resets the accumulator.
- `safety_guards.c:233`: with NaN cj, the clear check reads "not immediate",
  so a CLEAR_TRIP of an S12 trip is allowed.
- `tasks/thermo_task.c` computes `cj_valid`, but it never reaches
  `safety_guard_input_t`. `safety_core.c:1334` passes only `.cj_c`.

**Failure scenario:** the enclosure, and therefore the MAX31856 die, goes past
its CJ range (above 125 C). That is the hottest possible S12 case. The cold
junction reads NaN, S12 resets every tick and never trips, and an earlier S12
trip can be cleared.

It is worse than a stale reading because the accumulator is zeroed every tick.
The CJ-compensated hot-junction value is also suspect in that state, yet it
still feeds S1 as valid.

**Suggested fix:**

- Plumb `cj_valid` into the guard input.
- Treat `!cj_valid` / NaN `cj_c` as the S12 condition, or at least hold the
  accumulator and raise a WARN.
- Consider adding CJRANGE to `s5_bad_read_now()`.

### F5 LOW-MED: unvalidated CONTEXT floats can silence S2 and S10

**Where:**

- `tasks/link_frame.c:231-232`: `setpoint_c` / `measured_c` are unpacked raw.
- `snapshots.h:185-230`: `context_reduce_zones()`.
- By contrast, `firing_max_c` is `isfinite`-checked (`link_frame.c:325`).

**Failure scenario:** if the first eligible zone carries a NaN `setpoint_c`,
`max_setpoint` becomes NaN and every later `>` comparison is false. S2's
`tc - max_setpoint > margin` is then always false. A NaN `measured_c` does the
same to `nearest_measured`, which silences S10.

The outcome depends on zone order. A NaN in zone 2 is ignored, while a NaN in
zone 0 poisons everything. A `+Inf` setpoint silences S2 the same way. Both
guards are WARN/TRIP layers above S1, so S1 still holds.

**Suggested fix:** reject or mark ineligible any zone whose `setpoint_c` or
`measured_c` is not finite, either at unpack or in `context_reduce_zones()`.
Add a host test with a NaN in zone 0.

### F6 LOW: an S5 bad read skips the TC-independent guards

**Where:** `safety_guards.c:620-624`. On any bad read the tick returns before
S1/S11/S12/S8 and before the context block (S3, S13, S14, S15).

**Failure scenario:**

- S3 (current with nothing commanded) and S14 do not depend on the TC, but they
  are suspended for the whole S5 blind grace (60 s default) before S5 trips.
- With `safety_tc_installed == 0` declared and no injection, S5 is a permanent
  WARN, so S3 and S14 are suspended permanently.
- Enable is refused in that state, so the remaining exposure is a welded or
  stuck relay. S9 sits above the return and still runs.

**Suggested fix:** skip only the TC-consuming guards on a bad read, and still
evaluate S3, S14 and S15 against current and context.

### F7 INFO: the 32-bit ms tick wraps after ~49.7 days

**Where:**

- `tasks/reboot_announce.c`: `s_known` is never cleared, and age is computed from
  `to_ms_since_boot()`.
- The same arithmetic is used in `snapshot_is_fresh()`.

**Effect:** after about 49.7 days of uptime a stale announce can look fresh
again and reopen the 20 s reboot grace once, suppressing S6b's trip call for
that window. Freshness checks of a stale snapshot can briefly read as fresh at
the same wrap. The exposure is bounded and needs very long uptime.

**Suggested fix:** clear `s_known` once grace has expired, and compare ages with
wrap-safe unsigned subtraction everywhere.

## Negative tests

All six mutations target `firmware/SaftyFW/src/safety_guards.c`, run with
`tools\negtest.ps1 -Preset saftyfw-host -Mutations <json> -Parallel 2` in a
throwaway worktree.

The baseline passed. Results:

- **S1 ceiling +1 C** (`tc_c > ceiling + 1.0f`): CAUGHT.
  `test_safety_guards.c:3720`, `:3760` and `:3792` fail.
- **S5: drop `isnan(in->tc_c)` from `s5_bad_read_now()`**: CAUGHT.
  `test_safety_guards.c:378` fails.
- **S9 try_clear refusal made unreachable**: CAUGHT.
  `test_safety_guards.c:3606` and `:3608` fail.
- **S9: drop `in->context_valid &&` from the escalation condition**: **MISSED.**
  All tests pass. The gate is untested in both directions (F3).
- **S6b hard backstop doubled** (`>= hard * 2.0f`): CAUGHT.
  `test_safety_guards.c:2422`, `:3438` and `:4104` fail.
- **S12 trip threshold +50 C** (`cj_c > cj_max + 50.0f`): CAUGHT.
  `test_safety_guards.c:761`, `:774` and `:3394` fail.

The run's final verdict was ERROR with "REAL TREE CHANGED". That was this audit
doc being written into the review worktree while the run was in progress. The
worktree status afterwards showed only this untracked doc, both negtest copies
were removed, and the shared main tree's tracked status matched its
pre-run state.

## Checked and clean

- **relay_owner latch** (`tasks/relay_owner.c`): TRIP de-energizes GPIO6 and
  latches TRIPPED in the same case. ENERGIZE drives the pin only in ARMED. CLEAR
  resumes against the original GRACE clock, so a clear cannot shorten grace. The
  fast refusal outside the task is re-checked inside the task. Trip and clear
  are retried through the owed flags when the 4-deep queue is full.
- **S9 unclearable:** the refusal sits ahead of every other try_clear logic
  (`safety_guards.c:263`), and the same refusal is enforced at the wire layer.
- **S1:** a strict `>` against `abs_max_temp_c` for 3 consecutive valid
  readings. Bad ticks do not reset the streak (S5 returns early). An unset
  ceiling is backstopped while ARMED (`safety_core.c:1153-1169`).
  SET_FIRING_CEILING is inert.
- **abs_max owner rule:** `abs_max_temp_c` is `CHECK_F32_POS` with no upper
  cap (`config_params.c:465`), and the volatile path refuses only raising or
  clearing it while ARMED. The Pico never imposes a ceiling tighter than the
  configured value.
- **Persistent writes while ARMED:** `config_store_decide_write_ex()` refuses
  everything except tc_type-only (heat-safe-gated) and single-channel ct_cal.
  F1 is volatile-path only.
- **S6b:** the grace window suppresses only the trip calls, and it is bounded
  at 20 s. An announce counts as link activity. The hard limit is unconditional
  outside grace.
- **S8:** the window pauses across bad ticks instead of extrapolating.
- **dt clamp:** guard accumulators use a clamped `dt_s`, so a scheduling stall
  cannot jump an accumulator past a threshold or underflow it.
- **max31856:** an infinite tc is converted to NaN. OPEN/OVUV/TCRANGE make the
  read bad. An SPI failure makes the snapshot invalid
  (`thermo_task.c:625-643`, `valid = ok && !spi_failed`).
- **Intermittent bad reads:** S5 trips only on consecutive bad reads, as
  designed. S1 keeps working on the valid readings in between.
- **S11 on CT-less boards:** dormant, which is known and documented
  (`GUARD_TEST_MATRIX`). Optionally, `relay_owner_is_energized()` could serve as
  `heat_commanded` there.
- **`s_trip_seq` uint8 wrap:** documented and accepted. Clears bind to the
  trip_seq (M4 in the link review).
- **H5 / H9 / ESP guard 9:**
  - H5 (thermal_guard guard 1 heat-rise) and H9 (`ct_leak_alarm_service.c`) are
    ESP-side alarms. They do not feed any Pico guard input.
  - The Pico analogue of H9 is S3 (current with no heat commanded), which is
    context-gated by design. ESP guard 9 (control-tick liveness) stopping the
    ESP control loop leads to stale CONTEXT, then `context_valid=false`, then
    DEGRADED.
  - S1, S5, S6b and S12 stay live in that state. S9 does not (F3), and neither
    do S3 or S14 (by design).
