# Review: REVIEW_DEVBREAK4 fixes (2026-10-12)

Range reviewed on origin/dev (tip `ef37258a0`):

| SHA | Subject |
|---|---|
| 9e069743a | REVIEW_DEVBREAK4 fixes: settable host task handle + other-task refusal asserts, build-gate enumeration via git ls-files, reset-fence check strips dead code and pins both predicates, bridge stack derived from C source, 4096 margin registration, doc nits |
| 2e7dc13f2 | docs: neutralize non-object hashes in DEVBREAK4 review; refresh stale link_frame blob pin |
| 6f1cd6051 | REVIEW_DEVBREAK4: Fix status table and evidence rows |
| ef37258a0 | docs: refresh link_frame.c blob pin after rebase |

Method: fresh worktree `C:\wt\dbfx5_0uc0fs` at `ef37258a0`, minted with
submodules. Each touched check was run there unmutated. Every suspected gap was
negative-tested with `tools\negtest.ps1`: baselines passed, copies were removed
and the real tree was unchanged. The exception is the stack and host runs:
they report verdict ERROR because this review doc was created in the worktree
while they ran. That was the only change (`git status` showed just this file),
and their per-mutation verdicts stand. One mutation (G2) targets a gitignored
directory, which negtest refuses ("changed nothing in the copy"), so it was
applied by hand in the review worktree and removed afterwards. The stack checker
ran against a scratchpad copy of the ELF from `C:\wt\checkbuild_f4195c245f`,
linked 2026-10-12 00:26 from 3515c56cf, a descendant of 9e069743a. No firmware
was edited and no target build was run. The bench board was not touched.

Summary: no HIGH. Two MED: the rewritten reset-fence check misses seven
dead-code shapes that its own header and the DEVBREAK4 fix table say it
catches, and the fix table claims "Not feasible for a regex check: none". Five
LOW, several INFO. The FreeRTOS stub models task identity correctly.

## Tests run

| Test | Result |
|---|---|
| check_reset_fence_hooks.ps1 | PASS |
| check_build_gate_usage.ps1 | PASS (21 callers) |
| check_system_uart_bridge_stack_budget.ps1 (worktree, no build) | SKIP exit 3, no ELF (expected) |
| check_system_uart_bridge_stack_budget.py --elf (checkbuild_f4195c245f copy) | OK: creation 4096 == registration 4096, deepest 3216 B, honest headroom 580 B, above the 512 B floor |
| negtest fence (`-Preset check -PresetArg tools/check_reset_fence_hooks.ps1`) | F0 CAUGHT; F1 to F7 MISSED |
| negtest gate (`-Preset check -PresetArg tools/check_build_gate_usage.ps1`) | G0 CAUGHT; G1 MISSED; G2 (by hand) MISSED |
| negtest stack (`-Command "python ...check_system_uart_bridge_stack_budget.py --elf <copy>"`) | S0 CAUGHT; S1 MISSED (verdict field ERROR only because this review doc was written into the worktree mid-run; both mutation verdicts stand) |
| negtest host (`-Command "... build_host_tests.ps1 -OutDir {OUT} -Only link_watchdog"`, `-ExpectPattern "(?m)^\s+FAIL \|FAIL .*\.c:\d+\|RUN FAILURES\|BUILD FAILURES"`) | H0 CAUGHT (test_link_watchdog.c:229 and :251); H1 MISSED (same ERROR note as the stack run) |

## Stub semantics (MED-1 of DEVBREAK4)

`stubs/freertos/task.h` now keeps one `__declspec(selectany)` global current
task, default `(TaskHandle_t)1`, with `stub_set_current_task()` to change it.
Real FreeRTOS returns the running task's TCB from `xTaskGetCurrentTaskHandle()`.
A host test is single-threaded, so one global "current task" is the right model,
and the default of 1 keeps every other test's behaviour unchanged. The
`selectany` form matches the tree's other MSVC stubs. No finding on the stub.
The coverage gap is in how the test uses it (LOW-1 below).

