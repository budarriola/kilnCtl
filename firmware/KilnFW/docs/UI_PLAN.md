# UI_PLAN.md — LCD + web usability/cleanup plan (open work only)

Finished work has been moved out of this file per the 2026-08-21 cleanup
rule: done items either became reference material in
`docs/ARCHITECTURE_DECISIONS.md` / `docs/BRINGUP_HAZARDS.md`, or were
deleted outright as narrative with no lasting value. This file now tracks
**only work not yet done**. See those two docs for the no-scroll budget
rule, the current page/route map, the LVGL touch-hit-testing gotchas, and
what shipped in the 2026-08-19/20/21 passes (icon top bar, denser config
hub, paged diagnostics, profile-creation flow, web nav.js/app.js, etc.).

## Where the completed sections went

Code comments across `App/` still cite this file's pre-prune section
numbers (e.g. "UI_PLAN.md section 3 item 5"). Those sections no longer
exist here; this table says where each one's substance landed so such a
citation still resolves to something. Do not use this table to renumber
or restore anything below — it is a lookup, not a table of contents.

| Old section | Was about | Now |
|---|---|---|
| 1. LCD audit | Per-page ~264px no-scroll budget audit, shrunk touch targets | `ARCHITECTURE_DECISIONS.md` ("LVGL / LCD rendering"), `BRINGUP_HAZARDS.md` ("LVGL status-bar icon touch clipping") |
| 2. Web audit | Table overflow, button min-height, fixed-width elements | Completed, removed |
| 3, LCD items 1-6 | Status-card overrun fix, relay-row height cap, touch-target borders, touch-cal/test budget check, config-hub grid scrolling, touch-cal Cancel button | Completed, removed (touch-hit-testing gotchas kept in `BRINGUP_HAZARDS.md`) |
| 3, Web items 1-6 | Table `.table-scroll` wrapper, button min-height, AP-mode QR viewport, coupling-matrix zone count, fixed-width canvas audit, DHCP/static IP toggle | Completed, removed |
| LCD: profile creation page (planned) | New profile-builder page (graph view, point stepper) | Built as `ui_page_profile_builder_{zones,segment,review}.c` — see `ARCHITECTURE_DECISIONS.md` ("Page organization"); note the shipped flow uses stepper cards, not the originally-planned graph/drag view |
| Web: settings/profile import-export (planned) | Same feature, pre-prune draft | Settings half partially built as `backup_http.c` 2026-08-21 (open items in `TODO.md` 0.5); profile half DONE — `profiles_export_http.c`, see "Open: settings import/export (partial)" above |
| Web: page structure rework (planned) | Route map, dashboard reorg | Built — `safety_page.html`, `diagnostics_page.html`, `manual_page.html`, `settings_page.html` trimmed to the danger zone, `main_page.html` reordered/trimmed, `nav.js`/`app.js` — see `ARCHITECTURE_DECISIONS.md` ("Page organization") |
| Web: global-chrome rework (planned 2026-08-21) | Drop-down nav, Home button, dashboard reorder, settings trim | Built same day — see `ARCHITECTURE_DECISIONS.md` ("Page organization", "Global chrome rework") |
| 4 item 1 | Shared `nav.js` header + bottom nav bar | Built, then the bottom nav was deleted entirely in the chrome rework — `ARCHITECTURE_DECISIONS.md` ("Page organization") |
| 4 item 2 | Sticky Stop control on every page | Built — `app.js`, `ARCHITECTURE_DECISIONS.md` |
| 4 item 3 | Shared `/app.js` poller (single `/api/status` fetch, visibility-aware) | Built — `app.js`, `ARCHITECTURE_DECISIONS.md` |
| 4 item 4 | Connection-lost banner, `.kc-stale`/`.kc-live-value` | Built and adopted on every page — `ARCHITECTURE_DECISIONS.md` ("Page organization") |
| 4 item 5 | Confirm step for destructive actions | Built, and **corrected 2026-08-20**: Start and Stop both confirm now, not just destructive actions — see `ARCHITECTURE_DECISIONS.md` ("Page organization") and `App/drivers/ui/ui_confirm.c` |
| 4 item 6 | Auth / session-token layer, scope decision | Still open, explicitly deferred by the owner — see "Open" section above |
| 4 item 7 | Unit parity (°F/°C) | Built — shared device-backed `unit_pref.c/.h`, see `ARCHITECTURE_DECISIONS.md` ("Page organization") |
| 4 item 8 | `main_page.html` trim as it splits | Built — see `ARCHITECTURE_DECISIONS.md` ("Page organization") |
| 5. Data already on the wire | `/api/status` field inventory for new web pages | Completed, removed (fields now just exist in the API; nothing to track) |
| 6. TLS for web UI and OTA | TLS plan | Still open, explicitly deferred by the owner — see "Open: TLS" section above |
| 7. Not yet done / 2026-08-20 decisions | Recap of decided-but-unbuilt scope | Superseded by the "Open" sections above (this file now tracks only open work) |
| LCD navigation (Back-button audit) | `ui_page_temperature.c`'s wrong Back target | Completed, removed |

