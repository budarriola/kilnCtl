# Review: LCD19FX4 and DEVBREAKFX7 (2026-10-12)

Adversarial review of two dev landings:

- `c08745083` (code `12117c3a6`): REVIEW_LCD19FX3 fixes. Wake `power_state` tests, a short-OK keypad
  no-op test, and revised `closed_no_confirm` cause comments (`cases_lcd.py`, `test_bench_test_cases_lcd.py`).
- `3b1585b2b` (code `79596e562`): REVIEW_DEVBREAKFX6 fixes. New `test_main_reset_fence.c` host test that
  compiles the real `main.c`, the stack-size sub-check of `check_stack_margin_registration.ps1` rewritten
  to evaluate hex and macros with a skip ratchet, post-exit `test_link_watchdog.c` asserts run as the
  former job task, and a stricter `check_system_uart_bridge_stack_budget.py` stack reader.

Method: read both diffs and the code they test (`main.c`, `relay_authority.c`, `factory_reset.c`,
`lcd_auth_state.c`, `ui_lcd_keypad.c`). Then ran mutations with `tools\negtest.ps1`, each in a
throwaway worktree. KilnFW mutations used `-Preset kilnfw-host` with
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`. For every CAUGHT result I
read the log to confirm the mutant compiled and a test assertion failed.

No HIGH findings. No safety regression: neither commit changes firmware behavior. `79596e562` touches
only tests, checks and docs, and `12117c3a6` changes one comment in `cases_lcd.py`.

## Findings

### LOW-1: `79596e562` broke the full KilnFW host-test run (fixed later in `032909fa1`)

`firmware/KilnFW/App/test/build_host_tests.ps1:2242-2254` adds a 99th executable, `main_reset_fence`,
but the commit leaves `$totalExpected = 98` (line 3583) unchanged. Without `-Only`, the script builds
and runs every test, then refuses to report success:

    Built: 99/98 executables
    MISMATCH: 99 executables built but 98 were expected ... Refusing to report success.

This review's first negtest baseline, at dev `648e97e6d`, failed this way (exit 1). So
`check_00_kilnfw_host_tests.ps1` was red on dev from `79596e562` until `032909fa1` ("host test count
99"). The FX7 fix status claims every mutation was CAUGHT, so those runs were evidently made with
`-Only`. That flag sets `$totalExpected` to the selected count and cannot see the mismatch. No code
change is needed now. Process note: a commit that adds an `Invoke-HostTestExe` must bump
`$totalExpected` in the same change, and the agent should run the unfiltered host-test build once
before landing.

### LOW-2: the kiln_nvs predicate's `erases_kiln_nvs` gating is not pinned by the new test -- FIXED (LCD19FX5)

`test_main_reset_fence.c:96-110` (`test_installed_hooks_behave`) checks the hal_kv hook only with
`relay_authority_reset_set_erases_kiln_nvs(true)`. A `main.c` predicate that calls
`relay_authority_reset_refuses_writer()` instead of `relay_authority_reset_refuses_kiln_nvs_writer()`
(`main.c:226`) refuses kiln_nvs writes during every factory reset, including scopes that leave kiln_nvs
alone. See mutation K2. Effect if it shipped: over-refusal, meaning kiln_nvs writes (estop_verification,
run_state, ...) are lost during, for example, a wifi-scope reset. It cannot cause unsafe heat. Fix: add
one assertion with `relay_authority_reset_set_erases_kiln_nvs(false)` + `in_flight_begin()`, requiring
`!s_hook_kv("kiln_nvs")`.

### LOW-3: stack-size sub-check silently drops an unevaluable site when another pair under the same name compares -- FIXED (LCD19FX5)

`tools/check_stack_margin_registration.ps1:703-716`. A name is marked skipped only when
`$pairs -eq 0`. If one create/register pair evaluates, every unevaluable pair under the same name is
dropped without a line in the output. That breaks the header's promise that "SKIPPED names are never
silent". Mutation C1 adds a second `xTaskCreate(zone_sweep_task, "zone_sweep",
sizeof(StackType_t) * 999, ...)` beside the real `5120` site. The check passes, and the new site is
not mentioned anywhere. No task name has two create sites today, so this is latent. Fix: report every
unevaluated pair (for example, a `partial` list printed, and ratcheted, like `$skipped`).

### LOW-4: the ratchet freezes 23 unchecked create/register pairs, and its reasons are boilerplate (some wrong) -- FIXED (LCD19FX5)

`tools/check_stack_margin_registration.ps1:615-640`. 14 "create site only" and 9 "register site only"
entries share two copy-pasted reasons. Some of those reasons are wrong:

- `monitor_task` is a permanent boot task created in `monitor_task.c:130`, not a "dynamic/on-demand task".
- `i2c_owner_task`/`spi_owner_task` are the FreeRTOS names behind the registrations `i2c_owner_sx1509`,
  `i2c_owner_ns2009` and `spi_owner`. Their sizes arrive through the `stack_depth` parameter, so the
  stated reason is wrong for them too.
- `recovery_exit_reboot` (create) and `recovery_exit` (register) are one task, created and registered
  with literal `2048` B four lines apart (`ota_http_recovery.c:173,183`). A comment claims they are
  kept equal, but no check compares them.

Mutation C2 changes the create to `4096`, and the check still passes. This is the same zone_sweep
mismatch class the sub-check exists to catch, hidden by a name alias. I compared the current values
by hand (SX1509 4096/4096, NS2009 3072/3072, recovery_exit 2048/2048), and all agree today. Fix: add
an alias map (`recovery_exit_reboot` -> `recovery_exit`, ...) so these pairs are compared, and give
each ratchet entry its real reason.

### INFO-1: `$minStackCompared = 5` is not a ratchet -- FIXED (LCD19FX5)

`check_stack_margin_registration.ps1:641`. 31 pairs compare today, but the floor is 5. The
skip-set ratchet already makes a blind regex fail (every name becomes a new skip), so this is
belt-and-braces. Still, the comment calls it a "ratchet floor". Either raise it to 31 or drop the word.

### INFO-2: Resolve-StackExpr divides in floating point

`check_stack_margin_registration.ps1:670`. `Invoke-Expression` uses PowerShell's `/`, which gives
a double, and `[int64]` rounds it half-to-even. C truncates. `4097/2` evaluates to 2048 in C but 2049
here. No stack size uses division today.

### INFO-3: uart-bridge reader crashes, not fails, on a leading-zero decimal

`check_system_uart_bridge_stack_budget.py:127`. `int('0123', 0)` raises `ValueError`, and the call
runs at import. The process exits non-zero with a traceback instead of a `FAIL --` line. The effect
is still fail-loud, and C would read `0123` as octal anyway.

### INFO-4: wrong comment in test_main_reset_fence.c -- FIXED (LCD19FX5)

`test_main_reset_fence.c:18-19` says the phase prototypes "must match main_internal.h or the real
main.c fails to compile against them". The test defines `MAIN_INTERNAL_H` first, so `main_internal.h`
is never included in this executable, and nothing compares the two sets of prototypes. They match
today (`main_internal.h:116-133`). A drift would go unnoticed here and be caught only by the target
build.

### INFO-5: the short-OK keypad test pins the fake, not the judge

`test_bench_test_cases_lcd.py:3161-3170` (`test_pin_keypad_ok_on_short_entry_is_noop`) exercises
`PinKeypadUiTest`, the test double. Its X12 mutation (REVIEW_LCD19FX3) was in the fake too. The fake
does match firmware: `ui_lcd_keypad.c:160-162` `try_submit()` returns early when
`!lcd_pin_entry_can_submit()`, and `lcd_auth_state.c:95-100` returns `TOO_SHORT` without counting an
attempt. So this is a fidelity test, which is legitimate, but it proves nothing about `cases_lcd.py`.

### INFO-6: the wake docstring disagrees with the pinned unknown-state behavior

`cases_lcd.py:3047-3053` says the judge wakes "ONLY when the panel is confirmed off". The new test
`test_wake_unknown_future_power_state_taps_failsafe` pins a tap for `power_state=99` even with
`screen_on=True`. Per INFO-3 of the FX3 review this is a deliberate fail-safe (a backdrop tap cancels
the keypad and never starts a firing). The docstring should say "not confirmed ON".

## Verified correct

- `test_main_reset_fence.c` really compiles `../main.c` and runs `app_main()`. It catches a hook
  installed after the first phase (K1), a wrong predicate identity (K5), and a skipped bring-up phase
  (K6). It also checks hook behavior through the real `relay_authority.c`.
- `test_link_watchdog.c` post-exit asserts now run as the former job task (3), and a second job enters
  from task 4. Mutation K3 (job_exit does not clear) and mutation K4 (job_enter records a constant) are
  CAUGHT. `factory_reset.c:235/451` pair `job_enter`/`job_exit` on every path through the job.
- The LCD wake tests: an ERROR_HOLD-counts-as-ON mutant (P1b), an unreadable-state-taps mutant (P2)
  and an unknown-state-falls-back-to-screen_on mutant (P3) are all CAUGHT by assertion failures.
- `check_system_uart_bridge_stack_budget.py` parses the source before the ELF SKIP, so a bad source
  fails even on a machine without a build.
- The stack-size sub-check now evaluates hex. C3 (`5120` -> `0x1000` at the zone_sweep create) is
  CAUGHT.

## Mutation table

| ID | File | Mutation | Runner | Result | Evidence |
|----|------|----------|--------|--------|----------|
| K1 | `main.c` | hal_kv hook installed after `main_boot_early()` | kilnfw-host | CAUGHT | `test_main_reset_fence.c:88` all four hooks installed at first phase |
| K2 | `main.c` | kiln_nvs predicate calls `relay_authority_reset_refuses_writer()` (ignores the erases flag) | kilnfw-host | MISSED | LOW-2 |
| K3 | `relay_authority.c` | `job_exit` leaves `s_reset_job_task` set | kilnfw-host | CAUGHT | `test_link_watchdog.c:241,276` exemption gone after exit |
| K4 | `relay_authority.c` | `job_enter` records constant task 3 | kilnfw-host | CAUGHT | `test_link_watchdog.c:246,248` second job from task 4 |
| K5 | `main.c` | cfg_fs_mount hook gets the kiln_nvs predicate | kilnfw-host | CAUGHT | `test_main_reset_fence.c:90` cfg_fs_mount hook identity |
| K6 | `main.c` | `main_bridges_bringup()` behind a never-true volatile | kilnfw-host | CAUGHT | `test_main_reset_fence.c:87` all four phases ran |
| P1 | `cases_lcd.py` | `power_state != TOUCH_POWER_STATE_OFF` | pytest `-k 'Lcd19 or PinKeypad'` | CAUGHT, not counted | NameError swallowed by the helper's `except` (the constant is not imported), so the mutant is invalid; replaced by P1b |
| P1b | `cases_lcd.py` | ERROR_HOLD counts as ON | pytest | CAUGHT | `test_wake_taps_for_off_and_error_hold` assertion |
| P2 | `cases_lcd.py` | unreadable `screen_on` falls through to a tap | pytest | CAUGHT | `test_wake_never_taps_when_on_or_unreadable`, `test_wake_unreadable_state_never_taps` |
| P3 | `cases_lcd.py` | unknown `power_state` falls back to `screen_on` | pytest | CAUGHT | `test_wake_unknown_future_power_state_taps_failsafe` |
| C1 | `zones_current_sweep_task.c` | second `zone_sweep` create with `sizeof(...)` size | check `check_stack_margin_registration.ps1` | MISSED | LOW-3 |
| C2 | `ota_http_recovery.c` | `recovery_exit_reboot` create 2048 -> 4096 (register stays 2048) | check | MISSED | LOW-4 |
| C3 | `zones_current_sweep_task.c` | zone_sweep create `5120` -> `0x1000` | check | CAUGHT | `STACK MARGIN STACK-SIZE CHECK FAILED` |

The KilnFW mutations ran at dev `d78551461` (`-Rev`) through `-Command` with
`build_host_tests.ps1 -Only 'main_reset_fence|link_watchdog'` (baseline PASS). Every CAUGHT row above
is a compiled mutant with a test assertion `FAIL file:line`; none is a build failure. Full-suite
baselines could not be used: at `648e97e6d` the unmutated run fails LOW-1's count mismatch, and at
`d78551461` it fails in `test_autotune_engine_prestart.c` (lines 1426, 1584, 1596, 2371-2377, 2422),
which neither reviewed commit touches. That autotune failure is outside this review's scope, but
someone should check whether dev's full host-test run is red at `d78551461`.

LCD19FX5 negtests: K2, C1, C2 and sanity mutant S1 (zone_sweep register 5120 -> 4096) all CAUGHT. K2 by `test_main_reset_fence.c:110` (compiled mutant); C1/C2/S1 by the check's own throw. C1 now fails on the partial-site report, C2 on the recovery_exit alias comparison.
