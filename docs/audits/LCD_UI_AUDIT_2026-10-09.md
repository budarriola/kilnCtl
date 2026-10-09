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
| L1 | MED | auth | Wi-Fi writes (add network, forget, mode switch, AP SSID/password) run at USER PIN; web equivalents are ADMIN. **FIXED in ef99c327.** |
| L2 | MED | auth | Profile save/overwrite (builder) and live-edit decide (save as / overwrite) run at USER PIN; web equivalents are ADMIN. **FIXED in ef99c327.** |
| L3 | MED | auth | Manual relay toggle on the Temperature page runs at USER PIN with no danger-mode step; the web paths that call the same writer are ADMIN (and danger-mode gated). **FIXED in ef99c327.** |
| L4 | MED | scroll | Network Manage scan and saved lists are 70 px `lv_list`s holding up to 20 / 8 rows of 72 px touch targets, so they scroll. **FIXED in ef99c327.** |
| L5 | LOW | scroll | Temperature page relay row is a deliberately scrollable 40 px box; 4 buttons of 144 px wrap to 2 rows, so it always scrolls. **FIXED in ef99c327.** |
| L6 | LOW | info | The AP password is displayed in clear on the Network page behind a USER PIN. **FIXED in ef99c327.** |
| L7 | LOW | auth | With the LCD policy off, every gate collapses to full access, including Clear Trip. Matches the web auth-off rule; recorded so the owner can confirm it is intended for the trip clear. |
| L8 | INFO | timers | Page refresh timers are created once at build and never deleted; they keep running while the page is hidden. |
| L9 | HIGH | auth | First boot with no stored touch calibration returns from `kiln_ui_init()` before the LCD lock is initialised, so every gate (Clear Trip included) is open until reboot. **FIXED in ef99c327.** |
| L10 | MED | auth | The Network Manage Forget dialog is a raw `lv_msgbox`, not `ui_confirm`, so it survives an LCD relock and stays tappable. **FIXED in ef99c327.** |
| L11 | MED | auth | More USER-gated writes whose web equivalents are ADMIN: unit preference, touch calibration save, profile delete, live-edit apply, AP QR code, crash-report acknowledge. **FIXED in ef99c327.** |
| L12 | MED | touch | Topbar touch-group registry holds 4 groups and drops the rest silently; 21 topbar call sites, so pages visited after the 4th lose nearest-center arbitration. |
| L13 | MED | layout | Builder slot grid creates 100 slot cells in a 2-row box with scrolling removed; only 8 are reachable, and each build makes 100 profile reads. |
| L14 | MED | churn | Network Manage saved list is cleaned and rebuilt every second, even when hidden. **FIXED in ef99c327.** |
| L15 | MED | fit | Home right-hand rail height assert ignores the trip strip, lag notice and progress bar; with all visible, zone 3 and watts are clipped. |
| L16 | LOW | strings | AP QR payload buffer can truncate a max-length SSID + password; no escaping; always `T:WPA`. **FIXED in ef99c327.** |
| L17 | LOW | race | A second AP Save overwrites the job strings before the busy check. **FIXED in ef99c327.** |
| L18 | LOW | dialogs | `ui_confirm` does not NULL-check its `lv_malloc`'d context and frees it only in the button callbacks, so a dialog closed by relock leaks it. |
| L19 | LOW | keypad | `ui_lcd_keypad_show()` overwrites the pending callback if called while open; single global gate context. Reachability unconfirmed. |
| L20 | LOW | fit | Long-text labels with no fixed height wrap instead of truncating (home strips, topbar status, rail zone names); Diagnostics Trip detail rows have no budget assert. |
| L21 | LOW | cost | Home and Diagnostics refresh callbacks keep calling `dashboard_get_status()` (and a thermocouple read-all) while hidden. |
| L22 | LOW | misc | Start confirm re-resolves the profile at Confirm time, not the one shown; picker id cache can go stale (mislabel only); live-decide Save As has no confirm. **FIXED in ef99c327.** |
| L23 | INFO | threads | Debug flags and 64-bit flush stats shared across tasks without atomics; startup `lv_*` calls on app_main are an undocumented exception. |
| L24 | MED | chart | Home chart looks up history by `t / 30 s` as a ring index; once the 640-sample ring wraps (5 h 20 min) the actual trace is time-shifted and then flat. |
| L25 | LOW | chart | The dashed planned-line hook never runs: `LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS` is never set, so the plan draws solid (home and profile detail). |
| L26 | LOW | chart | The time axis ends at the plan's nominal horizon; a run that outlasts its plan stops drawing the actual trace and pins the now-dot to the last bucket. |
| L27 | LOW | cost | Each home tick makes up to 31 `s_exec.lock` acquisitions (`portMAX_DELAY`) plus a plan-curve rebuild on the LVGL task, also while hidden. |
| L28 | LOW | overflow | A valid but tiny ramp rate gives a horizon above 2^31 s; `lroundf()` into a 32-bit `long` then overflows in the tick labels and history index. |
| L29 | INFO | geometry | Tick and legend positions add the chart's content offset twice (2 px); the top Y label sits 3 px above the content box. |
| L30 | INFO | rail | Rail zone name/temp labels are rewritten every tick; zone names are read without the zones lock; aux caption table hard-codes 4 relays. |