## MED-1: reset-fence check misses seven dead-install shapes it claims to catch

`tools/check_reset_fence_hooks.ps1` header (lines 16-17) says a "commented-out,
preprocessor-dead or `if (0)`-wrapped install does not count". The DEVBREAK4 fix
table says nothing is infeasible. Mutating only the `hal_kv` install line in
`main.c`, the check still PASSes for:

| Id | Shape | Verdict |
|---|---|---|
| F0 | `#if 0` ... `#endif` around the line (control) | CAUGHT |
| F1 | braceless `if (0)` on the preceding line | MISSED |
| F2 | `#ifdef KILNCTL_NEVER_DEFINED_NEGTEST` wrap | MISSED |
| F3 | `#if 0L` wrap | MISSED |
| F4 | `#if !1` wrap | MISSED |
| F5 | `#if 1` / `#elif 1` wrap (the `#elif` branch is dead) | MISSED |
| F6 | string literal `"/*"` before an `#if 0` and `"*/"` inside it: the string-unaware comment strip eats the `#if 0`, leaving the install live | MISSED |
| F7 | all four setters moved into an uncalled `static` function before `app_main` | MISSED |

F2 is documented at line 50 ("Unknown conditions ... stay live"). It is still a
bypass, and every condition in `main.c` around these lines is depth 0 today.
F1 contradicts the header outright. F7 shows the check never requires the
installs to be in `app_main`.

Fix: anchor the four setter statements structurally, not by their presence.
- Require each one at preprocessor depth 0: no enclosing `#if`, `#ifdef`,
  `#ifndef` or `#elif` of any condition. Fail on any conditional, rather than
  trying to evaluate it.
- Require each one inside the `app_main` body at brace depth 1.
- Require the previous code line to end in `;`, `{` or `}`. That rules out a
  braceless `if`, `while` or `for` guard.
- Make the comment strip string- and char-literal-aware.

## MED-2: DEVBREAK4 fix table overstates coverage

`docs/audits/REVIEW_DEVBREAK4_2026-10-11.md` Fix status says "Not feasible for a
regex check: none; every R case is caught by the check or by the host test". F1
to F7 above are reachable shapes that are not caught. The
`GATE_NEGATIVE_TEST_EVIDENCE.md` row for `check_reset_fence_hooks.ps1` says
"negtest 2026-10-12 13/13 CAUGHT". R4, R5, R9 and R10 were caught by
`test_link_watchdog`, not by the check, so the check's own score is 9/9 of the
cases it was given. A reader of the evidence row would take the check as the
guard for the job-window class.

Fix: in the evidence row, split "check: 9/9 CAUGHT (R0-R3, R1b, R6-R8, R11)"
from "R4, R5, R9, R10: caught by test_link_watchdog, not this check". List F1 to
F7 as MISSED until MED-1 lands. Replace the "none" line with a pointer to this
review.

## LOW-1: host test always enters the reset job as the stub default task

`test_link_watchdog.c` calls `relay_authority_reset_job_enter()` while the stub
reports task 1, the default. It then switches to task 2 and asserts refusal. A
`job_enter` that records a constant `(void *)1` instead of
`xTaskGetCurrentTaskHandle()` is indistinguishable (H1). The DEVBREAK4 check
pins both predicate bodies but not `job_enter` or `job_exit`.

Fix: enter the job under a non-default task (`stub_set_current_task((TaskHandle_t)3)`).
Assert that task 3 is exempt and tasks 1 and 2 are refused, then restore task 1.
Do this for both the general and the kiln_nvs predicate. The kiln_nvs test does
not re-assert exemption after switching back; add that.

## LOW-2: build-gate lint silently drops non-ASCII paths

