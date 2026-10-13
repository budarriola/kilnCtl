# LCD UI review (2026-10-09)

Scope: KilnFW LCD UI on origin/dev at `db1f4df5`. That covers
`firmware/KilnFW/App/drivers/ui/*`, `lvgl_port.c`, the touch path
(`hw/FT6336U.c`, `hw/touch_dev.c`), display power and backlight
(`ui/screen_idle.c`, `hw/backlight_pwm.c`) and LCD login gating
(`ui/ui_lcd_lock.c`, `ui/lcd_auth_state.c`). It was re-checked at origin/dev
12 commits later: none of them touch `drivers/ui/` or
`control/profile_executor_run.c`.

This review reports findings only and changes no code. It follows
`docs/audits/LCD_UI_AUDIT_2026-10-09.md` (L1-L30) and does not repeat those
findings. Paths below are relative to `firmware/KilnFW/App/drivers/`.

Owner rules checked against:
- Unauthenticated users see dashboards only, on the web and on the LCD.
- Clearing a safety trip from the LCD needs an admin login and follows the
  web clear rules.
- LCD pages must not scroll (320x480; content fits).
- L7 (auth off opens every gate) stays as it is.

## Summary

| ID | Sev | Area | Finding |
|----|-----|------|---------|
| N1 | HIGH | auth | Residual of L9. On a board offered touch calibration with none stored, leaving the touch_cal page (Cancel, or a refused save) lands on the Config hub with no PIN, and no relock edge ever sends it home. From there an unauthenticated user reaches Profiles, then a profile, then Start, which has no gate of its own. **Status: FIXED in cc62edf2f.** |
| N2 | MED | mode gate | LCD Start (home and profile detail) and the UART bridge start call `profile_executor_run()` directly. They skip the web handler's `danger_mode_active()` 409 refusal, so a firing can start from the LCD while the danger-mode window is open. **Status: FIXED in cc62edf2f (autotune unchanged: its web route never refused in danger mode).** |
| N3 | MED | units | Home rail zone temperatures always print raw Celsius, with no unit suffix, even when the unit preference is Fahrenheit. The chart beside them is in Fahrenheit. **Status: FIXED in cc62edf2f.** |
| N4 | LOW | stale | The home rail zone temperature ignores `channels[i].stale` (age >= `KILN_TEMP_STALE_AGE_MS`, 10 s), so a frozen reading shows as live. The Temperature page and the home chart both hide stale values. **Status: FIXED in cc62edf2f.** |
| N5 | LOW | auth (owner confirm) | Tapping the home trip strip opens the Safety page with no PIN, while web `/safety` is ADMIN. The Clear button stays ADMIN-gated. **Status: FIXED in cc62edf2f (owner decision 2026-10-09).** |
| N6 | LOW | auth (read parity) | The LCD Diagnostics, Temperature, Network and Profiles pages need only the USER PIN (Menu), while their web equivalents are ADMIN pages and GETs, for example `GET /api/crash_report`. **Status: KEPT by owner decision 2026-10-09 (LCD read access stays at USER).** |
| N7 | INFO | stack | The LVGL task stack (10240 B) has no proven ceiling: the static budget check reports it INDETERMINATE, and the measured 6896 B was taken at boot, before the heaviest LVGL-task paths ran. |
| N8 | INFO | units | The builder's Target and Ramp number pads are Celsius-only by design while their value labels follow the unit preference. This is documented in code, but a Fahrenheit user sees one number on the card and a different one on the pad. **Status: FIXED in cc62edf2f (pads follow unit, convert to Celsius on store).** |

## Findings

### N1 (HIGH) touch_cal exit lands on the Config hub with no PIN -- FIXED in cc62edf2f

