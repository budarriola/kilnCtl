# REVIEW_LCDFX9 -- adversarial bypass review of `tools/check_lcd_admin_gates.ps1` (2026-10-12)

Reviewer: opus, adversarial bypass review, no code edits.

## Scope

`tools/check_lcd_admin_gates.ps1` as of origin/dev `5f24cf7c6` (the lcdfx9
fix for `docs/audits/REVIEW_LCDFX8_2026-10-12.md`). The check text did not
change between `5f24cf7c6` and the origin/dev tip this review was landed on.

## Goal

1. Find mutations that break an owner rule while the check stays green.
2. Confirm the check does not false-fail the real tree.
3. Confirm the owner rules are still enforced:
   - the LCD trip clear requires ADMIN (`project_owner_decisions_2026_10_07_lcd_safety_totp`);
   - the Safety page requires the USER PIN;
   - LCD reads stay USER (`project_owner_decision_lcd_read_gating`).
4. Mark the status of each REVIEW_LCDFX8 finding.

## Method

Every bypass below is a real `tools/negtest.ps1` run, never a reading of the regex:

```
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset check `
  -PresetArg tools/check_lcd_admin_gates.ps1 -Mutations <set>.json
```

| Set | Content | Baseline | Log directory |
|---|---|---|---|
| A | 32 mutations (25 new probes N01-N14, 7 controls C01-C07) | PASS (30.8 s) | `%TEMP%\negtest_logs\20261012_005640_y1ru` |
| B | REVIEW_LCDFX8 appendix A (27) + B (2), verbatim, 29 mutations | PASS | `%TEMP%\negtest_logs\20261012_010209_benq` |

negtest's own post-run guard reported the real tree unchanged after both runs.

C-language claims were checked with the board's xtensa toolchain
(`xtensa-esp32s3-elf-gcc` esp-15.2.0, `-std=gnu17 -Wall -Werror`), using
minimal reproductions, not full-tree builds:

- a lone CR ends a `//` comment;
- a local `const` shadowing an enumerator compiles cleanly;
- a local function pointer shadowing a global function compiles cleanly;
- `-Wshadow -Werror` rejects both shadows ("declaration of 'R_ADMIN' shadows
  a previous local"; "'is_cal' shadows a global declaration").

N13 (weak/strong override) was not compiled; its override uses an `int`
parameter and only shows that the check is blind to it.

## Summary

| Run | Probes | CAUGHT | MISSED |
|---|---|---|---|
| Set A probes (N01-N14) | 25 | 2 (N02c, N02d) | 23 |
| Set A controls (C01-C07) | 7 | 7 | 0 |
| Set B (LCDFX8 regression) | 29 | 29 | 0 |

No false positives: the baseline passes on the real tree in both runs.

| Severity | Count | Findings |
|---|---|---|
| HIGH | 3 | HIGH-1 role-enumerator shadow; HIGH-2 trip clear reachable through non-UI indirection; HIGH-3 lone CR |
| MED | 3 | MED-1 deny list incomplete; MED-2 AP-password secrecy not pinned; MED-3 defense-in-depth rechecks defeatable |
| LOW | 2 | LOW-1 gate implementation overridable or unpinned; LOW-2 R12 false-fail (LCDFX8 LOW-3, still open) |
| INFO | 3 | see below |

## Owner-rule cross-check

| Rule | Real tree | Check enforcement |
|---|---|---|
| LCD trip clear requires ADMIN (D2) | Holds: only the Safety page reaches `dashboard_safety_clear_trip()`, behind `ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)` and the `clear_action` recheck. | Not airtight: HIGH-1 (role shadow on other ADMIN gates), HIGH-2 (Home reaches the clear through non-UI wrappers, macros, function pointers or `##`), HIGH-3 (lone CR) all pass green. |
| Safety page requires the USER PIN | Holds. | Pinned by rows C3/C4 (unchanged since LCDFX8). |
| LCD reads stay USER | Holds. | The check polices reads only for the AP secret (L6), and MED-2 shows that is not pinned either. Ordinary reads are out of scope by design. |

## Findings

### HIGH-1 Role-enumerator shadow turns ADMIN gates into USER gates

**Probes (MISSED):**

- N01 relay toggle;
- N02a profile delete;
- N02b crash acknowledge;
- N03a touch-calibration save;
- N10b AP-password reveal (shadowed with `LCD_PIN_ROLE_NONE`).

**Mutation:** one statement at the top of the gated function:

```c
const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER;
```

**Why it works:**

- The row regexes match the token `LCD_PIN_ROLE_ADMIN` textually.
- `$pfx` admits a leading declaration with an initializer.
- C lets a block-scope object shadow an enumerator, and `-Wall -Werror` does not warn.
- `ui_lcd_lock_has_role()` then receives USER, which any unlocked USER-PIN session satisfies.

**Why N02c and N02d were caught:** the Safety-page `btn_clicked_cb` and
`clear_action` rows are start-anchored with no `$pfx`, so the extra
statement breaks the match. Every other ADMIN row allows the prefix.

**Fix:**

1. Refuse, in drivers/ui, any declarator (object, parameter, enumerator, typedef or
   function pointer) whose name is `LCD_PIN_ROLE_*`, a `$trusted` or `$openerTok`
   token, or any identifier named in a row regex.
2. Pin `-Werror=shadow` for drivers/ui sources in the KilnFW component
   CMakeLists.txt, and have the check assert that the flag is present.
   gcc rejects the shadow under that flag.

### HIGH-2 Trip clear (D2) reachable from Home through non-UI indirection

**Probes (MISSED).** Each adds a call on the Home page plus a helper outside drivers/ui:

| Probe | Indirection | Why the check misses it |
|---|---|---|
| N04 | two-level wrapper in `dashboard_exec_http.c` (`dashboard_lcd_home_clear()` calls `lcd_clear_inner()` calls the sink) | L-wrapper scan is one level deep |
| N05 | one-level wrapper written GNU style (`esp_err_t\ndashboard_lcd_quick_clear(void)`) | `Get-TopFuncRanges` regex `(?m)^[A-Za-z_][^;{}()#\n]*?\b(\w+)\s*\(` needs a return type on the same line as the name, so a name at the start of a line is never captured |
| N06 | `#define DASH_LCD_CLR dashboard_safety_clear_trip` in `dashboard_http.h` | L1-macro refuses `#define` only of trusted/opener names; non-UI macro aliases of a sink are not followed |
| N07 | `const dash_lcd_ops_t g_dash_lcd_ops = { .clear = dashboard_safety_clear_trip };` in `dashboard_exec_http.c` | global initializers outside drivers/ui are not scanned |
| N12 | `#define DASH_CAT(a, b) a##b` in `dashboard_http.h`; Home calls `DASH_CAT(dashboard_safety_, clear_trip)()` | the `##` ban covers drivers/ui only |

**Impact:** a tap on Home clears a latched safety trip with no ADMIN
login. That breaks owner decision D2 while the check reports PASS.

**Fix:**

1. Replace the one-level wrapper scan with a transitive taint closure over the whole
   App tree:
   - seed it with `$denyIdent`;
   - repeatedly add every function whose body names a tainted identifier;
   - add every global object whose initializer names one;
   - add every object-like or function-like macro whose replacement list names one.
   Any tainted name used in drivers/ui outside an allowed gated row fails.
2. Fix the range regex so a function name at the start of a line, with the
   return type on the previous line, is captured. Add a test fixture that
   uses that style.
3. Ban `##` in every App header, or allowlist the existing uses by file and
   macro name.
4. Optional backstop that does not depend on text matching: a link-level check.
   Use `xtensa-esp32s3-elf-objdump -d` on the KilnFW ELF, or GCC's
   `-fcallgraph-info` output. Assert that the only callers of
   `dashboard_safety_clear_trip` reachable from `ui_page_*` symbols pass
   through `ui_page_safety.c`'s `clear_action`.

### HIGH-3 A lone CR ends a `//` comment for GCC but not for the check

**Probes (MISSED):**

- N08a puts `// lcd note\r    (void)dashboard_safety_clear_trip();` on Home;
- N08b puts `// admin gate below\r    relay_toggle_apply(...); return;` at the top of `relay_toggle_cb`.

**Why it works:**

- The check deletes every `\r` (`-replace "\r",''`) before it strips comments, so it
  treats the whole line as one comment.
- GCC treats a lone CR as a line terminator. The `//` comment ends there, and the
  call that follows is compiled.
- Verified with xtensa gcc.

**Fix:**

- Fail on `\r(?!\n)` in any App `.c`/`.h` file before normalisation. No legitimate
  source has one.
- Or normalise a lone `\r` to `\n` rather than deleting it.
- Add a negative fixture for each.

### MED-1 Sink deny list is incomplete

**Probes (MISSED).** Each adds one call on the Home page:

- N09a `kiln_io_owner_command_set_relay(0, true, NULL)`;
- N09b `relay_cycles_reset(0)`;
- N09c `ramp_assist_cfg_set_enabled(false)`;
- N09d `factory_reset_execute(0)`;
- N09e `esp_restart()`.

**Other unlisted ADMIN-tier mutators found by inspection** (not mutated):

- `display_power_cfg_set*`;
- `autotune_engine_start`;
- `safety_cfg_write_*`;
- `danger_mode_*`;
- the timezone and relay-name setters.

None is called from drivers/ui today, so the real tree is fine. The
gap is that a new UI page could call any of them ungated and stay green.

**Fix:**

- Derive the deny list mechanically: take every function called by a
  ROUTE_TIER_ADMIN POST handler (via `route_tier_table.h`) and subtract a small
  reviewed read-only allowlist.
- Or deny a mutator-verb pattern (`_set`, `_reset`, `_clear`, `_write`, `_save`,
  `_delete`, `_start`, `_execute`, `esp_restart`) in drivers/ui unless it is
  allowlisted by file|function.
- At minimum, add the five probed names and the five families above.

### MED-2 AP-password secrecy (L6) is not pinned

**Probes (MISSED):**

- N10a changes the masked branch to
  `lv_label_set_text(s_ap_password_label, ap_password[0] ? ap_password : "(open network)");`.
  The label is still inside the ADMIN branch, but this branch is the one meant
  never to print the secret, and it is the only one a USER sees.
- N10c puts `lv_label_set_text(lv_label_create(lv_layer_top()), wifi_prov_get_ap_password());`
  on Home, which shows the AP password to anyone with no login.

**Fix:**

- Add the secret readers (`wifi_prov_get_ap_password` and any STA-password reader)
  to the sink inventory, with exactly one allowed file|function row.
- Pin the masked `lv_label_set_text` argument to string literals only.

### MED-3 Defense-in-depth rechecks can be defeated

**Probes (MISSED):**

- N11 adds `#define ui_safety_clear_verdict(v, r) UI_SAFETY_CLEAR_SEND` at file
  scope in `ui_page_safety.c`.
  - This disables the `clear_action` re-verdict. The primary `btn_clicked_cb`
    ADMIN gate is still intact, so this is not a standalone bypass.
  - The L1-macro rule covers only `$trusted`/`$openerTok` names, and
    `ui_safety_clear_verdict` is in neither.
- N03b adds `bool (*touch_cal_store_is_calibrated)(void) = cal_never;` as a local
  in `finish_calibration`.
  - This shadows the calibrated predicate, so the "first calibration is
    open" branch is always taken.
  - A USER can then overwrite an existing calibration.

**Fix:**

- Add every identifier named in a row regex (`ui_safety_clear_verdict`,
  `UI_SAFETY_CLEAR_SEND`, `touch_cal_store_is_calibrated`, ...) to `$trusted`.
- Together with the HIGH-1 declarator rule, this closes both probes.

### LOW-1 Gate implementation can be overridden or is not pinned

**Probes (MISSED):**

- N13 marks `ui_lcd_lock_has_role` `__attribute__((weak))` in `ui_lcd_lock.c` and
  adds a strong `ui_lcd_lock_has_role` returning true in `dashboard_exec_http.c`.
  - Attributes are stripped before matching, and the impostor scan covers
    drivers/ui only.
  - At link time the strong definition wins for every gate.
- N14 makes the real `ui_lcd_lock_has_role` body `return true;`.

`firmware/KilnFW/App/test/test_ui_lcd_lock.c` should catch N14 in host tests.
That was not run in this review. It cannot catch N13, which is a link-time
override in the target image only.

**Fix:**

- Refuse `weak` in `ui_lcd_lock.c` before attributes are stripped.
- Extend the impostor scan (a definition of any `$trusted` function outside its home
  file) to the whole App tree.
- Optionally assert that `ui_lcd_lock_has_role`'s body references the session role
  and the lock state.

### LOW-2 R12 false-fail (REVIEW_LCDFX8 LOW-3) still open

The lcdfx9 fix did not address it, and the real tree does not trigger it today.
The fix proposed in REVIEW_LCDFX8 still applies.

### INFO

- **INFO-1 No false positives.** The baseline passes on the real tree in both runs.
  The extra "preprocessor directive" message on controls C02/C03 is accurate:
  `ui_page_home_build` really contains `#define`/`#undef` lines (around source lines
  749 and 1179-1199). The check refuses those and reports them too.
- **INFO-2 The fixer's claim is verified.** All 29 REVIEW_LCDFX8 mutations
  (appendix A + B) are CAUGHT.
- **INFO-3 Compile claims rest on minimal reproductions.** The C behaviour above was
  checked with minimal xtensa gcc reproductions, not full-tree builds. N13 was not
  compiled.

## REVIEW_LCDFX8 finding status

| LCDFX8 finding | Status | Evidence / note |
|---|---|---|
| HIGH-1 | fixed inside drivers/ui | Set B B14, B16 CAUGHT; the class reopens outside UI as LCDFX9 HIGH-2 |
| HIGH-2 | fixed | B01-B03 CAUGHT |
| HIGH-3 | fixed | B04, B05 CAUGHT |
| HIGH-4 | fixed in drivers/ui | B21, B22 CAUGHT; still open App-wide (LCDFX9 LOW-1) |
| HIGH-5 | fixed | B17-B20 CAUGHT; role shadow is a new variant (LCDFX9 HIGH-1) |
| MED-1 | fixed for the listed names | B08-B12 CAUGHT; class still open (LCDFX9 MED-1) |
| MED-2 | fixed for one level | B06, B07 CAUGHT; deeper wrappers open (LCDFX9 HIGH-2) |
| LOW-1 | fixed | B25 CAUGHT |
| LOW-2 | fixed | B23 CAUGHT |
| LOW-3 | open | LCDFX9 LOW-2 |
| INFO-1..3 | informational | no action |

## Appendix A -- set A mutation JSON (32 mutations)

Anchors use LF. negtest converts them to the CRLF target files.

```json
[
 {
  "name": "N01_role_shadow_relay_toggle",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void relay_toggle_cb(lv_event_t *e)\n{\n",
  "replace": "static void relay_toggle_cb(lv_event_t *e)\n{\n    const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER;\n"
 },
 {
  "name": "N02a_role_shadow_profile_delete",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_profile_picker.c",
  "find": "static void delete_btn_clicked_cb(lv_event_t *e)\n{\n",
  "replace": "static void delete_btn_clicked_cb(lv_event_t *e)\n{\n    const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER;\n"
 },
 {
  "name": "N02b_role_shadow_crash_ack",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c",
  "find": "static void crash_ack_btn_clicked_cb(lv_event_t *e)\n{\n",
  "replace": "static void crash_ack_btn_clicked_cb(lv_event_t *e)\n{\n    const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER;\n"
 },
 {
  "name": "N02c_role_shadow_trip_clear_btn",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
  "find": "static void btn_clicked_cb(lv_event_t *e)\n{\n",
  "replace": "static void btn_clicked_cb(lv_event_t *e)\n{\n    const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER;\n"
 },
 {
  "name": "N02d_role_shadow_clear_action",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
  "find": "static void clear_action(void *user)\n{\n",
  "replace": "static void clear_action(void *user)\n{\n    const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER;\n"
 },
 {
  "name": "N03a_role_shadow_touch_cal_save",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_touch_cal.c",
  "find": "static void finish_calibration(void)\n{\n",
  "replace": "static void finish_calibration(void)\n{\n    const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER;\n"
 },
 {
  "name": "N03b_predicate_shadow_touch_cal_save",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_touch_cal.c",
  "find": "static void finish_calibration(void)\n{\n",
  "replace": "static bool cal_never(void) { return false; }\n\nstatic void finish_calibration(void)\n{\n    bool (*touch_cal_store_is_calibrated)(void) = cal_never;\n"
 },
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
  ]
 },
 {
  "name": "N05_gnu_style_wrapper_outside_ui",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "esp_err_t safety_clear_trip_post_handler(httpd_req_t *req)\n",
    "replace": "esp_err_t\ndashboard_lcd_quick_clear(void)\n{\n    return dashboard_safety_clear_trip();\n}\n\nesp_err_t safety_clear_trip_post_handler(httpd_req_t *req)\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);\n",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\nesp_err_t dashboard_lcd_quick_clear(void);\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)dashboard_lcd_quick_clear();\n"
   }
  ]
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
  ]
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
  ]
 },
 {
  "name": "N08a_lone_cr_ends_comment_home_trip_clear",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    // lcd note\r    (void)dashboard_safety_clear_trip();\n"
   }
  ]
 },
 {
  "name": "N08b_lone_cr_ends_comment_relay_toggle",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void relay_toggle_cb(lv_event_t *e)\n{\n",
  "replace": "static void relay_toggle_cb(lv_event_t *e)\n{\n    // admin gate below\r    relay_toggle_apply(lv_event_get_user_data(e)); return;\n"
 },
 {
  "name": "N09a_unlisted_kiln_io_owner_set_relay",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "#include \"lcd_auth_state.h\"\n",
    "replace": "#include \"lcd_auth_state.h\"\n#include \"kiln_io_owner.h\"\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)kiln_io_owner_command_set_relay(0, true, NULL);\n"
   }
  ]
 },
 {
  "name": "N09b_unlisted_relay_cycles_reset",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "#include \"lcd_auth_state.h\"\n",
    "replace": "#include \"lcd_auth_state.h\"\n#include \"relay_cycles.h\"\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)relay_cycles_reset(0);\n"
   }
  ]
 },
 {
  "name": "N09c_unlisted_ramp_assist_set",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "#include \"lcd_auth_state.h\"\n",
    "replace": "#include \"lcd_auth_state.h\"\n#include \"ramp_assist_cfg.h\"\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)ramp_assist_cfg_set_enabled(false);\n"
   }
  ]
 },
 {
  "name": "N09d_unlisted_factory_reset_execute",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "#include \"lcd_auth_state.h\"\n",
    "replace": "#include \"lcd_auth_state.h\"\n#include \"factory_reset.h\"\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)factory_reset_execute(0);\n"
   }
  ]
 },
 {
  "name": "N09e_unlisted_esp_restart",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    esp_restart();\n"
   }
  ]
 },
 {
  "name": "N10a_ap_password_shown_in_masked_branch",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_network.c",
  "find": "lv_label_set_text(s_ap_password_label, ap_password[0] ? \"******** (admin)\" : \"(open network)\");",
  "replace": "lv_label_set_text(s_ap_password_label, ap_password[0] ? ap_password : \"(open network)\");"
 },
 {
  "name": "N10b_role_shadow_ap_password",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_network.c",
  "find": "        const char *ap_password = wifi_prov_get_ap_password();\n",
  "replace": "        const char *ap_password = wifi_prov_get_ap_password();\n        const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_NONE;\n"
 },
 {
  "name": "N10c_ap_password_on_home",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    lv_label_set_text(lv_label_create(lv_layer_top()), wifi_prov_get_ap_password());\n"
   }
  ]
 },
 {
  "name": "N11_macro_verdict_defeats_clear_recheck",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
  "find": "static void clear_action(void *user)\n{\n",
  "replace": "#define ui_safety_clear_verdict(v, r) UI_SAFETY_CLEAR_SEND\nstatic void clear_action(void *user)\n{\n"
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
  ]
 },
 {
  "name": "N13_weak_gate_strong_override_outside_ui",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_lcd_lock.c",
    "find": "bool ui_lcd_lock_has_role(lcd_pin_role_t role)\n{\n",
    "replace": "__attribute__((weak)) bool ui_lcd_lock_has_role(lcd_pin_role_t role)\n{\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_exec_http.c",
    "find": "esp_err_t safety_clear_trip_post_handler(httpd_req_t *req)\n",
    "replace": "bool ui_lcd_lock_has_role(int role)\n{\n    (void)role;\n    return true;\n}\n\nesp_err_t safety_clear_trip_post_handler(httpd_req_t *req)\n"
   }
  ]
 },
 {
  "name": "N14_gate_body_returns_true",
  "file": "firmware/KilnFW/App/drivers/ui/ui_lcd_lock.c",
  "find": "bool ui_lcd_lock_has_role(lcd_pin_role_t role)\n{\n",
  "replace": "bool ui_lcd_lock_has_role(lcd_pin_role_t role)\n{\n    (void)role;\n    return true;\n"
 },
 {
  "name": "C01_static_inline_wrapper_non_ui_header",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.h",
    "find": "esp_err_t dashboard_safety_clear_trip(void);\n",
    "replace": "esp_err_t dashboard_safety_clear_trip(void);\nstatic inline esp_err_t dash_lcd_inline_clear(void) { return dashboard_safety_clear_trip(); }\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)dash_lcd_inline_clear();\n"
   }
  ]
 },
 {
  "name": "C02_direct_call_home",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)dashboard_safety_clear_trip();\n"
   }
  ]
 },
 {
  "name": "C03_fnptr_in_ui",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
    "find": "lv_obj_t *ui_page_home_build(void)\n{\n",
    "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    esp_err_t (*volatile clr)(void) = dashboard_safety_clear_trip;\n    (void)clr();\n"
   }
  ]
 },
 {
  "name": "C04_wrong_role_relay_toggle",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void relay_toggle_cb(lv_event_t *e)\n{\n",
  "replace": "static void relay_toggle_cb(lv_event_t *e)\n{\n    if (ui_lcd_lock_has_role(LCD_PIN_ROLE_USER)) { relay_toggle_apply(NULL); }\n"
 },
 {
  "name": "C05_goto_over_delete_gate",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_profile_picker.c",
  "find": "static void delete_btn_clicked_cb(lv_event_t *e)\n{\n",
  "replace": "static void delete_btn_clicked_cb(lv_event_t *e)\n{\n    goto skip;\n"
 },
 {
  "name": "C06_ifdef_second_relay_toggle",
  "edits": [
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "                          lv_event_get_user_data(e));\n}\n",
    "replace": "                          lv_event_get_user_data(e));\n}\n#endif\n"
   },
   {
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "static void relay_toggle_cb(lv_event_t *e)\n{\n",
    "replace": "#ifdef CONFIG_LCD_FAST_RELAY\nstatic void relay_toggle_cb(lv_event_t *e)\n{\n    relay_toggle_apply(lv_event_get_user_data(e));\n}\n#else\nstatic void relay_toggle_cb(lv_event_t *e)\n{\n"
   }
  ]
 },
 {
  "name": "C07_ui_macro_alias",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
  "find": "static void clear_action(void *user)\n{\n",
  "replace": "#define LCDCLR() dashboard_safety_clear_trip()\nstatic void clear_action(void *user)\n{\n"
 }
]
```

## Appendix B -- set B mutation JSON (REVIEW_LCDFX8 appendix A + B, 29 mutations)

```json
[
 {
  "replace": "static void relay_toggle_cb(lv_event_t *e)\n{\n    (dashboard_set_relay)(1, true, NULL);\n",
  "name": "B01 paren-call sink before relay gate",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void relay_toggle_cb(lv_event_t *e)\n{\n"
 },
 {
  "replace": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    dashboard_relay_result_t (*fp)(uint8_t, bool, uint32_t *) = dashboard_set_relay;\n    (void)fp(1, true, NULL);\n",
  "name": "B02 function-pointer sink call",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
 },
 {
  "name": "B03 macro alias of sink",
  "edits": [
   {
    "replace": "#include \"ui_lcd_lock.h\"\n#define RELAY_WR dashboard_set_relay\n",
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "#include \"ui_lcd_lock.h\"\n"
   },
   {
    "replace": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    (void)RELAY_WR(1, true, NULL);\n",
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
   }
  ]
 },
 {
  "replace": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    ESP_LOGI(TAG, \"r=%d\", (int)dashboard_set_relay(1, true, NULL));\n",
  "name": "B04 sink call inside ESP_LOGI args",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
 },
 {
  "replace": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    ESP_LOGI(TAG, \"%d\", (relay_toggle_apply(&s_relay_ctx[0]), 0));\n",
  "name": "B05 gated apply called inside ESP_LOGI args",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
 },
 {
  "name": "B06 static inline sink wrapper in a drivers/ui header",
  "edits": [
   {
    "replace": "static inline void ui_temp_force_on(void) { (void)dashboard_set_relay(1, true, NULL); }\n#endif // UI_PAGE_TEMPERATURE_SAFETY_H",
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature_safety.h",
    "find": "#endif // UI_PAGE_TEMPERATURE_SAFETY_H"
   },
   {
    "replace": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    ui_temp_force_on();\n",
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
   }
  ]
 },
 {
  "name": "B07 sink wrapper in drivers/http, called from UI",
  "edits": [
   {
    "replace": "void dashboard_lcd_quick_on(void) { (void)dashboard_set_relay(1, true, NULL); }\n\ndashboard_relay_result_t dashboard_set_relay(uint8_t relay_index, bool on, uint32_t *out_safety_sources)\n{",
    "file": "firmware/KilnFW/App/drivers/http/dashboard_http.c",
    "find": "dashboard_relay_result_t dashboard_set_relay(uint8_t relay_index, bool on, uint32_t *out_safety_sources)\n{"
   },
   {
    "replace": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    dashboard_lcd_quick_on();\n",
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
    "find": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
   }
  ]
 },
 {
  "replace": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    (void)kiln_io_set_relay(1, true);\n",
  "name": "B08 lower-level kiln_io_set_relay (not inventoried)",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
 },
 {
  "replace": "static void save_cb(lv_event_t *e)\n{\n    (void)e;\n    { uint8_t id = 0; char er[8]; (void)profiles_live_decide_apply(LIVE_EDIT_DECISION_DISCARD, NULL, false, &id, er, sizeof(er)); }\n",
  "name": "B09 profiles_live_decide_apply ungated in save_cb",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_live_decide.c",
  "find": "static void save_cb(lv_event_t *e)\n{\n    (void)e;\n"
 },
 {
  "replace": "static void row_name_clicked_cb(lv_event_t *e)\n{\n    (void)profiles_http_delete(9);\n",
  "name": "B10 profiles_http_delete ungated in row_name_clicked_cb",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_profile_picker.c",
  "find": "static void row_name_clicked_cb(lv_event_t *e)\n{\n"
 },
 {
  "replace": "static void prev_cb(lv_event_t *e)\n{\n    (void)e;\n    { bool to = false; (void)crash_report_acknowledge_timeout(100, &to); }\n",
  "name": "B11 crash_report_acknowledge_timeout ungated in prev_cb",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c",
  "find": "static void prev_cb(lv_event_t *e)\n{\n    (void)e;\n"
 },
 {
  "replace": "static void prev_cb(lv_event_t *e)\n{\n    (void)e;\n    if (s_pg && s_pg->working) { char er[8]; (void)edit_firing_apply(s_pg->working, &s_pg->ctx, er, sizeof(er)); }\n",
  "name": "B12 edit_firing_apply ungated in prev_cb",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_edit_firing.c",
  "find": "static void prev_cb(lv_event_t *e)\n{\n    (void)e;\n"
 },
 {
  "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    esp_err_t (*clr)(void) = dashboard_safety_clear_trip;\n    (void)clr();\n",
  "name": "B14 trip clear via function pointer in ui_page_home.c",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
  "find": "lv_obj_t *ui_page_home_build(void)\n{\n"
 },
 {
  "replace": "static void refresh_cb(lv_timer_t *t)\n{\n    (void)t;\n    esp_err_t (*clr)(void) = dashboard_safety_clear_trip;\n    if (s_view.tripped_live) (void)clr();\n",
  "name": "B15 (control) trip clear via function pointer in ui_page_safety.c refresh_cb",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
  "find": "static void refresh_cb(lv_timer_t *t)\n{\n    (void)t;\n"
 },
 {
  "replace": "lv_obj_t *ui_page_home_build(void)\n{\n    (void)(dashboard_safety_clear_trip)();\n",
  "name": "B16 trip clear paren-call in ui_page_home.c",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
  "find": "lv_obj_t *ui_page_home_build(void)\n{\n"
 },
 {
  "replace": "        if (s_current_point == 0) return;\n    }\n    err = touch_cal_store_save(&cal);",
  "name": "B17 touch cal save guard made a conditional return",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_touch_cal.c",
  "find": "        return;\n    }\n    err = touch_cal_store_save(&cal);"
 },
 {
  "name": "B18 touch cal save guard inside #if 0",
  "edits": [
   {
    "replace": "#if 0\n    if (touch_cal_store_is_calibrated() && !ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {",
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_touch_cal.c",
    "find": "    if (touch_cal_store_is_calibrated() && !ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {"
   },
   {
    "replace": "        return;\n    }\n#endif\n    err = touch_cal_store_save(&cal);",
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_touch_cal.c",
    "find": "        return;\n    }\n    err = touch_cal_store_save(&cal);"
   }
  ]
 },
 {
  "replace": "    if (0) if (!ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {\n        ui_lcd_lock_run_gated(\"Admin PIN to delete profile\"",
  "name": "B19 profile delete gate prefixed with if (0)",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_profile_picker.c",
  "find": "    if (!ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {\n        ui_lcd_lock_run_gated(\"Admin PIN to delete profile\""
 },
 {
  "replace": "    if (0) if (!ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {\n        ui_lcd_lock_run_gated(\"Admin PIN to acknowledge crash\"",
  "name": "B20 crash ack gate prefixed with if (0)",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c",
  "find": "    if (!ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {\n        ui_lcd_lock_run_gated(\"Admin PIN to acknowledge crash\""
 },
 {
  "replace": "#include \"lcd_auth_state.h\"\nstatic void ui_lcd_lock_run_gated(const char *prompt, lcd_pin_role_t role, void (*fn)(void *), void *ud)\n{\n    (void)prompt;\n    (void)role;\n    fn(ud);\n}\n",
  "name": "B21 file-local static ui_lcd_lock_run_gated impostor (temperature)",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "#include \"ui_lcd_lock.h\"\n"
 },
 {
  "replace": "#include \"lcd_auth_state.h\"\nstatic bool ui_lcd_lock_has_role(lcd_pin_role_t r) { (void)r; return true; }",
  "name": "B22 file-local static ui_lcd_lock_has_role impostor (touch cal)",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_touch_cal.c",
  "find": "#include \"ui_lcd_lock.h\""
 },
 {
  "replace": "    if (!s_pg || !s_pg->active || !s_pg->working || s_pg->decide_mode && !ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) return;",
  "name": "B23 apply_live_gated re-check short-circuited",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_edit_firing.c",
  "find": "    if (!s_pg || !s_pg->active || !s_pg->working || !ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) return;"
 },
 {
  "name": "B24 clear_action verdict switch inside #if 0",
  "edits": [
   {
    "replace": "    (void)user;\n#if 0\n    switch (ui_safety_clear_verdict(",
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
    "find": "    (void)user;\n    switch (ui_safety_clear_verdict("
   },
   {
    "replace": "    default:\n        break;\n    }\n#endif\n\n    esp_err_t err = dashboard_safety_clear_trip();",
    "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
    "find": "    default:\n        break;\n    }\n\n    esp_err_t err = dashboard_safety_clear_trip();"
   }
  ]
 },
 {
  "replace": "    dashboard_relay_result_t result = DASHBOARD_RELAY_OK;\n    (void)want_on;",
  "name": "B25 sink removed from its allow-listed function (staleness guard)",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "    dashboard_relay_result_t result = dashboard_set_relay(ctx->relay_index, want_on, &sources);"
 },
 {
  "replace": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    (void)dashboard_set_relay(1, true, NULL);\n",
  "name": "C1 (control) direct sink call",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
 },
 {
  "replace": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    (void)dashboard_set_\\\nrelay(1, true, NULL);\n",
  "name": "C2 (control) line-spliced sink call",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
 },
 {
  "replace": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    const char *k = \"\\\\\"; (void)k; (void)dashboard_set_relay(1, true, NULL);\n",
  "name": "C3 (control) escaped string then sink call",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
 },
 {
  "replace": "static void refresh_cb(lv_timer_t *t)\n{\n    (void)t;\n    if (lv_obj_is_valid(NULL)) clear_action(NULL);\n",
  "name": "V1 (HIGH-1 recheck) brace-less if then ungated clear_action",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
  "find": "static void refresh_cb(lv_timer_t *t)\n{\n    (void)t;\n"
 },
 {
  "replace": "static void relay_toggle_cb(lv_event_t *e)\n{\n    if (e) { relay_toggle_apply(NULL); }\n",
  "name": "V2 (MED-2 recheck) gated apply inside pre-gate braced block",
  "file": "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
  "find": "static void relay_toggle_cb(lv_event_t *e)\n{\n"
 }
]
```