L9 (second pass, below) is the only HIGH finding: it opens every gate, Clear Trip included, on one boot path. Otherwise Clear Trip itself is correct: ADMIN gate, role
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

Example: `ui/ui_page_network_manage.c:727` creates `refresh_cb` once at build
and never deletes it. Because screens are cached, this is not a leak and the
widgets the timer touches stay valid. Cost is CPU on the LVGL task for hidden
pages. Fix if it matters: pause/resume the timer on screen load/unload events.

## Second pass (core files, home, diagnostics, follow-up verification)

### L9 (HIGH) LCD lock never initialised on the touch-calibration boot path

`ui/kiln_ui.c:337`-`:340`: when touch calibration is offerable and no
calibration is stored, `kiln_ui_init()` returns `kiln_ui_show("touch_cal")`
before `ui_lcd_lock_init()`, `ui_lcd_lock_set_relock_cb()` and
`lcd_credential_bridge_init()` at `:354`-`:356`. Those are their only call
sites. With no policy function wired, the default policy reports disabled
and `ui_lcd_lock_has_role()` returns true for every role, ADMIN included.
No relock tick runs either.

Scenario: a board on the resistive NS2009 path with an erased calibration
(fresh board, NVS loss) and LCD lock enabled. The user calibrates, lands on
Config, and every gated action is open until the next reboot, including
Clear Trip (`ui/ui_page_safety.c:210`, `:238`) and all home actions. The bench
FT6336U panel is not offered calibration, so the bench does not show it.

Fix: move the three init calls above the touch-cal branch. Add a host test
that asserts the lock policy is wired after init with an uncalibrated store.

### L10 (MED) Forget dialog survives relock

`ui/ui_page_network_manage.c:408`-`:419` builds the Forget prompt with
`lv_msgbox_create(NULL)` directly. The relock path closes only `ui_confirm`
dialogs and the keypad (`ui/ui_lcd_lock.c:280`, `ui_confirm_close_open()`).

Scenario: a user opens Forget and walks away; the lock times out and the UI
goes home, but the top-layer msgbox stays up. Anyone can then tap Forget and
drop a saved network without a PIN.

Fix: build it with `ui_confirm`, or close it from the relock callback.

### L11 (MED) More USER-gated writes

Same class as L1-L3. Web equivalents are ADMIN.
- Unit preference toggle: `ui/ui_page_config.c:127`.
- Touch calibration save: `ui/ui_page_config.c:77`, `ui/ui_page_touch_cal.c:177`.
- Profile delete: `ui/ui_page_profile_picker.c:204`.
- Live-edit apply: `ui/ui_edit_firing_apply.c:232`, `:249`.
- AP QR code (shows the AP credential, see L6): `ui/ui_page_network.c:498`-`:504`.
- Crash-report Acknowledge: `crash_ack_btn_clicked_cb` in
  `ui/ui_page_diagnostics.c` has a tap-twice confirm but no role gate; the
  page is reached through the USER-gated hub (`ui/ui_page_config.c:86`).
  `/api/crash_report/ack` is ADMIN (`http/route_tier_table.h:382`), and the
  on-screen text says acknowledging allows manual relay-ON. The LCD calls
  `crash_report_acknowledge_timeout()`, the web `crash_report_acknowledge()`
  (`http/diagnostics_http.c:493`); confirm the former is a thin bounded-wait wrapper.

Fix: ADMIN role for each, with a role re-check inside the action as
`ui/ui_page_safety.c:210` does.

