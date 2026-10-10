# Review: LCD R4-R7, PcTools batch D, host-test campaigns 1-4 and 9c (2026-10-10)

Review only. Nothing here was fixed. Base reviewed: origin/dev `1d3f7dac8`.

| Batch | Commits |
|---|---|
| A. LCD R4-R7 (lcdfx2) | `3de432469`, `77fb02aad` |
| B. PcTools batch D | `0e76d38b6`, `f3377603b` |
| C. Host-test campaigns | `0e29a96a0` (9c), `4af0b912f` (1+2), `af1a9e26a` (3+4) |

Every negative test was run through `tools/negtest.ps1` in a throwaway copy.
Each run passed its baseline and reported `real_tree_unchanged: true`, and
`git status` was clean in the review worktree afterwards.

## A. LCD R4-R7

| Id | Severity | Finding |
|---|---|---|
| A1 | MED (owner decision) | `lcd_touch_cal_exit_target()` (`ui/lcd_auth_state.c`) now returns `"touch_test"` with no role held. That breaks the letter of the owner rule "no non-dashboard LCD page opens without a held role" (the lcd_read_gating and login_dashboard_only decisions). The real risk is low: `ui_page_touch_test.c` is a drawing canvas with Clear and Done (to home) buttons, shows no data, and is reachable only after a first-boot calibration save or as an admin. It still needs explicit owner sign-off, or a documented exception in the owner-decision note. |
| A2 | LOW | The exemption matches on the page name inside a pure helper. Any future caller that passes `"touch_test"` skips the role check as well. It would be safer to key it on the calling flow (for example a `from_calibration` flag), so the exemption cannot spread. |
| A3 | LOW | R4 (unit captured when the pad opens, `s_pad_pref` in `ui_page_profile_builder_segment.c`) and R5 (caption labels taken from child 0 of `build_card()`) have no test. The code reads correctly: child 0 is the caption label. |
| A4 | NIT | The new `test_ui_page_home_rail.c` checks for `ui_unit_entry` clamping and rounding are Celsius-only, so Fahrenheit rounding is not pinned. |
| -- | OK | `77fb02aad` only marks R4-R7 FIXED in `REVIEW_LCD_FIX_2026-10-10.md`. |

## B. PcTools batch D

| Id | Severity | Finding |
|---|---|---|
| B1 | MED | `debug_program(peer="pico")` now calls `_esp_profile_running_refusal()`. That helper fails closed, so the Pico reflash is refused whenever the ESP's profile or autotune state cannot be read. This includes the cases where flashing the Pico matters most: the ESP is in the recovery image, bricked, or the link is down. The override is `allow_running=True`, and the refusal text does say that the state could not be read. It still blocks a routine recovery path by default. `bench_test/cases_fl.py:391` calls `srv.debug_program(peer="pico", confirm=True)` without `allow_running`, so that case now fails whenever the ESP is unreadable. |
| B2 | LOW | Negtest gap: making the autotune branch of `_esp_profile_running_refusal()` (`mcp_server_debug.py`, `if at.state not in (0, 5, 6)`) a no-op is MISSED by `test_pctools_batch_d_2026_10_10.py`. No test covers debug refusing during an autotune, the `allow_running=True` override, or the idle pass. `test_debug_program_pico_refused_while_running` mocks the helper. |
| B3 | NIT | Refusal wording: `"refusing to reprogram the Pico under ESP while ..."` reads badly, because the helper appends `" ESP"` to the action string. |
| B4 | NIT | `zone_current_sweep_start`: a read-back state outside (running, done) is reported UNVERIFIED rather than FAILED. This is defensible, since the sweep may finish between calls. |
| B5 | NIT | The `profiles_save` FAILED message for a segment mismatch (tolerance 1.0 C) does not say which segment or field differed. |
| B6 | LOW | `negtest.ps1 -Parallel`: when the baseline fails, the human output and the JSON `mutations` list included only the mutations from workers 1..N. Worker 0, which holds the baseline, dropped its own mutations (seen live: 2 of 4 listed). The verdict and exit code are correct (ERROR, exit 2, no CAUGHT or MISSED), so this cannot produce a false green. The report is incomplete, though, and `check_negtest.ps1`'s `baselinefail_par` does not assert the mutation count. |
| -- | OK | The `_wifi_write_refusal` allow-list (exec state 0/3/4, autotune 0/5/6) fails closed and is tested. These enums match `devices_autotune.STATE_NAMES` and the exec state names. The read-back paths for ramp_assist and adaptive_tune report a mismatch as FAILED and a failed read as UNVERIFIED with state UNKNOWN. The `OSError` in `safety_cfg_http_client` is caught after `URLError`, which is the correct order. |

## C. Host-test campaigns

### Exit-code chaining

