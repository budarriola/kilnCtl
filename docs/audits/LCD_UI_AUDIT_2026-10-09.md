# LCD UI audit (2026-10-09)

Scope: static, read-only review of the KilnFW LVGL LCD UI at origin/dev
(tip 1e6d375f at the time of reading), `firmware/KilnFW/App/drivers/ui/`.
No code was changed, no board was touched, no test was run. Paths below are
relative to `firmware/KilnFW/App/drivers/` unless stated otherwise.

Owner rules checked against:
- LCD pages must not scroll; content must fit the panel.
- Unauthenticated users see dashboards only. Clearing a safety trip from the
  LCD needs an admin login and follows the web clear path.

Context that shapes the findings:
- Pages are built once and cached for the life of the boot (`ui/kiln_ui.h`,
  `kiln_ui_page_build_fn` contract). Per-visit object leaks and screen-delete
  use-after-free are therefore structurally unlikely. Timers created in a
  page's build function run for the rest of the boot.
- The only LCD gates are at the two exits off home: Menu and Profiles
  (`ui/ui_page_home_actions.c:415`, `:443`), plus Start/Stop, Pause, Edit
  (`:310`, `:313`, `:355`, `:385`). All use `LCD_PIN_ROLE_USER`. The only
  `LCD_PIN_ROLE_ADMIN` gate in the UI tree is Clear Trip
  (`ui/ui_page_safety.c:238`).

## Summary

| ID | Sev | Area | Finding |
|----|-----|------|---------|
| L1 | MED | auth | Wi-Fi writes (add network, forget, mode switch, AP SSID/password) run at USER PIN; web equivalents are ADMIN. |
| L2 | MED | auth | Profile save/overwrite (builder) and live-edit decide (save as / overwrite) run at USER PIN; web equivalents are ADMIN. |
| L3 | MED | auth | Manual relay toggle on the Temperature page runs at USER PIN with no danger-mode step; the web paths that call the same writer are ADMIN (and danger-mode gated). |
| L4 | MED | scroll | Network Manage scan and saved lists are 70 px `lv_list`s holding up to 20 / 8 rows of 72 px touch targets, so they scroll. |
| L5 | LOW | scroll | Temperature page relay row is a deliberately scrollable 40 px box; 4 buttons of 144 px wrap to 2 rows, so it always scrolls. |
| L6 | LOW | info | The AP password is displayed in clear on the Network page behind a USER PIN. |
| L7 | LOW | auth | With the LCD policy off, every gate collapses to full access, including Clear Trip. Matches the web auth-off rule; recorded so the owner can confirm it is intended for the trip clear. |
| L8 | INFO | timers | Page refresh timers are created once at build and never deleted; they keep running while the page is hidden. |

No finding was rated HIGH. Clear Trip itself is correct: ADMIN gate, role
re-checked inside the action, and the same `dashboard_safety_clear_trip()`
the web route `POST /api/safety/clear_trip` calls (`http/dashboard_exec_http.c:875`, `:885`).

## Findings

### L1 (MED) Wi-Fi writes at USER role

- `ui/ui_page_network_manage.c:188` `wifi_prov_add_network()` (connect modal)
- `ui/ui_page_network_manage.c:246` `wifi_prov_forget_network()` (forget worker, started from the confirm at `:363`-`:382`)
- `ui/ui_page_network.c:251` `wifi_prov_set_mode()`
- `ui/ui_page_network.c:314`, `:319` `wifi_prov_set_ap_ssid()` / `wifi_prov_set_ap_password()`

None of these callbacks checks a role; they inherit only the USER gate on
the home Menu button. The web counterparts `/provision` and `/forget` are
`ROUTE_TIER_ADMIN` (`http/route_tier_table.h:424`, `:425`).

Scenario: an operator with a user PIN opens Menu, Network, Manage, forgets
every saved network or switches the board to AP mode. The board drops off the
LAN. The same person could not do this from the web.

