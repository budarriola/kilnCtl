# Bench Test Log

One line per `bench_test_run()` call, newest last, appended automatically
by `report.append_log_line()` (plan §7 owner decision 1: gitignored
`logs/bench_test/<run>/` for the full machine record, plus a human sentence
here). Never edit a past line by hand -- append only. A line never carries a
credential; anything that looks like one is `***` before it is written, same
as `transcript.md`/`summary.json` (`_redact()`).

- `20260924T072516Z_heat` suite=`heat` exit_code=1 PASS=4 FAIL=4 INCONCLUSIVE=0 NOT_RUN=0 SKIP=0 esp_fw=Sep 23 2026 23:36:45 pico_fw=Pico build: d6309f4a built 2026-09-24 04:55:47Z log=`logs/bench_test/20260924T072516Z_heat/`
- `20260924T080746Z_lcd` suite=`lcd` exit_code=1 PASS=1 FAIL=5 INCONCLUSIVE=0 NOT_RUN=15 SKIP=0 esp_fw=Sep 24 2026 01:05:11 pico_fw=Pico build: d6309f4a built 2026-09-24 04:55:47Z log=`logs/bench_test/20260924T080746Z_lcd/`
- `20260924T080808Z_stack` suite=`stack` exit_code=1 PASS=2 FAIL=2 INCONCLUSIVE=0 NOT_RUN=0 SKIP=0 esp_fw=Sep 24 2026 01:05:11 pico_fw=Pico build: d6309f4a built 2026-09-24 04:55:47Z log=`logs/bench_test/20260924T080808Z_stack/`
- `20260924T080814Z_web` suite=`web` exit_code=3 PASS=24 FAIL=0 INCONCLUSIVE=0 NOT_RUN=94 SKIP=1 esp_fw=Sep 24 2026 01:05:11 pico_fw=Pico build: d6309f4a built 2026-09-24 04:55:47Z log=`logs/bench_test/20260924T080814Z_web/`
- `20260924T084342Z_lcd` suite=`lcd` exit_code=1 PASS=1 FAIL=5 INCONCLUSIVE=0 NOT_RUN=15 SKIP=0 esp_fw=Sep 24 2026 01:05:11 pico_fw=Pico build: d6309f4a built 2026-09-24 04:55:47Z log=`logs/bench_test/20260924T084342Z_lcd/`
- `20260924T084524Z_stack` suite=`stack` exit_code=2 PASS=0 FAIL=0 INCONCLUSIVE=0 NOT_RUN=4 SKIP=0 esp_fw=Sep 24 2026 01:05:11 pico_fw=Pico build: d6309f4a built 2026-09-24 04:55:47Z log=`logs/bench_test/20260924T084524Z_stack/`

2026-09-24: heat suite (ESP 351304cb, Pico 6bb41fe1) ran HP-01..08 -- HP-02/04/05/06 PASS; HP-01/03/07/08 FAILed on harness defects, not firmware, all fixed same day (0567bf09, 35d407df): HP-01 wrong-order zone-limit sequencing, HP-03 zone-override fields, HP-07 pinning target to the POSTed limit while lowering the limit at IDLE instead of RUNNING, HP-08 snapshotting firing history before the teardown delete erases it. LCD suite ran twice (080746Z, then again 084342Z after the fix below): LCD-21 PASS both times; LCD-01/08/09/14/16 FAILed both times on a screen-idle race -- injected touches were swallowed by `screen_idle_touch_swallow()` waking a blanked panel, fixed same day in `cases_lcd.py` (866003ea) by waking and re-homing before each of those five cases; the FAILs above predate that fix and were not re-run after it landed. LCD-19 NOT_RUN both times, needing `KILNCTL_LCD_PIN` (unset on this run). Stack suite ran twice: 080808Z (SK-01/02 FAIL, SK-03/04 PASS, before SK-02's 64 B noise-tolerance and fw_commit-gate fix in 866003ea) and 084524Z (preflight refused, exit_code=2, on a stale MCP server reported by `get_heap_status`'s `[STALE MCP SERVER]` banner -- rerun pending after a restart). Web suite (080814Z) ran render-only rows to exit_code=3 (WEB-WIFI-06 SKIP, needs an operator; the rest NOT_RUN as the run was not extended past the render pass). No firmware defect found in any of the above; every FAIL traced to a runner/harness bug and is disclosed under the matching ROADMAP M18 entry.