### L12 (MED) Topbar touch groups capped at 4, overflow silent

`ui/ui_theme.h:394` sets `UI_THEME_TOUCH_GROUP_MAX_GROUPS` to 4;
`ui/ui_theme.c:96` returns silently when full. Each topbar with two or more
icons registers a group (`ui/ui_topbar.c:277`-`:287`), and there are 21
`ui_topbar_create()` call sites outside `ui_topbar.c`. Pages are built lazily,
so only the first four pages visited in a boot get nearest-center
arbitration. Later pages fall back to z-order, which is the "Back shadowed by
later icons" bug the comment at `ui/ui_topbar.c:262`-`:276` says the group
fixes. Whether a given page's Back fails depends on visit order; not
reproduced on hardware.

Fix: raise the cap to cover every topbar (or give the topbar its own
arbitration), log on overflow, and add a check counting call sites against
the cap.

### L13 (MED) Builder slot grid: 100 cells, 8 reachable

`ui/ui_page_profile_builder_review.c:165` loops to `PROFILES_MAX_COUNT` (100,
`persist/profiles_types.h:30`), creating one cell per slot inside a grid
whose height is two touch rows (`:60`, `:227`) with scrolling removed
(`:233`). Only the first 8 cells are reachable, so the LCD can overwrite only
slots 0-7 once they are full. Each build also makes 100 profile reads. The
comment at `ui/ui_page_profile_detail.c:468`-`:473` describing this grid is stale.

Fix: page the grid like the picker, and read slot state once per page.

### L14 (MED) Saved-network list rebuilt every second

`refresh_cb` (`ui/ui_page_network_manage.c:517`, `:540`) calls
`refresh_saved_list()`, which runs `lv_obj_clean()` and rebuilds every row
(`:457`-`:512`) once per second, visible or not. It resets scroll position
(L4 makes that list scroll), can delete a button mid-press, and churns the heap.

Fix: rebuild only when the saved set changes, and skip while hidden.

### L15 (MED) Home rail can be clipped when status strips show

`ui/ui_page_home.c` (rail `_Static_assert` near `:1120`, strips near
`:725`-`:760`): the rail budget (208 px needed, 228 px allowed) assumes the
graph row gets all content height except the action row. It ignores the trip
strip, the lag notice and the progress wrap, which can all show at once (the
progress bar stays visible on FAULTED, `ui/ui_page_home_refresh.c:157`-`:165`).

Scenario: a safety trip faults the executor while a "Config mismatch (Pico)"
notice stands. The strips and progress take roughly 80-110 px, leaving the
graph row about 130-160 px against 208 px. The rail does not scroll, so the
zone 3 row and the watts label are silently cut off. Estimate from source,
not measured on the panel.

Fix: pin each strip to one line, assert the rail against the budget minus
both strips, the progress wrap and the action row, or hide the progress wrap
while the trip strip shows.

The Temperature relay row (L5) is the same class: with 3 buttons per row,
Relay 4 sits entirely below the 40 px box, and the page budget assert at
`ui/ui_page_temperature.c:147`-`:150` counts only 40 px for that row. The
comment at `:116`-`:120` is wrong for the current relay count.

### L16 (LOW) AP QR payload truncation and escaping