Fix: wrap each write in `ui_lcd_lock_run_gated(..., LCD_PIN_ROLE_ADMIN, ...)`,
or gate the Network nav cell in `ui/ui_page_config.c` at ADMIN. Mirror the
route tier table.

### L2 (MED) Profile writes at USER role

- `ui/ui_page_profile_builder_review.c:90` `profiles_http_save()` from
  `slot_clicked_cb` (`:133`) and the overwrite confirm (`:127`)
- `ui/ui_page_live_decide.c:125`-`:126` `profiles_live_decide_apply()`
  (discard / save as / overwrite), reached through the USER gate at
  `ui/ui_page_edit_firing.c:387`
- Live edit entry is gated USER at `ui/ui_page_home_actions.c:385`

Web: `POST /api/profile`, `/api/profile/delete`, `/api/profile/live/fork`,
`/api/profile/live`, `/api/profile/live/decide` are all `ROUTE_TIER_ADMIN`
(`http/route_tier_table.h:265`-`:266`, `:468`-`:470`).

Scenario: a user-PIN holder overwrites a stored schedule in a slot, or
overwrites the origin profile with a mid-firing edit.

Fix: ADMIN gate on save/overwrite/decide, or record an owner decision that a
USER may edit profiles on the LCD and align the web tier.

### L3 (MED) Manual relay toggle at USER role

`ui/ui_page_temperature.c` `relay_toggle_cb()` calls `dashboard_set_relay()`
for any non-zone relay. The two web callers of the same writer are
`POST /api/aux_outputs/manual` and `POST /api/diagnostics/danger/relay`, both
`ROUTE_TIER_ADMIN` (`http/route_tier_table.h:249`, `:341`); the danger route
also refuses with 409 unless danger mode is active
(`http/diagnostics_http.c` around `:1244`-`:1288`). The LCD has neither step.
The owner-side refusals (running profile, safety fault, update, crash unack)
still apply, because they live in `kiln_io_owner_command_set_relay()`.

Scenario: a user-PIN holder energises a spare (aux) relay while idle.

Fix: ADMIN gate on `relay_toggle_cb`, or make the LCD relay controls
view-only like the zone relays.

### L4 (MED) Network Manage lists scroll

`ui/ui_page_network_manage.c:46` sets `UI_PAGE_NETWORK_MANAGE_LIST_HEIGHT_PX`
to 70. The scan list (`:708`-`:710`) and saved list (`:716`-`:718`) are
`lv_list_create()` objects, which are scrollable by default, and the flag is
never cleared on either. The scan list takes up to
`UI_PAGE_NETWORK_MANAGE_SCAN_MAX` (20, `:27`) rows (`:151`-`:160`), each
widened to the 72 px touch target by `ui_theme_apply_touch_area()`; the saved
list takes up to 8 rows (`:28`, `:482`-`:510`). Any list of 2+ rows scrolls.
The `_Static_assert` at `:56` only proves the 70 px box fits the page, not its
content.

Scenario: a scan in a building with several APs returns 6+ results; the
operator has to drag-scroll a 70 px window to reach most of them.

Fix: page the lists (fixed rows per page with prev/next, the pattern
`ui/ui_page_profile_picker.c` and `ui/ui_page_profile_segments.c` already use)
and clear `LV_OBJ_FLAG_SCROLLABLE` on both lists.

### L5 (LOW) Temperature relay row scrolls by design

`ui/ui_page_temperature.c:114`-`:121` documents the relay row as a
"sanctioned internally-scrollable" box, and `:491`-`:497` sets a 40 px height
with `lv_obj_set_scroll_dir(..., LV_DIR_VER)`. Buttons are
`UI_THEME_MIN_TOUCH_TARGET_PX * 2` = 144 px wide and 36 px tall (`:506`-`:507`;
72 px from `ui/ui_theme.h:144`). With `KILN_IO_RELAY_COUNT` = 4
(`owners/kiln_io.h:65`) the row wraps to two lines (about 80 px) inside 40 px,
so it always scrolls. The "sanctioned" exception predates the owner's
no-scroll rule.

