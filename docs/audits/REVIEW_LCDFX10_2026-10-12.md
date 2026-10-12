# REVIEW_LCDFX10 -- adversarial review of the lcdfx10 fixes for REVIEW_LCDFX9 (2026-10-12)

Reviewer: opus, adversarial review. No code edits.

## Scope

origin/dev `d9aaa31a8` ("lcdfx10"). This series fixes REVIEW_LCDFX9 HIGH-1/2/3,
MED-1/2/3 and LOW-1/2 (`docs/audits/REVIEW_LCDFX9_2026-10-12.md`). Files under
review:

- `tools/check_lcd_admin_gates.ps1`: the L-declarator, L-cr, L-shadow,
  L-weak and L-secret rules, and the wider deny list.
- `tools/check_lcd_sink_callgraph.py`, its `.ps1` wrapper, and
  `tools/test_check_lcd_sink_callgraph.ps1`.
- `firmware/KilnFW/App/drivers/CMakeLists.txt:795-802`: `-Werror=shadow` on
  `drivers/ui/*.c`.
- `tools/run_all_checks.ps1:347-357`: the wiring.

## Method

Every claim below comes from a real `tools/negtest.ps1` run or a real build.
None comes from reading a regex.

| Set | What it covers | Command | Baseline | Log directory |
|---|---|---|---|---|
| A | REVIEW_LCDFX9 set A, 32 mutations, verbatim | `-Preset check -PresetArg tools/check_lcd_admin_gates.ps1` | PASS | `%TEMP%\negtest_logs\20261012_032110_jgl1` |
| B | REVIEW_LCDFX9 set B, 29 mutations, verbatim | same | PASS (40.7 s) | `%TEMP%\negtest_logs\20261012_033412_z8pp` |
| N14 | LOW-1 gate made always-true | `-Preset kilnfw-host`, kilnfw-host ExpectPattern | PASS | `%TEMP%\negtest_logs\20261012_033515_hods` |
| C | 9 new probes against the text check (appendix A) | `-Preset check -PresetArg tools/check_lcd_admin_gates.ps1` | PASS (39.2 s) | `%TEMP%\negtest_logs\20261012_035028_0kwr` |
| D | 10 probes against the compiler and the call-graph check (appendix B) | `-Command` running `check_00_kilnfw_target_build.ps1` in the copy, then `check_lcd_sink_callgraph.py --build-dir` on that copy's checkbuild | PASS (21692 symbols) | `%TEMP%\negtest_logs\20261012_033712_bxvz` |

Set D's command script (`run_cg.ps1`, kept in appendix B) prints
`BUILD_STEP_FAILED` and exits 9 when the build fails. Each mutation's
`expect` pattern is either the call-graph `FAILED` line or, for SH1 only, the
compiler's `shadows` diagnostic. A build failure therefore never scores
CAUGHT by accident.

For every CAUGHT, the matched line was read in the per-mutation log:

- check mutations: a named `FAIL:` rule line;
- N14: the mutant built 1/1 executables, then `RUN FAILURES (1): ui_lcd_lock`;
- set D: `LCD sink call-graph check FAILED` with the offending
  `ui_page_home_build` path, after `check_00 exit 0`.

Other builds and runs:

- `check_00_kilnfw_target_build.ps1` on the worktree: PASS. The only "shadow"
  text in the log is LVGL `*_box_shadow.c` file names.
- `check_00_kilnfw_recovery_target_build.ps1`: PASS (`recovery.bin` 783824 B).
- `check_lcd_sink_callgraph.py` on the published worktree build, full and
  `KILNCTL_CHECKS_FAST` modes: PASS (6208 symbols).
- Full `run_all_checks.ps1 -Fast -NoCache`: see "Full -Fast suite" below.

## Summary

0 HIGH, 4 MED, 5 LOW, 1 INFO.

The lcdfx10 fixes hold against REVIEW_LCDFX9's own probes:

- the text check catches set A 27/32 (the five it misses -- N04, N06, N07,
  N12, N14 -- are the ones handed to other layers by design) and set B 29/29;
- N14 is caught by the `ui_lcd_lock` host test;
- the new call-graph check catches N04, N06, N07 and N12 (and WR1), each
  from a successful build;