Driven by the standing user requirement: "the user interface is in need of
usability and cleanup fixes... the webpage should be optimized for a phone
or tablet. and the lcd should not require scrolling." That budget rule and
its arithmetic convention are recorded once in `ARCHITECTURE_DECISIONS.md`
("LVGL / LCD rendering" section) rather than re-derived per page here.

## Open: settings import/export (partial); profile import/export DONE

1. **Settings import/export** — partial (`backup_http.c`, see
   `docs/ARCHITECTURE_DECISIONS.md` "Backup / restore"). Open items tracked
   in `firmware/KilnFW/TODO.md` section 0.5.
2. **Profile import/export — DONE, `cf94b5c3`.** `profiles_export_http.c` (new module,
   not folded into the `profiles_http.c` split): `GET /api/profile/export?id=N`
   downloads one profile as JSON (`Content-Disposition: attachment`);
   `POST /api/profile/import[?id=N]` decodes that JSON with the existing
   `backup_json.h` reader (already shared with `backup_import.c`) and
   commits through `profiles_http_save()` — the same range/feasibility
   validation the interactive Save path and the whole-board backup restore
   both already run, not a third copy of those rules. Covers the richer
   `seg_kind`/`io_target`/`io_state`/`io_blocking`/`io_leave_on_at_end`
   segment fields (RELAY_IO segments), so a profile round-trips unchanged
   regardless of which kind of segment it uses — the whole-board backup
   format (`backup_export.c`/`backup_import.c`) only ever carried
   `target_c`/`ramp_c_per_hr`/`dwell_min`, a pre-existing, separate gap this
   does not need to fix. `profiles_page.html` gained an Export link per
   saved profile and an Import file-picker; the pure client-side helpers
   (`profileExportUrl`, `validateImportJson`) are covered by
   `App/test/test_profile_export_import.js`.

## Open, explicitly deferred by the owner: auth / session layer

**Do not implement.** Scope was decided 2026-08-20 (writes gated, reads
open, server-side session tokens, multi-user) but building it is not
authorized yet.

- **Open, no auth:** every page and every read endpoint (`GET /api/status`,
  `/api/profile_exec`, `/api/readiness`, `/api/history.csv`, `/api/zones`,
  `/api/profiles`, `/status`, `/scan`).
- **Gated once built:** every state-changing POST —
  `/api/diagnostics/danger/relay` (`/api/relay` before its 2026-08-27
  removal, see `docs/WEB_UI.md`),
  `/api/profile_exec/{start,stop,pause,resume,ack_last_run}`,
  `/api/profile`, `/api/profile/delete`, `/api/safety/clear_trip`,
  `/api/zones`, `/api/control`, `/api/autotune/*`,
  `/provision`, `/forget`, `/ip_config`, and the danger zone. OTA keeps its
  own existing per-request HMAC challenge (`App/drivers/ota_auth.{c,h}`)
  regardless — this session layer sits alongside it, never replaces it.

**Mechanism — reuse `ota_auth.{c,h}`, don't invent a second scheme.** That
module already has the primitives: `ota_auth_nonce_issue()`/`_check()` (16
random bytes, single use, 30 s expiry, IP-bound), `hmac_sha256()` over the
nonce keyed by the AP password (so the password itself is never sent),
`ota_auth_constant_time_equal()`, and `ota_auth_lockout_*()` for brute-force
backoff. Login flow mirrors OTA's challenge/verify: `GET /api/auth/challenge`
→ client HMACs the nonce with the AP password → `POST /api/auth/login`.

**Session tokens — server-side table, not a stateless signed cookie.**

- **Token**: 32 bytes from `esp_fill_random()`, hex-encoded, random (not
  derived) — nothing recoverable from it.
- **Storage**: fixed-size RAM table (start at 8 slots) of `{token_hash,
  client_ip, issued_ms, last_seen_ms}`, guarded the same way `ota_http.c`'s
  `s_ota_lock` is. Store the SHA-256 of the token, not the token itself.
  Compare with `ota_auth_constant_time_equal()`.
