# Review LCDFX8: adversarial bypass of check_lcd_admin_gates.ps1 (2026-10-12)

Reviewer: opus, read-only review; the bench board was not touched.

Scope: `tools/check_lcd_admin_gates.ps1` as of origin/dev `8780bc125`. That includes `1715ad8ff`, the
LCDFX7 check fix, and `291d061df`, the docs. The fixes under review close `REVIEW_LCDFX7_2026-10-11.md`
HIGH-1, MED-1, MED-2 and LOW-1..3.

Goal: write an LCD UI change that lets a USER or no-login touch reach an admin sink while the check stays
green. The admin sinks are relay write, trip clear, Wi-Fi forget, unit pref, touch cal save, profile
delete, crash ack and live decide.

Method: every attempt is a `tools/negtest.ps1 -Preset check -PresetArg tools/check_lcd_admin_gates.ps1`
mutation. Two sets were run:

- Set 1 has 27 mutations, run with `-Parallel 4`. Logs are in
  `%TEMP%\negtest_logs\20261012_001006_x57b`.
- Set 2 has 2 mutations, rechecking the LCDFX7 fixes.

The full JSON for both sets is in the appendix. The generator was a scratch PowerShell script whose
output is reproduced verbatim there.

## Summary

| ID | Finding |
|---|---|
| HIGH-1 | The trip clear can be reached ungated from any UI file except `ui_page_safety.c`. Owner rule D2 says the LCD trip clear needs an admin login. The ban regex only sees `name(`, so a function pointer (B14) or a parenthesised call `(dashboard_safety_clear_trip)()` (B16) in `ui_page_home.c` passes. |
| HIGH-2 | Every sink is matched as a call only (`name\s*\(`), so taking its address bypasses the inventory: `(dashboard_set_relay)(...)` (B01), a function pointer (B02) and a `#define` alias (B03) all pass. |
| HIGH-3 | An ESP_LOG statement is deleted before any analysis unless it contains a `$logTok` name, and the sinks and the `*_apply` names are not in `$logTok`. A sink or gated apply called inside log arguments is invisible (B04, B05). |
| HIGH-4 | Gate impostors pass: a file-local `static ui_lcd_lock_run_gated()` that just runs the callback (B21), or `static ui_lcd_lock_has_role()` returning true (B22). Every gate row then matches text that no longer gates. |
| HIGH-5 | Row-level guard bypasses: `if (0)` before the profile delete gate (B19) and before the crash ack gate (B20); the touch cal save guard turned into a conditional return (B17) or wrapped in `#if 0` (B18). Rows 42, 43 and 48 are not start-anchored and not preprocessor-aware. |
| MED-1 | The sink inventory is incomplete. Admin mutators called from the UI with no inventory row pass when called ungated anywhere: `profiles_live_decide_apply` (B09), `profiles_http_delete` (B10), `crash_report_acknowledge_timeout` (B11), `edit_firing_apply` (B12) and `kiln_io_set_relay` (B08). The same holds for `crash_report_acknowledge`, `profiles_favorites_set`, `profiles_http_save*` and the `wifi_prov_add_network`/`set_ap_*`/`set_mode`/`post_and_wait` family. |
| MED-2 | The inventory scans `drivers/ui/*.c` only. A `static inline` wrapper in a `drivers/ui` header (B06), or a wrapper defined in `drivers/http` and called from the UI (B07), passes. |
| LOW-1 | The `$sinkAllow` staleness loop is dead code. `if ($sk[1] -match '\w') { continue }` is true for every row, so the "allowlisted ... no longer present" failure can never fire. Removing the only `dashboard_set_relay` call passes (B25). |
| LOW-2 | Defense-in-depth rechecks are weaker than the primary gates. `apply_live_gated`'s inner ADMIN recheck can be short-circuited (`|| s_pg->decide_mode && !has_role`) and still pass (B23). The primary gate on the caller is intact, so this is not a bypass on its own. |
| LOW-3 | Carried over from LCDFX7 LOW-3: R12 (verdict held in a local) still false-fails loudly. That is safe in direction. |
| INFO-1 | False positives: the check passes on the real tree (`8780bc125`, about 20 s, 24 per-function gates). None of the mutations here are formatting-only, so no new false-fail was found. |
| INFO-2 | The LCDFX7 fixes hold where they claim to. V1 (HIGH-1 recheck: ungated `clear_action` after a brace-less `if`) and V2 (MED-2 recheck: apply inside a pre-gate braced block) are both CAUGHT. B15 (function pointer to the trip clear inside `ui_page_safety.c`) is CAUGHT by the per-file use count. B24 (`#if 0` around the clear_action verdict switch) is CAUGHT, but only incidentally, because the `#if 0` line breaks the row regex's adjacency. |
| INFO-3 | Unchanged from earlier reviews: the gate semantics still rest on host tests. |