- SaftyFW `build_host_tests.ps1`:
  - A build failure exits 1. A timeout gives exit code -1.
  - All 9 executables appear in both the `$results` table and the exit chain.
  - SaftyFW has no `-Only` filter, so every executable runs.
  - Confirmed live: a mutation that broke only `thermo_task_tests.exe` produced `SAFTYFW HOST TESTS: FAILED -- thermo_task_tests.exe (exit 1)`. A mutation that broke the `safety_core_host_tests.exe` build produced `BUILD FAILED`. Both ended in a nonzero exit.
  - Verdict: the chain cannot report green when an executable fails.
- C1 (LOW, robustness): the per-executable exit chain is written out by hand. A future executable added only through `Add-HostBuild` would build and run, but its exit code would be ignored. Iterating `$exitCodes` generically would close that gap.
- KilnFW `test_update_http_refusals.c`:
  - It `#include`s `test_update_fetch.c` with `main` renamed to `uf_main`.
  - Both files share `g_fail`, so the extra tests fail the executable.
  - `$totalExpected` is 83.
  - Sound.
  - NIT: the executable re-runs every `test_update_fetch` test, so that runtime is paid twice.

### Do the harnesses run the real code?

- Task harness (`stubs/task_harness`):
  - The stub `xTaskCreate` captures the real `thermo_task_fn` / `watchdog_task_fn`.
  - Hooks in `ulTaskNotifyTake`/`vTaskDelayUntil` script one loop iteration at a time and leave the loop with `th_abort()` (longjmp).
  - A hook that never aborts loops until the per-executable timeout, which gives exit -1 and a failure. That is safe.
- C2 (LOW):
  - `th_run_captured_task()` returns silently if no task was captured. The watchdog tests assert `th_captured_task_fn() != NULL`. The thermo scenarios do not assert it directly, but they read `s_out[]` and `s_wait_ticks[]`, which stay zeroed if the task never ran, and they assert `valid`/`checkin == 3`. So a run with no captured task would fail there too.
  - `th_abort()` outside a run is undefined behaviour (no guard). It is unreachable today.
- The thermo scenarios use the real `max31856.c` over `fake_spi`, and map each snapshot through the same `tc_valid = valid && fresh` mapping as `build_input()` into the real `safety_guards.c`. The assertions are specific values (exact `tc_c`, NaN checks, fault bits, S5 trip). None of them looked vacuous.
- `test_link_task_fuzz.c`:
  - It `#include`s the real `link_task.c`.
  - The fuzzer counts grants against an independently computed expectation, and asserts that real enable frames were exercised, so the fuzzer cannot pass vacuously by never producing one.

### Negative tests

| Mutation | Target | Result |
|---|---|---|
| A: remove the lower clamp in `ui_unit_entry_to_celsius` | `test_ui_page_home_rail.c:91` | CAUGHT |
| A: `roundf` replaced by `truncf` in `ui_unit_entry` display | `test_ui_page_home_rail.c:95` | CAUGHT |
| A: drop the `touch_test` exemption | `test_lcd_auth_state.c:362` | CAUGHT |
| B: ramp_assist mismatch check disabled | batch D pytest | CAUGHT |
| B: wifi autotune guard disabled | batch D pytest | CAUGHT |
| B: debug exec-state guard disabled | batch D pytest | CAUGHT |
| B: debug autotune guard disabled | batch D pytest | **MISSED** (B2) |
| C 9c: update settings mode gate disabled | `test_update_http_refusals.c:117-138` | CAUGHT |
| C 3+4: watchdog gate `<=` changed to `<` | `test_watchdog_task_loop.c:121` (and `test_watchdog_gate.c`) | CAUGHT |
| C 3+4: thermo `cj_valid` ignores NaN | `test_thermo_task_faults.c` (CJRANGE) | CAUGHT |
| C 1+2: enable frame `length != 2` changed to `< 2` | `test_link_task_fuzz.c:294/299/607/712` | CAUGHT |
| C 1+2: `safety_core` drops thermo freshness from `tc_valid` | `test_safety_core_host.c:735` | CAUGHT |
| C: S5 `s5_bad_read_now()` drops `FAULT_TCRANGE` | all SaftyFW host tests | **MISSED** (C3) |
| C: S5 `s5_bad_read_now()` drops `FAULT_OPEN` | all SaftyFW host tests | **MISSED** (C3) |

- C3 (LOW, test gap):
  - No SaftyFW host test feeds `safety_guards.c` a `fault_bits` OPEN/OVUV/TCRANGE value together with a finite `tc_c` and `tc_valid = true`.
  - The thermo campaign covers those fault bits only end to end. There the real `max31856.c` already NaNs `tc_c` and clears `valid`, so S5 still trips through `isnan`/`!tc_valid` and the fault-bit term in `s5_bad_read_now()` is never the deciding condition.
  - That term is defence in depth, and nothing pins it.
  - Fix: one direct `safety_guards` case per bit in `test_safety_core_host.c` or the guards tests.