`ui/ui_page_network.c:500` formats the Wi-Fi QR string into
`sizeof(s_ap_qr_last)`. A 32-byte SSID plus a 63-byte password plus framing
can exceed it, giving a QR that joins nothing. `;`, `,`, `:` and `\` are not
escaped, and the type is always `T:WPA`, also for an open AP.

Fix: size for the worst case plus escapes, escape per the Wi-Fi QR format,
emit `T:nopass` for an open AP.

### L17 (LOW) AP Save overwrites job strings before the busy check

`ui/ui_page_network.c:622`-`:623` copies the new SSID/password into the job
struct before `:575`'s busy check refuses a second request, so a double tap
can change what the in-flight worker writes.

Fix: check busy first, copy under the job mutex.

### L18 (LOW) ui_confirm context handling

`ui/ui_confirm.c:98` does not NULL-check `lv_malloc()`, and the context is
freed only in the yes/cancel callbacks (`:20`, `:65`), not in the
`LV_EVENT_DELETE` handler registered at `:102`. A dialog closed by relock
(`ui_confirm_close_open()`) leaks the context.

Fix: NULL-check and bail; free only in the delete handler.

### L19 (LOW) Keypad callback overwrite

`ui/ui_lcd_keypad.c:286`-`:296`: `ui_lcd_keypad_show()` overwrites the pending
callback and user data while the keypad is open, and `ui/ui_lcd_lock.c:358`
holds one global gate context. A second gated request before the first keypad
closes drops the first action silently. The backdrop is modal, so this likely
needs a programmatic caller; not confirmed reachable.

Fix: finish the previous request as cancelled, or refuse the second.

### L20 (LOW) Labels wrap instead of truncating; Trip detail has no budget

In LVGL v9, LONG_DOT/LONG_CLIP truncate only with a fixed height
(`ui/ui_page_safety.c:108` says so). The home trip strip and lag notice
(`ui/ui_page_home.c` near `:725`-`:760`; messages up to "Config mismatch
(Pico): %.130s"), the topbar status label, the rail zone-name labels and the
profile button label have no fixed height and wrap. The Diagnostics Trip
detail sub-page uses LONG_WRAP rows (latch text about 170 characters,
"Detected:" up to 340 bytes, fault source up to 192) with no `_Static_assert`;
all five filled likely exceed the 268 px content height and the bottom rows clip.

Fix: fixed one-line heights on those labels; pin line counts on Trip detail
as `ui/ui_page_safety.c:277`/`:279` does, and shorten the latch text.

### L21 (LOW) Hidden pages keep doing heavy reads

`ui_home_refresh_cb` (`ui/ui_page_home_refresh.c` near `:124`) and the
Diagnostics `refresh_cb` have no "page not active" early return, so they call
`dashboard_get_status()` (SPI reads, a queue wait, interrupts-disabled heap
walks) every tick while hidden; Diagnostics also runs
`thermo_owner_command_read_all()` every 2 s. The Safety page already guards
this (`ui/ui_page_safety.c:131`).

Fix: return early when the page is not active; keep a cheap path for anything
that must stay live.

### L22 (LOW) Smaller correctness items

- Start confirm: `ui_home_do_start` (`ui/ui_page_home_actions.c` near
  `:95`-`:115`) re-resolves the selected profile at Confirm time, not the id
  shown in the dialog body (`:196`-`:250`). A selection change while the
  dialog is open (from the web) starts a different profile. Fix: capture the
  id at dialog build and pass it as user data.
- `ui/ui_page_profile_picker.c:232` caches ids that `:199` can use after the
  list changes; the tap resolves the id fresh, so the effect is a mislabel.
- `ui/ui_page_live_decide.c:196`-`:205` runs Save As immediately with no
  confirm, unlike Overwrite.

### L23 (INFO) Cross-task diagnostics state and startup exception

- `s_auto_tap_dump` (`ui/kiln_ui.c:114`) is written from the UART task and
  read on the LVGL task without an atomic. Debug only.
- The 64-bit flush timing sum in `ui/lvgl_port.c` is written by the SPI
  completion path and read by HTTP handlers; a torn read skews a diagnostic mean.
- The tap-walk hidden-pass flag (`ui/kiln_ui.c:1029`, `:1032`) can be reset
  before a timed-out walk runs. Test path only.
- `lv_init()` and `kiln_ui_init()` run on app_main before lvgl_port_task
  exists. Safe, but an undocumented exception to "only lvgl_port_task calls lv_*".
- The auth-reset corner gesture (`ui/ui_page_home_actions.c` near `:560`-`:620`)
  clears the admin credential with no PIN. Intended physical-presence reset:
  needs E-stop asserted, no firing, heat not enabled, four corner taps and a confirm.

## Third pass (home chart, graph, rail helpers)

Read line by line at origin/dev 26fa740e: `ui/ui_page_home_chart.c`,
`ui/ui_page_home_graph.c`, `ui/ui_page_home_rail.c`,
`ui/ui_page_home_internal.h`, plus their call sites in
`ui/ui_page_home_refresh.c` and `ui/ui_page_home.c` where a finding depends on
them. LVGL behaviour checked against the pinned submodule (v9.5.0, 85aa60d1).

### L24 (MED) Chart history lookup breaks once the ring wraps

`ui/ui_page_home_refresh.c:679` maps a bucket time to a history entry with
`idx = lroundf(t_i / HISTORY_SAMPLE_PERIOD_S)`, where `t_i` is seconds since
run start (`:651`). `profile_executor_get_history()` takes `start_index`
relative to the OLDEST RETAINED sample (`control/profile_executor.h`, its doc
comment; `control/profile_executor_status.c:753`). The ring holds
`HISTORY_MAX_SAMPLES` = 640 samples at 30 s (`control/profile_executor.h:406`,
`:440`), about 5 h 20 min. A normal glaze firing runs longer than that.

After the wrap, bucket time `t` shows the sample taken at
`t + (elapsed - 19200 s)`, so the actual trace is shifted left against the
plan, and every bucket past `t` = 19200 s clamps to the newest sample (`:680`),
drawing a flat line at the current temperature. The now-dot uses that
shifted trace (`:819`-`:823`). Before the wrap, gaps also skew it: no sample
is taken on a faulted tick or with no zone active
(`control/profile_executor.c:1987`), while `total_elapsed_s` keeps counting.

Scenario: 8 h into a 10 h firing the LCD chart shows the first 2 h 40 min of
the curve as if it were the start of the run and a flat line from 5 h 20 min
to now. The plan line is right, so the kiln reads as far behind or ahead of
schedule. Display only; control is not affected.

Fix: each entry carries `elapsed_s`. Read the oldest entry's `elapsed_s` once
per tick and index by `(t_i - oldest_elapsed) / 30`, leaving buckets before
the oldest sample empty; or binary-search on `elapsed_s`. Add a host test with
a wrapped ring.

### L25 (LOW) Dashed planned line never dashes

`ui/ui_page_home.c:875` registers `ui_home_chart_draw_event_cb()`
(`ui/ui_page_home_chart.c:46`) for `LV_EVENT_DRAW_TASK_ADDED`. In LVGL 9.5
that event is sent only to objects with `LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS`
(LVGL `src/draw/lv_draw.c:148`). No file under
`firmware/KilnFW/App/` sets that flag, so the hook never runs and the planned
series draws solid, the same style as the actual series. The profile detail
chart has the same gap (`ui/ui_page_profile_detail.c:632`). Colour still tells
the two lines apart. The legend and the code comments say the plan is dashed.

Fix: `lv_obj_add_flag(chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS)` on both
charts. Check on the panel; dashed line drawing costs render time.

### L26 (LOW) Time axis stops at the plan horizon

While a run is active, `horizon_s` is the last plan point's time
(`ui/ui_page_home_refresh.c:619`), the nominal schedule length.
`total_elapsed_s` counts through ramp-lock stalls
(`control/profile_executor.c:825`-`:830`), so a slow kiln outlasts the plan.
Buckets only reach `horizon_s`, so the actual trace past the plan end is
never drawn, and `ui_page_home_now_bucket_index()`
(`ui/ui_page_home_graph.c:47`) clamps the now-dot to the last bucket. The
chart stops moving for the rest of the run.

Fix: use `max(plan horizon, total_elapsed_s)` as the horizon while active.

### L27 (LOW) Chart reads take the executor lock up to 31 times per tick

Each refresh calls `profile_executor_get_history_count()` and then
`profile_executor_get_history(&entry, idx, 1)` once per bucket
(`ui/ui_page_home_refresh.c:609`, `:681`). Each call takes `s_exec.lock` with
`portMAX_DELAY` (`control/profile_executor_status.c:735`) and unpacks one
PSRAM slot. It also rebuilds the plan curve every tick (`:616`). No lock is
held across producer calls here: each acquisition is separate. But the LVGL
task waits behind every executor tick up to 31 times per second, and it does
so while the page is hidden too (L21 covers the `dashboard_get_status()` part
of the same callback).

Fix: one paged read of the needed indices per tick (they are monotonic), skip
the chart block while hidden, and rebuild the plan curve only when the run
changes.

### L28 (LOW) Time-axis overflow with a tiny ramp rate

`http/profiles_http.c:1674` accepts any `ramp_c_per_hr` in `[0, 1000]`, so 0.01 C/h
is valid. `profile_feasibility_plan_curve()` then makes a segment of
`dist / 0.01 * 3600` seconds: 2000 C gives 7.2e8 s, and several such
segments pass 2^31 s. On ESP32 `long` is 32 bits, so `lroundf()` overflows
(undefined behaviour) at `ui/ui_page_home_chart.c:217`,
`ui/ui_page_home_graph.c:70`-`:72` and, through `t_i / 30`,
`ui/ui_page_home_refresh.c:679` (the result is clamped to `count - 1`
afterwards, so no out-of-range index follows). The `%lu:%02lu` buffers
(16 bytes, at most 11 characters) cannot overflow. Effect: garbage tick text.
Float precision on the bucket times is also lost far below that size.

Fix: clamp `horizon_s` to a display maximum (for example 99 h) before tick
math, or set a non-zero minimum ramp rate in the validator.

### L29 (INFO) Tick and legend geometry off by the chart padding

`lv_obj_set_pos()` on a child is relative to the parent's content area
(LVGL `src/core/lv_obj_pos.c:901`-`:902` adds the parent's padding).
`ui_home_chart_set_y_ticks()`, `ui_home_chart_set_x_ticks()` and
`ui_home_chart_set_legend()` add `content.* - chart_coords.*` (the 2 px pad,
`ui/ui_page_home.c:812`) as well (`ui/ui_page_home_chart.c:131`, `:203`-`:204`,
`:276`-`:277`). Every label sits 2 px right and down of the intended spot; the
top Y label lands at content y = -3, so its top pixel row is outside the chart
and clipped; bottom X labels touch the card's outer edge. The chart is not
scrollable (`ui/ui_page_home.c:883`), so nothing scrolls. Cosmetic.

### L30 (INFO) Rail helper notes

- `ui_home_rail_refresh()` (`ui/ui_page_home_refresh.c:995`) sets each zone's
  name and temperature text every tick even when unchanged, which invalidates
  those labels every second. The aux caption next to it is already
  write-on-change (`:1006`).
- `zones_config_get_name()` (`persist/zones_config_accessors.c:423`) copies
  the name with no lock while the HTTP task can rename a zone; a torn copy
  shows a garbled name for one tick. Always NUL-terminated; 20-byte buffer
  holds `ZONE_NAME_MAX_LEN` 15.
- `ui_page_home_rail_aux_caption()` (`ui/ui_page_home_rail.c:14`) hard-codes 4
  captions; it bounds-checks, so a fifth relay would just get no caption. A
  `_Static_assert` against `KILN_IO_RELAY_COUNT` would make that loud.

### Third pass: checked and found clean

- Threads: every function in the three `.c` files is called from
  `ui_home_refresh_cb()` (LVGL timer) or `ui_page_home_build()`; the graph and
  rail helpers make no `lv_*` call at all. No call from a non-LVGL task.
- Locks: these files take no lock. See L27 for the caller's executor reads.
- Indexing: `now_idx` is clamped to `point_count - 1`
  (`ui/ui_page_home_graph.c:53`-`:58`); `hist_zone` comes from a loop over
  `MAX31856_CHANNEL_COUNT` (`ui/ui_page_home_refresh.c:572`-`:575`); the plan
  buffer (`1 + 2 * PROFILE_MAX_SEGMENTS`) matches the curve's worst case and
  the curve writer checks `n < out_cap`; `lagging_zone_indices()` bounds by
  `out_cap` and 8 bits; the rail loops stop at `KILN_IO_RELAY_COUNT` /
  `MAX31856_CHANNEL_COUNT`, and `dashboard_status_t.channels[]` and
  `profile_exec_status_t.zones[]` are both that size.
- Leaks: no allocation in any of the four files; all chart children are built
  once.
- Scrolling: the chart, its legend rows and now-dot clear
  `LV_OBJ_FLAG_SCROLLABLE`; labels are positioned inside the chart.
- Integer math: `quantize_floor_i32()` handles negatives; the lag counter
  saturates at `UINT32_MAX`; `y_axis_range()` guarantees `hi > lo` and the
  5-degree minimum on the returned integers; `axis_ratchet_should_reset()` is
  driven by `uint32_t` elapsed with no subtraction.

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

The second pass covered `ui/kiln_ui.c`, `ui/lvgl_port.c`, `ui/screen_idle.c`,
`ui/ui_lcd_lock.c`, `ui/ui_lcd_keypad.c`, `ui/ui_confirm.c`, the theme and
topbar code, `ui/ui_page_home.c`, `ui/ui_page_home_actions.c`,
`ui/ui_page_home_refresh.c` and `ui/ui_page_diagnostics.c`. The third pass
covered `ui/ui_page_home_chart.c`, `ui/ui_page_home_graph.c`,
`ui/ui_page_home_rail.c` and `ui/ui_page_home_internal.h`. L12, L15, L19, the
Trip detail part of L20 and L24-L29 are estimates from source, not reproduced
on the panel.