- **Multi-user**: the table is why this beats a stateless HMAC cookie — N
  independent sessions, each revocable (logout, password change, "sign out
  all"); a stateless token cannot be revoked before it expires. Full table
  evicts the least-recently-seen slot — a stale session must never lock out
  a real operator.
- **Expiry**: sliding idle timeout (~30 min) plus a hard absolute cap
  (~12 h). Both cleared on reboot (RAM-only table) — deliberate.
- **Transport**: `Set-Cookie: kiln_sid=…; HttpOnly; SameSite=Strict; Path=/`.
- **IP binding**: bind the session to the login IP, same as OTA's nonce
  (`httpd_req_to_sockfd()` + `getpeername()`) — kills stolen-cookie replay
  from another LAN device. Cost: switching Wi-Fi network requires re-login.

**Honest limitation to state in the UI, not paper over:** plain HTTP means
the session token travels in clear text on every gated request and can be
sniffed on the LAN, even though the *password* never is. IP binding and the
idle timeout narrow that window, not close it. TLS (below) closes it; this
layer is designed to be correct without TLS first so a handshake bug and an
auth bug stay distinguishable during bring-up.

**Open sub-question, not blocking:** whether the AP password is the right
long-term credential, or whether a separate "web password" should be
settable. Reusing the AP password ships first — already provisioned,
already the OTA credential, no new stored secret.

## Display power: brightness/timeout/keep-on-while-firing/display-on-error — 2026-09-04

Owner request: a brightness control on the display settings page; a
selectable auto-off idle timeout (1/5/10/15/60 minutes, or Never); a "keep
display on while firing" switch that overrides that timeout during an active
firing; the first touch after the display is off only wakes it, never acts
on the UI underneath; and a "display on error" switch next to keep-on-while-
firing that forces the display on when an error occurs and holds it until a
touch dismisses it (which is itself swallowed, same as any other wake touch),
then resumes normal timeout behaviour.

**Implemented this pass:**
- `App/drivers/persist/display_power_policy.h/.c` — the pure decision core (no LVGL/
  NVS/FreeRTOS), same split as SaftyFW's `max31856_fault_pin_policy.c`/
  `discrete_pin_policy.c` and this codebase's own `backlight_pwm.h`'s
  `backlight_duty_percent_for_state()`. `display_power_policy_step()` takes
  `(now_ms, last_touch_ms, timeout_setting, firing_active,
  keep_on_while_firing, error_active, error_entered_this_tick,
  display_on_error, current_state, touch_event)` and returns the next
  `display_power_state_t` (ON / OFF / ERROR_HOLD) plus `swallow_touch`.
  Host-tested in `App/test/test_display_power_policy.c` (18 cases), including
  the rule 3/4/5 interactions the owner specifically asked to be covered:
  error arriving while firing, touch during error-hold resuming normal
  timeout behaviour, "Never" plus error, and a timeout expiring on the exact
  same tick as a touch.
- `App/drivers/persist/display_power_cfg.h/.c` — persisted settings (brightness
  0-100%, the six-way timeout enum, both switches) in one versioned NVS blob,
  same `kiln_nvs`/`kiln_cfg` pattern as `unit_pref.c`/`ramp_assist_cfg.c`.
  Every failure path (missing key, wrong size/version, an out-of-range field
  inside an otherwise well-formed blob) falls back to the safe defaults
  (100% brightness, Never timeout, both switches off — i.e. today's shipped
  behaviour, unchanged for any board that never opens this settings page).
  Host-tested in `App/test/test_display_power_cfg.c` (6 cases). NVS writes
  run from `settings_http.c`'s own httpd handler task (Pattern 3 in
  `App/test/flash_worker_lint.py`'s allowlist, same as `unit_pref.c`) — not
  dispatched to the flash worker and not PSRAM-stacked, so neither of the
  two hazards that allowlist exists to catch applies here; negative-tested by
  temporarily removing the allowlist entry and confirming the lint fails
  naming `drivers\display_power_cfg.c:178`/`:180` exactly, then restoring it.
- `App/drivers/http/settings_http.c` — `GET`/`POST /api/settings/display_power`,
  same bounded-body / refuse-don't-clamp shape as the existing
  `/api/settings/tz` handler. `display_power_cfg_start()` is called from
  `settings_http_start()` (this module's own existing app_main call site,
  see that function's comment) rather than adding a new call in `main.c`.
- `App/drivers/http/settings_display_page.html` — a new "Display power" card:
  brightness slider, timeout `<select>`, and the two switches, following the
  existing page's card/row markup and dark/light theme tokens. Loads current
  values on page load, POSTs all four fields together on Save.

**Inert pending hardware, by design:** brightness is fully built (setting,
persistence, API, UI) but does nothing on real hardware today —
`CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE` is off by default (`backlight_pwm.h`:
no flying wire fitted from a spare ESP32 GPIO to the backlight LED input yet;
another agent is mid-investigation on that bodge). `display_power_cfg.c`'s
header comment says this explicitly; the settings page shows an inline note
next to the slider saying the same thing to the owner. Wiring
`display_power_cfg_brightness_percent()` into `backlight_pwm.c`'s on/idle
duty (replacing its current Kconfig-constant percentages) is follow-up work
for whoever verifies that hardware.

**Wired into the running display stack — 2026-09-04, this pass:**
- `App/drivers/ui/screen_idle.c` — `screen_idle_run_policy_locked()` is now the
  single call site for `display_power_policy_step()`, called from
  `screen_idle_task`'s existing 50ms poll tick (`touch_event=false`) and from
  the new `screen_idle_touch_swallow()` on a press EDGE (`touch_event=true`,
  exactly once per press, per the header's calling contract). It supplies:
  - `firing_active` — `profile_executor_get_status()`'s `state ==
    PROFILE_EXEC_RUNNING || PROFILE_EXEC_PAUSED`, the same accessor
    `boot_button.c`/`danger_mode.c`/`gpio_probe.c` already use for this exact
    question. Real producer: `profile_executor_run.c`/`profile_executor_
    status.c` assign `s_exec.state = PROFILE_EXEC_RUNNING` when a firing
    actually starts — not a stub, not a value nothing writes.
  - `error_active` (and its `error_entered_this_tick` edge, computed against
    a remembered `idle->error_prev_active`) — `dashboard_get_status()`'s
    `diag_ever_received && diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED &&
    diag_age_ms < SAFETY_LINK_STALE_MS`, the **identical** fields and gate
    `ui_page_home.c`'s own safety-trip strip already keys off (that file's
    comment: "a STALE diag_state == TRIPPED is a silent link, not a live
    trip"). Real producer: `safety_link_frames.c`'s wire-frame parser sets
    `link->cached.diag_state = p[24]` from an actual DIAG frame off the RP2040
    safety processor. This module raises the display for exactly the
    condition the LCD already paints red, not a second, differently-gated
    notion of "error".
  - The three persisted settings, read live from `display_power_cfg.c`'s
    accessors (`display_power_cfg_timeout_setting()`/`_keep_on_while_firing()`/
    `_display_on_error()`) — so a Save on the settings page takes effect on
    the very next poll tick, no reboot.
  `idle->screen_on` (already read by `lvgl_port.c`'s `ili9488_flush_cb`/
  `lvgl_port_service_idle_blank()` — unchanged) is now `policy_state !=
  DISPLAY_POWER_OFF`, so `ERROR_HOLD` reads as "on" for that existing wake/
  blank plumbing, exactly as intended.
- `App/drivers/ui/lvgl_port.c` — `touch_read_cb()`'s physical-NS2009 branch and
  its LVGL-side injected-touch branch (`TOUCH_CMD_INJECT`) each now call the
  new `screen_idle_touch_swallow()` before arbitration/delivery; when it
  reports `swallow_touch`, `data->state` is forced back to
  `LV_INDEV_STATE_RELEASED` for every poll of that one press/release gesture
  (cached at the edge, not re-decided mid-drag), so the widget underneath
  never sees it — this is rules 3 and 4. A physical release (previously never
  reported to `screen_idle` at all — `touch_dev_read()` just stops returning
  `pressed`) now also notifies `screen_idle_touch_swallow(..., false, ...)`,
  clearing the edge tracker so the *next* separate press is evaluated fresh.

### Context rules for the display path — 2026-09-04 opus review

Four defects landed in `screen_idle.c`/`lvgl_port.c`/`display_power_policy.c`
in a single day (`e7b8efc` stack overflow, `7a8594d` lock held across SPI, a
queue wait and a heap walk, `51e1ef5` `lv_obj_invalidate()` from inside
`ili9488_flush_cb()`, and this review's same-tick error/touch fall-through).
None was a logic error in the ordinary sense: each was correct code running
in a context that did not permit it. The shared cause is that this path has
**five different execution contexts that all look like ordinary C**, and
nothing at the call site says which one you are in. These are the rules; a
change that breaks one is a defect even if it "works" on the bench.

| Context | Entered from | May NOT do |
| --- | --- | --- |
| `ili9488_flush_cb()` | inside `lv_timer_handler()`'s **active refresh** | any `lv_*` **mutator** — `lv_obj_invalidate`, `lv_obj_del`, `lv_screen_load`, `lv_refr_now`. Reads only. (`51e1ef5`) |
| `touch_read_cb()` | inside `lv_timer_handler()`'s indev read | block. Everything it calls (`screen_idle_touch_swallow`, `touch_dev_read`) must be bounded and short |
| `lv_timer` page-refresh callbacks (`ui_page_*.c`) | inside `lv_timer_handler()` | block for long. **Violated today**: `ui_page_diagnostics.c` (2 s) and `ui_page_temperature.c` (1 s) call `dashboard_get_status()` — five MAX31856 SPI reads, a `kiln_io_owner` round trip that can block 200 ms, and interrupts-disabled heap walks — on the `lvgl` task, which `get_stack_margin()` reports at 24.7% headroom (LOW) |
| `lvgl_port_task` loop, **before** `lv_timer_handler()` | own task | nothing special — this is the ONLY safe place for wake/blank `lv_*` mutators (`lvgl_port_service_idle_blank/_wake`) |
| `screen_idle_task` | own task | call the expensive producers **under `idle->lock`** — that lock is taken by the LVGL task every tick and every touch (`7a8594d`). Producer reads go in `screen_idle_refresh_inputs()`, off-lock |

**The invariant, stated once:** *`idle->lock` may only ever be held across
pure computation and plain struct field access — never across SPI, I2C, a
queue wait, a heap walk, an NVS access, or any `lv_*` call.* Everything the
policy needs from another subsystem enters as a pre-taken snapshot
(`idle->cached_*`), never as a call made under the lock. `display_power_cfg`'s
four accessors are safe under it only because they are plain static reads —
if one ever grows a lock or an NVS touch, it must move out to the snapshot
too.

**Why "read the comment" is not the mechanism.** Both fixed bugs had a
correct comment sitting next to the wrong code. `ili9488_flush_cb()`'s own
header described it as running inside the refresh while it invalidated an
object; the same-tick error branch this review fixed carried a comment saying
"the hold must still engage" directly above code that dismissed the hold, and
a second comment that contradicted itself within four lines ("only on a LATER
tick" … "in the same call"). The mechanism that actually holds is
`test_display_power_wiring.c`'s source scans, which fail the build. Extend
that file, not this table, when adding a rule — and **negative-test the scan
against a comment-only match**: the `lv_layer_top()` check added in this pass
passed with the fix deleted, because the fix's own comment contained the
string it searched for. `strip_c_comments()` in that file exists for that
reason.

**"Display off" mechanism — exactly what it physically does, and why:**
This board has **no backlight control line today**
(`CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE` is off by default — `backlight_pwm.h`:
"No backlight pin exists on the board... That wire is NOT fitted on the bench
board as of this writing"). With the flag off, `backlight_pwm_init()`/
`_start()` are no-ops that touch no GPIO/LEDC peripheral at all — confirmed
by reading `backlight_pwm.c`'s `#else` branch, not assumed. So `DISPLAY_
POWER_OFF` does **not** cut backlight power; it cannot, on unmodified
hardware. What it actually does is the same mechanism `screen_idle.c` already
used before this pass: `lvgl_port.c`'s `lvgl_port_service_idle_blank()`
paints the panel solid black (`ILI9488_clear(display, 0x0000)`) on the
on→off edge, and the backlight (whatever is driving it — hardwired on)
stays lit shining through black content, which blocks far more light than a
white/idle screen would but is not "off". If `CONFIG_KILNCTL_BACKLIGHT_PWM_
ENABLE` is ever turned on (the flying-wire bodge from `backlight_pwm.h`
lands), `backlight_pwm.c`'s own poll task already reads `screen_idle_get_
state()`'s `screen_on` flag independently and drives real LEDC PWM duty from
it — that path needed **no changes** in this pass, since it was already
written against `screen_idle_t`'s public `screen_on`, which this pass keeps
meaning exactly what it always meant ("the panel should show real content
right now").

**Tests added this pass:** `App/test/test_display_power_wiring.c` —
`screen_idle.c`/`lvgl_port.c` are not host-compilable (pull in `panel_spi.h`/
`NS2009.h`/`lvgl.h`), so this follows SaftyFW's `test_safety_core_s8_
wiring.c` precedent: a source-text scan of the real `.c` files, extracting
the specific function bodies under test so a match can only land inside them,
failing closed if the file/symbol can't be found at all. Verifies (1)
`screen_idle_run_policy_locked()` actually calls `display_power_policy_step()`
and reads the real `profile_executor_get_status()`/`dashboard_get_status()`
producers (not a stub), and that `error_entered_this_tick` is computed as a
real edge; (2) `touch_read_cb()` calls `screen_idle_touch_swallow()` from
**both** the physical and injected paths (≥2 call sites) and that a swallow
verdict actually resets `data->state` at each site (counts `if (swallow) {`
gates, not just "does the RELEASED text appear anywhere in the function" —
that weaker form of the check was tried first and did not catch a mutation
that dropped one gate's body while leaving its neighbor's release-branch text
elsewhere in the function). Negative-tested by hand: removing the physical
branch's `if (swallow) { data->state = ...; return; }` guard makes this test
fail with `found fewer than 2 'if (swallow) {' gates in touch_read_cb()...`;
restoring it makes the suite pass clean again (21/21 host-test executables).

**Needs a flash plus the owner's finger to verify — NOT done yet.** This pass
built and host-tested only (see this repo's git log for evidence); nothing
above has been flashed to real hardware, and the on-screen timeout/wake/
error-hold/touch-swallow behaviour has not been confirmed with an actual
finger on the glass. Brightness stays inert pending the backlight flying-wire
bodge, as before.

**2026-09-04, later the same day: hardware-verified, then a task-watchdog
reset mid-firing, root-caused and fixed.** An agent flashed this pass and, on
a LIVE firing (`fuzzy_ab_20260904d`, arm B1, profile 7, segment 2/3),
exercised the override case first: `POST /api/settings/display_power` with
`timeout=1min` (`DISPLAY_TIMEOUT_1_MIN`) + `keep_on_while_firing=1`, waited
>100 s, confirmed via `touch_get_state()` and numeric pixel sampling that the
screen correctly stayed ON through the timeout. Then set
`keep_on_while_firing=0`, waited 15 s, confirmed the screen correctly
BLANKED with the executor still `RUNNING`. **Both results stand as real
hardware verification of the override and its control case** — that evidence
is not in question and is not what follows.

Restoring the original settings and injecting a wake touch immediately reset
the board: `reset_reason: "task watchdog"`, executor gone from `RUNNING` to
`IDLE`, the firing lost. `get_device_log()` (kilnctrl MCP) recovered the
crash record `crash_report.c` captured on the next boot: `task='lvgl'`,
`cause=892351539`/`pc=0xfffffffd`/`addr=0x20293632` (values with no valid
Xtensa EXCCAUSE/PC meaning — the coredump's exception-info fields are simply
not meaningful for a task-watchdog stall, not evidence of a second, separate
corruption). Reproduced twice more, deliberately, on an **idle, non-firing**
board (timeout=1 min, let it blank, inject one touch) — same signature both
times, always landing immediately after the `"screen woke -- forcing a full
redraw"` log line, ruling out both the live firing and the concurrent
campaign/agent HTTP polling as the cause.

Root cause: `ili9488_flush_cb()` (`lvgl_port.c`) detected the off→on wake
edge and called `lv_obj_invalidate(lv_screen_active())` **from inside the
flush callback itself** — but that callback runs from inside
`lv_timer_handler()`'s own active refresh (it is called *to blit* one of the
areas that refresh is already iterating). Invalidating an object there
reenters LVGL's invalid-area bookkeeping mid-walk, which LVGL does not
support from a flush callback; the `lvgl` task never yielded long enough for
FreeRTOS's idle task to run, and `CONFIG_ESP_TASK_WDT_TIMEOUT_S` (5 s) fired
on it. Landed today in `7fc17cc`, same day as `e7b8efc`'s stack-overflow fix
and `7a8594d`'s SPI-under-lock fix — the third bug in this file in one day,
and the least-tested path (blank→wake plus a touch) of the three.

**Fix:** the wake-edge detection and its `lv_obj_invalidate()` call moved out
of `ili9488_flush_cb()` into a new `lvgl_port_service_idle_wake()`, called
from `lvgl_port_task()`'s own loop **before** `lv_timer_handler()` runs —
the same "outside any active refresh" footing `lvgl_port_service_idle_blank()`
already stood on for the on→off edge's `ILI9488_clear()` call.
`ili9488_flush_cb()` now only reads `screen_on` to decide whether to skip the
blit; it must never call an `lv_*` mutator again. Verified: rebuilt,
`flash_firmware(verify=True)` confirmed the new build/commit is the one
running, then the idle-board repro sequence was run twice more against the
fixed firmware (blank on a 1-minute timeout, inject touch; a second
back-to-back blank/wake cycle with two rapid touches) with zero resets and
`get_fw_version()` reporting the same unchanged commit/build time throughout
— the crash no longer reproduces.

**Regression test:** `App/test/test_display_power_wiring.c` section 6
(`run_section6_wake_invalidate_not_in_flush_cb`) — source-text-scans
`ili9488_flush_cb()` to prove it contains no `lv_obj_invalidate(` call, that
`lvgl_port_service_idle_wake()` exists and calls
`lv_obj_invalidate(lv_screen_active())`, and that `lvgl_port_task()` calls
`lvgl_port_service_idle_wake()` before `lv_timer_handler()` in source order.
Negative-tested by hand: reintroducing the bug (calling
`lv_obj_invalidate(lv_screen_active())` back inside `ili9488_flush_cb()`)
makes this check fail with exactly:
`FAIL test_display_power_wiring.c:623: ili9488_flush_cb() must NEVER call
lv_obj_invalidate() (or any other lv_* mutator) -- it runs from inside
LVGL's own active refresh, and reentering the invalid-area list from there
is exactly what produced the reproducible task-watchdog crash this test
pins. If a wake-redraw call belongs anywhere, it is
lvgl_port_service_idle_wake(), not here.` — reverting the mutation returns
the full host-test suite to green (`Built: 21/21 executables`, no failures).

**Separate, unrelated observation (not this task's to fix):** the campaign
runner's restore-on-exit did not run when it died with the board, so the
board's live zone config may not currently match the
`fuzzy_ab_baseline_20260903` preset — flagged for whoever is handling that
recovery, not touched here.

## Open, explicitly deferred by the owner: TLS for web UI and OTA

Requested explicitly: *"i also want tls for both ota and this. for now plan
only."* **Plan only — not authorized to build.** Closes the auth layer's
"sniffable on the LAN" gap once it lands, and protects OTA's firmware image
transfer (today plain HTTP).

**Server**: swap `httpd_start()` for `httpd_ssl_start()` (`esp_https_server`,
already in ESP-IDF). Every module reaches the server through
`wifi_provision_http_get_server()`, so no other `.c` file's registration
code changes. `httpd_ssl_config_t` wraps the existing `httpd_config_t`, so
the deliberate deviations already in `docs/WEB_UI.md` (`lru_purge_enable`,
`max_uri_handlers`, `stack_size`) carry over unchanged.

**Certificate — self-signed, generated on-device at first boot.** No CA
will issue for a LAN device with no public name; a cert baked into the
firmware image would be identical on every board and extractable from the
binary — worse than self-signed.

- **ECDSA P-256, not RSA-2048** — an order of magnitude faster handshake on
  an ESP32-S3, smaller key/cert/mbedTLS footprint. RSA's only edge is
  ancient-client compatibility a phone browser doesn't need.
- Generated once, persisted in NVS in its own namespace (see
  `docs/PROJECT_STATUS.md`'s one-partition-per-concern rule and its
  boot-time compatibility check, which the cert namespace needs to
  participate in).
- CN/SAN covering the mDNS name (`kilnctl.local`) and the current IP — IP
  SANs go stale on a DHCP lease change; plan for regeneration on IP change
  or accept name-only access and make mDNS the supported path. Decide
  during implementation.
- **Show the certificate fingerprint on the LCD** (`ui_page_diagnostics.c`,
  which already exists and has room). This is what makes self-signed
  defensible: the user compares the browser's "untrusted certificate"
  fingerprint against the physical panel and confirms it's *their* kiln,
  not something else answering on that IP.

**Port and redirect layout.** HTTPS on 443; a plain-HTTP listener stays on
80 doing nothing but a 301 to HTTPS, so a bookmarked/typed
`http://kilnctl.local` still lands. **AP-mode provisioning stays plain
HTTP** — phone captive-portal detection breaks on TLS, and a fresh board's
AP has no name/trusted cert anyway. `wifi_provision_page.html` served over
the fallback AP is the one deliberate exception; every STA-mode route gets
TLS.

**Cost.** PSRAM is on (`CONFIG_SPIRAM=y`, octal, 8 MB, `_USE_MALLOC=y`), so
mbedTLS's session/record buffers can land there — but the handshake still
burns **internal** SRAM for stack, and this firmware has hit internal-SRAM
exhaustion once already (see `BRINGUP_HAZARDS.md`). So when implementing:

- Size the HTTPS server task's own stack deliberately (don't take the
  default), same reasoning that already forced `stack_size = 8192` on the
  plain-HTTP server for `zones_post_handler`'s body buffer.
- Reduce `MBEDTLS_SSL_IN_CONTENT_LEN`/`OUT_CONTENT_LEN` from the 16 KB
  default — this UI's largest body is ~3.2 KB, so 4 KB is generous.
- Enable TLS session resumption/tickets — the shared poller reconnects
  regularly; a full ECDHE handshake every couple seconds per client is the
  one workload that would actually hurt.
- Measure `esp_get_minimum_free_heap_size()` and PSRAM free, before/after,
  with two or three phones connected — `ui_page_diagnostics.c` already
  displays both.

**OTA over TLS.** `ota_http.c`'s routes ride the same server, so they
inherit TLS with no per-route change; the HMAC challenge scheme stays as
defense in depth (proves knowledge of the AP password without sending it).
TLS adds confidentiality/integrity of the image transfer; it does **not**
replace image signing — secure boot / signed images guard against a
malicious image delivered over a perfectly good TLS connection, and signing
is not in this plan.

**Sequencing.** TLS lands *after* the auth/session layer above works over
plain HTTP, not before — otherwise a handshake bug and an auth bug are
indistinguishable during bring-up.

---

## Section 5 — owner requests, 2026-08-30

### 5.1 Zone names on the dashboard instead of "Channel N" — SHIPPED (STILL TRUE 2026-09-04)

Built. `renderChannels()` in `main_page.html` labels each live row via
`zoneIndexForChannel(ch.channel)` (`main_page.html:580-586`, iterates zones
lowest-index-first and returns on the first `thermo_mask` bit match — the
overlapping-mask tie-break resolves to the lowest zone index, as specified)
and `zoneName(zi)` (`:1050-1052`). All four required states hold: an
unclaimed channel (`zi === null`) falls back to `'Channel ' + ch.channel`;
`zoneName()`'s own `||` fallback renders `Zone N` for an empty name string;
`zonesCache` not yet loaded is covered (`zoneIndexForChannel` reads an empty
`zones` array and returns `null`, same as the unclaimed-channel path); and
the channel number stays visible either way — as a `title` tooltip when a
zone name is shown, inline in the `Channel N` fallback otherwise.

### 5.2 Everything is 0-based, everywhere — FIXED (verified 2026-09-04)

Every display violation the 2026-08-30 audit named below has been corrected
at source: `zones_page.html:778` now reads `'Channel ' + tch` (no `+ 1`),
`:792` reads `'CT ' + cti`, `:1496`/`:1832` read `'Zone ' + i`, and
`ui_page_temperature.c`'s relay-tile `snprintf` calls (lines 252/261/275/478)
all print `(unsigned)r` with no offset. The mixed-convention cases called
out below (relay tiles vs. toasts, `zones_page.html`'s CT warning vs. its
zone cards) are resolved the same way — 0-based throughout. Kept below for
the record of what was found and the two 1-based-on-purpose exceptions,
which still apply unchanged.

Zones, relays, thermocouple channels and CT channels are all indexed from 0
in the firmware, and every operator-facing label must say so too. No
`+ 1` on any of these indices, in any page, log line, LCD label, or PC-tool
string. Mixed conventions *within one screen* are the worst case and take
priority.

Audit complete 2026-08-30. Findings:

**Display violations to fix (user-visible, safe to change):**

| File:line | Offender | Note |
|---|---|---|
| `zones_page.html:472` | `'Channel ' + (tch + 1)` | mask bit `tch` is 0-based |
| `zones_page.html:486` | `'CT ' + (cti + 1)` with `data-ch=cti` | **1-based label and 0-based attribute on the same line** |
| `zones_page.html:825` | `'Zone ' + (i + 1)` | CT-warning fallback |
| `ui_page_temperature.c:252,261,275,475` | `"Relay %u", (r + 1)` | LCD relay tiles |

**Mixed conventions on one screen — the worst class, fix first:**

- `zones_page.html` labels zone cards `Zone i` (line 523, 0-based) and the
  autotune dropdown `Zone i` (1067, 0-based) but the CT warning `Zone i+1`
  (825). Same page, same concept, two answers.
- `ui_page_temperature.c` prints relay **tiles** 1-based (`r + 1`) and relay
  **error toasts** 0-based (via `UI_RELAY_DISPLAY`), while printing zones
  0-based (line 411) — three conventions on one screen. Already tracked in
  `TODO.md:92-94`; this audit corroborates it.
- `zones_page.html:472` says `Channel tch+1` where `diagnostics_page.html:703,843`
  says `Channel c.channel` — same concept, two pages, two bases.

**Do NOT blind-fix — these are 1-based on purpose:**

- **The relay wire protocol is 1-based.** `relay_mask` bit *r* maps to wire/API
  relay *r+1*, and `main_page.html:461-464` / `diagnostics_page.html:936-941`
  key their handlers on the real 1-based wire number while display-shifting
  only the printed label. Any further 0-basing must stay display-only; moving
  the wire numbering breaks the SX1509 `SET_RELAY` opcode.
- `zones_config_migrate.c:251` (this was in `zones_http.c` around line 1522
  before the v8→v9 migration code was split out) writes
  `"Zone %c", '0' + p + 1` as the **persisted NVS**
  default timing-profile name during the v8→v9 migration. Existing boards
  already carry those names. Renaming is a data migration, not a display fix.

**Stale docs:** `SX1509.md:97` claims the GUI is 1-based (it was moved to
0-based 2026-08-27); `WEB_UI.md:238` documents `relay_cycles[i]` as relay
*i+1*.

**Already correct:** `ui_page_home.c:525`, `ui_page_profile_builder_review.c:264`,
`ui_page_profile_builder_zones.c:161`, `ui_page_profile_detail.c:386`, and all
of `tools/PcTools`.

### 5.3 LCD chart markers to match the web — DONE (shipped at 6 ticks, not 11)

**Status as of 2026-09-06:** built and in the tree (`ui_page_home.c`,
`ui_page_home_chart.c`) — labelled ticks outside the plot on both axes,
`UI_PAGE_HOME_X_TICK_COUNT` (4, matching the web's time axis exactly) and
`UI_PAGE_HOME_Y_TICK_COUNT` (6, not the owner-decided 11 — see the
2026-09-01 note beside that macro's definition: a real-panel check found
the 11-tick, scaled-down-14px-font attempt aliased badly, so the shipped
version uses a real `montserrat_10` face at 6 ticks instead of a scaled
bitmap at 11). If the owner wants the literal 11-tick count restored now
that a real small font is compiled in, that is a follow-up, not a defect —
the deviation is deliberate and documented at the code site.

The owner asked for "10 vertical markers like the web GUI" plus matching
horizontal markers on the LCD home chart.

**A first attempt set `lv_chart_set_div_line_count(s_chart, 11, 4)` and was
reverted the same day.** The premise was wrong: *the web chart has no
gridlines*. `drawYAxis()` strokes a 3px tick just **outside** the plot
(`moveTo(padL - 3, vy) -> lineTo(padL, vy)`) beside a numeric label;
`drawChartAxis()` does the same below it. The only full-width strokes inside
the web's plot are its border and `drawFreezingRef()`'s dashed 0 °C line.

So "markers" means *labelled ticks outside the plot*, and `lv_chart` division
lines — which run through it — are the wrong primitive. Setting `vdiv=4` in
particular restores the exact geometry (lines at 0, 1/3, 2/3, 1) behind the
2026-08-22 "chart reads as three separate panels" report, plus ten more cuts.
See the comment at the `lv_chart_set_div_line_count()` call site.

**Owner decision 2026-08-30: option 3 — all 11 ticks labelled, smaller font.**
Implement labelled ticks outside the plot (not div lines), 11 on the
temperature axis and 4 on the time axis, matching the web's counts exactly.
If the labels prove unreadable on the real panel, come back to this list
rather than silently dropping labels.

**The density concern that prompted the question, kept for the record:**
The LCD chart is `flex_grow` residual height on a 480px page — order
120-160px. The web's canvas is several times taller. Eleven numeric
temperature labels that read cleanly there will not fit here, so matching the
*count* does not match the *appearance*. Options, in rough order of
preference:

1. Ten ticks, but label only a subset (min / mid / max), keeping the existing
   corner overlay labels as the numeric readout.
2. Fewer ticks, all labelled — matching the web's *intent* (a readable
   temperature scale) rather than its literal count.
3. All 11 labelled, accepting a smaller font — likely unreadable at this
   size, and `feedback_lcd_no_scrolling` means there is no room to grow the
   chart instead.

(Options 1 and 2 above were offered and not chosen.)
