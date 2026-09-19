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
| `lv_timer` page-refresh callbacks (`ui_page_*.c`) | inside `lv_timer_handler()` | block for long. **Violated today**: `ui_page_diagnostics.c` (2 s) and `ui_page_temperature.c` (1 s) call `dashboard_get_status()` — three MAX31856 SPI reads, a `kiln_io_owner` round trip that can block 200 ms, and interrupts-disabled heap walks — on the `lvgl` task, which `get_stack_margin()` reports at 24.7% headroom (LOW) |
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

---

## Section 6 — LCD dashboard and profiles rework, owner request 2026-09-19

Six LCD items (ROADMAP.md row "LCD dashboard and profiles rework — owner
request 2026-09-19"). Nothing here is built yet. All of it is LVGL-side:
no new HTTP route (the URI handler cap is 150 used of 151), no new task, no
new timer, no new colour, no page that scrolls.

Two facts hold for every item and are not repeated per item:

* **The 1 Hz tick already fetches everything.**
  `ui_page_home_refresh.c:102-104` already calls `dashboard_get_status(&ds)`
  and `profile_executor_get_status(&st)` every tick. Both return a snapshot
  and take whatever lock they need *internally*
  (`profile_executor_status.c:250` takes `s_exec.lock` and is prestart-
  hardened), so a caller holds no lock and crosses no producer call. Items 1,
  3 and 5 read from those two structs and add no producer call of their own.
* **The budget.** `UI_THEME_PAGE_CONTENT_BUDGET_PX` = `DISPLAY_WIDTH(320)
  - 2*8 - 32 - 4` = **268px** of content height (some page comments round it
  to "267"; use the macro, never a literal). Usable content width is
  `480 - 2*UI_THEME_PADDING_PX` = **464px**. Font line height is
  `UI_THEME_FONT_LINE_HEIGHT_PX` = 20 (montserrat_14, the default);
  `lv_font_montserrat_10` is the only smaller font enabled in
  `sdkconfig.defaults` and renders ~14px per line. montserrat_12 is **not**
  enabled — do not use it without enabling it and re-checking flash size.

### 6.1 Dashboard: selected profile name left of Start, tapping it opens a picker

**Files.** `ui_page_home.c` (`ui_page_home_build()`, the `action_row` block
at the end), `ui_page_home_internal.h` (declare the new
`s_ui_home_profile_btn` / `s_ui_home_profile_label`),
`ui_page_home_refresh.c` (`ui_home_refresh_cb()`, set the label text), the
new picker page from 6.2, and `ui_profile_list_order.c/.h` (new pure module,
see 6.2).

**Data source and locks.** The selected profile is `profile_exec_status_t`'s
profile id from the `profile_executor_get_status()` snapshot the refresh tick
already holds; the display name is built from `profiles_http_get(id, &prof)`
(`persist/profiles_store.h`, plain C, no httpd) plus `profiles_builtin_get()`
for ids `>= PROFILE_BUILTIN_ID_BASE` (128). No lock is held by the caller.

**Layout (464px wide row).** `action_row` today is `LV_FLEX_FLOW_ROW`, gap 4,
`LV_SIZE_CONTENT`, holding the hidden `s_ui_home_pause_btn` and
`s_ui_home_fire_btn`. Change it to `lv_obj_set_width(action_row, lv_pct(100))`
with `LV_FLEX_ALIGN_END` on the main axis, and insert the profile button as
the **first** child with `lv_obj_set_flex_grow(profile_btn, 1)`. Widths:
Start is about 96px drawn, Pause about 96px when visible (0 when hidden —
LVGL skips hidden children in flex, the idiom this page already relies on),
gaps 4. Profile button width is therefore `464 - 4 - 96 = 364` idle and
`464 - 4 - 96 - 4 - 96 = 264` while running. Both hold a `LV_LABEL_LONG_DOT`
name label at montserrat_14 (about 33 and about 24 characters). Height stays
36px, matching Start, so `action_row`'s contribution to the column is
unchanged at 36px and **no page-height arithmetic moves**. Touch:
`ui_theme_apply_touch_area(profile_btn, true)` gives the 36px control the
required `UI_THEME_MIN_TOUCH_TARGET_PX` (72) effective height, exactly as the
Start button already does.

**Tests owed.** No new `_Static_assert` (no height change). The picker's
ordering is covered by 6.2's host test. This item owes no new C test unless
the label-truncation helper is factored out, in which case it joins
`test_ui_page_home_graph.c`.

**Numeric verification.** `capture_lcd.ps1 -Full` maps LCD `(x,y)` to frame
`(102 + 1.890*x, 12 + 1.903*y)`. The profile button's left edge is LCD `x=8`,
its vertical centre about LCD `y=250`, giving frame `(117, 488)`. Sample an
8x8 box there with `sample_lcd_region.ps1 -X 117 -Y 488 -W 8 -H 8` and require
the mean RGB to be `UI_THEME_COLOR_CARD` `0x242a3a` (not `BG` `0x1a1f2b`),
proving a card-backed control is present where empty background used to be,
against the script's own bezel reference.

### 6.2 LCD Profiles page becomes that list, with New and per-profile delete

**Files.** `ui_page_profiles.c` (rewritten — the four `build_nav_item()` cells
"My Profiles", "Built-ins (28)", "Restore hidden", "New Profile" and their
callbacks `mine_nav_cb` / `builtins_nav_cb` / `restore_hidden_cb` /
`new_profile_nav_cb` all go away), `ui_page_profiles_mine.c` and
`ui_page_profiles_family.c` (delete them, and drop their
`kiln_ui_register_page()` calls), `kiln_ui.c` (register the new
`"profile_picker"` page; drop `"profiles_mine"` / `"profiles_family"`), plus
the new `ui_page_profile_picker.c/.h` and `ui_profile_list_order.c/.h`.

**The list is one widget used twice** — the Profiles page and 6.1's picker are
the same builder with a `mode` flag: picker mode returns a selection, manage
mode shows New and per-row Delete. Build it once in
`ui_page_profile_picker.c`; do not fork it.

**Data source and locks.** Ids come from `profiles_http_get()` over slots
0..`PROFILES_MAX_COUNT-1` (8) plus `profiles_builtin_entry()` over the builtin
table, skipping `profiles_builtin_is_hidden()`. Favorites come from
`profiles_favorites_is(id)` / `profiles_favorites_masks(&user, &builtin)`
(`persist/profiles_favorites.h`). **`profiles_favorites.c` contains no mutex,
semaphore or critical section** — reads are plain RAM reads, safe directly on
the LVGL tick, and no new HTTP route is needed to reach them.

**Ordering must match the web dashboard exactly.** `main_page.html`'s
`orderProfilesByFavorite()` (around line 1246) is a **stable partition**:
favorites first, intra-group order untouched, nothing dropped; and
`profileOptionLabel()` prepends a star as a **display-only label prefix** —
the option's value stays `p.id`. `ui_profile_list_order()` must have the same
three properties: stable partition, set equality, star only in the rendered
text and never in the id the row carries. Do not re-derive the favorite set
from a second source.

**Delete parity with the web.** `profiles_edit_http.c` (around line 603)
deletes and then calls `(void)profiles_favorites_set((uint8_t)id, false)` so
no dangling favorite survives. The LCD delete owes **both** calls, in that
order. Builtins (`id >= PROFILE_BUILTIN_ID_BASE`) are a `const` table with no
writable storage — the row must not offer Delete for them at all, matching the
web page's exportable-but-not-deletable rule.

**Layout — four 64px rows, per the owner's decision (6.8 item 5).** Four rows
fit the 268px budget, but only exactly, and only with the whole content column
given over to rows:

```
UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE      4
UI_PAGE_PROFILE_PICKER_ROW_H_PX           64
UI_PAGE_PROFILE_PICKER_ROW_GAP_PX         (UI_THEME_PADDING_PX / 2)         /* 4 */
UI_PAGE_PROFILE_PICKER_WORST_CASE_HEIGHT_PX =
    (4 * 64) + (3 * 4) = 256 + 12 = 268   <=  268
```

**Zero slack.** 268 equals the budget exactly, so the `_Static_assert` passes
(`<=`) and any later addition to this page's content column — a header, a
status line, a wider gap — fails the build. That is the intended behaviour, not
a problem to route around: this page has no room left and the compiler now says
so.

What that exactness costs, stated plainly so it is not discovered later:

* **The page has no header row at all.** The 36px header the 3-row layout used
  is gone. `36 + 4 + 4*64 + 3*4 = 308 > 268`, so a header and four 64px rows
  cannot coexist under any gap arithmetic (even a 0px gap leaves
  `36 + 4*64 = 292 > 268`).
* **So the New button moves to the topbar**, as a second icon slot beside the
  gear on the Profiles page. The topbar sits above the content column and is
  already subtracted out of `UI_THEME_PAGE_CONTENT_BUDGET_PX`, so it is free
  height — it is the only place left that is. This is still "at the top of the
  screen". `ui_topbar_create()` already sizes its icon row from `icon_count`,
  so this is one more `build_icon()` call, not new layout machinery; with 6.6's
  `LV_FLEX_ALIGN_END` in place the gear stays flush right and New lands
  immediately to its left. **Consequence: 6.2 now edits `ui_topbar.c` and
  therefore depends on 6.6** — see the revised wave order in 6.7.
* **The "N of M" page indicator moves into the topbar title** (title string
  becomes e.g. `Profiles 1/2`) rather than a content row, for the same reason.
  Paging itself is unchanged: `ui_topbar_set_prev_enabled()` /
  `_set_next_enabled()`, exactly as `ui_page_profiles_mine.c` does today.
  Page count is `ceil(total_visible / 4)`.
* **Rows are 64px drawn with no touch-area extension, 8px under
  `UI_THEME_MIN_TOUCH_TARGET_PX` (72).** This is a real deviation from the
  touch policy and follows directly from the 64px decision. Do **not** try to
  recover it with `ui_theme_apply_touch_area(row, true)`: the compact extension
  is `UI_THEME_PADDING_PX / 2` = 4px per side, which would reach 72px effective
  only by consuming the entire 4px inter-row gap from both sides at once, so
  two vertically adjacent rows' hit-boxes would overlap. That is precisely the
  mis-tap defect documented and fixed on the relay-life page
  (`ui_page_diagnostics.c`'s `UI_PAGE_DIAGNOSTICS_RELAY_LIFE_ROW_GAP_PX`
  comment), and re-creating it under a Delete button would be worse there than
  it was there. 64px rows, plain hit-boxes, no extension.

New button: 36x36 topbar icon with `LV_SYMBOL_FILE` (no new colour —
`UI_THEME_COLOR_ACCENT_4`, as Start already uses), calling
`ui_page_profile_builder_start_new()` then
`kiln_ui_show("profile_builder_zones")` — the same two calls
`new_profile_nav_cb()` makes today.

**The picker (6.1) uses the identical 4x64 column** and never had a header, so
opening it over the dashboard changes nothing about this arithmetic.

Row internals (464px wide, 64px tall): star/name label `flex_grow(1)` with
`LV_LABEL_LONG_DOT`, then a 64x36 Delete button right-aligned and vertically
centred, for user slots only. Name width is `464 - 8 (pad) - 4 - 64 = 388`
with Delete, 452 without. The row's own vertical padding is
`(64 - 36) / 2 = 14` above and below the Delete button; the name label is a
single montserrat_14 line (20px) centred in the same 64px.

**Tests owed.**

* New pure module `ui_profile_list_order.c/.h`, host test
  `firmware/KilnFW/App/test/test_ui_profile_list_order.c`. It **must be added
  to the hardcoded list in `build_host_tests.ps1`** (beside
  `test_ui_page_home_graph.c`, around line 103) — that script does not glob.
  Assertions mirroring `test_profile_favorites_order.js`: (a) favorites first
  and intra-group order preserved; (b) output is a set-equal permutation of
  the input, nothing dropped; (c) the star appears in the label only and the
  row's id is unchanged; (d) a builtin id never yields a deletable row.
* `check_ui_budget_asserts.ps1` must gain an entry requiring
  `ui_page_profile_picker.c`'s `_Static_assert`, and must lose the
  `ui_page_profiles_mine.c` entry when that file is deleted — the script greps
  exact literals, so a stale entry fails the build. The literal the new entry
  pins is:
  `_Static_assert(UI_PAGE_PROFILE_PICKER_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX, ...)`
  with the same "split across more pages, don't scroll" message shape the other
  pages use. No existing entry's text changes: 6.3 grows a macro the
  `ui_page_temperature.c` assert already references, and 6.4 removes a control
  shorter than its row, so both of those asserts keep their current literals.
* Negative-test both: flip the partition to unstable and confirm the host test
  goes red; bump the picker's row count to 5 and confirm the `_Static_assert`
  fires (at 4 rows the page sits exactly on the budget, so a single extra row
  is enough to prove the check is live). A check that has not been seen failing is vacuous.

**Numeric verification.** With a known favorite set, capture and sample the
first row's star glyph cell. With 64px rows and 4px gaps, row N's vertical
centre is LCD `y = N*68 + 32`, so row 0 is LCD `(16, 32)`, frame `(132, 73)`,
and row 3 is LCD `(16, 236)`, frame `(132, 461)`; sample `-W 10 -H 10`. A lit
star is `UI_THEME_COLOR_TEXT_PRIMARY` `0xf0f0f0` against `CARD` `0x242a3a`, so
the mean RGB of a favorite row's star cell must be measurably brighter than the
same cell on a non-favorite row. Judge this numerically only.

### 6.3 Temperature page also shows the safety relay's state

**File.** `ui_page_temperature.c` — the "Relays" card builder and its refresh
tick.

**Data source and locks.** `dashboard_status_t`'s `safety_relay_known` and
`safety_relay_energized` (`dashboard_http.h`), already populated from the
existing safety-link status on the poll the page performs. **No new poll of
the Pico.** Render `safety_relay_known == false` as `n/a`, never as `off` —
unknown and de-energized are different states. Do **not** use
`safety_heating_enabled` for this; that header warns it means
`SAFETY_FLAG_ENABLED`, not "heat was granted", and reading it as the latter
already shipped as a bug once (`danger_mode.c`, 2026-08-27).

**Layout.** Today's worst case is `3*28 (zone rows) + 70 (Relays card) + 20
(msg) + 5*4 (gaps) = 194` against 268. Add one line inside the existing Relays
card rather than a new card:

```
UI_PAGE_TEMPERATURE_SAFETY_LINE_PX  (UI_THEME_FONT_LINE_HEIGHT_PX + (UI_THEME_PADDING_PX / 4))   /* 22 */
```

added to `UI_PAGE_TEMPERATURE_RELAYS_CARD_HEIGHT_PX`, giving `194 + 22 = 216
<= 268`. The existing `UI_PAGE_TEMPERATURE_WORST_CASE_HEIGHT_PX` assert then
recomputes itself; its literal text is unchanged, so
`check_ui_budget_asserts.ps1` needs no edit. Label text follows the web's own
wording in `renderSafetyCard()` and the page's 0-based display convention:
`Safety (K4): ON / off / n/a`.

**Tests owed.** `test_safety_relay_pill.js` pins the web rendering of the same
three states; the LCD string formatter should be a small pure helper
(`ui_page_temperature_safety_text(known, energized)`) with a host-test
assertion for all three, so the two surfaces cannot drift apart silently.

**Numeric verification.** Sample the safety line's text cell with the relay
de-energized and again energized; the ON state uses the existing
`UI_THEME_COLOR_ACCENT_4` `0x5cc06e` and off uses `TEXT_SECONDARY` `0x9aa0ae`
— no new colour. Require a measured channel difference, with a bezel reference
sampled in the same run.

**Bench verification 2026-09-19 (partial — off state only).** Board flashed
from a clean worktree at `73c1da94` and photographed with
`capture_lcd.ps1`; regions sampled numerically (ffmpeg `rawvideo`/`rgb24`),
never judged by eye. The line renders as `Safety (K4): off`. Mean RGB of the
brightest 12% of pixels: label cell `(164.7, 205.2, 220.3)`, the `off` word
`(163.0, 199.3, 217.3)`, card background `(177.2, 213.3, 215.1)`, black-bezel
reference in the same frame `(0.0, 0.0, 4.5)`. The `off` word's green channel
is *below* its blue channel (199.3 < 217.3), so it is not being drawn in
`ACCENT_4` `0x5cc06e` — the de-energized colour is correct. **The ON state was
not exercised:** energizing K4 means granting heat enable, which is out of
scope for a no-heat bench task, so the ACCENT_4 half of this check is still
owed. Note also that the safety line sits inside a specular glare band from
the bench lamp, which inflates all three channels and rules out an absolute
match against `0x9aa0ae`; only the channel *ordering* above is load-bearing.

### 6.4 The LCD loses the ability to reset relay life — done 2026-09-19, commit dcfadd79

Bench-verified 2026-09-19 on firmware built from `73c1da94`: Diagnostics page
7 of 8 ("Relay Life") shows five display-only rows (Relay 1-4, Safety (K4))
and no control below them. Numerically, on an 853x578 capture the text rows
read top-12% luminance 212.1 (Relay 1) and 213.3 (Safety (K4)), while the
whole area below the last row reads top-12% 143.5 with a peak of 179.1 —
below even the dimmest glyph — i.e. nothing is rendered there. Black-bezel
reference in the same frame: mean RGB `(1.9, 0.0, 0.4)`. Source side,
`ui_page_diagnostics.c` contains no reset control; its only `Safety (K4)`
reference is the display-only `build_relay_life_row()` call.

### 6.5 Right quarter of the dashboard: relays, zone temperatures, zone power

**Files.** `ui_page_home.c` (`ui_page_home_build()`, wrap the chart in a new
row container), `ui_page_home_internal.h` (declare the rail's shared widgets),
`ui_page_home_refresh.c` (`ui_home_refresh_cb()`, fill it), and a new pure
`ui_page_home_rail.c/.h` for the row-composition logic, following the
`ui_page_home_graph.c/.h` split that exists precisely so layout logic is
host-testable off-target.

**Data source and locks.** Everything comes from the two snapshots the tick
already has — relays from `ds.relay_on[KILN_IO_RELAY_COUNT]`, temperatures and
staleness from `ds.channels[]` (`temp_c`, `valid`, `stale`), per-zone power
from `st.zones[].duty`, whole-kiln watts from `ds.power_w` gated on
`ds.power_valid`. Zone naming reuses the existing `zones_config_*` accessors
the page already calls. **No new producer call and no new task, so nothing is
owed to `check_stack_margin_registration.ps1`.**

**Layout.** Today the chart is a direct child of `content` with `lv_pct(100)`
and `flex_grow(1)`. Insert a `graph_row` (`LV_FLEX_FLOW_ROW`, `lv_pct(100)`,
`flex_grow(1)`, `pad_gap UI_THEME_SPACE_1` = 4, non-scrollable) and move the
chart into it at `lv_pct(74)`, with the rail at `lv_pct(25)`:

```
row inner width     = 464
chart               = 0.74 * 464 = 343
rail                = 0.25 * 464 = 116
gap                 = 4          (343 + 4 + 116 = 463 <= 464)
```

Vertically the row inherits the chart's old height, which on the idle page is
`268 - 36 (action row) - 4 (gap) = 228px` (the trip strip, lag notice and
progress wrap are hidden and cost zero). Rail contents, at 108px inner width
after its own 4px padding:

```
"Relays" caption   (montserrat_10)                 14
gap                                                 4
4 relay pills, 24px wide, 4px gaps: 4*24+3*4 = 108  22
gap                                                 4
3 zone blocks, each
     zone name (montserrat_10)          14
     temperature (montserrat_14)        20
     duty bar                            8
     2 internal gaps                     4
                                     = 46          138
2 inter-block gaps                                  8
gap                                                 4
kiln power line (montserrat_10, hidden unless
     ds.power_valid -- zero height when hidden)     14
                                        TOTAL     208   <= 228
```

20px of slack. Pin it with a new `UI_PAGE_HOME_RAIL_WORST_CASE_HEIGHT_PX`
`_Static_assert` in `ui_page_home.c` against the same 228px expression
(derived from `UI_THEME_PAGE_CONTENT_BUDGET_PX`, not a literal) and add that
file to `check_ui_budget_asserts.ps1` — `ui_page_home.c` has no entry there
today because the chart was pure `flex_grow`; a fixed-height rail changes that.
Colours are the existing relay and zone accents already used by the legend and
the temperature page; **no new colour is defined**.

**Style parity with the web.** `relayStatusHtml()` prints `R<n-1>: ON / off /
n/a`, and `renderChannels()` shows zone name, temperature, a stale marker and
`(duty NN%)`. The rail is the same information set with the graphic dropped,
and the same 0-based relay display convention (`UI_RELAY_DISPLAY`) the LCD
already uses.

**Tests owed.** `test_ui_page_home_rail.c` (new, register it in
`build_host_tests.ps1`) over the pure composition helper: a stale channel
renders the stale form rather than a plausible stale number; an invalid channel
renders `--.-` and never `0.0`; `power_valid == false` hides the kiln power
line entirely rather than printing `0 W`; duty clamps to 0..100.
Negative-test each by inverting the condition under test.

**Numeric verification.** Sample the rail's left edge, LCD `x=352`, against the
chart's own area at LCD `x=300`, both at LCD `y=120`, giving frames `(767,
240)` and `(669, 240)`: the rail must read `CARD` while the chart area reads
the chart's own background, proving the 3/4 split landed where the arithmetic
says. Then sample a relay pill at LCD `(360, 40)`, frame `(782, 88)`, with the
relay commanded on and off and require a measured difference. Bezel reference
in every run.

### 6.6 Dashboard Settings button flush top-right — done 2026-09-19, commit dcfadd79

Bench-verified 2026-09-19 on firmware built from `73c1da94`. Full-frame
capture of the dashboard, luminance column scan across the topbar band: the
gear glyph occupies frame x 1090-1111 (peak luminance 213) and the panel's
right edge is at frame x ~1147; columns 1112-1146 are flat background
(104-115) with no second glyph, so the gear is the rightmost rendered element
and sits within the topbar container's own right padding. Scaling by the
measured panel width (298-1147 for 480 LCD px, 1.769 px per LCD px) puts the
glyph centre at LCD x 453.7, matching the firmware's own tap-target dump
centre of 453.5 exactly. The hidden warning indicator's slot (LCD x 396-431,
frame 998-1060) reads flat background in the same scan, confirming it
collapses to zero width without pushing the gear off the right edge.

### 6.7 Parallelisation and file collisions

Wave 1 — four implementers, zero shared files:

| Worker | Item | Files owned |
| --- | --- | --- |
| A | 6.6 | `ui_topbar.c` |
| B | 6.4 | `ui_page_diagnostics.c` |
| C | 6.3 | `ui_page_temperature.c` |
| D | 6.2's pure half | **new** `ui_profile_list_order.c/.h`, **new** `test_ui_profile_list_order.c`, `build_host_tests.ps1` |

Wave 2 — after D lands (both consume its ordering helper), and E additionally
after A, because the owner's 4x64 row decision moved the New button into the
topbar (see 6.2):

| Worker | Item | Files owned |
| --- | --- | --- |
| E | 6.2 | **new** `ui_page_profile_picker.c/.h`, `ui_page_profiles.c`, delete `ui_page_profiles_mine.c` / `ui_page_profiles_family.c`, `kiln_ui.c`, `check_ui_budget_asserts.ps1`, and `ui_topbar.c` (one extra icon slot) **after A** |
| F | 6.5 | `ui_page_home.c`, `ui_page_home_internal.h`, `ui_page_home_refresh.c`, **new** `ui_page_home_rail.c/.h` |

Wave 3 — 6.1, which needs E's picker page *and* F's home-page edits.

**Collisions, stated explicitly.** 6.1 and 6.5 both edit `ui_page_home.c`,
`ui_page_home_internal.h` and `ui_page_home_refresh.c` — give them to the same
implementer (F, sequentially) rather than merging two worktrees over the same
three files. 6.1 and 6.2 share the list widget and the ordering helper; D
isolates that into files neither of the others owns. `build_host_tests.ps1` is
touched by D and by F (its new rail test) — order those two edits, or let F
append after D lands. `check_ui_budget_asserts.ps1` is touched by E (picker
entry) and F (home entry); same treatment. `ui_topbar.c` is touched by A (6.6's
alignment fix) and now also by E (6.2's New icon), because four 64px rows leave
no content height for a page header — A must land first, and E's icon addition
is then a one-line `build_icon()` call on top of A's corrected alignment. A and
E are the only two items that touch that file.

Nothing in waves 1-3 touches `main_page.html`, any HTTP handler, any
`httpd_uri_t` registration, or any task creation.

### 6.8 Owner decisions — five answered 2026-09-19, plus a sixth owner rule

These are **decided**, not open. They are recorded here because the arithmetic
and the wave order above depend on them.

1. **Rail power readout (6.5): per-zone duty %, plus one kiln-total watts line
   shown only when `ds.power_valid`.** As recommended. The watts line is hidden
   when `power_valid` is false and costs zero height when hidden, which is how
   6.5's 208px total already budgets it.
2. **Profile name while a firing runs (6.1): greyed and non-clickable whenever
   the executor snapshot is not IDLE.** As recommended — render with
   `UI_THEME_COLOR_TEXT_SECONDARY` and clear the clickable flag. Changing the
   profile under a running firing stays out of scope.
3. **Restore hidden is web-only.** Hidden builtins do not appear in the LCD
   list at all; `profiles_builtin_restore_all()` remains reachable only from the
   web page.
4. **LCD delete uses the two-tap arm/confirm idiom with a roughly 5 s window.**
   The same pattern 6.4 removes from the relay-life page. No modal, no new page,
   no new colour.
5. **Four rows per page at 64px, with prev/next paging — the owner overrode the
   3x72 recommendation.** It fits, exactly: `4*64 + 3*4 = 268`, equal to
   `UI_THEME_PAGE_CONTENT_BUDGET_PX`. Three consequences follow and are already
   written into 6.2 and 6.7 — the page loses its header row entirely (a header
   plus four 64px rows is `36 + 4 + 256 + 12 = 308 > 268`, and even at a 0px
   gap `36 + 256 = 292 > 268`); the New button and the "N of M" indicator
   therefore move into the topbar, which makes 6.2 depend on 6.6; and rows are
   64px drawn with **no** touch-area extension, 8px under
   `UI_THEME_MIN_TOUCH_TARGET_PX`, because the compact extension would need the
   whole 4px inter-row gap from both sides and would let adjacent rows'
   hit-boxes overlap — the mis-tap defect already fixed once on the relay-life
   page. The 8px touch-target shortfall is the accepted cost of this decision.

6. **The dashboard's `Kiln: <name>` line is hidden when the board holds only
   one kiln configuration — owner rule 2026-09-19** ("if there is only one
   kiln config on the board do not show it on the lcd"). Owned by wave F
   (6.5), since it lives in `ui_page_home_refresh.c`'s status line: count the
   slots with `kiln_cfg_store_list()` (a RAM read, safe on the 1 Hz LVGL
   tick) and append the `  Kiln: %s` suffix only when that count is two or
   more. With zero or one config the suffix is omitted entirely — not
   `(none)`, not the single name — and the freed width goes to the rest of
   the status text. The web dashboard keeps showing the active name
   regardless. Host test: the status-line formatter with counts 0, 1, 2 and
   an active id set; the two-or-more case must still name the active config.