- `-Werror=shadow` is live on the application target build (SH1 fails to
  compile) and both target builds pass.

What remains:

- MED-1: the gate evidence table has no rows for the two new checks, so the
  full `-Fast` suite is red at `d9aaa31a8` and on the dev tip.
- MED-2: the `clear_action` exemption is by bare name; a global
  `clear_action` called from Home passes the call graph (CG1 MISSED).
- MED-3: renaming the sinks makes the call graph a silent no-op (RN1 MISSED).
- MED-4: L-secret misses a pre-gate use of `ap_password` and a deleted
  `return;` in the masked block (P1, P2 MISSED).
- LOW-1..5: section-name attribution (IRAM, plain `.rodata`/`.data`),
  pragma-disabled `-Wshadow`, no freshness check or caching, test gaps,
  L-weak scope.
- INFO-1: host tests and the recovery image do not compile ui code with
  `-Wshadow`; acceptable.

## Findings

### MED-1 -- the standing suite is red: the new checks have no row in the gate evidence table

- **Where:** `tools/check_gate_negative_test_table.ps1` against
  `docs/audits/GATE_NEGATIVE_TEST_EVIDENCE.md`. The new check and its test
  came in with `107412719`.
- **Scenario:** the full `-Fast` run at `d9aaa31a8` fails with:

  ```
  FAIL: GATE_NEGATIVE_TEST_EVIDENCE.md out of sync with discovered checks:
    no row for discovered check: tools/check_lcd_sink_callgraph.ps1
    no row for discovered check: tools/test_check_lcd_sink_callgraph.ps1
  ```

  The suite classifies this as NEW. It is still missing on the origin/dev tip
  `8e37d48b6`, so every dev promotion run fails on it.
- **Fix:** add two rows. The set D results below (N04, N06, N07, N12 and WR1
  CAUGHT) are real negative-test evidence for the call-graph check.

### MED-2 -- the call-graph exemption for `clear_action` is by bare name, so a global `clear_action` is a hidden path

- **Where:** `tools/check_lcd_sink_callgraph.py:86-91`.
  - At the gate node the walk checks only `rev.get(node)`, the
    `ui_page_safety.c.obj::clear_action` key, and then `continue`s.
  - Global references are keyed by the bare name `clear_action`, which
    `rev.get(fn)` at line 95 would follow, but line 91 skips it.
  - `GATE_CALLER` is matched by function name only, so any object's
    `btn_clicked_cb` qualifies.
- **Scenario:** probe CG1, set D `m06`.
  - The mutation drops `static` from `clear_action`.
  - It adds `void dashboard_lcd_y(void) { clear_action(NULL); }` in
    `drivers/http/dashboard_exec_http.c` and calls it from Home's trip-strip
    opener.
  - The mutant built (`check_00 exit 0`). The call graph then reported
    `LCD sink call-graph check passed (21693 referenced symbols)`: **MISSED**.
  - An ungated Home tap now runs the ADMIN-only clear action.
  - The text check caught CG1 only through two unrelated rules: its
    "0 static definitions of clear_action" row and C4's pinned opener shape.
    The call graph, which is the backstop meant for wrapper paths, did not
    see it.
- **Fix:** at the gate node, also walk the referrers in `rev.get(fn)`. Refuse
  any of them except `ui_page_safety.c.obj::btn_clicked_cb`. Compare the
  caller's object as well as its name, and fail if `clear_action` is
  referenced by bare name at all.

### MED-3 -- the call-graph check passes vacuously when the sinks are renamed or absent

- **Where:** `tools/check_lcd_sink_callgraph.py:23` and `:70-79`.
  - `SINKS` is a fixed name set.
  - `analyse()` never asserts that each sink is defined and referenced, or
    that the gate edge `btn_clicked_cb -> clear_action` exists.
- **Scenario:** probe RN1, set D `m08`.
  - Both sinks are renamed with an `_x` suffix tree-wide, plus an ungated
    direct call from Home.
  - The mutant built (`check_00 exit 0`), and the call graph said
    `passed (21692 referenced symbols)`: **MISSED**.
  - A rename refactor of `dashboard_safety_clear_trip` therefore turns the
    whole check into a no-op with no signal.
  - The text check caught RN1 (D2 recheck, staleness, L-wrapper, C4), but the
    call graph is meant to stand on its own.
