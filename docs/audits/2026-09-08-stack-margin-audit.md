# 2026-09-08 stack-margin audit

Only `main` and `httpd_worker` had ELF-based regression checks before this
pass. `e84a2db5` showed the static walk under-reports httpd's real usage by
~1800 B (dispatch overhead + ISR window-spill it cannot see); that same
under-report class applies, to varying degrees, to every task below.

## Task inventory (from `check_stack_margin_registration.ps1`'s required
list, cross-checked against live `xTaskCreate*()` call sites)

28 `stack_margin_register()` call sites registered; 24 tasks reported live
(board reachable, `get_stack_margin()` queried directly):

| task | configured | live free | honest headroom* | status |
|---|---|---|---|---|
| httpd_worker | 8192 | 1636 | **1560 B** (19.0%, per `check_httpd_task_stack_budget.py`) | LOW, not CRITICAL |
| system_uart_bridge | 3072 | 892 | **292 B** (9.5%) | MARGINAL, new check added |
| info_uart_bridge | 3584 (PSRAM) | 1116 | 516 B (14.4%) | thin, DRAM-free |
| safety_owner_evt | 3072 | 1196 | 596 B (19.4%) | marginal |
| backlight_pwm | 3072 | 1156 | 856 B (27.9%) | marginal, low regression risk |
| profile_exec_wdt | 4096 | 1852 | 1552 B | OK |
| uart_owner_evt_task | 3072 | 2340 | 1740 B | OK |
| screen_idle | 6144 | 2004 | 1704 B | OK |
| lvgl | 8192 | 2584 | 1984 B | OK (past corruption incident, watch) |
| danger_mode | 3072 | 2320 | 1720 B | OK |
| link_watchdog | 3072 | 2388 | 2088 B | OK |
| boot_button | 3072 | 2384 | 2084 B | OK |
| thermo_owner | 4096 | 2668 | 2068 B | OK |
| profile_executor | 4096 | 2808 | 2208 B | OK |
| kiln_io_owner | 4096 | 2840 | 2240 B | OK |
| telemetry_log | 6144 (PSRAM) | 3096 | 2796 B | OK, DRAM-free |
| spi_owner | 4096 | 3048 | 2748 B | OK |
| i2c_owner_sx1509 | 4096 | 2796 | 2496 B | OK |
| safety_proto_rx | 8192 | 3512 | 2912 B | OK |
| uart_proto_rx | 8192 | 3592 | 2992 B | OK |
| gpio_probe | 6144 | 3720 | 3420 B | OK |
| autotune_engine | 4096 | 3316 | 2716 B | OK |
| bx_flash_worker | 8192 | 4204 | 3904 B | OK |
| safety_poll | 8192 | 4696 | 4096 B | OK |

\* Honest headroom = live free bytes minus an assumed unmodeled-overhead
allowance, reasoned per task rather than copying httpd's 1800 B (which was
derived specifically from httpd dispatch + ISR spill and validated against
live probing). Assumption used here, **not independently validated the
same way**: 300 B baseline for ISR window-spill (applies to every task,
Xtensa windowed-register ABI can spill onto whichever stack is current),
+300 B more (600 B total) for tasks whose loop body dispatches on message
type through a switch/table (link-frame bridges, owner event tasks,
profile/autotune/danger command handling) since that shape is the same
"per-message routing cost the static walk's single measured path can't
see" as httpd's per-request cost, just smaller in magnitude. Not present:
`i2c_owner_ns2009`, `ota_rollback_reboot`, `ota_pico_rollback`,
`recovery_exit` — conditional/one-shot tasks not created this boot.

## Unregistered long-lived tasks (finding)

`check_stack_margin_registration.ps1` passes (28 call sites, cap 40, no
duplicates) — its required list is internally consistent. But five
long-running (`while (true)`, never self-deleting) UART bridge tasks exist
in source with **no** `stack_margin_register()` call site and are absent
from the required-name list, so their high-water mark is unreachable by any
tooling:

- `uart_bridge_thermo.c` — `thermo_bridge_task` (4096 B configured)
- `uart_bridge_touch.c` — `touch_bridge_task` (3072 B)
- `uart_bridge_ui_test.c` — `ui_test_bridge_task` (3072 B)
- `uart_bridge_io.c` — `io_bridge_task` (4096 B)
- `uart_log_bridge.c` — `uart_log_bridge_task` (4096 B)

These are not the short-lived/self-deleting UI or wifi helper tasks the
required-list design deliberately excludes (`wifi_scan_ui`, `ota_confirm`,
`cfg_autofmt`, `dns_hijack`, `zone_sweep`, etc.) — they are peers of
`system_uart_bridge`/`info_uart_bridge`/`safety_proto_rx`, created once at
boot and running forever, just never wired into the registry. Recommend
adding `stack_margin_register()` call sites and required-list entries for
all five in a follow-up pass; out of scope to add here (touching bridge
task bodies broadly was flagged off-limits for this pass beyond the one
negative-tested edit).

