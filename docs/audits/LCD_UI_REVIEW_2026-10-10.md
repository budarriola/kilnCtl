# LCD UI review, 2026-10-10

Scope: `firmware/KilnFW/App/drivers/ui/` at origin/dev `d93cf774a`, focused on
changes since `docs/audits/LCD_UI_REVIEW_2026-10-09.md` (`db1f4df5`). Commits in
range touching ui/: `cc62edf2f` (N1-N5, N8 fixes), `3de432469` (builder pads,
touch_test without a role), `7cc343260` (touch_test exception keyed on the
calibration-saved flow), `21b163662` (builder segment seams), `ea86f4d43`
(`READINESS_GATE_MSG_CAP` err_msg buffers), `2cd02753c` (network_manage Wi-Fi
hint), `5fa28ed3b` (comments only). Read-only review; no firmware edited.

Owner rules checked against:

- Unauthenticated: dashboards only (2026-09-28).
- Safety page from the trip strip needs the USER PIN; Clear needs ADMIN and
  follows the web clear (2026-10-07, 2026-10-09).
- Read pages (Diagnostics, Temperature, Network, Profiles) stay USER (N6, kept).
- touch_test after a calibration save is a documented no-role exception
  (2026-10-10).
- System-mode gate: relay/zone writes refused while running (409).

## Summary

| ID | Severity | Area | Short |
|----|----------|------|-------|
| L1 | LOW (FIXED, lcdfx commit) | relock | `ui_num_pad` survives the relock edge, stays over the home dashboard |
| L2 | LOW (FIXED, lcdfx commit) | live edit | generation check runs before fork and flash reads, not just before the save; post-save re-read adopts a foreign generation |
| L3 | LOW (FIXED, lcdfx commit) | role gating | Config hub cells still have no per-cell gate (N1 defence in depth not done) |
| I1 | INFO | live edit | no `cfg_fs` mounted pre-check (web answers 503); fails cleanly inside `live_profile` |
| I2 | INFO | auth reset | gesture confirm does not re-check E-stop / firing / heat at confirm time |
| I3 | INFO | stack | N7 still unproven; two more callbacks put `READINESS_GATE_MSG_CAP` buffers on the LVGL stack |

No HIGH or MED findings. No write action reachable without its role.

## Findings

### L1 (LOW, FIXED) -- number pad not closed on relock

Fixed: `ui_num_pad_close()` (no Done callback) called on the relock edge; host test in `test_ui_lcd_lock.c`.

`ui_num_pad.c:72` parents the pad to `lv_layer_top()`; the public API is only
`ui_num_pad_show()` (`ui_num_pad.c:138`), and `close_modal()` (`:24`) is
static. The relock edge in `ui_lcd_lock.c:263-287` closes the stay-unlocked
prompt, the PIN keypad and `ui_confirm`, then runs `s_relock_cb`
(`kiln_ui.c:139`, show "home"). It never closes the number pad.

Scenario: an ADMIN opens the profile builder, taps a segment field
(`ui_page_profile_builder_segment.c:268-316`) or a zone field
(`ui_page_profile_builder_zones.c:119-127`), and walks away. The session times
out (or the web changes the PIN policy and calls `ui_lcd_lock_force_lock()`).
Home is shown underneath, but the full-width (440 px) pad stays on the top
layer, hiding the dashboard from an unauthenticated viewer. Tapping Done writes
the value into builder RAM state for a page that is no longer shown. Nothing
persists: the builder save is ADMIN-gated (`ui_page_profile_builder_review.c:141`).
Impact is dashboard obscured plus a stale RAM edit, hence LOW.

Fix direction: add `ui_num_pad_close()` (or `ui_num_pad_force_close()`) and
call it on the relock edge beside `ui_confirm_close_open()`.

### L2 (LOW, FIXED) -- LCD live-edit generation check window

Fixed upstream in `062456379` (Web4 review A1): `live_profile_save_working_if_gen()` compares and writes under live_profile's save lock; LCD apply and `profiles_live_http.c` both use it and the LCD adopts the generation its own save produced.

`ui_edit_firing_apply.c:168` compares `live_profile_generation()` against the
generation captured when the page opened (`:140`), then validates (`:179`),
reads the origin record from flash, may auto-fork (`:232`), and only then saves
(`:249`). After the save, `:255` re-reads the generation unconditionally.

Scenario: the web `PUT /api/profiles/live` edit lands while the LCD is between
`:168` and `:249` (the fork and flash reads make this window tens of
milliseconds). The LCD save overwrites the web edit without a conflict, and
`:255` adopts whatever generation now stands, so the next LCD apply also sees
no conflict. The web edit path (`profiles_live_http.c:404-520`) checks `gen=`
immediately before its write, so its window is narrower; neither side holds a
lock across check and write. Lost update of one live edit, no safety effect
(bounds and window checks still run on the LCD candidate), hence LOW.

Fix direction: a compare-and-save in `live_profile` (save takes the expected
generation and refuses under its own lock), used by both callers.

### L3 (LOW, FIXED) -- Config hub cells rely on the Menu gate

Fixed: each of the five cells runs through `ui_lcd_lock_run_gated(..., LCD_PIN_ROLE_USER, ...)` in `ui_page_config.c`. No host test (LVGL page, not host-compilable).

