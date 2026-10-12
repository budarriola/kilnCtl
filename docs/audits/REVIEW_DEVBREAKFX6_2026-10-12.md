# Review: devbreakfx6, the REVIEW_DEVBREAKFX5 fixes (2026-10-12)

Range: code commit `1ead3e7bc`, doc commit tip `4e5977267` on origin/dev (parent `6c4acd56c`).
It fixes `docs/audits/REVIEW_DEVBREAKFX5_2026-10-12.md` MED-1, LOW-1..LOW-5. INFO-4 was left
unfixed on purpose and is not re-reported.

Adversarial review. Every verdict below comes from a `tools\negtest.ps1` run in a throwaway
worktree. Every run's baseline passed, and every run reported `real_tree_unchanged: true` and
`copies_removed: true`. The board was not touched.

## Verdict on promote

**Does not block a promote.** The firmware part of this range is comment text plus one host
test. No finding is a firmware behavior defect. The findings below are about how strong the
checks and the test are:

- MED-1: the reset-fence check can still be bypassed.
- LOW-1: the stack-literal cross-check skips names silently.
- LOW-2: one assertion in the host test became vacuous.
- LOW-3: the Python stack reader can still be fooled by a decoy.

These can be fixed on dev after the promote.

## Tests run

| Run | Command | Baseline | Result |
|---|---|---|---|
| fence | `-Preset check -PresetArg tools/check_reset_fence_hooks.ps1 -Mutations fence.json` | PASS | F0, F10, F13, F14 CAUGHT; F8, F9, F11, F12, F15, F16 MISSED |
| fence2 | same, `fence2.json` | PASS | F14b, F17 MISSED |
| stack | `-Preset check -PresetArg tools/check_stack_margin_registration.ps1 -Mutations stack.json` | PASS (47 create sites, 23 literal pairs compared) | M0 CAUGHT; M1, M2, M3 MISSED |
| py | `-Command "python readstack.py"` (imports `check_system_uart_bridge_stack_budget`, calls `read_configured_stack()`, exits 0 only when it returns 4096) | PASS | P0, P1, P2 CAUGHT; P3 MISSED |
| host | `-Command "powershell -NoProfile -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir {OUT} -Only link_watchdog"`, `-ExpectPattern "(?m)^\s+FAIL \|FAIL .*\.c:\d+\|RUN FAILURES\|BUILD FAILURES"` | PASS | H1 CAUGHT; H2, H3 MISSED |
| LOW-3 discovery | `run_all_checks.ps1 -ListOnly` at `6c4acd56c` and at `4e5977267` | n/a | both "199 check scripts discovered", output diff empty; `Get-KilnCheckScripts` and the old inline filter both give 182 files |
| G2 by hand | ignored `build_agent/check_negtest_gate.ps1` holding a slot across `Start-Sleep` | PASS (21 callers) | FAIL, file named (caught); an ignored `build_agent/helper_gate.ps1` with the same body passes (see INFO-2) |

## MED-1: reset-fence check still has structural bypasses

The DEVBREAK5 fix closed F0-F7. It counts brace depth and conditional depth, and requires each
setter to sit at `app_main` brace depth 1, outside every conditional, with a previous line that
ends in `;`, `{` or `}`. Seven new shapes still make the four install lines dead while the check
passes. Six of them are plain C that compiles:

| Mutation | Bypass |
|---|---|
| F8 | `if (0) {` before the installs, an extra `}` inside `#if 0 ... #endif`, and the real `}` after the installs. Braces are counted inside dead preprocessor regions, so depth reads 1 |
| F9 | `#define NEGTEST_DEAD_BEGIN if (0) {` / `#define NEGTEST_DEAD_END }`. The macro ban covers only the predicate, setter, callee, strcmp and NULL names, not an arbitrary macro that opens a block |
| F11 | `goto negtest_skip;` before the installs, with the label after them |
| F12 | `if (MAIN_TAG != NULL) return;` before the installs |
| F14b | `%:if 0` (digraph `#if`), a `(void)0;` filler line, the setter, then `%:endif`. F14 without the filler is caught only by the previous-line rule |
| F15 | `#define app_main negtest_never_called`; the installs sit in that decoy, followed by `#undef`, and the real `app_main` is empty |
| F17 | `if (0) <%` + `(void)0;` + the four installs + `%>`. Pure C with digraph braces, no preprocessor |