## Risk ranking (smallest honest headroom first)

1. **Correction (this pass):** the table above previously showed
   httpd_worker's honest headroom as **-164 B / CRITICAL**, computed by
   subtracting the 1800 B unmodelled-overhead allowance from the *live*
   high-water free figure (1636 - 1800 = -164). That double-counts: the
   live high-water mark, being an actual runtime measurement, already
   includes whatever the ESP-IDF dispatch overhead and ISR window-spill
   actually cost on this board — the 1800 B allowance exists only to
   correct the *static* ELF walk's naive free figure (`8192 -
   CEILING_BYTES`, which sees none of that runtime cost), not to be
   applied a second time on top of a live reading.
   `check_httpd_task_stack_budget.py` applies the allowance correctly, to
   the static naive free (3360 B): 8192 - 4832 - 1800 = **1560 B
   (19.0%)**, which matches the live 1528-1636 B readings within 0.3
   points — the intended cross-check, not a coincidence. The right
   number is **1560 B / 19.0%, classified LOW** (marginal, not yet
   panicking), not -164 B / CRITICAL. No firmware or formula change
   needed; only this doc's table was wrong, and it has been corrected
   above. httpd_worker remains the tightest task and worth watching, but
   it is not already over budget.
2. **system_uart_bridge** — 292 B honest headroom, internal DRAM (no
   MALLOC_CAP_SPIRAM), and an active command dispatcher (relay set, CT
   cal, danger-mode, factory-reset, watchdog-cfg). New ELF check added
   this pass (see below).
3. **info_uart_bridge** — 516 B, PSRAM-backed so no DRAM-exhaustion risk,
   but documented in-source as already hammered to a 476 B live floor
   under adversarial load (2026-09-04 note). Deserves a check but lower
   priority than #2 since it isn't drawing from the scarce DRAM pool.
4. **safety_owner_evt** — 596 B, safety-critical consequence if it
   overflows, but board tag is OK (not LOW) and it's a generic
   `owner_task` entry shared across multiple owner instances — a
   per-task ELF check would need the call graph disentangled from other
   owners using the same root, more work than this pass's budget allowed.
5. **backlight_pwm** — 856 B, but a static, rarely-touched PWM loop; low
   plausibility of regression.

## New check added + negative-tested

`firmware/KilnFW/App/test/check_system_uart_bridge_stack_budget.py` (+
`.ps1` wrapper), same pattern as `check_httpd_task_stack_budget.py`: reuses
`check_main_task_stack_budget.py`'s ELF/objdump/deepest-path machinery,
rooted at `system_bridge_task`, ceiling not fraction-of-stack (this task
isn't comfortably under budget). Measured real worst case 2192 B against
the built ELF; `CEILING_BYTES` set to that.

Negative test: added `volatile char negative_test_stack_hog[2400]` to
`system_bridge_task` in `uart_bridge_system.c`, rebuilt
(`ninja -j24` in `firmware/KilnFW/build`), re-ran the check:

```
check_system_uart_bridge_stack_budget: FAIL -- 4592 B exceeds the 2192 B ceiling.
```

Reversed the edit by hand, rebuilt, confirmed `git diff` empty and the
check passes again at 2192 B.

## Risk from work landed today

Persist bridges, the wizard's progress store, and the cold-junction change
were reviewed against this list:

- The wizard progress store (`setup_progress_http.c`) runs on
  **`httpd_worker`** — already the known-CRITICAL task, already covered by
  the existing dedicated check (its own past incident, `90bd8b2c`, was
  exactly this file).
- Persist bridges (`cfg_fs*`, off-limits for this pass) run mostly on
  **`httpd_worker`** (HTTP config endpoints) and **`system_uart_bridge`**
  (factory-reset / cfg-format command path — visible in this run's own
  `system_bridge_task` call-graph dump: `factory_reset_execute` ->
  `execute_scope` -> `cfg_fs_confirm_format_device` -> ... ->
  `get_vfs_for_path`, 1856 B of the measured 2192 B). This is exactly why
  #2 above was picked for the new check rather than a task with no active
  development traffic.
- The cold-junction change (`link_task.c`, off-limits for this pass) is
  reachable from **`thermo_owner`** and/or **`safety_poll`**, both
  currently OK (2068 B / 4096 B honest headroom respectively) — not
  flagged as at-risk today, but worth re-measuring once that change lands
  fully.

## Suite / test results

- `tools/run_all_checks.ps1` (foreground, Bypass): **72 passed, 0 skipped,
  0 failed** (71 previously known + 1 new check).
- `firmware/KilnFW/App/test/build_host_tests.ps1`: 32/32 host test
  executables built and passed.