Result counts:

- Set 1: 22 MISSED, 5 CAUGHT. The CAUGHT ones are B15, B24 and the controls C1, C2 and C3.
- Set 2: 2 CAUGHT.
- The baseline passed and the real tree was unchanged in both sets.

## Owner-rule cross-check

**D2: LCD trip clear needs an admin login.** On the real tree this holds:

- the Clear button gate is ADMIN;
- `clear_action` rechecks ADMIN through `ui_safety_clear_verdict(..., has_role(ADMIN))`;
- `ui_page_safety.c` is pinned by use counts (B15 CAUGHT).

The check does not enforce D2 outside `ui_page_safety.c`. HIGH-1 is the gap.

**Safety page reads need the USER PIN.** On the real tree the trip strip opener is USER-gated and the C3/C4 nav and strip pins hold. No mutation here targeted the read path.

**Diagnostics, Temperature, Network and Profiles are readable with USER and their writes are ADMIN.** On
the real tree each write callback has an ADMIN row. MED-1 shows that a new ungated write path to several
of those mutators would not be noticed.

## Findings

### HIGH-1: trip clear reachable from another UI file

The L4 ban is `\b(dashboard_safety_clear_trip|safety_link_send_clear_trip)\s*\(`. The use-count pin that
caught B15 is per-file and covers only `ui_page_safety.c`. In `ui_page_home.c`, both of these pass:

```c
esp_err_t (*clr)(void) = dashboard_safety_clear_trip;
(void)clr();
```

and

```c
(void)(dashboard_safety_clear_trip)();
```

A home screen any no-login user sees could then clear a latched trip.

**Fix:** in every UI `.c` and `.h` file, count every identifier occurrence of each sink (`\bname\b`,
strings blanked, the call inside the allow-listed function excepted), not only `name(`. Any second
occurrence fails.

### HIGH-2: address-of and alias bypass the sink inventory

This is the same root cause as HIGH-1, applied to the general inventory. B01, B02 and B03 pass with
`dashboard_set_relay` reached from `build_zone_temp_row`.

**Fix:**

- Use the identifier count from HIGH-1.
- Add the sink names to `$trusted`/`$openerTok` so a `#define` whose body names a sink fails. Better, fail on any `#define` in a UI file whose body names a sink or a gated apply.

### HIGH-3: ESP_LOG hides calls

`$logTok` decides whether a log statement is kept. It lists `dashboard_safety_clear_trip` and
`*_open_apply`/`*_gated_*`. It does not list `dashboard_set_relay`, `wifi_prov_forget_network`,
`unit_pref_set*`, `touch_cal_store_save`, `relay_toggle_apply`, `units_toggle_apply`, `delete_do`,
`crash_ack_do` or `forget_worker_task`.

**Fix:** keep every log statement and only blank its format string. Alternatively, build `$logTok` from
the sink inventory, the use-count rows and every row's apply name, so a new row cannot forget it.

### HIGH-4: gate impostors

Nothing requires `ui_lcd_lock_run_gated`/`ui_lcd_lock_has_role` to be the real ones.

**Fix:**

- Fail if any `drivers/ui` file other than `ui_lcd_lock.c` defines or `#define`s a `ui_lcd_lock_*`
  name, or `#define`s `LCD_PIN_ROLE_*`.
- Require every gated file to include `"ui_lcd_lock.h"`.

### HIGH-5: guards not start-anchored or preprocessor-aware

The affected rows:

- Rows 43 and 48 (profile delete and crash ack click callbacks) are end-anchored but accept any text,
  including `if (0)`, before the gate `if`.
- Row 42 (`finish_calibration`) accepts `[^{}]*\breturn\s*;` inside the guard block, so a conditional
  return passes. It also accepts a guard that `#if 0` removes.

**Fix:**

- Anchor these rows the way clear_action is anchored: the gate statement is the first statement, or it
  is preceded only by `(void)x;` lines.
- Reuse the LOW-1 straight-line-then-unconditional-`return;` sub-pattern for row 42.
- Refuse `#if`/`#ifdef`/`#else` inside any function body the check gates. This is simplest, and no gated
  function uses them today.

### MED-1: inventory incomplete

The inventory has five sinks. The LCD reaches at least ten more ADMIN-tier mutators. Today these are
gated only by row regexes on their current callers, so a new ungated caller passes (B08-B12).

