# Dev tools review, 2026-10-09

This review is read-only. It covers the tools/ side of origin/main..origin/dev, at
d3e2a6ba: PowerShell checks, release scripts, PcTools Python, bench_test cases and MCP
servers. Firmware is reviewed separately. No code was edited, the board was not
touched, and run_all_checks was not run. Every line number below is at d3e2a6ba.

## Findings, most severe first

### HIGH-1: ST-04 judges a failed KilnFW build as PASS (fe39e090)
`tools/PcTools/src/kilnctrl/bench_test/cases_static.py:31,38,154-170`.
`_STATUS_RE.search()` returns the first `<tag>: OK|TIMEOUT|FAILED` line it finds.
`build_kilnfw` prints the SaftyFW report first and the KilnFW report second. When the
output is `saftyfw: OK ...` followed by `kilnfw: FAILED (exit 1)`, the parser sees exit
0. The case then size-checks whatever stale `build/KilnCtrl.bin` is on disk and returns
PASS. The tests in `test_bench_test_cases_static.py` only use one-line fakes, so this
path is never exercised.
Fix: parse every status line (or the line for the named tag) and fail if any of them is
not OK.

### HIGH-2: a workbench output-check failure is judged PASS (fe39e090)
`cases_static.py:31,36-45,54-64`. The workbench `_summarize` can print
`FAILED (exit 0, but output check failed)`, for example when `run_pctools_tests` loses
an xdist worker or the slow tests are skipped. `_STATUS_RE` takes the `0` from that
line, so `_exit_status` returns `(0, "FAILED")`. `_exit_only` only checks `rc != 0`, so
it returns `None` and the case reports PASS. This affects every ST case that goes
through `_exit_only`.
Fix: return FAIL whenever `word == "FAILED"`, whatever the exit code.

### MEDIUM-1: release gate bench-evidence can pass vacuously, and can never pass for the safety suite (215ef8e2, 4be2ab67, 36d9f6a9)
**Status: fixed in tools/release_gates.py (full-suite coverage, per-case verdicts, EXPECTED_INCONCLUSIVE allowlist, tagged dirs); wired into make_release.ps1 in 9e664dfb.**
`tools/release_gates.py:217-237,247`.
- Line 227 requires `exit_code == 0`. The runner returns exit 3 for any
  SKIP/INCONCLUSIVE/NOT_RUN result, so the `"SKIP"` allowance on that line is dead code.
- The function never reads `requested_cases`, so a `--cases OT-B01` run counts as
  evidence for the whole `ota` suite.
- A summary with an empty `cases` dict and exit 0 also qualifies.
- The glob `*_<suite>/summary.json` misses tagged run directories
  (`<ts>_<suite>_<tag>`, from `report.make_run_id`).
- The `safety` suite can never qualify. SP-10 is INCONCLUSIVE by design (acc9944a),
  SP-04 is NOT_RUN unless OT-B01 ran in the same session, and SP-03/SP-06 need heat. A
  full safety run therefore always exits 3.
- `test_release_gates.py` (`BenchEvidence._run`) builds summaries with PASS+SKIP and
  exit 0, a combination the real runner never produces. That is why the tests are green.
- `bench-evidence` is not called from `make_release.ps1`. Gate `bench-pass-7d` in
  `docs/release_gates.json` stays manual.

Fix:
- require `requested_cases` to be empty or equal to the full suite, and require a
  non-empty `cases` dict;
- define an explicit per-suite allow-list of expected INCONCLUSIVE results;
- glob `*_<suite>*`;
- build the test fixtures from real runner output.

### MEDIUM-2: OT-B02 counts INCONCLUSIVE results as PASS (d3e2a6ba)
`tools/PcTools/src/kilnctrl/bench_test/cases_ota.py:2455-2463`. The case returns
INCONCLUSIVE only when every other OT row is NOT_RUN or SKIP. If the rows mix
INCONCLUSIVE and NOT_RUN, the case returns PASS with "N OT cases, none FAIL/ERROR".
Fix: return INCONCLUSIVE unless at least one row is PASS and none are INCONCLUSIVE
(or report the INCONCLUSIVE rows by name).