Fix: narrower buttons (4 x about 110 px fits one line), or a fixed 2x2 grid
budgeted in the page's `_Static_assert`.

### L6 (LOW) AP password shown in clear

`ui/ui_page_network.c:495`-`:496` reads `wifi_prov_get_ap_ssid()` /
`wifi_prov_get_ap_password()` for display (and the edit modal pre-fills the
password at `:648`). Behind a USER PIN only. Web exposes the AP identity on an
ADMIN page.

Fix: mask the password unless the session has ADMIN, or move the page behind
ADMIN with L1.

### L7 (LOW) LCD policy off collapses Clear Trip to no login

`ui/ui_page_safety.c:210` passes `ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)`,
which returns true whenever the LCD policy is disabled (`ui/ui_lcd_lock.h`,
`ui_lcd_lock_has_role` comment). On a board with LCD auth off, anyone at the
panel can clear a trip after opening the Safety page. This matches the web
"open when login off" rule, so it is not a defect by the letter of the code,
but the owner decision of 2026-10-07 ("LCD clear needs admin login") does not
say whether it survives auth-off.

Fix: owner decision. If the clear must always need a credential, refuse it
when no admin PIN is set and show "Set an admin PIN to clear from the LCD".

### L8 (INFO) Refresh timers outlive page visibility

Example: `ui/ui_page_network_manage.c:726` creates `refresh_cb` once at build
and never deletes it. Because screens are cached, this is not a leak and the
widgets the timer touches stay valid. Cost is CPU on the LVGL task for hidden
pages. Fix if it matters: pause/resume the timer on screen load/unload events.

## Checked and found clean

- Clear Trip: ADMIN gate, role re-checked inside the action, same backend
  call as the web route, no trip latch held on the page.
- String copies of board data in the files read: SSIDs use `%.*s` with
  `WIFI_PROV_SSID_MAX_LEN` (`ui/ui_page_network_manage.c:153`) or `snprintf`
  into sized buffers (`:400`, `:483`, `:486`, 48-byte text fits 32 + 13);
  the overwrite prompt bounds the profile name with `snprintf`
  (`ui/ui_page_profile_builder_review.c:144`). No unbounded
  `strcpy`/`sprintf`/`strcat` found; worst case is silent truncation.
- Wi-Fi scan, connect and forget run on worker tasks and hand results back
  through mutex-guarded job structs polled from the LVGL timer
  (`ui/ui_page_network_manage.c:240`-`:262`); the worker tasks make no
  `lv_*` call.
- Profile picker and segments pages page their lists with a fixed rows-per-page
  and clear `LV_OBJ_FLAG_SCROLLABLE` (`ui/ui_page_profile_picker.c:408`, `:489`;
  `ui/ui_page_profile_segments.c:29`, `:187`). The picker resolves a row's
  profile id at tap time from the current page, not a captured id
  (`ui/ui_page_profile_picker.c:79`).
- Cross-task touch injection and tap-target walks go through
  `lvgl_port_inject_touch()` / `lvgl_port_collect_tap_targets()` handoffs that
  run the `lv_*` work on lvgl_port_task (`ui/lvgl_port.h`).

## Not covered

This pass did not complete a line-by-line review of `ui/lvgl_port.c`,
`ui/kiln_ui.c`, `ui/screen_idle.c`, the home page family
(`ui/ui_page_home*.c`) or `ui/ui_page_diagnostics.c` for the leak,
stale-pointer and cross-task criteria. Those files carry `_Static_assert`
no-scroll budgets (`ui/ui_page_home.c:1151`, `ui/ui_page_diagnostics.c:331`-`:369`)
but data-dependent content in them was not size-checked here. A follow-up
pass should cover them.