- **Fix:** fail unless each sink appears as a defined symbol and has at least
  one referrer, and unless the edge from
  `ui_page_safety.c.obj::btn_clicked_cb` to
  `ui_page_safety.c.obj::clear_action` exists. Add a test case that feeds a
  dump with the sink renamed and expects exit 1.

### MED-4 -- L-secret checks only the masked block; it misses a fall-through and an early use of the secret

- **Where:**
  - `tools/check_lcd_admin_gates.ps1:521-527` (L-secret) and `:47` (the L6
    row regex);
  - `firmware/KilnFW/App/drivers/ui/ui_page_network.c:541-557`.
- **Scenario** (set C, text check, baseline PASS). Both probes leave the
  check green, and a USER-level LCD session (or the locked screen with AP mode
  active) shows the SoftAP password in clear:
  - P1: line 546 `lv_label_set_text(s_ap_ssid_label, ap_ssid);` becomes
    `...ap_password);`. The password is written before the gate. **MISSED.**
  - P2: the `return;` is deleted from the `!show_secret` block. The masked
    branch falls through into the reveal at 554-557 (QR unhidden, password
    label overwritten). **MISSED.**
  - P3 (control, `|| 1` on the role check) was CAUGHT by the L6 row, so the
    harness itself works.
- **Fix:**
  - require the masked block to end with `return;`;
  - forbid every use of `ap_password` other than `ap_password[0]` before
    `bool show_secret`;
  - better: move `wifi_prov_get_ap_password()` below the gate, into the
    ADMIN path, and pin that order. Then nothing before the gate can hold the
    secret.

### LOW-1 -- relocations in sections the SEC regex does not parse are mis-attributed (IRAM, plain .rodata/.data)

- **Where:** `tools/check_lcd_sink_callgraph.py:32`, the regex
  `^RELOCATION RECORDS FOR \[(\.[a-z_]+)\.(.+)\]:`.
  - It needs `.<kind>.<name>`, with kind limited to `[a-z_]`.
  - `.iram1.N` parses as kind `.iram1` with a name of `N`. That turns into a
    node `obj::N`, not the function, so the path is broken.
  - A plain `.rodata` or `.data` with no suffix does not match the SEC regex.
    `cur` keeps the previous section, so the relocations land on the wrong
    node.
- **Scenario:** an `IRAM_ATTR` non-UI wrapper (probe IR1, set D `m07`):
  the mutant built (`check_00 exit 0`) and the call graph reported
  `passed (21693 referenced symbols)`: **MISSED**. The text check caught IR1
  only through C4's pinned opener shape. Today no `IRAM_ATTR` exists in `drivers/http`,
  `drivers/safety` or `drivers/ui`, so this is latent.
- **Fix:** match any section name. Attribute a code section to the function
  symbol found at its offset in the symbol table (`objdump -t`), not to the
  section name. Fail on any relocation section the parser cannot attribute.

### LOW-2 -- `#pragma GCC diagnostic` can switch -Werror=shadow off, and the text check strips pragmas before scanning

- **Where:**
  - `tools/check_lcd_admin_gates.ps1:113` strips `#pragma`/`#warning`/
    `#error`/`#line` lines.
  - L-shadow (`:503-509`) bans `-Wno-shadow` only in CMake.
- **Scenario:**
  - SH1, set D `m09`, is a header macro that declares a local
    `LCD_PIN_ROLE_ADMIN` in `relay_toggle_cb`. The compiler caught it as a
    real build error:
    `dashboard_http.h:585:50: error: declaration of 'LCD_PIN_ROLE_ADMIN' shadows a previous local [-Werror=shadow]`.
    So the backstop works when it is active.
  - SH2, set D `m10`, is SH1 plus `#pragma GCC diagnostic ignored "-Wshadow"`
    in `ui_page_temperature.c`: the mutant built (`check_00 exit 0`), so the
    pragma switched `-Werror=shadow` off, and the call graph passed
    (21692 symbols): **MISSED** by both build-side layers.
  - In set C the text check caught both only incidentally, through L3's
    "calls 'DASH_ROW_INIT(' before the gate". A shadowing declaration that
    needs no pre-gate call would not hit that rule.