F16 (`#define hal_kv_set_write_refuse_hook(fn) ((void)(fn))` in `hal_kv.h`) is also MISSED, but
the check only reads `main.c` and `relay_authority.c`. That is a scope limit, not a regression.

None of these is a likely accident. But the FX5 fix table and the GATE_NEGATIVE_TEST_EVIDENCE row
now read as if the dead-install class is closed, and it is not. Each round of regex hardening has
moved the bypass rather than removed it (DEVBREAK4, then FX5 F1-F7, now F8-F17). Also,
`test_link_watchdog` tests `relay_authority.c` only, so no host test covers a dead install in
`main.c`.

Recommendation: stop extending the regex. Two parts:

1. Install check. Pick one:
   - A runtime self-check: the firmware reads back each installed hook pointer once, after the
     installs, and refuses to start writers or reports a readiness/crash item if any is NULL.
   - A host test that links `main.c`'s install sequence (or a small `reset_fence_install()`
     function factored out of it) and asserts the four hooks are set.
2. Documentation. Write the check's own limits in its header and in the evidence row: it catches
   accidental deletion and the shapes listed, not adversarial dead code.

## LOW-1: stack-literal cross-check skips silently, contrary to its comment

`check_stack_margin_registration.ps1`'s new block compares only names whose create and register
calls both use a decimal literal in the same file. Its comment says non-literal sites are
"listed in the output"; the code skips them without printing anything. Every one of the
following is skipped:

- The macro-sized pairs: bx_flash_worker, dns_hijack, kiln_cfg_swap, pico_auto_update,
  safety_poll, touch_uart_bridge, update_fetch, update_fetch_wr.
- The cross-file pairs: i2c_owner_ns2009/sx1509, spi_owner, safety_owner_evt, safety_proto_rx,
  uart_owner_evt_task, uart_proto_rx, httpd_worker, http_async_job, recovery_exit.
- lvgl, which is sized with `sizeof`.

The floor is 5 compared pairs against today's 23, so 18 pairs can drop out with no output.

| Mutation | Verdict |
|---|---|
| M0 control: zone_sweep register 5120 -> 4096 | CAUGHT |
| M1 safety_poll register `SAFETY_POLL_TASK_STACK` -> `1024` (macro create, literal register) | MISSED |
| M2 zone_sweep create `5120` -> `4096 + 0` | MISSED |
| M3 zone_sweep create `5120` -> `0x1000` | MISSED |

Fix:

- Print every skipped name with its reason (macro, expression, cross-file).
- Make a one-side-literal pair (M1) fail, or allowlist it explicitly.
- Accept hex.
- Raise the floor to the current count, or make the floor a ratchet.

## LOW-2: FX5 LOW-1 fix made the post-exit assertion vacuous (regression)

`test_reset_refuses_writer_exempts_reset_job_task` now enters the job as task 3. It then
switches to task 1, calls `relay_authority_reset_job_exit()`, and asserts "after the job exits
the exemption is gone" while still on task 1. Task 1 is refused whether or not `job_exit`
cleared anything, because the job is task 3, so the assertion can no longer fail.

In the parent `6c4acd56c`, the job was task 1 and the same assertion caught a `job_exit` that did
not clear the job. H2 (`s_reset_job_task = NULL;` -> `(void)0;`) is MISSED now, so this range
lost that coverage. The kiln_nvs twin never asserted after exit.