`check_build_gate_usage.ps1` runs `git ls-files -co --exclude-standard`. With
the default `core.quotepath=true`, git prints a non-ASCII path quoted and
octal-escaped (`"tools/negtest_gate_caf\303\251.ps1"`). The script then resolves
each entry with `Get-Item ... -ErrorAction SilentlyContinue` and drops what does
not resolve, so G1 (a held-slot `Start-Sleep` in `tools/negtest_gate_café.ps1`)
PASSes. G0, the same body under an ASCII name, is CAUGHT.

Fix: use `git -c core.quotepath=off ls-files -z -co --exclude-standard`, split on
NUL, and decode as UTF-8 (set `[Console]::OutputEncoding` or read through
`System.Diagnostics.Process` with a UTF-8 stdout). Fail loud on any listed path
that does not resolve instead of skipping it.

## LOW-3: gitignored check scripts run but are not linted

`run_all_checks.ps1` (lines 200-215) discovers `check_*.ps1` with
`Get-ChildItem`. It excludes only `\build\`, `node_modules`, `logs\`, dotted
directories and one named script. A `check_*.ps1` under an ignored directory
such as `build_agent/` is run by the suite. `--exclude-standard` hides it from
the gate lint. G2: `build_agent/check_negtest_gate.ps1` holding a slot across
`Start-Sleep`, lint PASS, while `run_all_checks.ps1 -ListOnly` lists the
script.

Fix: lint the union of the git listing and run_all_checks' own discovery set.
The cleanest way is to factor that discovery into a shared function both
scripts call.

## LOW-4: zone_sweep has the same creation/registration drift

DEVBREAK4 LOW-4 fixed `system_uart_bridge` (registered 3072, created 4096) with a
pair check for that one task. `zones_current_sweep_task.c` has the same mismatch:
it creates `zone_sweep` with 5120 (line 2503) and registers it with 4096 (line
2514). `get_stack_margin` therefore grades free space against a denominator
1024 B too small. Nothing checks the pair across the tree.

Fix: register 5120 (or share one `#define` between the two calls). Add a
tree-wide create-vs-register comparison to
`check_stack_margin_registration.ps1`. For every task name it requires, it should
match the literal stack in the creating `xTaskCreate*` call against the
`stack_margin_register` literal, and fail on mismatch. Allow an explicit
exemption list for names created through a macro or in another file.

## LOW-5: stack checker reads `#if 0` code as live

`read_configured_stack()` in `check_system_uart_bridge_stack_budget.py` strips
comments but not preprocessor-dead code. S1 adds an `#if 0` decoy creation call
with 4096 and changes the real creation to `2048U`, which the
`(\d+),` pattern does not match. Result: MISSED. The check exits 0 and measures the ELF as if the stack were 4096 B.

Fix: strip `#if 0` regions as `check_reset_fence_hooks.ps1` does, or refuse any
conditional around the creation. Accept `\d+[uUlL]*` as the literal, so a suffix
cannot hide the real call.

## INFO

- INFO-1: `git ls-files` does not recurse into submodules, so
  `tools/mykicadMcp` is no longer enumerated by the gate lint. Today it holds
  only `start_mcp_http_server.ps1`, which does not use the gate. Note it in the
  script header, or add `--recurse-submodules`.
- INFO-2: false-positive risk. `check_reset_fence_hooks.ps1` requires exactly one
  definition of each relay_authority predicate. A legitimate forward prototype
  in `relay_authority.c` would count as a second definition and turn the check
  red. Match definitions only where the signature is followed by `{`.
- INFO-3: stale comments. `uart_bridge_system.c:217-219` still calls the bridge
  "one of the six candidate stacks left unresized", and `factory_reset.c:213`
  still says "system_uart_bridge, 3072 B". The stack is 4096 and has been
  measured (3216 B deepest).
- INFO-4: 2e7dc13f2 rewrote real hex ids in the DEVBREAK4 doc with spaces
  (`e33e 0f44`, `60c6 b410`, `0a08 0005 11`) to get past
  `check_doc_hash_citations.ps1`. They are no longer greppable. The citation
  check's blob syntax (`blob:<path>`...) or an allowlist entry would keep them
  intact.