- **Fix:** before stripping, fail on any `#pragma GCC diagnostic` (or
  `_Pragma`) that names `shadow` in `drivers/ui/*` or in the headers they
  include. Also fail if a later `set_source_files_properties` on a ui file
  replaces `COMPILE_OPTIONS`.

### LOW-3 -- the call-graph check has no freshness check, runs objdump serially and is not cached

- **Where:** `tools/check_lcd_sink_callgraph.py:131-144`.
- **Scenario 1: stale objects.**
  - The check grades whatever `*.c.obj` files sit under `firmware/KilnFW/build`.
  - In a tree where an older `check_00` published objects and the source has
    since changed (any `-Fast` run, which skips phase 1), it grades the old
    objects and prints PASS.
- **Scenario 2: runtime.**
  - Each run does about 2000 serial `objdump -r` calls.
  - That took several minutes per run in set D, under load. It is phase 2
    time on every full run.
- **Fix:**
  - compare `obj_manifest.txt`, or the newest obj mtime, with the newest
    `drivers/**/*.c`/`*.h` mtime, and print SKIP-FAST (or FAIL) when the
    objects are older;
  - run objdump in a thread pool;
  - add a `# checkcache: ok` marker to the wrapper if its inputs can be
    fingerprinted.

### LOW-4 -- test gaps in `tools/test_check_lcd_sink_callgraph.ps1`

- **Where:** `tools/test_check_lcd_sink_callgraph.ps1`.
  - Cases A to F have no case for a renamed or absent sink (MED-3), a
    bare-name `clear_action` (MED-2), an IRAM section (LOW-1), or a
    `btn_clicked_cb` in another object.
  - Case F (`:57`) calls `Run @()` with no `--build-dir`. That scans the real
    default build, slow and with an ignored result, before the no-build SKIP
    case it means to test.
- **Fix:** add those cases. Point case F at an empty temp directory.

### LOW-5 -- L-weak covers only `ui_lcd_lock.c`/`.h`

- **Where:** `tools/check_lcd_admin_gates.ps1:514`.
- **Scenario:** a `weak` definition in another `drivers/ui` file is not
  flagged. Examples are a weak helper on the gate path or a weak page
  callback, either of which a strong definition elsewhere replaces at link
  time.
- **Fix:** flag `weak` anywhere in `drivers/ui`, and in any definition of a
  gate or sink symbol.

### INFO-1 -- host tests and the recovery image do not compile the gate code with -Wshadow

- The host-test build is MSVC with no shadow flag.
- The recovery image compiles no `drivers/ui` source.
- The application target build is the only configuration that runs
  gate-bearing ui code, and it carries the flag.
- Acceptable. No action.

## Verify items

1. **Sets A and B re-run.**
   - Set A: 27 of 32 CAUGHT by the text check. The 5 MISSED are exactly the
     ones the fix assigns elsewhere: N04, N06, N07 and N12 to the call
     graph, N14 to the host test.
   - Set B: 29 of 29 CAUGHT.
   - N14 is CAUGHT by the `ui_lcd_lock` host test as a real assertion failure
     (the mutant compiled).
   - N04, N06, N07 and N12: see set D.
2. **Call-graph check wiring.**
   - Auto-discovered by `run_all_checks.ps1` (`-ListOnly` shows
     `tools\check_lcd_sink_callgraph.ps1` and
     `tools\test_check_lcd_sink_callgraph.ps1`).
   - In a full run, phase 1's `check_00_kilnfw_target_build.ps1` publishes
     the `esp-idf\{App,drivers,kilnlink,hwabstraction_esp}` object trees into
     `<tree>\firmware\KilnFW\build` before phase 2 runs the check.
   - It SKIPs (exit 3) only when no `ui_page_safety.c.obj` exists, and prints
     SKIP-FAST only under `KILNCTL_CHECKS_FAST`.
   - It can pass vacuously when the sinks are renamed (MED-3), and it grades
     stale objects (LOW-3).
   - Its blind spots are a global `clear_action` (MED-2) and unparsed
     sections (LOW-1).
   - It covers both trip-clear sinks. Every direct non-UI caller on the real
     tree is in `NON_UI_ALLOW`, and the probe found no other caller.
   - The ADMIN exemption is too wide (MED-2).