Fix: switch to task 3 (the former job task) before the post-exit assertion in both tests.

H3 (`job_enter` records the constant `(void *)3`) is also MISSED. It is contrived because it
matches the test's own task id, so it is INFO. A second job entered from a different task (for
example task 4) would close it.

## LOW-3: Python uart-bridge stack reader still fooled by a live dead-code decoy

`strip_dead_c()` now removes `#if 0`/`#if !1` regions, and P1 (the FX5 S1 shape: an `#if 0`
decoy plus a `2048U` real create) is CAUGHT. P2 (an `#ifdef UNKNOWN` decoy) fails safe and is
CAUGHT.

P3 is MISSED: the real create uses `0x800` and the decoy is
`if (0) { (void)xTaskCreatePinnedToCore(..., "system_uart_bridge", 4096, ...); }`. The `(\d+)`
pattern does not match a hex literal, so the reader sees only the decoy and returns 4096.

Fix: require exactly one create match and exactly one register match, and treat a create site it
cannot parse (hex, macro, expression) as a failure, not as "absent".

## INFO

- INFO-1 (LOW-3 of FX5, discovery): no change. `lib_check_discovery.ps1` gives the same 182 files
  as the old inline filter, and `-ListOnly` reports 199 both before and after.
- INFO-2 (LOW-2 of FX5, `-z` ls-files): non-ASCII and NUL-split paths are now handled. In G2,
  `build_agent/` is gitignored:
  - an ignored `check_*.ps1` there is linted, through the `Get-KilnCheckScripts` union;
  - an ignored non-check helper `.ps1` there is not linted.

  That matches the suite, which runs only discovered checks, so the coverage looks intended.
  Untracked, not-ignored scripts are linted.