- INFO-5: no false positives found on the real tree: all three checks pass
  unmutated, and the stack checker passes against a current ELF.

## Appendix: mutation JSON

### fence.json (`-Preset check -PresetArg tools/check_reset_fence_hooks.ps1`)

```json
[
 {
  "name": "F0_control_if0_wrap",
  "file": "firmware/KilnFW/App/main.c",
  "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "#if 0\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n#endif\n"
 },
 {
  "name": "F1_braceless_if0_prev_line",
  "file": "firmware/KilnFW/App/main.c",
  "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "    if (0)\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n"
 },
 {
  "name": "F2_ifdef_never_defined",
  "file": "firmware/KilnFW/App/main.c",
  "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "#ifdef KILNCTL_NEVER_DEFINED_NEGTEST\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n#endif\n"
 },
 {
  "name": "F3_if_0L",
  "file": "firmware/KilnFW/App/main.c",
  "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "#if 0L\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n#endif\n"
 },
 {
  "name": "F4_if_not1",
  "file": "firmware/KilnFW/App/main.c",
  "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "#if !1\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n#endif\n"
 },
 {
  "name": "F5_elif_after_true_if",
  "file": "firmware/KilnFW/App/main.c",
  "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "#if 1\n#elif 1\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n#endif\n"
 },
 {
  "name": "F6_string_hides_if0",
  "file": "firmware/KilnFW/App/main.c",
  "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "    static const char *const negtest_a = \"/*\"; (void)negtest_a;\n#if 0\n    static const char *const negtest_b = \"*/\";\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n#endif\n"
 },
 {
  "name": "F7_moved_to_uncalled_function",
  "edits": [
   {
    "file": "firmware/KilnFW/App/main.c",
    "find": "    pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
    "replace": ""
   },
   {
    "file": "firmware/KilnFW/App/main.c",
    "find": "void app_main(void)\n{",
    "replace": "__attribute__((unused)) static void negtest_never_called(void)\n{\n    pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n}\n\nvoid app_main(void)\n{"
   }
  ]
 }
]
```

### gate.json (`-Preset check -PresetArg tools/check_build_gate_usage.ps1`)

```json
[
 {
  "name": "G0_control_ascii_new_script",
  "diff": "C:/Users/budar/AppData/Local/Temp/claude/c--Users-budar-OneDrive-Desktop-kilnCtl/7750d99a-89c2-44c9-aacd-c71d7e18e7dc/scratchpad/dbfx5/g0.diff"
 },
 {
  "name": "G1_non_ascii_filename",
  "diff": "C:/Users/budar/AppData/Local/Temp/claude/c--Users-budar-OneDrive-Desktop-kilnCtl/7750d99a-89c2-44c9-aacd-c71d7e18e7dc/scratchpad/dbfx5/g1.diff"
 },
 {
  "name": "G2_gitignored_dir_check_script_run_by_run_all_checks",
  "diff": "C:/Users/budar/AppData/Local/Temp/claude/c--Users-budar-OneDrive-Desktop-kilnCtl/7750d99a-89c2-44c9-aacd-c71d7e18e7dc/scratchpad/dbfx5/g2.diff"
 }
]
```

g0.diff:

```diff
diff --git a/tools/negtest_gate_ascii.ps1 b/tools/negtest_gate_ascii.ps1
new file mode 100644
--- /dev/null
+++ b/tools/negtest_gate_ascii.ps1
@@ -0,0 +1,3 @@
+$g = Enter-KilnBuildGate -Label "negtest" -Lane light
+Start-Sleep -Seconds 5
+Exit-KilnBuildGate $g
```

g1.diff:

```diff
diff --git a/tools/negtest_gate_café.ps1 b/tools/negtest_gate_café.ps1
new file mode 100644
--- /dev/null
+++ b/tools/negtest_gate_café.ps1
@@ -0,0 +1,3 @@
+$g = Enter-KilnBuildGate -Label "negtest" -Lane light
+Start-Sleep -Seconds 5
+Exit-KilnBuildGate $g
```