3. **-Werror=shadow.**
   - It is applied to every `drivers/ui/*.c` in the one build that compiles
     them (the application). `drivers/ui` has no subdirectories.
   - The recovery image compiles no `drivers/ui` source.
   - Host tests compile only `ui/lcd_auth_state.c` and
     `ui/ui_page_profile_picker_format.c`, with MSVC and no shadow flag
     (INFO-1).
   - Both target build checks pass.
4. **False positives.** None:
   - the baselines of sets A, B, C, D and N14 all pass;
   - the call-graph check passes on both the published subset (6208
     symbols) and the full checkbuild (21692 symbols).
5. **Full -Fast suite.** See below.

## Full -Fast suite

Command: `run_all_checks.ps1 -Fast -NoCache` at `d9aaa31a8`.

Result: 189 passed, 4 failed, 4 SKIP-FAST. `check_lcd_admin_gates.ps1`,
`check_lcd_sink_callgraph.ps1` (which ran on the published worktree build,
not a skip) and `test_check_lcd_sink_callgraph.ps1` all PASS.

The 4 failures:

| Check | Cause | Attributable to lcdfx10? |
|---|---|---|
| `check_js_host_tests.ps1` | `test_zones_type_toggle.js` (kgR6/kgRawAttrs), the known failure | No. Fixed on dev by `2b20295ee`/`8e37d48b6` |
| `check_page_js_tests.ps1` | same `test_zones_type_toggle.js` | No |
| `check_gate_negative_test_table.ps1` | no evidence row for the two new call-graph checks | **Yes (MED-1)** |
| `check_main_baseline.ps1` | 9 assertions fail ("clean HEAD == origin/main run records a baseline", the `-FailOnlyOnNew` cases), plus a `Remove-Item` on a missing temp `baselines` dir at `:349` | No. lcdfx10 does not touch it |

The main baseline used for comparison (`129586d4f`) is not at the merge-base,
so the run lists all 4 as NEW. Only the table failure is new with lcdfx10.

## Appendix A -- set C mutation JSON (9 mutations, text check)