**Fix:** list every admin mutator the UI links against, as above, in `$sinkAllow`. Better, invert the
default: derive the list from the `ROUTE_TIER_ADMIN` handlers' callees, or from a named header
annotation. That way a newly used mutator fails until it is classified.

### MED-2: wrappers outside the scanned set

The inventory loop is `Get-ChildItem $UiDir -Filter *.c`.

**Fix:**

- Include `*.h` in `drivers/ui`.
- Fail when a non-UI file defines a non-static function whose body calls a sink and that function is
  called from `drivers/ui`. A cheap version: list every external function the UI calls that is defined
  in `drivers/http`, and require each one to be either the sink itself or on an explicit allowlist.

### LOW-1: dead staleness loop

`$sk[1] -match '\w'` was presumably meant to skip the one regex row (`unit_pref_set\w*`). It skips all
of them. With the loop dead, a refactor that moves a sink call out of the allow-listed function is
caught only if the new location is also scanned. A deleted gate path would leave a stale row.

**Fix:** skip only when `$sk[1]` contains a regex metacharacter (`-match '[\\*+?\[]'`). Negative-test it
with B25.

### LOW-2: recheck short-circuit

B23 passes because row 47 matches `!ui_lcd_lock_has_role\(ADMIN\)\)\s*return\s*;` anywhere.

**Fix:** anchor row 47 on the whole condition, or require the recheck to be its own statement
(`if (!ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) return;`).

## Fix status

| ID | Status | Notes |
|---|---|---|
| HIGH-1..5, MED-1, MED-2, LOW-1, LOW-2 | open | Re-run the appendix sets after the fix. Every B mutation except B25 should be CAUGHT. B25 should be CAUGHT once LOW-1 is fixed. B15, B24, C1-C3, V1 and V2 must stay CAUGHT. |

## Appendix A: mutation set 1 (27 mutations, `-Parallel 4`)

Base `8780bc125`. Verdicts:

- MISSED: B01-B12, B14, B16-B23, B25.
- CAUGHT: B15, B24, C1, C2, C3.

There is no B13; the number was not used.

Run with:

```
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset check -PresetArg tools/check_lcd_admin_gates.ps1 -Mutations <file>.json -Parallel 4
```

The JSON follows, verbatim.