- INFO-3: an unstaged deletion of a tracked `.ps1` makes the gate lint FAIL ("listed path does
  not resolve"). That is a false reject during work in progress, and it fails loud, so it is
  acceptable.
- INFO-4: `-C "<Root>"` breaks when `-Root` ends in a backslash. It fails loud and no caller
  passes one.
- INFO-5: Python `strip_dead_c()` treats `#elif` after `#if 1` as live. That gives two matches,
  which fails safe.
- INFO-6: C has no raw string literals, so that bypass class does not apply. A char literal `'"'`
  followed by a comment (F13) is handled correctly.
- INFO-7: the fence check's existing shadow rule catches `#define <predicate> NULL` (F10). Of the
  digraph cases, only bare `%:if 0` (F14) is caught, and only through the previous-line rule.
- INFO-8: the `uart_bridge_system.c` and `factory_reset.c` changes are comment-only and accurate:
  4096 create and register, measured deepest path 3216 B.

## Appendix: mutation JSON

### fence.json (`-Preset check -PresetArg tools/check_reset_fence_hooks.ps1`)

```json
[
 {"name": "F0_control_braceless_if0", "file": "firmware/KilnFW/App/main.c",
  "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "    if (0)\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n"},
 {"name": "F8_if0_brace_balances_real_if_block", "file": "firmware/KilnFW/App/main.c",
  "find": "    pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "    if (0) {\n#if 0\n    }\n#endif\n    pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n    }\n"},
 {"name": "F9_macro_hides_if0_brace", "edits": [
  {"file": "firmware/KilnFW/App/main.c", "find": "void app_main(void)\n{",
   "replace": "#define NEGTEST_DEAD_BEGIN if (0) {\n#define NEGTEST_DEAD_END }\nvoid app_main(void)\n{"},
  {"file": "firmware/KilnFW/App/main.c",
   "find": "    pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
   "replace": "    NEGTEST_DEAD_BEGIN;\n    pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n    NEGTEST_DEAD_END\n"}]},
 {"name": "F10_arg_macro_to_NULL", "file": "firmware/KilnFW/App/main.c",
  "find": "void app_main(void)\n{",
  "replace": "#define relay_authority_reset_refuses_writer NULL\nvoid app_main(void)\n{"},
 {"name": "F11_goto_skips_installs", "edits": [
  {"file": "firmware/KilnFW/App/main.c", "find": "    uart_bridge_ext_save_reservation_init();\n",
   "replace": "    uart_bridge_ext_save_reservation_init();\n    goto negtest_skip;\n"},
  {"file": "firmware/KilnFW/App/main.c", "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
   "replace": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\nnegtest_skip:;\n"}]},
 {"name": "F12_early_return_before_installs", "file": "firmware/KilnFW/App/main.c",
  "find": "    uart_bridge_ext_save_reservation_init();\n",
  "replace": "    uart_bridge_ext_save_reservation_init();\n    if (MAIN_TAG != NULL) return;\n"},
 {"name": "F13_control_char_literal_quote_then_comment", "file": "firmware/KilnFW/App/main.c",
  "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "    static const char negtest_q = '\"'; (void)negtest_q; /*\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n    */\n"},
 {"name": "F14_digraph_if0", "file": "firmware/KilnFW/App/main.c",
  "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "%:if 0\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n%:endif\n"},
 {"name": "F15_app_main_renamed_decoy_holds_installs", "edits": [
  {"file": "firmware/KilnFW/App/main.c",
   "find": "    pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
   "replace": ""},
  {"file": "firmware/KilnFW/App/main.c", "find": "void app_main(void)\n{",
   "replace": "#define app_main negtest_never_called\nvoid app_main(void)\n{\n    pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n}\n#undef app_main\n\nvoid app_main(void)\n{"}]},
 {"name": "F16_header_macro_nops_setter", "file": "firmware/hwAbstraction/interface/hal_kv.h",
  "find": "void hal_kv_set_write_refuse_hook(hal_kv_write_refuse_fn_t fn);\n",
  "replace": "void hal_kv_set_write_refuse_hook(hal_kv_write_refuse_fn_t fn);\n#define hal_kv_set_write_refuse_hook(fn) ((void)(fn))\n"}
]
```

### fence2.json (same preset)

```json
[
 {"name": "F14b_digraph_if0_with_filler_statement", "file": "firmware/KilnFW/App/main.c",
  "find": "    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "%:if 0\n    (void)0;\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n%:endif\n"},
 {"name": "F17_digraph_braces_if0_block", "file": "firmware/KilnFW/App/main.c",
  "find": "    pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n",
  "replace": "    if (0) <%\n    (void)0;\n    pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer);\n    hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);\n    %>\n"}
]
```

### stack.json (`-Preset check -PresetArg tools/check_stack_margin_registration.ps1`)

```json
[
 {"name": "M0_control_zone_sweep_register_4096", "file": "firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c",
  "find": "    stack_margin_register(\"zone_sweep\", &s_sweep.task, 5120);\n",
  "replace": "    stack_margin_register(\"zone_sweep\", &s_sweep.task, 4096);\n"},
 {"name": "M1_macro_create_literal_register_mismatch", "file": "firmware/KilnFW/App/drivers/safety/safety_link.c",
  "find": "    stack_margin_register(\"safety_poll\", &link->poll_task, SAFETY_POLL_TASK_STACK);\n",
  "replace": "    stack_margin_register(\"safety_poll\", &link->poll_task, 1024);\n"},
 {"name": "M2_create_expression_hides_mismatch", "file": "firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c",
  "find": "xTaskCreate(zone_sweep_task, \"zone_sweep\", 5120, NULL",
  "replace": "xTaskCreate(zone_sweep_task, \"zone_sweep\", 4096 + 0, NULL"},
 {"name": "M3_create_hex_literal_hides_mismatch", "file": "firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c",
  "find": "xTaskCreate(zone_sweep_task, \"zone_sweep\", 5120, NULL",
  "replace": "xTaskCreate(zone_sweep_task, \"zone_sweep\", 0x1000, NULL"}
]
```

### py.json (`-Command "python readstack.py"`)

`readstack.py` runs from the negtest copy's root:

```python
import os, sys
sys.path.insert(0, os.path.join(os.getcwd(), "firmware", "KilnFW", "App", "test"))
import check_system_uart_bridge_stack_budget as c
v, e = c.read_configured_stack()
print("READ", v, e)
sys.exit(0 if v == 4096 else 1)
```

```json
[
 {"name": "P0_control_create_3072", "file": "firmware/KilnFW/App/drivers/bridge/uart_bridge_system.c",
  "find": "\"system_uart_bridge\", 4096, &ctx, 5,\n", "replace": "\"system_uart_bridge\", 3072, &ctx, 5,\n"},
 {"name": "P1_fx5_S1_if0_decoy_plus_2048U", "file": "firmware/KilnFW/App/drivers/bridge/uart_bridge_system.c",
  "find": "    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 4096, &ctx, 5,\n",
  "replace": "#if 0\n    xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 4096, &ctx, 5, NULL, 0);\n#endif\n    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 2048U, &ctx, 5,\n"},
 {"name": "P2_ifdef_unknown_decoy_fails_safe", "file": "firmware/KilnFW/App/drivers/bridge/uart_bridge_system.c",
  "find": "    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 4096, &ctx, 5,\n",
  "replace": "#ifdef KILNCTL_NEGTEST_NEVER\n    xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 4096, &ctx, 5, NULL, 0);\n#endif\n    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 2048U, &ctx, 5,\n"},
 {"name": "P3_hex_real_plus_if0_statement_decoy", "file": "firmware/KilnFW/App/drivers/bridge/uart_bridge_system.c",
  "find": "    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 4096, &ctx, 5,\n",
  "replace": "    if (0) { (void)xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 4096, &ctx, 5, NULL, 0); }\n    BaseType_t created = xTaskCreatePinnedToCore(system_bridge_task, \"system_uart_bridge\", 0x800, &ctx, 5,\n"}
]
```

### host.json (kilnfw host build, `-Only link_watchdog`)

```json
[
 {"name": "H1_job_enter_records_constant_1", "file": "firmware/KilnFW/App/drivers/owners/relay_authority.c",
  "find": "    s_reset_job_task = (void *)xTaskGetCurrentTaskHandle();\n", "replace": "    s_reset_job_task = (void *)1;\n"},
 {"name": "H2_job_exit_does_not_clear", "file": "firmware/KilnFW/App/drivers/owners/relay_authority.c",
  "find": "    s_reset_job_task = NULL;\n", "replace": "    (void)0;\n"},
 {"name": "H3_job_enter_records_constant_3", "file": "firmware/KilnFW/App/drivers/owners/relay_authority.c",
  "find": "    s_reset_job_task = (void *)xTaskGetCurrentTaskHandle();\n", "replace": "    s_reset_job_task = (void *)3;\n"}
]
```

H1 CAUGHT on six assertions, `test_link_watchdog.c` lines 230, 234, 236, 256, 260 and 262. H2 and
H3 exited 0 with no FAIL line.

## Fix status (FX7)

MED-1: fixed by test_main_reset_fence.c (real main.c, app_main run, 4 hooks asserted); F8/F9/F11/F12/F14b/F15/F17 plus F0/F10b/F13/F14/F16b all CAUGHT by a real assertion. The static check stays but documents its limits (F10/F16 as written fail to compile in the test, not counted; rerun as F10b/F16b). LOW-1: stack-size check evaluates hex, sums and unique #define macros; M1/M2/M3 CAUGHT; 25 skipped names printed with reasons and ratcheted by $stackSkipAllowed. LOW-2: H1/H2/H3 CAUGHT. LOW-3: P0-P3 CAUGHT. INFO gate-lint items not changed.