`ui_page_config.c` cells for Temperature (`:70`), Network (`:76`),
Diagnostics (`:101`), Safety (`:107`) and Profiles (`:118`) navigate without
their own role check. Today every entry into the hub is gated: Menu is USER
(`ui_page_home_actions.c:434`), touch_cal exits go home without USER
(`lcd_auth_state.c`, `lcd_touch_cal_exit_target()`), and Back from sub-pages
is only reachable from inside the hub. Profile detail Start now has its own
USER gate (`ui_page_profile_detail.c:553-559`), which is the pattern N1 asked
for.

Scenario: a future entry point to "config" (a new shortcut, a test bridge
`kiln_ui_show("config")`, a refactor of the touch_cal exit) reopens N1:
an unauthenticated user reaches the read pages. Not reachable today, hence LOW.

### I1 (INFO) -- no cfg_fs pre-check on LCD live edit

The web edit calls `cfg_fs_http_refuse_if_unmounted()` first (503). The LCD
path does not; `live_profile_save_working()` fails with its unmounted error
(`fill_unmounted_err`) after validation and a possible fork attempt. The user
sees a clear error either way. No action needed beyond consistency.

### I2 (INFO) -- auth-reset gesture confirm has no re-check

`ui_home_auth_reset_corner_tap_cb` (`ui_page_home_actions.c:584-639`) checks
E-stop asserted, no firing and heat not granted (`:597-609`) when arming. The
confirm callback (`:538-575`) calls `auth_reset_gesture_confirm()` without
re-checking them. A firing cannot start without USER, and the 30 s window is
short, so the preconditions are unlikely to change in between. Listed so a
future change to the gesture keeps the window short.

### I3 (INFO) -- LVGL stack ceiling still unproven (N7)

LVGL task stack is still 10240 B with no measured worst case. Since the last
review, `ea86f4d43` sized two err_msg buffers at `READINESS_GATE_MSG_CAP` on
the LVGL stack (the profile detail Start and home Fire callbacks), on top of
the existing paths. `profile_exec_status_t` (1512 B) is heap-allocated on these
paths, which is correct. Recommend a stack-margin reading after a bench run
that exercises Start from profile detail and the builder pads.

## Checked and clean

- Trip strip opens Safety only with USER (`ui_page_home.c:223-240`,
  `lcd_safety_strip_needs_pin()`).
- Safety Clear is ADMIN and re-checked at confirm (`ui_page_safety.c:210/238`,
  `ui_safety_clear_verdict`), and calls the same `dashboard_safety_clear_trip()`
  as the web route (`dashboard_exec_http.c:891-918`, ADMIN at
  `route_tier_table.h:294`).
- touch_test exception: only a successful save goes to touch_test
  (`ui_page_touch_cal.c:196`, `lcd_touch_cal_saved_exit_target()`); refused save
  and Cancel use `lcd_touch_cal_exit_target()` (home without USER,
  `:181-183`, `:245`). touch_test has Home only, no Back (`ui_page_touch_test.c:140`,
  `:153-157`). Recalibrate from Config is ADMIN unless never calibrated
  (`ui_page_config.c:88-95`).
- Every write action is gated: Units (ADMIN, `ui_page_config.c:145/165`), crash
  ack (ADMIN, `ui_page_diagnostics.c:1536-1545`), edit firing apply/decide
  (ADMIN, re-checked, `ui_page_edit_firing.c:389-400`), live decide (ADMIN, `ui_page_live_decide.c:137`), network mode / AP
  edit / show secret (ADMIN, `ui_page_network.c:385/392/547/623/719`),
  `ui_page_network_manage.c` add / forget (ADMIN, `:406/418/461`), builder save (ADMIN, `ui_page_profile_builder_review.c:141`),
  picker delete (ADMIN, `ui_page_profile_picker.c:236-245`), relay toggle (ADMIN,
  `ui_page_temperature.c:439`). Start/Stop/Pause/Menu/picker are USER
  (`ui_page_home_actions.c:312-335, 374, 434, 462`); Edit/Keep is ADMIN (`:404`).
- Mode gate: the relay toggle goes through `dashboard_set_relay()`, which
  returns `DASHBOARD_RELAY_ERR_RUNNING` while running. Live edit is a profile
  edit (ZONE_RAMP target/ramp/dwell on non-finished segments only), not a
  relay/zone write, so the mode gate does not apply; it matches the web live edit
  on validation (`profiles_validate_candidate` HARD) and the window check
  against the origin record. Danger mode, recovery and readiness are enforced
  in `profile_executor_run()` (`control/profile_executor_run.c:247-268`), which
  both LCD Start paths call.
- LVGL confinement: external callers into ui are getters
  (`kiln_ui_current_page`, flush/touch stats), an atomic request
  (`ui_lcd_lock_force_lock`, `ui_lcd_lock.c:349`), or dispatched onto the LVGL
  task (`lvgl_port_collect_tap_targets()`, used by `kiln_ui_click_by_name()` and
  the UI-test bridge). The relock edge runs on the LVGL timer. `screen_idle`
  still takes its snapshot outside the lock.
- Rail value formatting (`ui_page_home_rail.c`) fits its 16 B buffer; stale
  readings handled.
- Fit: no new scrolling containers in range; builder pads and the network_manage
  hint stay inside 480x320.

## Status of 2026-10-09 findings

| ID | Status |
|----|--------|
| N1 | Fixed for touch_cal exit (`cc62edf2f`, `7cc343260`); per-cell defence in depth fixed as L3 |
| N2-N5, N8 | Fixed in `cc62edf2f`, verified |
| N6 | Kept by owner (LCD reads stay USER) |
| N7 | Open, INFO (I3) |