Where:
- `ui/kiln_ui.c:347`-`:349`: on an offerable panel (`TOUCH_CAL_SUPPORT_SUPPORTED`, the resistive NS2009 path) with no stored calibration, boot shows `touch_cal`, not home.
- `ui/ui_page_touch_cal.c:239`-`:244` (`cancel_press_cb`): calls `kiln_ui_show("config")` unconditionally.
- `ui/ui_page_touch_cal.c:180`-`:182`: a save refused for lack of admin also goes to `config`.
- `ui/ui_lcd_lock.c:42`: `static bool s_was_locked = true;`. The relock callback (go home) fires only on an unlocked-to-locked edge. A user who never unlocked produces no edge, so nothing ever sends the Config hub back home.

The L9 fix (`ef99c327`) moved `ui_lcd_lock_init()` ahead of the touch_cal branch, so the gates now exist. But the Config hub's nav cells are not gated one by one: they rely on the home Menu USER gate (`ui/ui_page_home_actions.c:433`) to keep unauthenticated users out. The touch_cal page enters Config behind that gate's back.

What becomes reachable with no PIN, from `ui/ui_page_config.c`:
- Temperature (`:70`), Network (`:76`), Diagnostics (`:101`), Profiles (`:118`) and Safety.
- Profiles, then the picker (`ui/ui_page_profile_picker.c:268`-`:269`), then the profile detail page. Its Start button (`ui/ui_page_profile_detail.c:504`, `start_btn_cb`, then `confirm_start_cb` at `:478`) has no `ui_lcd_lock_run_gated` call. It relies on the home Profiles USER gate (`ui/ui_page_home_actions.c:461`).
- The result is that an unauthenticated user can start a firing.
- Writes that carry their own ADMIN gate stay closed: units, recalibration, relay toggle, Wi-Fi, profile save and delete, and crash ack.

Scenario:
1. Take a resistive-panel board with LCD auth enabled and the calibration erased (a fresh board or NVS loss).
2. Boot. The panel shows touch_cal.
3. Tap Cancel. The panel shows Config.
4. Go Profiles, pick a profile, tap Start, confirm. The kiln fires with no PIN ever entered.

The bench FT6336U panel reports `self_calibrating`, so the bench cannot show this.