```json
[
 {
  "name": "P1 AP password into SSID label before the admin check",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_network.c",
    "find": "lv_label_set_text(s_ap_ssid_label, ap_ssid);",
    "replace": "lv_label_set_text(s_ap_ssid_label, ap_password);"
   }
  ]
 },
 {
  "name": "P2 masked branch falls through to reveal (return removed)",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_network.c",
    "find": "            s_ap_qr_last[0] = '\\0';\n            return;\n        }",
    "replace": "            s_ap_qr_last[0] = '\\0';\n        }"
   }
  ]
 },
 {
  "name": "P3 (control) show_secret forced true",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_network.c",
    "find": "bool show_secret = ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN);",
    "replace": "bool show_secret = ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN) || 1;"
   }
  ]
 },
 {
  "name": "SH1 header macro shadows LCD_PIN_ROLE_ADMIN in relay_toggle_cb",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\n#define DASH_ROW_INIT(x) x; const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "static void relay_toggle_cb(lv_event_t *e)\n{\n",
    "replace": "static void relay_toggle_cb(lv_event_t *e)\n{\n    int lcd_n = DASH_ROW_INIT(0);\n    (void)lcd_n;\n"
   }
  ]
 },
 {
  "name": "SH2 SH1 plus #pragma GCC diagnostic ignored -Wshadow",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\n#define DASH_ROW_INIT(x) x; const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "static void relay_toggle_cb(lv_event_t *e)\n{\n",
    "replace": "static void relay_toggle_cb(lv_event_t *e)\n{\n    int lcd_n = DASH_ROW_INIT(0);\n    (void)lcd_n;\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "#include \"zones_config_accessors.h\"",
    "replace": "#include \"zones_config_accessors.h\"\n#pragma GCC diagnostic ignored \"-Wshadow\""
   }
  ]
 },
 {
  "name": "CG1 global clear_action called from a non-UI wrapper reached from Home",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
    "find": "static void clear_action(void *user)\n{",
    "replace": "void clear_action(void *user);\nvoid clear_action(void *user)\n{"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "esp_err_t dashboard_safety_clear_trip(void)\n{",
    "replace": "extern void clear_action(void *user);\nvoid dashboard_lcd_y(void) { clear_action(NULL); }\n\nesp_err_t dashboard_safety_clear_trip(void)\n{"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\nvoid dashboard_lcd_y(void);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "(void)user_data;\n    ui_page_safety_open(false);",
    "replace": "(void)user_data;\n    dashboard_lcd_y();\n    ui_page_safety_open(false);"
   }
  ]
 },
 {
  "name": "IR1 IRAM_ATTR non-UI wrapper (macro alias) reached from Home",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "    return safety_link_send_clear_trip(s_dash.safety);\n}\n",
    "replace": "    return safety_link_send_clear_trip(s_dash.safety);\n}\n\n#include \"esp_attr.h\"\n#define DASH_KICK dashboard_safety_clear_trip\nIRAM_ATTR void dashboard_lcd_kick(void) { (void)DASH_KICK(); }\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\nvoid dashboard_lcd_kick(void);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "(void)user_data;\n    ui_page_safety_open(false);",
    "replace": "(void)user_data;\n    dashboard_lcd_kick();\n    ui_page_safety_open(false);"
   }
  ]
 },
 {
  "name": "WR1 (control) plain non-UI wrapper (macro alias) reached from Home",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "    return safety_link_send_clear_trip(s_dash.safety);\n}\n",
    "replace": "    return safety_link_send_clear_trip(s_dash.safety);\n}\n\n#define DASH_KICK dashboard_safety_clear_trip\nvoid dashboard_lcd_kick(void) { (void)DASH_KICK(); }\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\nvoid dashboard_lcd_kick(void);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "(void)user_data;\n    ui_page_safety_open(false);",
    "replace": "(void)user_data;\n    dashboard_lcd_kick();\n    ui_page_safety_open(false);"
   }
  ]
 },
 {
  "name": "RN1 both sinks renamed, ungated direct call from Home",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/safety/safety_link.h",
    "find": "esp_err_t safety_link_send_clear_trip(SafetyLinkClass *link);",
    "replace": "esp_err_t safety_link_send_clear_trip_x(SafetyLinkClass *link);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/safety/safety_link_commands.c",
    "find": "esp_err_t safety_link_send_clear_trip(SafetyLinkClass *link)\n{",
    "replace": "esp_err_t safety_link_send_clear_trip_x(SafetyLinkClass *link)\n{"
   },
   {
    "file": "firmware/KilnFW/App/drivers/safety/safety_link_frames.c",
    "find": "if (safety_link_send_clear_trip(link) == ESP_OK",
    "replace": "if (safety_link_send_clear_trip_x(link) == ESP_OK"
   },
   {
    "file": "firmware/KilnFW/App/drivers/bridge/uart_bridge_safety.c",
    "find": "err = safety_link_send_clear_trip(ctx->link);",
    "replace": "err = safety_link_send_clear_trip_x(ctx->link);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "    return safety_link_send_clear_trip(s_dash.safety);",
    "replace": "    return safety_link_send_clear_trip_x(s_dash.safety);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "esp_err_t dashboard_safety_clear_trip(void)\n{",
    "replace": "esp_err_t dashboard_safety_clear_trip_x(void)\n{"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "esp_err_t err = dashboard_safety_clear_trip();",
    "replace": "esp_err_t err = dashboard_safety_clear_trip_x();"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip_x(void);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
    "find": "esp_err_t err = dashboard_safety_clear_trip();",
    "replace": "esp_err_t err = dashboard_safety_clear_trip_x();"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "(void)user_data;\n    ui_page_safety_open(false);",
    "replace": "(void)user_data;\n    (void)dashboard_safety_clear_trip_x();\n    ui_page_safety_open(false);"
   }
  ]
 }
]
```

## Appendix B -- set D mutation JSON (10 mutations, build + call graph) and its command script

`run_cg.ps1` (negtest `-Command "powershell -ExecutionPolicy Bypass -File run_cg.ps1 -Root {ROOT}"`, `-Parallel 3 -TimeoutMin 90`):