### LOW-1: lone-comma line nests "zone_sweep" in an array (latent; whole-file EOL churn in a62d5c42)
**Status: Fixed in c231c97c** (comma restored; check now throws on any non-string $requiredNames entry; .gitattributes `*.ps1 text eol=crlf`).
`tools/check_stack_margin_registration.ps1:177-178`. The old blob had
`"kiln_cfg_swap"\r,\r\n`. PowerShell treats the bare CR as a newline, so the comma
starts its own line and becomes a unary comma. As a result `zone_sweep` is stored as a
one-element Object[]. Verified in PowerShell: `$requiredNames -contains 'zone_sweep'`
is False.
- `.Contains($_)` at line 278 still matches, because PowerShell converts the array to a
  string there, so the check still works today. Any `-contains`/`-in` use of this list
  would silently drop zone_sweep.
- a62d5c42 also converted the blob from CRLF to LF, so its diff rewrites all 1229 lines
  when only about 47 changed. The repo has core.autocrlf=true and no .gitattributes.
Fix: put the comma back on the `kiln_cfg_swap` line, and add a .gitattributes `eol`
rule for `*.ps1`.

### LOW-2: teardown hooks still run after the executor is not confirmed idle (8b8617f2, 5207d0c3)
**Status: already fixed on dev before this pass** (runner.py `skip_hooks`/`teardown_hooks_skipped`).
`tools/PcTools/src/kilnctrl/bench_test/runner.py:375-390`. When the stop poll fails,
teardown marks the run tainted but still runs the hooks. The aux `_restore` hook calls
`control_set_aux_output(confirm=True)`, which is a relay-config write while a firing may
still be running. The write is refused by the MCP mid-run precheck and by the firmware's
409 `system_mode_gate`, so no relay changes. It is still the wrong order.
Fix: skip mutating hooks when `teardown_executor` is set.

### LOW-3: make_release.ps1 Test-DramBssBudget has no test (c712ca2c)
**Status: Fixed in c231c97c** (check_release_manifest.ps1 asserts Test-DramBssBudget refuses an unmeasurable ELF).
The gate itself is correct: it reuses `check_kilnfw_dram_bss_budget.py` and fails on any
nonzero exit. No test proves that it fails a release.

### LOW-4: route-tier stale-row check may flag routes with a null Method (7ff9bf71)
**Status: Fixed in c231c97c** (unparsed-method routes no longer make their rows stale, test_check_route_tier_coverage.ps1 2c; test_check_uri_handler_cap_max_routes.ps1 covers KILN_HTTP_MAX_ROUTES).
`tools/check_route_tier_coverage.ps1`. A route with no parsed Method is left out of
`registeredKeys`, so its tier row can be reported as stale. Neither this check nor
`check_uri_handler_cap.ps1`'s new `KILN_HTTP_MAX_ROUTES` (192,
`http_auth_http.c:50`) >= cap rule has a negative test.

### INFO
- `tools/worktree_mint.ps1` run from the main tree (still on the main version) bases the
  new worktree on origin/main, not origin/dev. This review had to re-detach onto
  origin/dev by hand.
- `bench_pico_reboot_midfiring.py` (5e26edea, 8b69f0b9) is correctly gated (an explicit
  flag plus a RUNNING-state check), makes no relay write, and its field names match the
  firmware. Its RealBoard path is tested only with fakes.
- These were checked and look correct:
  - `cases_aux.py`: the tool names, result strings, executor state enum and firmware
    409 text all match.
  - `check_duplicate_symbols.ps1` (e24d3255, 8982a37a): it refuses when the manifest
    covers 0 objects, so it does not pass vacuously.
  - workbench stall/ceiling/tree-kill (007e2ad6, 99d9e094).
