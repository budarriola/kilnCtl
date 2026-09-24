# profile_executor panic during HP-07, 2026-09-24

Investigated read-only. No ack, clear, reset, flash or heat was performed. The
crash record is still unacknowledged on the board.

## Verdict

**This is a new defect, with high confidence.** It is a deliberate `assert()`, not
memory corruption, and it is not the closed PC-of-zero "panic at profiles_stop"
incident.

The M15 mode-state self-check (`exec_mode_state_check()`, rule 5) aborted the
executor task:

- A global thermal guard tripped **while the run was dwelling**.
- `escalate_guard_trip()` set `state = FAULTED` but left `s_exec.dwelling == true`.
- The same RUNNING tick then reached the assert at `profile_executor.c:1814`.

HP-07 was redesigned today (0567bf09 / 35d407df) so that the guard limit equals
the profile target. It is the first bench case to trip a guard mid-dwell, which
is why this latent defect from 2a04d26e (2026-09-04) shows up now.

## Evidence

### Crash record (`GET /api/crash_report`)

| Field | Value |
|---|---|
| `exc_task` | `profile_executo` |
| `exc_cause` | IllegalInstruction |
| `reset_reason` | PANIC |
| `fw_build` | `Sep 24 2026 01:05:11` (ESP 351304cb) |
| `crash_uptime_s` | 5018 |
| `dump_id` | 861174328 |

### Symbolizing ELF

`find_crash_elf` confirmed the ELF as
`firmware/KilnFW/elf_archive/KilnCtrl-9cc2af58c0de.elf` (archived
2026-09-24T08:07:25Z, commit 351304cb). The raw dump is archived at
`firmware/KilnFW/coredump_archive/coredump-a3cd564130c2.bin` (+ `.json`).

### Panic reason, symbolized

```
assert failed: executor_task_entry profile_executor.c:1814
  (mode_violations == 0 && "exec_mode_state_check found a mode-state violation -- see the ESP_LOGE just above for which rule")

#0 panic_abort                     panic.c:471
#1 0x4037ff8c esp_system_abort
#2 0x4037e5e4 __assert_func
#3 0x42085b1f executor_task_entry   profile_executor.c:1814
#4 0x4215dfbd vPortTaskWrapper
```

### Registers

| Register | Value |
|---|---|
| exccause | 0x0 |
| excvaddr | 0x0 |
| epc1 | 0x4037716f |
| pc | 0x4037ffc0 `<panic_abort+16>` |
| a0 | 0x8037ff8c |
| a1 | 0x3fcbe060 |
| crashed TCB | 0x3fce8fa0 |

"IllegalInstruction" is how `panic_abort` traps. It is not a wild PC: the
backtrace is intact and ends in `__assert_func`.

### Log ring recovered from the dump's RAM

Timestamps are milliseconds since boot.

```
I (4965492) feedforward ambient reference 27.7C
I (4965552) profile 'BENCH_HP' (id 7, zone_mask 0x01) running          <- HP-07 start
W (5020572) safety_link: isolated fault line ASSERTED (sources 0x00 -> 0x20)
W (5020572) profile_executor: zone 0's heat is blocked: sources 0x20
E (5020582) profile_executor: GLOBAL thermal guard tripped on zone 0, whole run faulted: 36.4C >= max_temp_c 36.4C
E (5020582) profile_executor: exec_mode_state_check: rule 5: dwelling==true while state=4 (not RUNNING/PAUSED)
```

State 4 is `PROFILE_EXEC_FAULTED`.

### Stack is not implicated

- `get_stack_margin` now reports `profile_executor` at 2804 B free of 4096.
- The stack suite run earlier the same morning (`logs/bench_test/20260924T085048Z_stack`)
  reported a high-water mark of 2708 B free of 4096, OK.

## Mechanism (source at 351304cb; line numbers unchanged at 0df96d5d)

1. HP-07 (`tools/PcTools/src/kilnctrl/bench_test/cases_heat.py`) lowers zone 0's
   `max_temp_c` to ambient + 3 (36.4 C). It then runs slot 7 with
   `target_c == limit_c`, ramping at 600 C/h into a 2 min dwell.
2. Zone 0 reaches target, and `profile_executor.c:963` sets `s_exec.dwelling = true`.
3. Zone 0 reaches 36.4 C >= `max_temp_c`. At `profile_executor.c:1551`,
   `thermal_guard_tick()` fires and calls `escalate_guard_trip()`. The global
   branch (`profile_executor_relay_io.c:720`) sets `s_exec.state = PROFILE_EXEC_FAULTED`.
   It does **not** clear `dwelling`; neither do the per-zone abort branch (:761)
   or the all-heaters-faulted branch (:789).
4. The tick does not return. It falls through to the end of the locked RUNNING
   body: `exec_mode_state_check()` at `profile_executor.c:1813`, then
   `assert(mode_violations == 0 ...)` at :1814.
5. Rule 5 (`profile_executor.c` ~103-165) forbids `dwelling && state not in {RUNNING, PAUSED}`.
   The check returns 1, the assert fires, `abort()` runs, and the ESP reboots.

Assertions are live in production builds
(`CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y`).

**Why the table claims this cannot happen.** The rule-5 rationale in
`profile_executor_internal.h` (~85-215) says every path out of RUNNING/PAUSED
"resets the whole s_exec_state_t including dwelling". That is false. `dwelling`
is cleared only at run start (`profile_executor_run.c:412`) and at a segment
advance (`profile_executor.c:1056`). The rule-4 text makes the same false claim
that DONE/FAULTED clear the zones' `active`.

**Other sites that leave `dwelling` stale, but survive today:**