g2.diff (applied by hand; negtest refuses a gitignored target):

```diff
diff --git a/build_agent/check_negtest_gate.ps1 b/build_agent/check_negtest_gate.ps1
new file mode 100644
--- /dev/null
+++ b/build_agent/check_negtest_gate.ps1
@@ -0,0 +1,3 @@
+$g = Enter-KilnBuildGate -Label "negtest" -Lane light
+Start-Sleep -Seconds 5
+Exit-KilnBuildGate $g
```

### stack.json (`-Command "python firmware\KilnFW\App\test\check_system_uart_bridge_stack_budget.py --elf <ELF copy>"`)

```json
[
 {
  "name": "S0_control_creation_3072",
  "file": "firmware/KilnFW/App/drivers/bridge/uart_bridge_system.c",
  "find": "    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 4096, &ctx, 5,\n",
  "replace": "    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 3072, &ctx, 5,\n"
 },
 {
  "name": "S1_creation_macro_vs_if0_decoy",
  "file": "firmware/KilnFW/App/drivers/bridge/uart_bridge_system.c",
  "find": "    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 4096, &ctx, 5,\n",
  "replace": "#if 0\n    xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 4096, &ctx, 5, NULL, 0);\n#endif\n    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 2048U, &ctx, 5,\n"
 }
]
```

### host.json (`-Command "powershell -NoProfile -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir {OUT} -Only link_watchdog"`)

```json
[
 {
  "name": "H0_control_stub_setter_noop",
  "file": "firmware/KilnFW/App/test/stubs/freertos/task.h",
  "find": "{ g_test_stub_current_task = t; }",
  "replace": "{ (void)t; }"
 },
 {
  "name": "H1_job_enter_records_constant",
  "file": "firmware/KilnFW/App/drivers/owners/relay_authority.c",
  "find": "    s_reset_job_task = (void *)xTaskGetCurrentTaskHandle();\n",
  "replace": "    s_reset_job_task = (void *)1;\n"
 }
]
```

## Fix status

| Finding | Status | Evidence |
|---|---|---|
| MED-1 fence check bypasses | fixed | literal-aware comment strip, `[uUlL]*` suffixes, structural anchor (app_main body, brace depth 1, outside every conditional, previous line ends `;{}`); negtest F0-F7 all CAUGHT |
| MED-2 overstated docs | fixed | DEVBREAK4 doc and GATE_NEGATIVE_TEST_EVIDENCE row corrected: 9/9 for the check, R4/R5/R9/R10 caught by test_link_watchdog, F1-F7 recorded as MISSED then fixed |
| LOW-1 host test task 3 | fixed | test_link_watchdog enters the job as task 3; H0, H1 CAUGHT |
| LOW-2 gate usage -z/UTF-8 | fixed | `git ls-files -z`, UTF-8, unresolved path fails loud; G0, G1 CAUGHT; G2 applied by hand (gitignored build_agent script with Enter and no Exit): check exit 1, then removed, exit 0 |
| LOW-3 lint discovery union | fixed | shared `tools/lib_check_discovery.ps1` used by run_all_checks and the lint |
| LOW-4 zone_sweep stack | fixed | registered at 5120; tree-wide create-vs-register literal comparison (23 pairs); S0, S1 and extra M0 (zone_sweep 4096) CAUGHT |
| LOW-5 stack checker dead code | fixed | `strip_dead_c` plus `[uUlL]*` suffixes in check_system_uart_bridge_stack_budget.py |
| INFO stale 3072 comments | fixed | uart_bridge_system.c, factory_reset.c |
| INFO-4 hex ids in DEVBREAK4 doc | left | check_doc_hash_citations risk; not restored |

Runs: full KilnFW host suite 98/98 passed; check_00_kilnfw_target_build PASS; all stack checks, reset-fence, build-gate-usage, doc-hash-citations pass against the built ELF.