Fix, either one:
- Send every touch_cal exit (Cancel, refused save, and `touch_test`'s Done) to `home` whenever `ui_lcd_lock_has_role(LCD_PIN_ROLE_USER)` is false.
- Or give profile detail Start its own `LCD_PIN_ROLE_USER` gate, matching home Start, and gate the Config hub cells. This removes the reliance on "every path into Config passes Menu".

The first fix closes the hole; the second is defence in depth.

### N2 (MED) LCD Start skips the danger-mode refusal -- FIXED in cc62edf2f (autotune unchanged: its web route never refused in danger mode)

The web start handler (`http/dashboard_exec_http.c:804`) refuses with a 409 when `danger_mode_active()` is true. Its comment explains why:
- While danger mode is active, `relay_on_blocked()` skips every safety-fault and OTA gate on the four relays.
- The window can auto-expire and reboot mid-firing.

That check lives only in the HTTP handler. `control/profile_executor_run.c` checks recovery mode (`:257`) and readiness (`:263`) but not danger mode.

Callers that bypass the check:
- LCD home Start: `ui/ui_page_home_actions.c:101`-`:142` (`ui_home_do_start`).
- LCD profile detail Start: `ui/ui_page_profile_detail.c:488`.
- UART bridge start: `bridge/uart_bridge_ext_control.c:567`.

`safety/danger_mode.c` refuses to open the window during a firing, which covers the other direction only.

Scenario:
1. An admin opens danger mode on the web Diagnostics page.
2. Someone at the LCD with a USER PIN taps Start.
3. The firing runs with safety-fault gating suspended on the heater relays, and is cut by the window's expiry reboot.

Fix: move the `danger_mode_active()` refusal into `profile_executor_run()` beside the recovery and readiness checks. That way every entry point shares it, and the HTTP handler keeps its 409 by mapping the error. The same reasoning probably applies to `autotune_engine_run()`; it was not checked here.

### N3 (MED) Home rail zone temperatures ignore the unit preference -- FIXED in cc62edf2f

`ui/ui_page_home_refresh.c:1105`-`:1107` passes `ds->channels[i].temp_c` (Celsius) straight to `ui_page_home_rail_format_zone_temp()` (`ui/ui_page_home_rail.c:35`-`:45`). That function prints `"%.1f"` with no conversion and no suffix.

Every other temperature on the page goes through `unit_pref_convert(..., UNIT_PREF_KIND_ABSOLUTE)`:
- the idle chart dot at `:543`
- the planned and actual traces at `:738` and `:771`
- the axis ticks at `:582`

Scenario: with units set to F and the kiln at 1000 C, the chart axis reads around 1832 while the rail reads "1000.0". An operator comparing the two against a cone target in F misreads the kiln by hundreds of degrees.

Fix: convert with `unit_pref_convert(temp_c, ds->temp_unit, UNIT_PREF_KIND_ABSOLUTE)` before formatting, or pass the unit into the formatter. Add a unit case to `test/test_ui_page_home_rail.c`; today it cannot see this.

### N4 (LOW) Home rail shows stale temperatures as live -- FIXED in cc62edf2f

`ui/ui_page_home_refresh.c:1105` computes `valid = ... && ds->channels[i].valid` and ignores `.stale`. `dashboard_get_status()` sets `stale` when the reading is older than `KILN_TEMP_STALE_AGE_MS` (10 s), at `http/dashboard_http.c:248`.

Two places on the LCD already hide stale values:
- the Temperature page (`ui/ui_page_temperature.c:243`): `valid && !stale`
- the home chart (`ui/ui_page_home_refresh.c:542`)

Only the rail shows a frozen number as if it were live. Scenario: a thermocouple owner stalls, and the rail keeps showing the last good number with no marking.

Fix: pass `valid && !stale`, or render a stale marker.

### N5 (LOW, owner confirm) Trip strip opens Safety with no PIN -- FIXED in cc62edf2f (owner decision 2026-10-09)

`ui/ui_page_home.c:221`-`:227` calls `ui_page_safety_open(false)`, marked "ungated entry". The web `/safety` page is `ROUTE_TIER_ADMIN`.

What it exposes: trip reason, guard and age. The Clear action is still re-checked at ADMIN (`ui/ui_page_safety.c:210`, `:238`), so no write is exposed.

This conflicts with the letter of "dashboards only", though showing why the kiln stopped may be intended. It needs an owner decision; otherwise, gate the strip at USER.

### N6 (LOW) LCD read pages are USER where the web is ADMIN -- KEPT by owner decision 2026-10-09 (LCD read access stays at USER)

The Menu gate is `LCD_PIN_ROLE_USER` (`ui/ui_page_home_actions.c:433`). Behind it, the Diagnostics page shows crash and reset data, while `GET /api/crash_report` is ADMIN on the web. Temperature, Network and Profiles are likewise ADMIN pages on the web.

The writes on those pages are ADMIN-gated since L1/L2/L11. Only the reads differ. Recorded for parity, not as a hole.

### N7 (INFO) LVGL task stack ceiling unproven

The LVGL task stack is 10240 B (`ui/lvgl_port.c:1354`). It was sized from a 6896 B high-water mark at 215 s uptime, and the task is registered for stack-margin reporting.

`check_all_task_stack_budgets.py` reports `lvgl` INDETERMINATE: the call graph includes indirect LVGL event calls.

Paths that run on this task and probably were not exercised in that measurement:
- builder save (`profiles_http_save`, NVS plus cfg file)
- live decide
- `profile_executor_run()` (NVS breadcrumb)
- `ui_page_diagnostics` refresh, which calls `dashboard_get_status()` with a thermocouple read

Suggest a bench soak that drives those pages and then reads `get_stack_margin`.

### N8 (INFO) Builder pads stay in Celsius -- FIXED in cc62edf2f (pads follow unit, convert to Celsius on store)

`ui/ui_page_profile_builder_segment.c:96`-`:104` shows the target and ramp in the display unit. `target_card_cb` and `ramp_card_cb` (`:262`-`:296`) open pads captioned "Target C" and "Ramp C/hr" with Celsius bounds and values.

The code comments state this was deliberate to avoid a setpoint unit bug. No wrong value can be stored. The seam is visible: tap "1832 F", and the pad shows 1000. Recorded so it is not re-reported; a later pass could convert bounds, initial value and result together.

## Checked and clean

- **LVGL thread confinement.** All `lv_*` calls run on `lvgl_port_task`. Cross-task inputs go through `lvgl_port_inject_touch`, `lvgl_port_collect_tap_targets` and the atomic `s_force_lock_requested`. The relock edge runs on the LVGL timer, never on httpd. `kiln_ui_set_collect_hidden` from the UART task is already L23.
- **Reentrant invalidate in flush (51e1ef5 class).** The only invalidate calls in `lvgl_port.c` (`:909`-`:913`) run in the task loop before `lv_timer_handler()`, not in the flush callback. `kiln_ui_show()`'s `lv_refr_now()` (`ui/kiln_ui.c:932`) runs from event or timer context, not from flush.
- **Lock held across producers.** `ui/screen_idle.c:195`-`:243` takes the `dashboard_get_status()` and executor snapshot outside `idle->lock` and publishes it under the lock. The policy step under the lock does no producer call.
- **Clear trip.**
  - `clear_action` re-derives the verdict at ADMIN (`ui_safety_clear_verdict`) at click time and after the PIN.
  - It calls the same `dashboard_safety_clear_trip()` as the web route.
  - A stale link is never cleared blind.
- **Label formatting.**
  - No `sprintf`, `strcpy` or `strcat` in `drivers/ui`.
  - The `zones_buf`/`piece` loops (`ui/ui_page_profile_detail.c`, `ui/ui_page_home_actions.c`) bound each `memcpy` by the remaining capacity.
  - The scan-result copies are bounded by count.
  - `strncpy` in `ui_edit_firing_apply.c` is zero-initialised and bounded.
- **C/F elsewhere.** Every other temperature site uses `unit_pref_convert` with the right ABSOLUTE or RATE kind and a matching suffix. Rates carry "/hr" and no +32. The Diagnostics page mixes `unit_pref_get()` and `ds.temp_unit`. Both read the same preference, so they can only disagree within one refresh tick.
- **FT6336U.** The touch count is masked to 4 bits and accepted only for 1-2 points. Coordinates are masked to 12 bits. The 4-byte read buffer is zero-initialised.
- **Scrolling.** No new scrollable containers beyond L4/L5. The Config hub keeps its 3-row budget with or without the touch_cal cell.

## Host-test vacuity (negtest)

Run with `tools\negtest.ps1` against `build_host_tests.ps1 -Only 'test_ui_page_safety_logic|test_lcd_auth_state|test_ui_page_home_rail'`. The `-Only` regex was wrapped in a script. The baseline passed, and all 5 mutations were CAUGHT by the intended test file:

| Mutation | File | Caught by |
|----------|------|-----------|
| Clear verdict ignores `has_admin` | `ui/ui_page_safety_logic.c` | `test_ui_page_safety_logic.c:41` |
| Clear allowed when trip not live | `ui/ui_page_safety_logic.c` | `test_ui_page_safety_logic.c:49`, `:51`, `:55`, `:84` |
| Relock never closes an open keypad | `ui/lcd_auth_state.c` | `test_lcd_auth_state.c:286` |
| Lock timeout never expires | `ui/lcd_auth_state.c` | `test_lcd_auth_state.c:213`, `:214`, `:225`, `:250` |
| Rail renders NaN with valid flag | `ui/ui_page_home_rail.c` | `test_ui_page_home_rail.c:66` |

Coverage gaps that the host tests cannot see:
- The page-level gating (which buttons call `ui_lcd_lock_run_gated`, and where touch_cal exits go) lives in LVGL page code with no host test. N1 and N5 are of that kind.
- The rail formatter takes no unit, so N3 is invisible to `test_ui_page_home_rail.c`.
- No test covers `profile_executor_run()` against danger mode (N2).