| Site | Transition | Why it does not panic today |
|---|---|---|
| `profile_executor.c:2001` | watchdog to FAULTED | Set outside the tick; the next tick starts non-RUNNING and `continue`s at ~589-625 before the check |
| `profile_executor.c:1028` | DONE from a dwell | `continue`s before the check |
| `profile_executor.c:908` | RELAY_IO DONE path | `continue`s before the check |

All three survive only because of control flow, not because the state is correct.

**Prior warning.** `docs/audits/executor_panic_stack_overflow_2026-09-09.md:163-179`
already flagged this assert as "a real abort surface in this task" but could not
construct a violating sequence. A guard trip during a dwell is that sequence.

**Safety impact.** Relays had already been forced off before the assert, and the
Pico fault line was asserted (source 0x20). So the reboot did not leave heat on.
It did lose the run, its firing-history record and the HP-07 serial session, and
it left a "firing interrupted by a reboot" latch. That latch then made every
remaining case of the concurrent heat run fail (see below).

## Timeline, investigated run `logs/bench_test/20260924T085059Z_heat`

Board uptime maps to unix time as `unix ~= uptime + 1790237246`.

| Case | Uptime (s) | Result |
|---|---|---|
| HP-01 | 2613-2827 | PASS |
| HP-02 | 2827-3569 | FAIL "a profile is already running" at ~3569 |
| HP-03 | 3569-3759 | INCONCLUSIVE |
| HP-04 | 3759-4235 | PASS |
| HP-05 | 4235-4943 | FAIL "already running" at ~4942 |
| HP-06 | 4943-4961 | PASS; its run started 4951.0 and halted 4961.5 |
| HP-07 | 4961-5039 | FAIL, serial port gone. Its run started 4965.5; guard trip and assert at **5020.58** |
| board_after | uptime 17 | `reset_reason` panic |

**The HP-02/HP-05 refusals came before the crash.** They were not inconsistent
executor state. `profile_executor_run.c:252-256` gives "already running" only
for RUNNING/PAUSED; a FAULTED executor would have said "a thermal guard is latched".

The cause was a **second heat suite run against the same board and the same
slot 7**, `logs/bench_test/20260924T085635Z_heat`, from unix 1790240195.8 to
1790242639.7 (uptime ~2950-5394):

- **Its HP-01** (single zone, 1074 s, uptime ~2950-4024) was firing when our
  HP-02 tried to start. HP-08's firing history shows a foreign 1 s, zone_mask 1
  run at uptime 3570, beside our refusal.
- **Its HP-03** (on/off device zone) started a zone_mask 4 firing at uptime 4942,
  the moment our HP-05 was refused. Our HP-06 ("stop is never gated") started 10 s
  later, which is consistent with that foreign run's 3 s duration.
- **Its own failures follow from our interference and our crash:**
  - HP-01: "zones [1, 2] rose" — overlaps our HP-04 all-zone firing.
  - HP-03: `PROFILES 0x05 ... NACK` — the board rebooted at 5020.
  - HP-04/05/06: refused on the unacknowledged crash report.
  - HP-07: `409 a firing was interrupted by a reboot`.
- **Our HP-03 INCONCLUSIVE** (no relay states, flat ~27.8 C) is plausibly the
  same interference. Not investigated further.

So the refusals were a **bench-scheduling/harness defect**: nothing stops two
`bench_test_run(suite="heat")` invocations from driving one board at the same
time. They were not a firmware defect.

## Same or new?

**New.**

- **Closed "panic at profiles_stop"** (memory `project_profile_executor_panic_at_stop.md`):
  that one had PC 0, a corrupted backtrace, and `abort()` was eliminated.
  This panic has a clean backtrace and a deliberate assert.
- **`docs/audits/esp_panic_after_zone0_guard_trip_2026-09-10.md`**: an ESP panic
  right after a per-zone guard trip, backtrace lost. That trip was ~2 min into a
  ramp, where `dwelling` was probably false, so rule 5 does not obviously explain
  it. It is a *possible* earlier instance, but that is unproven.

## Proposed fix (not implemented)

1. **Clear `dwelling` on every transition to FAULTED or DONE.** Add one helper in
   `profile_executor_relay_io.c` / `profile_executor_internal.h`, e.g.
   `exec_enter_terminal(profile_exec_state_t st)`. It should set the state and
   clear `s_exec.dwelling` and `s_exec.ramp_lock_held`. Use it at:
   - `profile_executor_relay_io.c:720`, `:761` and `:789` (the three
     `escalate_guard_trip()` branches);
   - `profile_executor.c:2001` (watchdog);
   - `profile_executor.c:908` and `:1028` (DONE paths).

   This is preferred over relaxing rule 5, because rule 5's stated intent is
   that teardown resets the flag.
2. **Correct the rule-4/rule-5 rationale** in `profile_executor_internal.h`. It
   should name the real clearing sites, and either make `active` actually clear
   on FAULTED/DONE or document that it does not.
3. **Add a host test.** Start a run, reach a dwell, drive the thermal guard over
   `max_temp_c`, tick once, and require `exec_mode_state_check() == 0`. Run it
   once against the unfixed code to confirm it fails (negative test).
4. **Reconsider the production `assert()`** at `profile_executor.c:1814`. Relays
   are already off by this point, and a reboot mid-firing destroys the
   run/history evidence. A logged fault plus a latched FAULTED state may serve
   better than `abort()`. This is an owner decision.
5. **Harness:** add a run-level exclusivity lock to `bench_test_run` (or refuse
   `suite="heat"` while another heat run is live), so two sessions cannot share
   slot 7 on one board.

## Board state at investigation end

- Up about 204 s after the post-crash boot; no new panic.
- Crash record left unacknowledged, for the owner.