```json
[
    {
        "replace":  "static void relay_toggle_cb(lv_event_t *e)\n{\n    (dashboard_set_relay)(1, true, NULL);\n",
        "name":  "B01 paren-call sink before relay gate",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
        "find":  "static void relay_toggle_cb(lv_event_t *e)\n{\n"
    },
    {
        "replace":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    dashboard_relay_result_t (*fp)(uint8_t, bool, uint32_t *) = dashboard_set_relay;\n    (void)fp(1, true, NULL);\n",
        "name":  "B02 function-pointer sink call",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
        "find":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
    },
    {
        "name":  "B03 macro alias of sink",
        "edits":  [
                      {
                          "replace":  "#include \"ui_lcd_lock.h\"\n#define RELAY_WR dashboard_set_relay\n",
                          "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
                          "find":  "#include \"ui_lcd_lock.h\"\n"
                      },
                      {
                          "replace":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    (void)RELAY_WR(1, true, NULL);\n",
                          "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
                          "find":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
                      }
                  ]
    },
    {
        "replace":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    ESP_LOGI(TAG, \"r=%d\", (int)dashboard_set_relay(1, true, NULL));\n",
        "name":  "B04 sink call inside ESP_LOGI args",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
        "find":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
    },
    {
        "replace":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    ESP_LOGI(TAG, \"%d\", (relay_toggle_apply(\u0026s_relay_ctx[0]), 0));\n",
        "name":  "B05 gated apply called inside ESP_LOGI args",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
        "find":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
    },
    {
        "name":  "B06 static inline sink wrapper in a drivers/ui header",
        "edits":  [
                      {
                          "replace":  "static inline void ui_temp_force_on(void) { (void)dashboard_set_relay(1, true, NULL); }\n#endif // UI_PAGE_TEMPERATURE_SAFETY_H",
                          "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature_safety.h",
                          "find":  "#endif // UI_PAGE_TEMPERATURE_SAFETY_H"
                      },
                      {
                          "replace":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    ui_temp_force_on();\n",
                          "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
                          "find":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
                      }
                  ]
    },
    {
        "name":  "B07 sink wrapper in drivers/http, called from UI",
        "edits":  [
                      {
                          "replace":  "void dashboard_lcd_quick_on(void) { (void)dashboard_set_relay(1, true, NULL); }\n\ndashboard_relay_result_t dashboard_set_relay(uint8_t relay_index, bool on, uint32_t *out_safety_sources)\n{",
                          "file":  "firmware/KilnFW/App/drivers/http/dashboard_http.c",
                          "find":  "dashboard_relay_result_t dashboard_set_relay(uint8_t relay_index, bool on, uint32_t *out_safety_sources)\n{"
                      },
                      {
                          "replace":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    dashboard_lcd_quick_on();\n",
                          "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
                          "find":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
                      }
                  ]
    },
    {
        "replace":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    (void)kiln_io_set_relay(1, true);\n",
        "name":  "B08 lower-level kiln_io_set_relay (not inventoried)",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
        "find":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
    },
    {
        "replace":  "static void save_cb(lv_event_t *e)\n{\n    (void)e;\n    { uint8_t id = 0; char er[8]; (void)profiles_live_decide_apply(LIVE_EDIT_DECISION_DISCARD, NULL, false, \u0026id, er, sizeof(er)); }\n",
        "name":  "B09 profiles_live_decide_apply ungated in save_cb",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_live_decide.c",
        "find":  "static void save_cb(lv_event_t *e)\n{\n    (void)e;\n"
    },
    {
        "replace":  "static void row_name_clicked_cb(lv_event_t *e)\n{\n    (void)profiles_http_delete(9);\n",
        "name":  "B10 profiles_http_delete ungated in row_name_clicked_cb",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_profile_picker.c",
        "find":  "static void row_name_clicked_cb(lv_event_t *e)\n{\n"
    },
    {
        "replace":  "static void prev_cb(lv_event_t *e)\n{\n    (void)e;\n    { bool to = false; (void)crash_report_acknowledge_timeout(100, \u0026to); }\n",
        "name":  "B11 crash_report_acknowledge_timeout ungated in prev_cb",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c",
        "find":  "static void prev_cb(lv_event_t *e)\n{\n    (void)e;\n"
    },
    {
        "replace":  "static void prev_cb(lv_event_t *e)\n{\n    (void)e;\n    if (s_pg \u0026\u0026 s_pg-\u003eworking) { char er[8]; (void)edit_firing_apply(s_pg-\u003eworking, \u0026s_pg-\u003ectx, er, sizeof(er)); }\n",
        "name":  "B12 edit_firing_apply ungated in prev_cb",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_edit_firing.c",
        "find":  "static void prev_cb(lv_event_t *e)\n{\n    (void)e;\n"
    },
    {
        "replace":  "lv_obj_t *ui_page_home_build(void)\n{\n    esp_err_t (*clr)(void) = dashboard_safety_clear_trip;\n    (void)clr();\n",
        "name":  "B14 trip clear via function pointer in ui_page_home.c",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
        "find":  "lv_obj_t *ui_page_home_build(void)\n{\n"
    },
    {
        "replace":  "static void refresh_cb(lv_timer_t *t)\n{\n    (void)t;\n    esp_err_t (*clr)(void) = dashboard_safety_clear_trip;\n    if (s_view.tripped_live) (void)clr();\n",
        "name":  "B15 (control) trip clear via function pointer in ui_page_safety.c refresh_cb",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
        "find":  "static void refresh_cb(lv_timer_t *t)\n{\n    (void)t;\n"
    },
    {
        "replace":  "lv_obj_t *ui_page_home_build(void)\n{\n    (void)(dashboard_safety_clear_trip)();\n",
        "name":  "B16 trip clear paren-call in ui_page_home.c",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_home.c",
        "find":  "lv_obj_t *ui_page_home_build(void)\n{\n"
    },
    {
        "replace":  "        if (s_current_point == 0) return;\n    }\n    err = touch_cal_store_save(\u0026cal);",
        "name":  "B17 touch cal save guard made a conditional return",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_touch_cal.c",
        "find":  "        return;\n    }\n    err = touch_cal_store_save(\u0026cal);"
    },
    {
        "name":  "B18 touch cal save guard inside #if 0",
        "edits":  [
                      {
                          "replace":  "#if 0\n    if (touch_cal_store_is_calibrated() \u0026\u0026 !ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {",
                          "file":  "firmware/KilnFW/App/drivers/ui/ui_page_touch_cal.c",
                          "find":  "    if (touch_cal_store_is_calibrated() \u0026\u0026 !ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {"
                      },
                      {
                          "replace":  "        return;\n    }\n#endif\n    err = touch_cal_store_save(\u0026cal);",
                          "file":  "firmware/KilnFW/App/drivers/ui/ui_page_touch_cal.c",
                          "find":  "        return;\n    }\n    err = touch_cal_store_save(\u0026cal);"
                      }
                  ]
    },
    {
        "replace":  "    if (0) if (!ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {\n        ui_lcd_lock_run_gated(\"Admin PIN to delete profile\"",
        "name":  "B19 profile delete gate prefixed with if (0)",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_profile_picker.c",
        "find":  "    if (!ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {\n        ui_lcd_lock_run_gated(\"Admin PIN to delete profile\""
    },
    {
        "replace":  "    if (0) if (!ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {\n        ui_lcd_lock_run_gated(\"Admin PIN to acknowledge crash\"",
        "name":  "B20 crash ack gate prefixed with if (0)",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c",
        "find":  "    if (!ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {\n        ui_lcd_lock_run_gated(\"Admin PIN to acknowledge crash\""
    },
    {
        "replace":  "#include \"lcd_auth_state.h\"\nstatic void ui_lcd_lock_run_gated(const char *prompt, lcd_pin_role_t role, void (*fn)(void *), void *ud)\n{\n    (void)prompt;\n    (void)role;\n    fn(ud);\n}\n",
        "name":  "B21 file-local static ui_lcd_lock_run_gated impostor (temperature)",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
        "find":  "#include \"ui_lcd_lock.h\"\n"
    },
    {
        "replace":  "#include \"lcd_auth_state.h\"\nstatic bool ui_lcd_lock_has_role(lcd_pin_role_t r) { (void)r; return true; }",
        "name":  "B22 file-local static ui_lcd_lock_has_role impostor (touch cal)",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_touch_cal.c",
        "find":  "#include \"ui_lcd_lock.h\""
    },
    {
        "replace":  "    if (!s_pg || !s_pg-\u003eactive || !s_pg-\u003eworking || s_pg-\u003edecide_mode \u0026\u0026 !ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) return;",
        "name":  "B23 apply_live_gated re-check short-circuited",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_edit_firing.c",
        "find":  "    if (!s_pg || !s_pg-\u003eactive || !s_pg-\u003eworking || !ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) return;"
    },
    {
        "name":  "B24 clear_action verdict switch inside #if 0",
        "edits":  [
                      {
                          "replace":  "    (void)user;\n#if 0\n    switch (ui_safety_clear_verdict(",
                          "file":  "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
                          "find":  "    (void)user;\n    switch (ui_safety_clear_verdict("
                      },
                      {
                          "replace":  "    default:\n        break;\n    }\n#endif\n\n    esp_err_t err = dashboard_safety_clear_trip();",
                          "file":  "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
                          "find":  "    default:\n        break;\n    }\n\n    esp_err_t err = dashboard_safety_clear_trip();"
                      }
                  ]
    },
    {
        "replace":  "    dashboard_relay_result_t result = DASHBOARD_RELAY_OK;\n    (void)want_on;",
        "name":  "B25 sink removed from its allow-listed function (staleness guard)",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
        "find":  "    dashboard_relay_result_t result = dashboard_set_relay(ctx-\u003erelay_index, want_on, \u0026sources);"
    },
    {
        "replace":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    (void)dashboard_set_relay(1, true, NULL);\n",
        "name":  "C1 (control) direct sink call",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
        "find":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
    },
    {
        "replace":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    (void)dashboard_set_\\\nrelay(1, true, NULL);\n",
        "name":  "C2 (control) line-spliced sink call",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
        "find":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
    },
    {
        "replace":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n    const char *k = \"\\\\\"; (void)k; (void)dashboard_set_relay(1, true, NULL);\n",
        "name":  "C3 (control) escaped string then sink call",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
        "find":  "static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)\n{\n"
    }
]
```

## Appendix B: mutation set 2 (LCDFX7 fix rechecks)

Both CAUGHT on `8780bc125`.

```json
[
    {
        "replace":  "static void refresh_cb(lv_timer_t *t)\n{\n    (void)t;\n    if (lv_obj_is_valid(NULL)) clear_action(NULL);\n",
        "name":  "V1 (HIGH-1 recheck) brace-less if then ungated clear_action",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_safety.c",
        "find":  "static void refresh_cb(lv_timer_t *t)\n{\n    (void)t;\n"
    },
    {
        "replace":  "static void relay_toggle_cb(lv_event_t *e)\n{\n    if (e) { relay_toggle_apply(NULL); }\n",
        "name":  "V2 (MED-2 recheck) gated apply inside pre-gate braced block",
        "file":  "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c",
        "find":  "static void relay_toggle_cb(lv_event_t *e)\n{\n"
    }
]
```