```powershell
param([string]$Root)
Set-Location $Root
& powershell -ExecutionPolicy Bypass -File (Join-Path $Root 'firmware\KilnFW\App\test\check_00_kilnfw_target_build.ps1')
$bx = $LASTEXITCODE
$full = ([System.IO.Path]::GetFullPath($Root)).TrimEnd('\')
$sha = [System.Security.Cryptography.SHA256]::Create()
$h = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($full.ToLowerInvariant()))
$tag = (($h | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 10)
$bd = "C:\wt\checkbuild_$tag\firmware\KilnFW\build"
Write-Output "CG_BUILD_DIR $bd (check_00 exit $bx)"
if ($bx -ne 0) { Write-Output "BUILD_STEP_FAILED exit $bx"; exit 9 }
& python (Join-Path $Root 'tools\check_lcd_sink_callgraph.py') --build-dir $bd
exit $LASTEXITCODE
```

```json
[
 {
  "name": "N04_two_level_wrapper_outside_ui",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "esp_err_t safety_clear_trip_post_handler(httpd_req_t *req)\n",
    "replace": "static esp_err_t lcd_clear_inner(void)\n{\n    return dashboard_safety_clear_trip();\n}\n\nesp_err_t dashboard_lcd_home_clear(void)\n{\n    return lcd_clear_inner();\n}\n\nesp_err_t safety_clear_trip_post_handler(httpd_req_t *req)\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);\n",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\nesp_err_t dashboard_lcd_home_clear(void);\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)dashboard_lcd_home_clear();\n"
   }
  ],
  "expect": "LCD sink call-graph check FAILED"
 },
 {
  "name": "N06_macro_alias_in_non_ui_header",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);\n",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\n#define DASH_LCD_CLR dashboard_safety_clear_trip\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)DASH_LCD_CLR();\n"
   }
  ],
  "expect": "LCD sink call-graph check FAILED"
 },
 {
  "name": "N07_fnptr_struct_outside_ui",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "esp_err_t safety_clear_trip_post_handler(httpd_req_t *req)\n",
    "replace": "const dash_lcd_ops_t g_dash_lcd_ops = { .clear = dashboard_safety_clear_trip };\n\nesp_err_t safety_clear_trip_post_handler(httpd_req_t *req)\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);\n",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\ntypedef struct { esp_err_t (*clear)(void); } dash_lcd_ops_t;\nextern const dash_lcd_ops_t g_dash_lcd_ops;\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)g_dash_lcd_ops.clear();\n"
   }
  ],
  "expect": "LCD sink call-graph check FAILED"
 },
 {
  "name": "N12_token_paste_in_non_ui_header",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);\n",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\n#define DASH_CAT(a, b) a##b\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)DASH_CAT(dashboard_safety_, clear_trip)();\n"
   }
  ],
  "expect": "LCD sink call-graph check FAILED"
 },
 {
  "name": "WR1 (control) plain non-UI wrapper (macro alias) reached from Home",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "    return safety_link_send_clear_trip(s_dash.safety);\n}\n",
    "replace": "    return safety_link_send_clear_trip(s_dash.safety);\n}\n\n#define DASH_KICK dashboard_safety_clear_trip\nvoid dashboard_lcd_kick(void) { (void)DASH_KICK(); }\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\nvoid dashboard_lcd_kick(void);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "(void)user_data;\n    ui_page_safety_open(false);",
    "replace": "(void)user_data;\n    dashboard_lcd_kick();\n    ui_page_safety_open(false);"
   }
  ],
  "expect": "LCD sink call-graph check FAILED"
 },
 {
  "name": "CG1 global clear_action called from a non-UI wrapper reached from Home",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
    "find": "static void clear_action(void *user)\n{",
    "replace": "void clear_action(void *user);\nvoid clear_action(void *user)\n{"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "esp_err_t dashboard_safety_clear_trip(void)\n{",
    "replace": "extern void clear_action(void *user);\nvoid dashboard_lcd_y(void) { clear_action(NULL); }\n\nesp_err_t dashboard_safety_clear_trip(void)\n{"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\nvoid dashboard_lcd_y(void);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "(void)user_data;\n    ui_page_safety_open(false);",
    "replace": "(void)user_data;\n    dashboard_lcd_y();\n    ui_page_safety_open(false);"
   }
  ],
  "expect": "LCD sink call-graph check FAILED"
 },
 {
  "name": "IR1 IRAM_ATTR non-UI wrapper (macro alias) reached from Home",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "    return safety_link_send_clear_trip(s_dash.safety);\n}\n",
    "replace": "    return safety_link_send_clear_trip(s_dash.safety);\n}\n\n#include \"esp_attr.h\"\n#define DASH_KICK dashboard_safety_clear_trip\nIRAM_ATTR void dashboard_lcd_kick(void) { (void)DASH_KICK(); }\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\nvoid dashboard_lcd_kick(void);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "(void)user_data;\n    ui_page_safety_open(false);",
    "replace": "(void)user_data;\n    dashboard_lcd_kick();\n    ui_page_safety_open(false);"
   }
  ],
  "expect": "LCD sink call-graph check FAILED"
 },
 {
  "name": "RN1 both sinks renamed, ungated direct call from Home",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/safety/safety_link.h",
    "find": "esp_err_t safety_link_send_clear_trip(SafetyLinkClass *link);",
    "replace": "esp_err_t safety_link_send_clear_trip_x(SafetyLinkClass *link);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/safety/safety_link_commands.c",
    "find": "esp_err_t safety_link_send_clear_trip(SafetyLinkClass *link)\n{",
    "replace": "esp_err_t safety_link_send_clear_trip_x(SafetyLinkClass *link)\n{"
   },
   {
    "file": "firmware/KilnFW/App/drivers/safety/safety_link_frames.c",
    "find": "if (safety_link_send_clear_trip(link) == ESP_OK",
    "replace": "if (safety_link_send_clear_trip_x(link) == ESP_OK"
   },
   {
    "file": "firmware/KilnFW/App/drivers/bridge/uart_bridge_safety.c",
    "find": "err = safety_link_send_clear_trip(ctx->link);",
    "replace": "err = safety_link_send_clear_trip_x(ctx->link);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "    return safety_link_send_clear_trip(s_dash.safety);",
    "replace": "    return safety_link_send_clear_trip_x(s_dash.safety);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "esp_err_t dashboard_safety_clear_trip(void)\n{",
    "replace": "esp_err_t dashboard_safety_clear_trip_x(void)\n{"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "esp_err_t err = dashboard_safety_clear_trip();",
    "replace": "esp_err_t err = dashboard_safety_clear_trip_x();"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip_x(void);"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
    "find": "esp_err_t err = dashboard_safety_clear_trip();",
    "replace": "esp_err_t err = dashboard_safety_clear_trip_x();"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "(void)user_data;\n    ui_page_safety_open(false);",
    "replace": "(void)user_data;\n    (void)dashboard_safety_clear_trip_x();\n    ui_page_safety_open(false);"
   }
  ],
  "expect": "LCD sink call-graph check FAILED"
 },
 {
  "name": "SH1 header macro shadows LCD_PIN_ROLE_ADMIN in relay_toggle_cb",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\n#define DASH_ROW_INIT(x) x; const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "static void relay_toggle_cb(lv_event_t *e)\n{\n",
    "replace": "static void relay_toggle_cb(lv_event_t *e)\n{\n    int lcd_n = DASH_ROW_INIT(0);\n    (void)lcd_n;\n"
   }
  ],
  "expect": "shadows"
 },
 {
  "name": "SH2 SH1 plus #pragma GCC diagnostic ignored -Wshadow",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\n#define DASH_ROW_INIT(x) x; const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "static void relay_toggle_cb(lv_event_t *e)\n{\n",
    "replace": "static void relay_toggle_cb(lv_event_t *e)\n{\n    int lcd_n = DASH_ROW_INIT(0);\n    (void)lcd_n;\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "#include \"zones_config_accessors.h\"",
    "replace": "#include \"zones_config_accessors.h\"\n#pragma GCC diagnostic ignored \"-Wshadow\""
   }
  ],
  "expect": "LCD sink call-graph check FAILED|shadows"
 }
]
```
