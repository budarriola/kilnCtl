# ROADMAP status log, 2026-08 through 2026-09-28

Moved verbatim out of `ROADMAP.md` on 2026-10-02 so the roadmap states current status only.
History, not a plan: nothing here is maintained. Part 1 is the dated status log that used to
head `ROADMAP.md`. Part 2 is every index row that pass removed (finished work) or replaced with a
shorter current-state row, in their original wording. Where the two disagree with the live
`ROADMAP.md`, the roadmap is right.

## Part 1: status log

> **Status:** planning · **Last reviewed:** 2026-09-28. `04fb2afe` fixed
> `GET /api/zones`/backup export rounding PID gains and coupling cells to
> `%.4f` (small `Ki` values printed as zero); they now print at `%.9g`, and
> OTA-bench PID comparisons are tolerance-based. `cc71612d` closed the
> guard-disable-ack-gate item by owner decision (thermal protection gets no
> disable switch, ever) and recorded HP-07's 2026-09-28 bench rerun as a
> PASS. `49b32a24` fixed the `tuning_valid` relative-tolerance gap. `d484e51a`/
> `f017b285` landed the "Edit firing" button next to Start/Stop on the web
> (opens `/live_profile`, login modal first). `fd792793` landed hiding the
> web dashboard's "previous firing ended" card except for an interrupted or
> faulted run. `c2240cb1`/`389275a5` landed a 5-degree minimum vertical span
> on every LCD and web temperature graph; `e6d7db5f`/`3d90c8eb` landed
> non-repeating web y-axis tick labels. **Landed 2026-09-28:** the three
> owner login-gate requests — unauthenticated web and LCD users now see
> dashboards only (`48962395`/`d25d5ccf`, plus `5901b09d`'s doc fixes;
> `/api/profile_exec/stop` moved to USER tier, `/api/board_temps` and
> `/api/firing_history` to ADMIN, and a `59e84b57` follow-up moved the other
> three former SAFETY_REDUCE routes — `current_sweep/abort`, `autotune/abort`
> and `diagnostics/danger/stop` — to ADMIN too (the physical E-stop stays
> the unauthenticated backstop for all of them; `ROUTE_TIER_SAFETY_REDUCE`
> itself is left in the enum, unused), and the LCD PIN
> gate (`89fbe6d6` requires the PIN before Stop, Pause/Resume, Menu and the
> profile picker; relock returns home; shares the web login's
> `login_backoff` lockout ladder; `e4c5d7da` fixed the keypad's lockout text
> to state the actual wait, "Wrong PIN -- try again in Ns"). `/wifi`,
> `/networks` and `/scan` are now open only while unprovisioned, ADMIN once
> provisioned (`59e84b57`/`f3991c09`, owner decision). `057ca631`/
> `c26df2ce` landed the divergence-triggered stop recording FAULTED with a
> reason (was HALTED with an empty reason). `c22ff081`/`5d0a2756`/
> `7b107411` landed `backup_import`'s NVS-save batching (single zones save,
> whole-snapshot rollback on a mid-batch failure) plus Pico-ceiling tracking
> after import/rollback. **In progress:** the LCD "edit current firing"
> screen is in Opus review; Wi-Fi AP fallback with auto-reconnect to the
> home network when no web users are logged in is being implemented.
> **Pending bench work:** flash the landed firmware above; verify the login
> gates on real hardware; bench-verify the live-edit feature end to end. **LCD end-of-run decision
(2026-10-01, flashed and bench-verified 2026-10-01: LCD-22..LCD-25 all PASS, run `20261002T061918Z_lcd_lcd22_25`):** the home "Keep?" button and the Edit
firing page open a PIN-gated Discard / Save as / Overwrite page that calls the
same `profiles_live_decide_apply()` the web decide route uses; LCD bench case
LCD-25 written and PASS (Discard edit path; Save as/Overwrite not exercised).
> **Bench rerun sweep, 2026-09-30 (`4de7b489`):** heat suite HP-01..08 reran
> clean 8/8 PASS on the dwell-fault-panic and same-zone start-race fixes
> (`773ec669`, `540b2d72`); `backup_import` round trip re-measured at 0.80 s
> post the NVS-save batching fix (`c22ff081`/`5d0a2756`/`7b107411`), down
> from ~61 s. LCD-19 reran still INCONCLUSIVE and a new LCD-16 nav anomaly
> (`click_by_name('settings')` -> `not_found`) was flagged; both under
> investigation, not yet root-caused.
> **2026-09-27:** the system-mode gate's last deferred slice (2, "recovery
> wording") landed: `system_mode_
> gate_check()` now runs inside `profile_executor_run()`'s and
> `autotune_begin_run_locked()`'s choke points and inside their two HTTP
> call sites, so a start refused for recovery mode reads the same string on
> HTTP/UART/LCD alike; `App/drivers/http/recovery_start_refusal.h` (the
> HTTP-only wording it retired) is deleted. All five `docs/
> SYSTEM_MODE_GATE_PLAN.md` slices are now LANDED — 2026-09-25 landed
> zone-config writes, factory reset, and cfgfs format (plan slices 4/5, on
> top of plan slice 3's manual relay writes), two new bench MCP tools
> (`control_set_zone_limits`, `safety_get_unset_commissioning_params`)
> landed, and a further LCD-01/LCD-19 harness/firmware review-fix chain
> landed (thirty-ninth sweep) — open items below.
> - **Safety fault-line starved-ack fix, 2026-09-27 — LANDED:** `escalate_guard_
>   trip()` (`profile_executor_relay_io.c`) asserts the isolated hardware fault
>   line, and the very next thing that happens is `profile_executor_halt()` ->
>   `clear_this_runs_faults()` deasserting it again, sometimes milliseconds
>   later — fast enough that the RP2040 safety processor's own S6a mainFault
>   debounce (`SAFTYFW_MAIN_FAULT_DEBOUNCE_MS`, 200 ms) could see the line drop
>   before its debounce window finished counting, starving the backup
>   processor's own latch of the trip KilnFW had just decided to escalate.
>   Fixed in `safety_link.c`/`.h`: a fault source that has been asserted must
>   now stay asserted for `SAFETY_FAULT_MIN_HOLD_MS` (300 ms = the Pico's
>   debounce plus a 100 ms margin) before a deassert request actually takes
>   effect; a deassert requested before that elapses is deferred and finished
>   later by the existing `safety_poll_task()` tick
>   (`safety_link_service_pending_fault_deassert()`), never by blocking the
>   caller. A re-assert during the hold cancels the pending deassert. External
>   status reporting (`safety_link_get_fault_sources()`, `GET` endpoints, link
>   frames) needed no change: a bit pending deassert is deliberately still
>   counted asserted in `fault_sources`, so every existing reader keeps seeing
>   the true physical line state. Host-tested (assert/immediate-deassert/
>   still-high, hold-elapses/releases, re-assert-cancels, already-past-hold-is-
>   immediate, per-bit independence) in `test_safety_link_compile.c`; the two
>   independently-built firmware targets' constants are tied together by
>   `firmware/KilnFW/App/test/safety_fault_hold_mirror_drift_check.py`, which
>   fails loudly if either constant changes without the other being revisited.
> - **System-mode command gate, 2026-09-25 — owner decisions recorded, slices
>   2/3/4/5 LANDED, review fixes applied:** `docs/SYSTEM_MODE_GATE_PLAN.md`, answering
>   `firmware/KilnFW/TODO.md` 10.14's Phase 6 (a single owning function/table
>   deciding whether a *class* of command is allowed given current system mode
>   — firing/autotune/OTA/recovery/safety-tripped/readiness — composed with,
>   never replacing, the existing owner locks, the OTA/profile mutual
>   interlock, and `readiness_gate.h`'s firing checklist, which already
>   unifies profile/autotune start refusals across HTTP/UART/LCD). `system_mode_
>   gate.h`/`.c` landed with a host test (negative-tested), and
>   `SYS_ACTION_RAW_RELAY_DEBUG_WRITE` is wired into `kiln_io_owner.c`'s single
>   `relay_on_blocked()` choke point: owner decision was **blanket refusal**
>   (any relay, not scoped to what the run claims) of manual relay-ON while a
>   firing or autotune session is active, enforced on all three transports —
>   HTTP's danger-mode relay route (409 Conflict), UART's
>   `SET_RELAY`/`SET_RELAY_MASK`, and the LCD's manual override.
>   **Review fix, same day:** `danger_mode_request_start()` only refused an
>   active profile, not autotune, so danger mode + autotune bypassed the gate
>   over every transport; `relay_on_blocked()` now runs the mode gate even
>   when danger mode is active — the one gate danger mode does not skip —
>   correcting the original claim that this refusal was "not reachable via
>   HTTP": it is, and refuses with 409. A second review fix moved the
>   profile/autotune-running read behind a leaf getter
>   (`relay_authority_heat_run_active()`, under `s_heat_claim_mux` only, no
>   blocking call inside) to remove a lock-order cycle between `owner_task`
>   and `profile_executor`/`autotune_engine`'s own tick locks (each tick holds
>   its own module lock across a call back into `kiln_io_owner.c`, which used
>   to wait on that same lock from `owner_task`). **Slices 4/5 landed same
>   day (not slice 2 -- see correction below):** zones/config writes are now
>   refused ALL (not scoped) while a firing or autotune session runs, gated
>   at the caller in `zones_http_post.c`, `uart_bridge_ext_control.c`
>   (`SET_ZONE_PID`/`SET_ZONE_MODEL`), `kiln_cfg_http.c` (apply, at submit
>   time), and `backup_import.c` (at the top of
>   `backup_import_post_handler()`, ahead of `ota_http_check_interlocks()`
>   -- the original `"MODE_GATE_REFUSED:"` sentinel-through-`err_msg` design
>   was replaced during review, 2026-09-25, once the ordering bug below was
>   found) — never the `zones_config` accessors themselves, so
>   autotune/adaptive_tune's own direct writes are untouched (host-tested
>   cross-product in `test_system_mode_gate.c`, plus dedicated regression
>   tests proving autotune's own writes are NOT gated). Factory reset
>   (`factory_reset.c`, after auth, plus the UART-exclusive
>   `factory_reset_execute()` entry point) and cfgfs format
>   (`cfg_fs_format_http.c`, first line of the handler) now refuse outright
>   while running. All four HTTP refusals share one sender,
>   `system_mode_gate_http_send_refusal()` (slice 4), a 409 distinct from the
>   OTA interlock's 428. **PcTools clients now distinguish this 409 from
>   OTA's 428, 2026-09-25:** `zones_http_client.is_system_mode_gate_refusal()`
>   matches the reason substring ("firing or autotune run is active") common
>   to every `system_mode_gate_http_send_refusal()` body; `control_set_zone_limits`,
>   `cfgfs_format`, and `kiln_config_apply` each surface it as an explicit
>   "refused: system_mode_gate refused..." result distinct from their own
>   other 409s and from OTA's 428, with dedicated fake-HTTP unit tests. (No
>   HTTP `factory_reset` MCP tool exists in PcTools today -- only a UART
>   `system_factory_reset()` -- so that item is N/A here, not overlooked.)
>   **Review fix, 2026-09-25 (dead-code ordering):** in `zones_http_post.c`,
>   `kiln_cfg_http.c`, and `backup_import.c`, the mode gate used to run
>   AFTER `ota_http_check_interlocks()`, which answers first while a firing
>   is active and made the mode gate's own 409 unreachable in that state --
>   fixed by moving the mode-gate check to run first (interlock, then
>   `http_async_job_busy()`, land after it), with a handler-level test
>   proving the ordering in `test_zones_http.c`
>   (`test_zones_post_refused_by_mode_gate_before_interlock`).
>   **Additional zone writers gated, 2026-09-25 (same review pass):**
>   `POST /api/zones/pid` (`zones_http_pid.c`) -- revoking its prior
>   deliberate carve-out that allowed PID-only edits even while a firing was
>   RUNNING/PAUSED, per owner decision Q2 (refuse ALL zone/relay/guard
>   config writes, not scoped to which field changed); `iter_tune_http.c`'s
>   `restore_commissioned`; and `adaptive_tune_http.c`'s `enable`/`revert`
>   handlers. Autotune/adaptive-tune's own internal accept-path writes
>   (calling `zones_config_set_*()` directly while a run IS active) stay
>   ungated by design.
>   **Narrowed, 2026-09-25 (later same day, owner decision):**
>   `adaptive_tune_http.c`'s enable handler now gates only turning adaptive
>   tune ON -- turning it OFF during a run is allowed, since it can only
>   PREVENT a future change, never apply one. The revert handler is
>   unaffected and still refuses unconditionally while a run is active.
>   **2026-09-28: further gaps closed.** `adaptive_tune_http.c`'s
>   `enable`/`revert` handlers now have their own host-test executable
>   (`test_adaptive_tune_http_gate.c`), and the UART
>   `factory_reset_execute()` path now has a host test in `test_ota_http.c`
>   proving the `FACTORY_RESET_ERR_MODE_GATE_REFUSED` return-code contract
>   `uart_bridge_system.c`'s mapping depends on. Separately, PcTools'
>   `factory_default_then_load_preset()` used to treat a still-live UART
>   link as proof a factory reset succeeded (`get_fw_version()` is a query,
>   answered whether or not the board ever rebooted) -- fixed via
>   `InfoClient.wait_for_boot_push()`, which waits for the device's own
>   unsolicited boot-time push and reports "refused" on a timeout. And the
>   96-byte mode-gate reason buffers this plan's handlers added were
>   confirmed already covered by the existing
>   `check_httpd_task_stack_budget.py`/`check_system_uart_bridge_stack_budget.py`
>   pair (each walks its full call graph from one root rather than
>   enumerating buffers by name) -- no new check needed. Full detail:
>   `docs/SYSTEM_MODE_GATE_PLAN.md` §3.6 slices 4/5/7/8.
>   **Closed 2026-09-28** (`a2bc530e`): `uart_bridge_ext_control.c`'s
>   `SET_ZONE_PID`/`SET_ZONE_MODEL` gate now has its own host-test harness,
>   `test_uart_bridge_ext_control_gate.c` -- the gate itself was already in
>   place at `uart_bridge_ext_control.c:99-104`; only the test coverage was
>   missing. Negative-tested (12/24 assertions fail with the gate forced off,
>   restored by hand, full rebuild, 24/24 green, 64/64 host-test executables).
>   **Correction, 2026-09-25 (review):** an earlier draft of this note
>   claimed the recovery-mode HTTP-only-wording slice 2 was "folded into"
>   the same-day zones/config work above -- that was false; slice 2 is a
>   separate, still-open item (recovery-mode command wording, not the
>   zones/config refusal rule) and has not been done. See the plan doc's §5
>   for the full owner decisions and §3.6 for per-slice status.
> - **`safety_get_unset_commissioning_params` MCP tool landed, 2026-09-25**
>   (`397208ba`, review fixes `2d38f97f`): a READ-ONLY MCP tool re-deriving
>   `readiness_http.h`'s commissioning-required exclusion rule so the
>   `safety_commissioned` readiness item's count names which
>   `safety_cfg_store` params are actually unset, rather than just a count.
>   First live bench read: six params unset -- `i_normal_a[0..2]`
>   (hardware-gated, no CT sensors wired) and `zone_ct_channel[0..2]`.
>   **Owner decision 2026-09-25: commission these on the bench board --
>   work in progress, not complete.**
> - **`control_set_zone_limits` MCP tool landed, 2026-09-25** (`d80287ee`,
>   review fix `28dc49b7`): a narrow zones-limit writer touching only one
>   zone's `max_temp_c`/`min_temp_c` via the existing GET-merge-POST path,
>   refusing without `confirm=True` or while a profile/autotune is
>   running/paused/active, validating zone index and min<max, and reading
>   back afterward to catch both a value that didn't land and any
>   collateral change to another field. The review fix dropped a
>   client-side `abs_max` gate that blocked every legitimate ceiling raise
>   (this firmware *derives* the Pico's `abs_max_temp_c` from the zone max,
>   it is not an independent cap) and instead reports the derived
>   `safety_ceiling` from `GET /api/zones` with a WARNING on mismatch.
> - **LCD-19/LCD-01 bench-runner review fixes, 2026-09-25** (`d4e7ff29`,
>   `f40e8d37`): stray-overlay detection now checks the current page before
>   acting on a leftover OK/Cancel overlay (was clicking/backdrop-tapping
>   home-page widgets from other pages), an empty tap-target listing is
>   never treated as a real answer, and PIN-keypad detection/backdrop-tap
>   handling were fixed. **LCD-01 PIN keypad firmware fixes, 2026-09-25**
>   (`0ef18917`, `466b29b2`): fixed a real key-height regression (keys were
>   rendering at ~31 px against the >=48 px the static assert claimed, from
>   an inherited LVGL card style) and moved an LVGL call off the UART
>   bridge task onto `lvgl_port_task`. **Not yet flashed to the bench** --
>   ESP is still `111b1b6f`.
> - **SET_FIRING_CEILING (0x09) reverted, 2026-09-24 owner decision:** "the safety limits should be the same[,] the safty processor is a backup incase the esp fails" -- the level-triggered resend (`6f8ed940`, `34f242c3`) and the Pico-side `min(abs_max_temp_c, firing_max_c + firing_margin_c)` tightening in `safety_guards.c` are removed; S1's ceiling is now unconditionally `abs_max_temp_c`, resolving the "standing tension" this entry used to flag against the "Pico ceiling never tighter than the ESP's abs_max" rule. The cool-down-on-hot-kiln start-time check
>   (`5840a82c`/`341739ab`) existed only because the Pico tightening could trip a cool-down-only profile started on a hot kiln; with the tightening gone that hazard is gone, so the check was removed in the same revert (`CommonFW/docs/LINK_PROTOCOL.md` §0x09). The generic LCD "Cannot Start" modal it used is pre-existing and stays. `check_firing_ceiling_margin_mirror_drift.ps1` and its Python check (mirrored the now-removed ceiling margin, not the start-time check) were deleted along with the ceiling code.
> - **`exec_mode_state_check()` violation now latches FAULTED instead of
>   rebooting** (`docs/audits/profile_executor_panic_2026-09-24.md` item 4):
>   `002e71bd` replaces the target-build hard `assert()` at that call site with
>   `exec_handle_mode_state_violation()` — logs the violated rule, forces
>   FAULTED with heaters off via the same sequence `escalate_guard_trip()`'s
>   GLOBAL branch uses, and continues the task loop; a per-run latch stops a
>   persisting violation from re-firing, and a lifetime counter plus the latch
>   are reported over the existing `GET /api/profile_exec` route (no new URI
>   handler). Host/debug builds keep the original hard assert. `118beb79`
>   review-fixed it to call `exec_enter_terminal_state()` (rather than an
>   inlined clear), added the missing `io_segs_force_all_off(false)` every
>   other FAULTED path already does, and preserved an already-FAULTED run's
>   guard-trip fault reason.
> - **`exec_enter_terminal_state()` zone-active clear narrowed to IDLE only**:
>   `4012f8c7` first routed `profile_executor_halt()` through the helper and
>   made it clear every zone's `active` flag on every terminal transition
>   (FAULTED/DONE/IDLE); `fe938ef3` found that too broad — `force_all_relays_off()`,
>   `firing_stats_maybe_finalize()`, `profile_executor_get_status()`, and
>   `clear_this_runs_faults()` all iterate active zones after a FAULTED/DONE
>   transition, so clearing `active` there made them no-ops or dropped data.
>   The clear now runs only on the IDLE transition (i.e. `halt()`), which
>   already runs all of the above first.
> - **Autotune now aborted on a rule-1 mode-state violation** (`5821eeec`,
>   review-fixed `933a7eec`): `exec_handle_mode_state_violation()` forced the
>   run FAULTED and released its own relay claim, but left a driving autotune
>   session's separate claims (`RELAY_OWNER_AUTOTUNE`,
>   `HEAT_ENABLE_CLAIMANT_AUTOTUNE`) untouched, so autotune kept heating the
>   zone through the fault. It now re-derives rule 1's own "actively driving"
>   condition and calls `autotune_engine_abort()` (`s_at.lock` only, so the
>   `s_exec.lock` -> `s_at.lock` order is preserved) when it holds. The review
>   fix corrected the re-derivation to sample `zones[].active` before
>   `exec_enter_terminal_state()` clears it, since a FAULTED run's zone stays
>   active until IDLE and autotune's own start check
>   (`profile_executor_zone_is_active()`) allows starting one on it. **Open
>   advisory:** autotune-start and profile-start each cross-check the other
>   before taking their own lock, so simultaneous starts can both pass --
>   being fixed in a parallel worktree.
> - **Bench board lock hardened**: `8e696d78` fixes a stale-reclaim race (a
>   third acquirer could win the gap between the lock's rename-aside and
>   put-back) by serializing reclaimers on an `.board_lock.reclaiming`
>   O_EXCL file, and closes a reader-vs-mutating ordering gap by having both
>   sides publish their own claim before checking the other's.
> - **LCD bench runner: click-then-read race and evidence paths fixed**:
>   `93355ee9` retries a swallowed click only when the board is still on the
>   pre-click page (a real page move now fails fast as `wrong_page` instead
>   of tapping a stale target), and gives each captured frame its own
>   filename so LCD-04's before/after frames stop overwriting each other.
>   **Round 2 landed** (`2484b813`): LCD-01 is now diagnostic-only (a cast
>   reference, never a FAIL), LCD-09 pages Prev/Next by tab position rather
>   than name, LCD-16 reports INCONCLUSIVE rather than FAIL on an unchanged
>   tab set, and a theme-mirror drift test was added. **Round 3 in progress,
>   not yet landed:** stale `FRAME_CORNERS` (camera moved again per this
>   file's own 2026-09-24 camera-aim note), LCD-16 Prev/Next paging, an
>   LCD-14 redesign, and cases naming nonexistent widgets.
> - **Swallowed touch tap made observable, 2026-09-24** (`914098d5`, review
>   fix `16ccde78`): `kiln_ui_click_by_name()` now reports
>   `KILN_UI_CLICK_SWALLOWED` directly (GET_STATE also carries power state/
>   swallow count/reason) instead of the PC side only ever inferring a
>   swallow from a page that failed to change; `cases_lcd.py` retries a
>   reported swallow on its own small bounded budget, separate from the
>   existing page-didn't-change retry. **Landed in source, not yet flashed
>   to the bench board.** Follow-up (same worktree, `tapverdict`): a distinct
>   `KILN_UI_CLICK_VERDICT_UNKNOWN` result for the case where
>   `kiln_ui_click_by_name()`'s own bounded wait for the swallow verdict
>   times out (a slow LVGL flush can outrun the old 100ms bound even on a
>   press that landed cleanly) — wait extended to 250ms once confirmed to
>   run on `ui_test_bridge_task`, not the LVGL task; wire-compatible new
>   value threaded through `protocol.py`/`ui_test_client.py`/
>   `ui_test_runner.py`/`cases_lcd.py`, each with its own attribution
>   (neither a pass nor a genuine defect, and never blind re-clicked since
>   the press may have landed -- judged by the page change instead); `judgments.py`'s
>   `BLANKED_SCREEN_HINT` reworded to cover both old and new firmware
>   shapes; a resolved swallow retry count is now recorded
>   in a passing case's `observed` dict instead of discarded.
> - **Forgot-password design replaced with TOTP, owner change 2026-09-24**
>   (`84fa2e7b`): `docs/EMAIL_PASSWORD_RESET_PLAN.md` is renamed to
>   `docs/TOTP_PASSWORD_RESET_PLAN.md` (owner rejected email) — authenticator-app
>   TOTP (RFC 6238, fixed SHA1/6-digit/30s), server-rendered QR enrollment on
>   the settings page, ESP-side mbedtls HMAC-SHA1 verification with replay
>   protection and an SNTP sync gate, and a two-route open-tier reset flow
>   that fits the existing URI handler cap. **Reset-only, not a login second
>   factor.** The existing LCD four-corner physical reset gesture
>   (`auth_reset_gesture.c`) is untouched and stays the independent fallback,
>   and now also disenrolls TOTP on a successful gesture confirm so a gesture
>   reset can never leave an orphaned TOTP secret locking the account. Work
>   tranches: **WT-C landed** (PcTools MCP wrappers `totp_enroll_status`/
>   `totp_reset_password`, review-fixed `c4d752df` -- tool count now 190,
>   plan section 6a specifies 503 on an unsynced board clock and a plain-text
>   429 body); live-board verification still pending WT-A. **WT-A firmware
>   core landed in part** (`0f5151f0`, `totp.c`/`totp_config.c`: RFC 6238
>   core plus NVS persistence, `totp_config_verify_and_consume()` persists
>   the matched replay-counter step before returning OK). **WT-A routes
>   landed 2026-09-24** (`f5f06dee`, two opus reviews, fixes `2d061cb0`
>   `4d32fefd` `9f28c985` `9708015f`): `/forgot` + `/reset`, enrollment,
>   reset tokens cleared on enroll-confirm/disable, board-wide failure cap
>   clears only on reboot. Not yet flashed; live-board verification pending.
>   WT-D (host tests: RFC 6238 Appendix B vectors) landed alongside the core.
>   **WT-B landed** (`62f8bd4e`, gesture follow-up `cafc80f3`): forgot-password
>   flow in the login modal, settings-page enrollment with a client-side QR,
>   `test_forgot_password_modal.js` (48) and `test_qrcode_encoder.js` (17).
>   **Two 2026-09-25 owner decisions landed** (`docs/TOTP_PASSWORD_RESET_PLAN.md`
>   section 8): the credential-wipe ADMIN action now also disenrolls TOTP
>   (secret + reset tokens, same primitives the four-corner gesture already
>   calls, gesture itself unchanged); new TOTP enrollment now refuses 409
>   while web auth is off (`totp_disable` and the forgot/reset routes are
>   unaffected, so an existing enrollment survives an auth-off toggle),
>   settings page shows "Turn on web login before enrolling an
>   authenticator." Not yet flashed; live-board verification pending.
> - [ ] **Follow-on, not started:** `docs/TOTP_LOGIN_2FA_PLAN.md` — optional
>   TOTP as a second factor at ordinary login (reset plan section 5); WT-A has
>   landed, so this now waits only on the owner's review of that plan.
> - **Lazy login pop-up, landed** (`090aaa9f`, review fixes `58e10e11`):
>   serves page shells without redirecting to a login page on load, replacing
>   that with one shared, themed, cancelable login modal instead -- the
>   surface the TOTP plan's "Forgot password?" link above is meant to attach
>   to. An explicit `PAGE_SHELL_URI` allowlist (15 entries,
>   `route_tier_table.h`, `http_auth_is_page_shell_get()`) replaced an
>   any-non-`/api`-GET match so a future untabled GET still fails closed to
>   ADMIN. `58e10e11` then fixed three further gesture-gated gaps: Dashboard
>   Start's `window.confirm()` no longer silently drops the 3 s write window;
>   the long-press PID popup read (fired from a background timer) can now
>   prompt via an opt-in `__kcUserAction` flag; and a cancelled sign-in no
>   longer leaves stale "Applying.../Uploading.../Saving.../Loading..." text
>   on the PID popup, backup restore, display settings, or safety config
>   pages. **Open advisory, not addressed:** no page-load prompt once on a
>   gated non-dashboard page, and some "Loading..." text is still left after
>   a cancel in `live_profile`'s `refreshLive`/`pollAdaptiveTune`.
> - **`profile_executor` dwell-fault assert, real defect, fixed 2026-09-24**
>   (`docs/audits/profile_executor_panic_2026-09-24.md`, `3ce065ca`): HP-07's
>   global thermal guard tripped while the run was dwelling; `escalate_guard_trip()`
>   set `state = FAULTED` but left `s_exec.dwelling` true, and the same tick's
>   `exec_mode_state_check()` rule 5 then asserted and rebooted the board
>   (`profile_executor.c:1814`). Fixed by `773ec669`: a new
>   `exec_enter_terminal_state()` helper (`profile_executor_relay_io.c`) sets
>   state and clears `dwelling`/`ramp_lock_held` together, wired into all six
>   FAULTED/DONE transition sites; a host test drives `escalate_guard_trip()`
>   from a dwelling RUNNING state and requires zero `exec_mode_state_check()`
>   violations afterward (confirmed failing against the unfixed helper first).
>   **HP-07 re-run on the bench 2026-09-28, PASS** (`20260928T201814Z_heat_hp07_rerun`,
>   `docs/BENCH_TEST_LOG.md`), superseding the 2026-09-25 FAIL
>   (`trip_reason=0 != expected 6`): zone0 `max_temp_c` lowered to
>   ambient+3 C (32.26 C) while idle with the profile target set equal to it,
>   the live approach tripped S6a as designed (`trip_reason=6`,
>   `trip_mask=0x0020`), `safety_clear_trip()` cleared it in 0.61 s, the limit
>   was restored, and the board was left idle with no trip.
>   `adc7f65c` corrects the rule-4/rule-5 rationale comments the audit found
>   false. **Owner decision:** the production `assert()` at
>   `profile_executor.c:1814` is left unchanged for now — a future change is
>   expected to latch FAULTED and log instead of aborting, but that is not
>   implemented yet. Crash report (`dump_id` 861174328) acknowledged by owner
>   decision, not by this fix. The panicked run's own HP-02/03/05 FAILs were
>   not the defect: a **second, concurrent** `bench_test_run(suite="heat")`
>   (`20260924T085635Z_heat`) was driving the same board and slot 7 at the
>   same time — a bench-scheduling gap, not a mode-state bug.
> - **Cross-process bench board lock landed** (`d5bfac19`, review fixes
>   `759660c2`): `bench_test/board_lock.py` adds a lock file at
>   `logs/bench_test/.board_lock` keyed off a per-suite mutation table
>   (heat/autotune/ota/flash/safety/web/nightly/full are MUTATING; lcd moved
>   to MUTATING in the review pass since it injects real touches and can clear
>   a latched trip or write policy; smoke/static/stack stay READ_ONLY and
>   never touch the file). Wired into `BenchTestRunner.run()` itself, so
>   `bench_test_run`, `ota_matrix_run`, and any direct `run_suite()` caller
>   all get it; a live holder refuses immediately, naming pid/suite/start
>   time, and read-only suites are now refused while a live mutating run
>   holds the lock. This is the fix for the "two heat runs sharing one board"
>   scheduling gap the panic investigation above found.
> - **Stack judge follow-up, INCONCLUSIVE**: `9d905ab9` corrects the SK-01/02
>   plan note (baseline script's real output dir, the 512 B `min_free_bytes`
>   floor wording) — cross-commit review only, no new bench run.
> - **LCD-01/08/09/14/16 judge/navigation fixes landed** (`0df96d5d`): the
>   chroma judge's near-black fallback now requires a brightness floor
>   (`CHROMA_MIN_BRIGHTNESS_RATIO`), and case-level tests now prove a FAIL
>   naming the missed hop instead of clicking a stale target when the first
>   hop's page never arrives. `f3164403` re-review-fixes LCD-19/WEB-SEC-04
>   (PIN format validation, dismiss-survives-error restore, per-`send()`
>   `EnterPinTest` replies). **Neither the stack nor the LCD suite has been
>   rerun against these fixes yet** — both reruns and the heat rerun (against
>   the dwell-fault fix above) are still pending.
> **Previously reviewed:** 2026-09-24, the first real-hardware
> runs of the bench test system's heat/LCD/stack/web suites, and the runner
> fixes they found (thirty-sixth sweep) — open items below.
> - **First real-hardware bench_test runs, 2026-09-24** (ESP `351304cb`, Pico
>   `6bb41fe1`): heat suite `20260924T072516Z_heat` — HP-02/04/05/06 PASS;
>   HP-01/03/07/08 FAILed on harness defects, not firmware (wrong-order
>   zone-limit sequencing, missing zone-override fields, target-vs-limit
>   pinning, firing history erased by teardown before it was read), all fixed
>   same day (`0567bf09`, `35d407df`). LCD suite `20260924T080746Z_lcd` then
>   `20260924T084342Z_lcd` — LCD-21 PASS both runs; LCD-01/08/09/14/16 FAILed
>   (first run on a screen-idle touch-swallow race, `screen_idle_touch_swallow()`
>   ate the injected wake tap, fixed in `cases_lcd.py` `866003ea`; second run,
>   after that fix, with the panel confirmed lit, on judge/navigation defects:
>   tile label "Network / Wi-Fi" vs expected "Network", clicks not_found after
>   enumeration, ACCENT_4 colour threshold -- fix in progress); LCD-19 NOT_RUN, needs `KILNCTL_LCD_PIN`. Stack
>   suite `20260924T080808Z_stack` (SK-01/02 FAIL, SK-03/04 PASS, before the
>   SK-02 noise-tolerance/fw_commit-gate fix, also `866003ea`) then
>   `20260924T084524Z_stack` (preflight refused, exit_code=2, on a stale MCP
>   server — rerun pending). Web suite `20260924T080814Z_web` ran its
>   render-only rows clean (24 PASS, `WEB-WIFI-06` SKIP needing an operator).
>   No firmware defect found in any of the above; every FAIL traced to the
>   runner and is fixed or pending a rerun. Full per-run detail:
>   `docs/BENCH_TEST_LOG.md`. **Pending:** rerun LCD and stack against the
>   fixed harness, capture a stack-margin baseline at the running commit, and
>   run LCD-19 with `KILNCTL_LCD_PIN` set.
> **Previously reviewed:** 2026-09-24, `estop_verified` closed
> end to end on the bench, the Pico reflashed, and the bench Wi-Fi hotspot
> landed (thirty-fifth sweep).
> - **`estop_verified` closed end to end, 2026-09-24 (bench time):** owner
>   pulled the bench E-stop jumper; S7 (`SAFETY_TRIP_ESTOP`, trip_reason 8,
>   `trip_mask 0x0080`) latched and was confirmed on the Pico diag output, the
>   ESP's safety cache, and readiness's `safety_trip`, with every relay
>   de-energized. Owner refitted the jumper (pole 1 stays permanently unwired
>   by owner decision, unchanged), the trip was cleared via
>   `safety_clear_trip()`, and the verification was recorded with read-back
>   through the new `estop_verify` MCP tool (`fc16c186` adds the tool wrapping
>   `POST /api/estop/verify`; `055703af` fixes a trip-item-missing gap and a
>   stale success summary found while using it). Every Class C heat/firing
>   row previously BLOCKED on `estop_verified` is now unblocked — **not yet
>   run**, just no longer gated.
> - **Bench Pico reflashed to HEAD** (`6bc4fc23`'s successor commit,
>   `6bb41fe1`, SaftyFW build `d6309f4a`) via `debug_program(peer="pico")`
>   after the owner reseated the CMSIS-DAP probe — closes the "Pico when the
>   probe is replugged" open item from prior sweeps.
> - **Secured bench Wi-Fi hotspot landed** (`3eadbe35`, `5279552c`):
>   `docs/BENCH_HOTSPOT.md` and `tools/PcTools/scripts/bench_hotspot.ps1`
>   stand up a second, isolated 2.4 GHz AP (Windows Mobile Hotspot over
>   `NetworkOperatorTetheringManager`, WPA2) for AP-fallback/provisioning
>   testing without touching the lab network; credentials live only in
>   User-scope `KILNCTL_HOTSPOT_SSID`/`KILNCTL_HOTSPOT_PASSWORD`. This
>   supplies the second AP the AP-fallback row below needed — the
>   AP-fallback test itself has not been run yet.
> - **Commissioning pass, 2026-09-24, at HEAD:** readiness 17 ok / 1 not_done
>   (`safety_commissioned`: `i_normal_a[0..2]` unset, needs a CT current
>   sweep under load) / 3 other (`deliberately_off`: `guard_cross_zone`;
>   `calibration`; `pico_update`; `cannot_yet`: `ct_attribution` pending the
>   same sweep). `cfgfs` mounted, 9 files; only `dual_write.zones` is not yet
>   file-backed, expected to self-clear on the next zones write.
> **Previously reviewed:** 2026-09-23, the machine-wide build
> gate, the `safety_get_param` GET_PARAM wire, and cfgrecrc closed
> (thirty-fourth sweep) — open items below.
> - **Machine-wide heavy-build gate landed** (`e3aaa4ff`, opus-review fixes
>   `2afc1d44`/`d487db9f`/`4d8c3327`, PowerShell-side mutex-prefix parity
>   `e51f8b36`): `tools/build_gate.ps1`
>   (`Enter-KilnBuildGate`/`Exit-KilnBuildGate`) and its Python mirror
>   `tools/PcTools/src/mcpkit/buildgate.py` cap heavy builds (ESP-IDF target
>   builds, MSVC host-test builds) at `KILNCTL_BUILD_GATE_SLOTS` (default 2 then; 4 since 2026-10-07)
>   across every session on the machine at once, via named kernel mutexes
>   (`Global\kilnctl_build_slot_<i>`) rather than a semaphore, so a killed
>   agent can't strand a slot. Answers the five hard freezes (Kernel-Power 41)
>   recorded in one week under uncoordinated parallel `idf.py`/ninja/MSVC
>   builds. Wired into every `check_00_*.ps1`, `build_host_tests.ps1`, and the
>   `build_kilnfw`/`build_saftyfw*` MCP tools (see `docs/agent_rules/COMMON.md`
>   "Heavy builds"); never bypass it with `KILNCTL_BUILD_GATE_SLOTS=0` or a
>   direct `idf.py`/`cmake`/`ninja`/`cl.exe` call outside those entry points.
> - **`safety_get_param` MCP tool landed, and the GET_PARAM wire it depends on
>   is now real end to end.** `d3c9f194` (opus-review fixes `8bdb232b`) added
>   the PC-side GET_PARAM(0x23)/PARAM(0x1E) wire codec and a READ-ONLY
>   `safety_get_param` tool that reports a Pico refusal as a refusal, never as
>   "not found". `6cb77943` then wired the missing ESP32-S3 side —
>   `SAFETY_CMD_GET_PARAM`/`SAFETY_CMD_PARAM` passthrough,
>   `KILNLINK_PROTOCOL_VERSION` 7 — so a param such as `0x0505` (the config
>   re-CRC fail counter, see cfgrecrc below) can actually be read over the
>   isolated link rather than timing out; stale "unsupported"/"not wired"
>   wording and hard-coded commit-hash citations were removed from the PC-side
>   docstrings the same day (`0a40224e`, `3f62cbbf`). Separately,
>   `safety_link`'s own GET_PARAM/GET_STACK_MARGIN reply loop only drained one
>   frame per timeout budget instead of looping across it — fixed same day
>   (`6bc4fc23`, follow-ups `a33239d7`) and covered by a new host-test spin
>   guard on the receive stub (`8ce6d794`).
> - **SaftyFW periodic in-RAM config re-CRC landed, closing cfgrecrc**
>   (`7b0cf2b3`, opus re-review fixes `9abc5c4e`/`ca70e522`/`fb756d1d`):
>   `config_check_period_s` now actually ticks — the RAM copy of the
>   commissioned config is re-CRC'd on that cadence and a mismatch restores
>   from the persisted `s_persisted_record` (fixed same day, `f6d88133`: the
>   first landing wrongly restored compiled defaults, which would have
>   silently disarmed S1/other guards on a real corruption, the same class of
>   defect as the `repair_to_defaults` finding elsewhere in this file). The
>   failure counter is `0x0505` over GET_PARAM, per the row above.
> - **Test-hardening landed alongside the iter_tune step-7 and Pico-OTA-relay
>   rows below** (not separately gated): `2ded3afa` added a host test for
>   `ota_pico_relay.c`'s state machine (107 checks); `327f0b4d` fixed two of
>   those tests an Opus review found vacuous; `b8f8ab8e` pinned
>   `test_retransmit_exhausted_rounds` against an off-by-one-high loop bound
>   found while fixing it.
> - **Bench state unchanged**: ESP still `9c26dd91`, Pico still `987050f6` (no
>   CMSIS-DAP probe enumerates), Class C heat/firing rows still BLOCKED on
>   `estop_verified`.
> **Previously reviewed:** 2026-09-23, hmacmirror/d3bootid/
> maxreassert closed and the compile_esp/pico_backends -Fast SKIP-FAST fix
> (thirty-third sweep).
> - **`check_ota_http_context_mirror.py` landed, closing hmacmirror**
>   (`6d2e4349`): extracts the `OTA_HTTP_CONTEXT_*` enumerators from
>   `ota_state.h` and fails if any lacks an explicit case in `ota_http.c`'s
>   `ota_http_verify_request()` switch or `test_ota_http.c`'s `ctx_str_for()`
>   — a member missing a case previously fell silently into `default:`. Also
>   strips `#` comments from the Python allow-list region before matching
>   string literals (a commented-out entry no longer counts as live), and
>   documents that `strip_c_comments()`'s line-first ordering still mishandles
>   the inverse `/* a // b */` shape (accepted, fails loud via the empty-set
>   guard). Negative-tested: a fake enum member in `ota_state.h` failed,
>   naming both missing sites; restored by hand, empty diff confirmed.
> - **`sw_reset` boot_id fallback landed, closing D3** (`5293cf10`, opus-review
>   fixes `f93c6ec5`): on a `NO_REPLY` outcome from a plain reboot-in-place,
>   the ESP now snapshots the Pico's `boot_id` before sending
>   `SAFETY_CMD_REBOOT` and polls up to 3000 ms (200 ms steps) watching for a
>   `boot_id` change, reporting `SW_RESET_PICO_CONFIRMED_BY_BOOT_ID` once it
>   moves rather than the old, non-committal "not confirmed" answer — this is
>   exactly the thirty-second sweep's D3 bench finding (`boot_id` 113→127
>   advancing while the Pico's own ACK never made it out inside its 10 ms TX
>   drain window before the watchdog reset). Review fixes: `kArmWaitMs` is now
>   derived from the new `SAFETY_LINK_REBOOT_BOOT_ID_WATCH_MS` constant
>   instead of a second hardcoded number (the pair had drifted apart once
>   already), the handler's stale ~345ms-only comment restated with the real
>   two-part worst case, and `confirmed_by_boot_id` added to the
>   all-outcomes-distinct sentence test. 58/58 KilnFW host tests, negative-
>   tested (forced always-confirmed, watched the new assertion fail, restored
>   by hand, rebuilt clean). SaftyFW itself untouched — the Pico still can't
>   be flashed (no CMSIS-DAP probe enumerates).
> - **SaftyFW MAX31856 live-config re-assertion landed, closing maxreassert**
>   (`da214e3b`, opus-review fixes `30c91165`): `thermo_task` now calls
>   `max31856_verify_live_config()` every ~30s of elapsed time (not poll
>   count) once a part has verified, re-running `max31856_configure()` on a
>   CR0/CR1 mismatch against a live power-on-default reset the old
>   verified-once cache never re-checked. Review fixes: the real fail-safe for
>   the detection window is the pre-existing DRDY-silence branch, not the
>   `!verified` downgrade (which self-heals synchronously within the same
>   iteration); cadence changed from poll-count to elapsed-time so it can't
>   drift to ~60s of wall clock in the DRDY-silent scenario it exists to catch
>   (a poll-count cadence would have landed on S5's 60s `blind_grace_s` trip
>   boundary instead of beating it); `thermo_task_live_config_mismatch_count()`
>   documented as SWD-readable-only with no wire caller — wiring it onto Frame
>   A's `flags2` was evaluated and deferred, since `link_frame_pack_status()`'s
>   ~14 positional call sites made it materially larger than this fix's scope.
>   `thermo_task`'s measured stack grew 1224→1232 B (ceiling bumped to match,
>   then re-verified unchanged in the follow-up commit); 23 B new `.bss`.
>   279/279 host tests, negative-tested. One follow-up (a boundary/ms-wrap
>   host test for `max31856_live_check_tick()`) landed same day, `8fb05c36`
>   (Opus B1) — the production comparison already used the wraparound-safe
>   `(int32_t)(now_ms - due_ms) >= 0` idiom, so this was test-only. The
>   remaining follow-up, tracked in `firmware/SaftyFW/docs/THERMOCOUPLE.md`: the
>   deferred `flags2` wire surface. **Closed 2026-09-23** (`cc060a5c`): wired
>   onto Frame A `flags2` bit2 (`LINK_FLAG2_TC_CONFIG_REASSERTED`, 0x04).
> - **`compile_esp_backends.ps1`/`compile_pico_backends.ps1` now SKIP-FAST
>   under `-Fast`** (`71e19c86`): both depend on phase-1 target-build output
>   (`KilnFW/build/compile_commands.json`,
>   `SaftyFW/build/{build.ninja,CMakeFiles/rules.ninja}`) that `-Fast` skips,
>   so they always FAILed under `-Fast`; now they test `KILNCTL_CHECKS_FAST`
>   and print `SKIP-FAST` (exit 3) only for that missing-build-output reason —
>   same convention as the other three SKIP-FAST checks — while every other
>   failure, and the missing-file case with the env var unset, still FAILs.
> - **Bench state unchanged**: ESP still `9c26dd91`, Pico still `987050f6` (no
>   CMSIS-DAP probe enumerates), Class C heat/firing rows still BLOCKED on
>   `estop_verified`.
> **Previously reviewed:** 2026-09-23, the ota_http HMAC
> stack-overflow fix, the coordinated_gpio_test MCP tool, and the D4
> config_volatile_dirty observability landing (thirty-second sweep).
> - **`ota_http.c` HMAC context buffer overflow fixed** (`252225f7`): the
>   stack buffer was sized 13 B while the longest context string,
>   `"boot-guard-reset"`, is 16 B — a 3-byte stack overflow on every call
>   using that context (the `flash_firmware()` post-flash reset path from the
>   thirty-first sweep's D2 finding included). Now sized from
>   `OTA_HTTP_CONTEXT_STR_MAX`, with a `_Static_assert` per literal plus a
>   runtime guard. Still in flight, not yet landed: the shared
>   `OTA_HTTP_CONTEXT_STR_MAX` header plus
>   `tools/check_ota_http_context_mirror.ps1` to keep the PC-side mirror from
>   drifting out of sync again (`hmacmirror`).
> - **`coordinated_gpio_test` MCP tool landed** (series `46ec4c4d`/
>   `e9a9e52e`/`742405ab`/`256ead69`): the two-board GPIO test is now reachable
>   through the `kilnctrl` facade (185 tools now, up from 184), with lazy
>   `ProbeClient` construction so a preflight refusal never opens a client,
>   the refusal itself made inert on the boards, and `guard_pin` checked
>   before every read.
> - **D4 config-store regression now observable**: `config_volatile_dirty`
>   (`config_store_is_volatile_dirty()`, `c118f417` with `85bcc43a`) adds a
>   diagnostic flag bit — `KILNLINK_DIAG_FLAG_CONFIG_VOLATILE_DIRTY`, bit 7,
>   the last spare bit — surfaced in `config_volatile_dirty` on
>   `GET /api/status`; the PC decoder and mirror-drift map were updated to
>   match. This makes the thirty-first sweep's D4 finding (a
>   `config_version`/`config_crc` regression across a Pico reset with every
>   observable field unchanged) visible without a JTAG read; it does not fix
>   the underlying RP2040 config-store atomicity defect, which stays open
>   (`docs/CONFIG_FILESYSTEM.md`).
> - **`PROFILE_SLOTS_100` section 8 Q1 resolved** (`9ae25c39`): the
>   cfg-partition-tail question was already decided and flashed to the bench
>   at `5f58ba09` on 2026-09-20 (ROADMAP.md row L) — the plan doc's open
>   question was stale prose, marked resolved rather than open.
> - **Still in flight, no SHA yet**: `d3bootid` (ESP-side `sw_reset`
>   confirmation fallback keyed on the Pico's `boot_id`, closing D3),
>   `maxreassert` (SaftyFW MAX31856 live-reset config re-assertion),
>   `cfgrecrc` (SaftyFW periodic config re-CRC — `config_check_period_s`
>   finally ticking).
> - **Bench state unchanged**: ESP still `9c26dd91`, Pico still `987050f6`
>   (no CMSIS-DAP probe enumerates), Class C heat/firing rows still BLOCKED
>   on `estop_verified`.
> **Previously reviewed:** 2026-09-23, the bench reflash itself landed, plus
> the partition-table-offset resolver and boot_guard_reset-after-erase fixes
> (thirty-first sweep).
> - **Bench reflash done, 2026-09-23**: ESP flashed to `9c26dd91` from
>   `C:\wt\sdkpin_np8lkw\firmware\KilnFW` (prebuilt, not rebuilt),
>   `erase_partitions=["nvs"]`, flash verified against the running `app`
>   partition and build timestamp; `web_auth_setup` re-bootstrapped the admin
>   record from env vars (web auth back on, LCD auth off, 30 min/10 min
>   timeouts). Readiness 16 ok / 2 not_done / 3 other. Pico is **still**
>   `987050f6` — no CMSIS-DAP debug probe enumerates on USB (physical fault,
>   needs the owner to replug/check the probe). Class C heat/firing rows stay
>   BLOCKED on `estop_verified`, which needs the bench E-stop jumper pulled by
>   the owner; not faked. Full detail:
>   `docs/BENCH_TEST_LOG.md`'s "M18" section is the place for this run's
>   evidence — not yet appended, since this sweep is docs-only for
>   `ROADMAP.md`; the run itself is documented in the bench agent's own
>   scratchpad output only as of this sweep.
> - **Findings from that run:** (D2) `flash_firmware()`'s post-flash
>   `boot_guard_reset` 403'd — root cause is ordering: erasing `nvs` in the
>   same session resets the board's AP password to its default before the
>   tool signs the HMAC with the env-var password, so the reset can never
>   succeed on an `nvs`-erasing flash; tool-side fix landed same day
>   (`fc22cc9a`, skip the attempt rather than 403 with a stale password), the
>   env-var side is an owner action. (D3) `POST /api/sw_reset` reported the
>   Pico reboot "not confirmed" even though `boot_id` advanced (113→127) —
>   the confirmation path doesn't work against the Pico's current
>   `987050f6` build; an ESP-side `boot_id` fallback is in flight, not yet
>   landed. (D4) `config_version`/`config_crc` regressed to an older record
>   across that same reset with every observable commissioning field
>   unchanged — matches the known, still-unfixed RP2040 config-store
>   atomicity defect (`docs/CONFIG_FILESYSTEM.md`); an observability flag for
>   this is in flight, not yet landed. (D5) the bench LCD camera capture is
>   badly out of focus/overexposed (left ~60% of the panel saturates to
>   white) — hue reads correct where unsaturated, but LCD colour rows are not
>   judgeable until the camera is refocused. Also in flight, not yet landed:
>   a `coordinated_gpio_test` MCP wrapper and a 3-byte HMAC-buffer overflow
>   fix in `ota_http`.
> - **Partition-table offset no longer hardcoded**: `flash_firmware()`/
>   `fixture_flash()` now resolve `CONFIG_PARTITION_TABLE_OFFSET` from the
>   target's own `sdkconfig` instead of assuming `0x8000` (`7d774a5b`,
>   opus-review fixes `fae0de6d` — quoted values, note surfaced to
>   `fixture_flash()`'s result, a real-OSError-vs-absent distinction, a
>   `build/sdkconfig` fallback path). Docs for flashing all three published
>   binaries (`bootloader.bin`/`partition-table.bin`/`KilnCtrl.bin`) and how
>   the offset is resolved landed the same day (`f00b0a20`).
> - **`task_liveness` dead-code removal** (`a36e76fa`): the names-only
>   `load_required_task_names`/`parse_required_task_names` wrappers were
>   flagged by the zero-caller sweep once both real callers switched to the
>   tag-aware `load_required_task_specs()`; removed, with the shared
>   name-parsing edge-case tests rewritten against `parse_required_task_specs()`
>   directly rather than dropped.
> **Previously reviewed:** 2026-09-23, bootloader/partition-table publishing,
> bench sdkconfig pins, and OTA rollback task-handle hardening (thirtieth
> sweep).
> - **Both bench-commission tooling gaps from the twenty-ninth sweep are now
>   closed**: `check_00_kilnfw_target_build.ps1` publishes `bootloader.bin`/
>   `partition-table.bin` alongside `KilnCtrl.elf`/`.bin` (`d23d4eae`,
>   freshness-gate fixes `51ae1ce0`/`ad2ee170` grade the published bootloader
>   against its own subproject config header); `flash_firmware()`'s
>   missing-binaries refusal now names both files explicitly and suppresses
>   a dangling "did not include it" note when `KilnCtrl.bin` itself is also
>   missing (`9c26dd91`). See the M18 pending checklist below — the bench
>   reflash itself has not happened yet.
> - **Bench WiFi/lwIP/GPIO_PROBE sdkconfig values pinned into
>   `sdkconfig.defaults`** (`a513aa75`): `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM`,
>   `CONFIG_ESP_WIFI_RX_BA_WIN`, `CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS`, and
>   `CONFIG_KILNCTL_ENABLE_GPIO_PROBE` (unconditional — no
>   `DEV_AFFORDANCES`-style umbrella symbol exists in this Kconfig tree to
>   gate it on; flagged for the owner as a possible follow-up). All four
>   added to `check_sdkconfig_defaults_applied.ps1`'s watched-key list
>   (`0a8db924`), negative-tested by hand-editing a generated value and
>   confirming the check catches it. `.dram0.bss` +64 B (96,792 -> 96,856 B),
>   fully explained by `gpio_probe.c`'s own statics plus alignment, not a
>   regression.
> - **`test_sdkconfig_sibling_pair_guard.py` wired into `run_all_checks.ps1`**
>   (`b9f574d3`), closing the same class of orphaned-check gap
>   `test_stack_budget_symbol_bounds.py` was already wired around;
>   `build_kilnfw()` follow-up (`5a72eb50`) skips the sdkconfig sibling
>   refresh when ninja did not actually relink, avoiding unnecessary rebuild
>   churn.
> - **`run_all_checks.ps1` -Fast SKIP-FAST classification hardened per opus
>   advisories** (`af5a64ba`): `KILNCTL_CHECKS_FAST` is now set/cleared right
>   after `$ErrorActionPreference` and cleared from every exit path
>   (including a trap for an uncaught terminating error) so it never leaks
>   into the calling shell; CLAUDE.md's `-Fast` wording corrected to mention
>   `check_00_kilnfw_host_tests.ps1` as a fourth skip; new
>   `tools/check_skip_fast_classification.ps1` negative-test-shaped self-test
>   drives two throwaway dummy checks through a real nested `run_all_checks.ps1`
>   run to prove the SKIP vs. SKIP-FAST bucketing end to end.
> - **OTA on-demand task handles nulled before they go stale, stack_margin
>   registration de-duplicated** (`95be3327`, opus-review fixes `22fcc257`,
>   header-comment correction `24e59726`): the ESP rollback task's handle is
>   now cleared after `vTaskDelete` so a second POST can't observe a dangling
>   handle, and duplicate `stack_margin_register()` calls across the three
>   OTA HTTP call sites (all serialized on the single httpd worker task, not
>   inside the created tasks' bodies, and not guarded by any per-feature
>   mutex — `recovery_exit` takes none) are now de-duped rather than
>   double-counted; a mismatched `configured_stack_bytes` on a duplicate
>   registration is silently kept (first registration wins), now documented
>   in a header comment.
> - **Not yet flashed**: the bench board stays at `6630e769`/`05f1ab1f`
>   (ESP/Pico) — none of today's commits are on hardware yet; a target build
>   at `24e59726`+ was in progress as of this sweep, flash not yet done.
> **Previously reviewed:** 2026-09-23, -Fast SKIP
> reclassification, three more httpd-stack heap moves, and task-liveness
> lifecycle tagging (twenty-ninth sweep).
> - **`-Fast`-caused SKIPs are now non-fatal without weakening SKIP-fails-by-
>   default** (`2a1c5c96`): `run_all_checks.ps1` sets `KILNCTL_CHECKS_FAST` in
>   the environment only when `-Fast` is passed. `check_recovery_image_size.py`,
>   `check_web_gzip_parity.py`, and `check_embedded_pico_image_fresh.ps1` each
>   test that var and, only for the one SKIP reason that is a direct
>   consequence of the phase-1 build `-Fast` itself skipped (missing
>   `recovery.bin`, a missing KilnFW `.gz` build dir, or missing SaftyFW slot
>   bins), print `SKIP-FAST: ...` instead of `SKIP: ...`. `run_all_checks.ps1`
>   buckets these separately, reports the count in its summary, and never
>   fails the run over them — without `-AllowSkips`, and without changing
>   that a plain SKIP for any other reason still fails the run by default.
>   Verified both ways: `KILNCTL_CHECKS_FAST` unset still prints plain SKIP
>   and fails; set, the same checks print SKIP-FAST and the run exits 0.
> - **Three more `httpd_worker`-reachable stack locals moved to heap**
>   (`a1e11d26`, Opus-reviewed follow-up `a37abc03`): `backup_import_apply_locked()`'s
>   profile-slot-simulation scratch (two `PROFILES_MAX_COUNT` bool arrays plus
>   a 128 B `coll_err`, and now also the ~428 B per-iteration `profile_t`
>   existence-probe local) folded into one heap-allocated
>   `backup_import_slot_scratch_t`; `profile_post_handler()`'s ~428 B
>   `profile_t tmp` moved to heap; `sw_reset_post_handler()`'s 960 B response
>   buffer moved to heap (OOM falls back to a short fixed string), its cap
>   named `SW_RESET_BODY_CAP` instead of a bare literal at four sites, and its
>   two fallback strings hoisted into one `kSwResetFallbackBody` constant. Two
>   `sizeof()` truncation-check bugs fixed along the way (would have silently
>   shrunk to `sizeof(char*)` once the locals became heap pointers).
>   `check_httpd_task_stack_budget.py` honest free: 2104 B (LOW) -> 2520 B,
>   deepest path now `backup_import_post_handler` at 3872 B. `.dram0.bss`
>   unchanged — stack locals only, no new statics.
> - **`check_task_liveness` required tasks now carry a lifecycle tag**
>   (`01ea790a`, ambiguous-tag hardening `4f38b22a`, docstring/doc cleanup
>   `eb161458`): the first live run against the bench board found six false
>   positives that are all by design — `pico_auto_update` self-deletes after
>   boot (`boot-once`, also gated on not-recovery-mode and a healthy safety
>   link); `gpio_probe`/`i2c_owner_ns2009` are build-config/hardware-probe
>   conditional (`config`); `ota_pico_rollback`/`recovery_exit`/
>   `ota_rollback_reboot` are transient HTTP-handler tasks (`on-demand`). Each
>   `$requiredNames` entry in `check_stack_margin_registration.ps1` now carries
>   its lifecycle as a trailing `# liveness: <tag>` comment on the same source
>   line; `task_liveness.py` parses it and splits fault buckets
>   (`fault_dead`/`fault_absent`) from informational ones
>   (`info_dead`/`info_absent`) — only an untagged (`always`) DEAD/ABSENT, or a
>   `boot-once` ABSENT, is still a fault. The parser now rejects a tagged line
>   with more than one or zero quoted names (`TaskLivenessParseError`) and
>   finds `liveness:` anywhere in the comment rather than anchored to its
>   start, closing a silent-drop and a silent-ignore gap the first pass left
>   open. Dead `_strip_ps1_comment_lines()` helper removed. 50 tests passed
>   (47 pre-existing + 3 new).
> - **Not yet flashed**: the bench board stays at `6630e769`/`05f1ab1f`
>   (ESP/Pico) — none of today's commits are on hardware yet.
> **Previously reviewed:** 2026-09-23, SaftyFW clean-worktree
> configure tooling plus a task-liveness MCP tool (twenty-eighth sweep).
> - **`build_saftyfw()` resolves `PICO_SDK_PATH` automatically** (`af4b23e0`):
>   a new `mcpkit/pico_sdk.py` `resolve_pico_sdk_path()` (env var, else the
>   bench default at `C:\pico-tools\pico-sdk` if it looks real, else a clear
>   error) is now the single source of truth, and `build_saftyfw()` takes an
>   optional `saftyfw_root` (mirroring `flash_firmware`'s `kiln_fw_root`) and
>   configures its build directory from scratch when needed. Verified end to
>   end in a worktree with `PICO_SDK_PATH` unset. Follow-up (`b41ef7b6`)
>   rejects a relative or non-SaftyFW-shaped `saftyfw_root` up front, and makes
>   `_cmake_build()`'s configure-success gate positive (proceed only on a
>   `<tag>-configure: OK` first line) instead of the old negative check, which
>   let a "cmake not found" configure fall through to `cmake --build` on an
>   unconfigured directory and misreport "configured from scratch".
> - **`check_task_liveness`**, a new READ-ONLY MCP tool (`a0829d71`) that
>   cross-checks a live `stack_margin_read()` snapshot against the
>   `$requiredNames` list `check_stack_margin_registration.ps1` enforces at
>   the source level — that check only proves every required task has a
>   registration call site, never that `xTaskCreate*()` actually fired on a
>   given boot, and a failed creation is log-only (`ESP_LOGE`, non-fatal,
>   nothing HTTP-visible). Reports alive / DEAD (registered, not running) /
>   ABSENT (never registered) / extra (informational). `capability_preflight`
>   now runs this cross-check too and refuses a run with a dead or absent
>   required task, same as an unacknowledged crash report, unless
>   `allow_missing_tasks=True`. `kilnctrl`'s tool count moves 183 -> 184.
> - **Stale blob citation fixed** in
>   `docs/audits/release_gate_vacuity_audit_2026-09-16g.md` (`30e13029`):
>   `wifi_provision_http.c`'s blob moved again (`b7bc31d2`, twenty-seventh
>   sweep's heap move), reddening `check_doc_hash_citations.ps1` against the
>   old 8fc84030 citation; the underlying claim still held, so the citation
>   was updated to blob:firmware/KilnFW/App/drivers/http/wifi_provision_http.c
>   06e191a4 (not backtick-quoted, per this doc's own convention for a
>   superseded citation) and the old one marked superseded.
>   That 06e191a4 citation is itself now superseded by a further unrelated
>   edit to the file; the underlying claim (the `max_uri_handlers` cap-vs-
>   route-count guard) still holds, so it was refreshed to
>   blob:firmware/KilnFW/App/drivers/http/wifi_provision_http.c
>   f22fcfff (not backtick-quoted, superseded) was itself superseded by the
>   2026-09-28 AP-fallback change's unrelated `/status` JSON
>   `ap_pending_teardown` field; the underlying claim still holds, refreshed to
>   blob:firmware/KilnFW/App/drivers/http/wifi_provision_http.c
>   d23cefe4 (not backtick-quoted, superseded) was itself superseded by
>   `93a8716f`'s shared AP-subnet helper/HTTP-layer message/confirm-side
>   guard change, unrelated to this check's subject; refreshed to
>   blob:firmware/KilnFW/App/drivers/http/wifi_provision_http.c`cebd2f59` (citation refreshed 2026-10-05 after WP8 cap bump 184, earlier `622f0539`, earlier ` `ff1851ca`/`a3c7d454` changed the file).
> - **Pending:** `cfg_fs_list`'s scratch heap move is still in review.
> - **Bench commission flash still blocked, and now on a second, independent
>   gap**: `check_00_kilnfw_target_build.ps1` publishes only
>   `KilnCtrl.elf`/`KilnCtrl.bin`, never `bootloader.bin` or
>   `partition-table.bin`, which `flash_firmware()` requires — a rebuild that
>   also produces those two is needed before the pending ESP fixes
>   (`6a308c6e` DRAM fix, plus this sweep's tooling) can be flashed. A
>   pre-flash `check_task_liveness` baseline against the still-running old
>   board build (`6630e769`) reported 29/35 alive, `pico_auto_update` DEAD,
>   and `gpio_probe`/`i2c_owner_ns2009`/`ota_pico_rollback`/
>   `ota_rollback_reboot`/`recovery_exit` ABSENT — all five are conditional
>   tasks (compiled/started only under specific build or runtime gates), so
>   this is not necessarily a fault; classifying which ABSENTs are expected
>   for this build config is an open follow-up.
> - **Not yet flashed**: the bench board stays at `6630e769`/`05f1ab1f`
>   (ESP/Pico) — none of today's commits are on hardware yet.
> **Previously reviewed:** 2026-09-23, four more httpd-stack
> handler moves plus a stack-local check fix (twenty-seventh sweep).
> - **`kiln_cfg_store_export_package_json`'s 896 B blob scratch moved off the
>   8 KB httpd stack** (`d2c25d7b`), reachable from `export_get_handler` and
>   `backup_export_get_handler`; malloc'd instead, freed on every return path.
> - **Three more `httpd_worker`-reachable handler buffers moved to PSRAM
>   heap** (`b7bc31d2`): `networks_get_handler` (2704 B),
>   `autotune_status_get_handler` (1300 B), `thermo_faults_get_handler`
>   (1536 B). Follow-up (`96ff1765`) fixed a `sizeof(json)` on the now-heap
>   pointer in `thermo_faults_get_handler` that review caught before push —
>   the pointer's `sizeof` is 4, not the 1536 B buffer, which would have
>   500'd every `GET /api/thermo/faults` call.
> - **`check_no_exec_status_stack_locals.py` now also catches an initialized
>   `profile_exec_status_t` stack local** (`= {0};`/`= {};`/compound-literal
>   forms), not just a bare declaration (`d4dfd75c`). `telemetry_log_task`'s
>   own one-time allocation was made `static` in the same commit, removing an
>   OOM failure mode; `.dram0.bss` is now 96856 B of the 101000 B ceiling
>   (headroom 4144 B).
> - **Pending:** `cfg_fs_list`'s scratch heap move is in review; a
>   task-liveness MCP tool is in progress; SaftyFW clean-worktree configure
>   tooling (a `PICO_SDK_PATH` resolver) is in progress.
> - **Not yet flashed**: the bench board stays at `6630e769`/`05f1ab1f`
>   (ESP/Pico) — none of today's commits are on hardware yet.
> **Previously reviewed:** 2026-09-23, three bench-finding
> DRAM/stack fixes plus a SaftyFW telemetry gap (twenty-sixth sweep).
> - **`kiln_cfg_store` transient blob scratch moved to PSRAM** (`6a308c6e`):
>   root cause of the bench board's 6675 B internal-heap low-water at boot —
>   two ~7.5 KiB `kiln_cfg_store_blob_t` mallocs live at once on the main
>   task during boot.
> - **Two `httpd_worker`-reachable handler locals moved off the 8 KB httpd
>   stack** (`26e382bd`): live margin had read CRITICAL, 1172 B free.
> - **SaftyFW: S1/S8 disabled-by-zero get distinct kilnlink diag bits 5/6**
>   (`03c872e2`), plus a web status banner;
>   `DASHBOARD_JSON_STATUS_BUF_SIZE` grown 5248 -> 5376 (measured worst case
>   5273 B).
> - **Not yet flashed**: the bench board stays at `6630e769`/`05f1ab1f`
>   (ESP/Pico) — none of the three fixes above are on hardware yet.
> **Previously reviewed:** 2026-09-23, docs/checklist
> upkeep + `boot_guard_get` MCP tool + a gzip content-parity standing check
> (twenty-fifth sweep, docs- and tooling-only — no firmware source changed
> this sweep).
> - **`zones_config_store`: the file-won migration-persist fault latch now
>   carries the real on-disk version** instead of a hardcoded 0
>   (`b03f9003`, blob citation refreshed `78447367`).
> - **`boot_guard_get`**, a new READ-ONLY MCP tool wrapping `GET
>   /api/boot_guard` (`d9f06f38`) so the recovery-mode counter can be
>   checked without a flash in flight; `kilnctrl`'s tool count moves from
>   182 to 183.
> - **A standing check for TODO.md 10.6a's gzip content-parity item**
>   (`ba2d0622`, SKIP-on-stale-build-dir fix `bd298ee0`): `KilnCtrl.bin`'s
>   embedded `.gz` web assets are now byte-compared against their sources,
>   SKIPping loudly on a stale/unbuilt tree rather than failing the whole
>   suite.
> - **Doc checklist upkeep**: verified stale boxes ticked across
>   SAFETY_MODEL/BOOTLOADER/COMMISSIONING/DISPLAY_ST7796_PLAN (`bee3430d`),
>   four stale/completed `firmware/KilnFW/TODO.md` items closed
>   (`47d336da`), and the bench-preset `calibration_missing` item ticked
>   and cited in COMMISSIONING.md (`87776202`).
> - **`config_presets.py` docstring corrected** (`215ee790`): the
>   `ct_channel_map`/`calibration_missing` gating was misdescribed; comment
>   only, no behavior change.
> - **Software backlog state, 2026-09-23:** a classifier pass found every
>   remaining unchecked box in `firmware/SaftyFW/TODO.md` (26) and
>   `tools/PcTools/TODO.md` (4, two duplicates) is hardware-gated.
>   `firmware/KilnFW/TODO.md`'s remaining open items are 6A.3 (guard-disable
>   ack gate, design-only, owner decision pending) and the two "POST
>   handlers should post commands to owner tasks" items (~lines 1950/1958,
>   architectural, scoped not urgent per `docs/HTTP_HANDLER_OWNERSHIP.md`).
> **Previously reviewed:** 2026-09-22, crash-report v3 +
> zones write-back guard + JSON-overflow hardening + web logout + Pico
> image-identity v2 + host-test standing checks (twenty-fourth sweep).
> - **zones_config: write-back hardened** — a file-sourced migration is now
>   also written back to NVS (`3b979ec4`), and that write-back never
>   overwrites a newer-than-firmware NVS blob (`cce21da0`).
> - **Five HTTP handlers fail loud (500) instead of a truncated 200 on JSON
>   overflow** — `diagnostics_http.c`, `board_temps` http, `kiln_cfg_http.c`
>   and `profiles_catalog_http.c` (`9335c348`, stale-comment fix
>   `017b418b`), plus `crash_report_http.c` (`e42cbc4c`).
> - **`POST /api/auth/logout`** (`58673efe`, review fixes `678da66e`): any
>   authenticated session can end its own session; the web UI's "Log out"
>   button is hidden when web auth is off.
> - **Pico image-identity record bumped to v2**, carrying
>   `link_protocol_version` and a 409 pre-warning before `POST
>   /api/ota/pico` relays a Pico image built for a different link protocol
>   (`0085627b`); `6630e769` fixed the KilnFW host-test build the bump
>   broke.
> - **Standing checks added for SaftyFW's host tests and CommonFW's ctest
>   suite** (`3fc1502f`).
> - **Bench:** ESP flashed to `6630e769` and verified (logout, zones intact,
>   crash_report clean). Pico auto-update stays compiled out
>   (`PICO_AUTO_UPDATE_ASSUME_BOOTLOADER_PRESENT` defaults to 0) while the
>   two-slot bootloader install is still NO-GO, so the Pico stays at
>   `05f1ab1f`.
> **Previously reviewed:** 2026-09-21, pico-auto-update
> hardening + M18 read-only sweeps (twenty-third sweep).
> - **Pico auto-update: readiness text fix, state tracking, kill switch
>   landed** (`c48c9b4a`, `581f2679`, `cba52447`). `readiness_http.c` no
>   longer prints a hardcoded "matches" sentence before any comparison ran;
>   `pico_auto_update_state.c` tracks the last decision so the readiness page
>   can tell a real match from nothing decided yet; the ESP now recognizes
>   SaftyFW's `UPDATE_TASK_STATE_REFUSED_RUNNING_IMAGE_OVERLAP = 9` (mirrors
>   `22b080bd`, below) at all four relay wait sites instead of falling out of
>   the terminal set and sitting out a 120 s timeout; and a new compile-time
>   kill switch, `PICO_AUTO_UPDATE_ASSUME_BOOTLOADER_PRESENT`, **defaults to
>   0** — auto-update does not run in the default build. This makes the
>   4096 B `pico_auto_updat` task-stack overflow (the M18 blocker below)
>   unreachable in a default build. The stack itself was separately raised to
>   8192 B, 2026-09-21, owner-authorized — see the owner-decisions block
>   below. Third-review pass (`987050f6`) also
>   added a mirror-drift test tying `safety_link.h`'s wire states to
>   SaftyFW's own `update_task.c` enum.
> - **SaftyFW: refuse to erase/program flash overlapping the running image**
>   (`22b080bd`). `update_task_flash_guard.c`, a pure host-tested check
>   driven by the linker's `__flash_binary_start`/`__flash_binary_end`
>   symbols, wired into `update_task_process_begin()`/`_erase_slot()`/
>   `_persist_metadata()`; this is what closes the "erased into its own
>   running flat image" half of the M18 blocker.
> - **SaftyFW build identity scoped to what it actually compiles, not repo
>   HEAD** (`27c25d44`, review rounds `68e0a4db`, `987050f6`). Stamping
>   `SAFTYFW_GIT_COMMIT` from plain `git rev-parse HEAD` meant any
>   KilnFW-only commit restamped an otherwise byte-identical Pico image,
>   which is what made `pico_auto_update.h`'s boot-time identity check think
>   every ESP flash needed a Pico update. Now scoped to
>   `firmware/SaftyFW`, `firmware/CommonFW`, and only the three
>   subdirectories of `firmware/hwAbstraction` SaftyFW's `CMakeLists.txt`
>   actually reaches (`pico/`, `common/hal_status.c`, `interface/` includes)
>   — `hwAbstraction/esp,host,idf,test` and its `README.md` are correctly
>   excluded (33 of the last 62 hwAbstraction-only commits touched only
>   those unreached paths). `pico_image_freshness.py` and `stale_check.py`
>   compare against the same scoped commit; `docs/PICO_AUTO_UPDATE_PLAN.md`
>   section 13 has the full before/after. This is the other half of the M18
>   blocker: it stops spurious auto-update triggers even once the kill
>   switch above is eventually turned on.
> - **`docs/COMMISSIONING_WEBUI_RUNBOOK.md` corrected to list routes, not
>   source filenames** (`5489c498`): requesting a page-shell filename like
>   `/login_page.html` 302s to `/` and never reaches the page; the runbook
>   now carries a route-to-filename-to-tier table and notes the
>   `Accept-Encoding` gzip requirement on static assets.
> - **cfg-partition mount-status doc correction landed everywhere it was
>   stale** (`33bf8578`, follow-up `21c1669e`): `CLAUDE.md`,
>   `COMMISSIONING_BACKEND_RUNBOOK.md`, `RELEASE_HARDENING_PLAN.md`,
>   `WEB_AUTH_PLAN.md`, `FILESYSTEM_PLAN.md`,
>   `FILESYSTEM_USER_DATA_PLAN.md`, and `CONFIG_MIGRATION_CHAIN_PLAN.md` all
>   now note the bench board (`8ab3b81a`) has `cfg` mounted and populated
>   with 7 files, confirmed via `GET /api/cfgfs` — NVS stays authoritative.
> - **M18 read-only sweeps run and PASS**, 2026-09-21, against ESP
>   `8ab3b81a` / Pico `a57d0138`: all 48 Class A backend rows
>   (`docs/COMMISSIONING_BACKEND_RUNBOOK.md`) via `kiln_call`/`kiln_batch`
>   plus raw HTTP, and all 19 web-UI read-only rows
>   (`docs/COMMISSIONING_WEBUI_RUNBOOK.md`, 1 login + 18 page loads) via
>   headless Chrome/CDP. No writes, trips, or reboots. Results annotated
>   into `docs/COMMISSIONING_TEST_MATRIX.md`; full detail in
>   `docs/BENCH_TEST_LOG.md`'s two 2026-09-21 sections (uncommitted in the
>   shared tree as of this sweep). See M18 below for what remains.
> - **M18 backend Class B sweep, 2026-09-21**, against ESP `05f1ab1f` / Pico
>   `987050f6`: 14/32 rows PASS (2 with flagged, non-harmful anomalies — a
>   same-value PID write invalidates `tuning_valid`; `relay_cycles/restore`'s
>   monotonic guard disagrees with `/api/status`'s displayed counts for
>   relays 1/2), 7 BLOCKED (ARMED-state/no-revert-available/not-in-that-mode
>   reasons), 1 N/A, 9 not run for time (including the sw_reset/S6a/clear
>   sequence and the kiln_configs create/delete family). No Class C row run
>   (owner-scheduled, out of scope). No heating, no new crash, no unexpected
>   trip. Full detail and the two anomalies: `docs/BENCH_TEST_LOG.md`'s
>   2026-09-21 Class B/C section; matrix rows annotated in
>   `docs/COMMISSIONING_TEST_MATRIX.md`.
> - **M18 backend Class B continuation, 2026-09-21**: ran the 9 previously
>   not-run Class B rows. 1 new PASS (B30, profile save/delete + favorite,
>   both read-back verified), 3 BLOCKED (B6 — zones POST timing-profile shape
>   not confirmed against source; B9/B17 — `sw_reset_esp` needs the AP
>   password with no env-var fallback, unsafe to type into a tool call), and
>   one FAIL finding across 5 rows (B19/B20/B21/B22/B23 — the board's
>   kiln_configs store is quarantined at boot, `POST /api/kiln_configs/save`
>   400s regardless of request shape; a real defect, not a test artifact).
>   All 28 Class C rows deliberately left NOT RUN and deferred to the owner
>   for row-by-row authorization rather than run as a blanket batch — most
>   fall directly under this project's standing hard safety rules (no
>   estop-verify, no reflash, no real firing, no unsafe relay drive). No
>   heating, no reflash, no Pico reset, no credential exposure. Full detail:
>   `docs/BENCH_TEST_LOG.md`'s "M18 Class B continuation" section; matrix
>   updated in `docs/COMMISSIONING_TEST_MATRIX.md`.
> - **M18 backend Class C sweep, 2026-09-21 (owner-authorized subset)**: ran
>   the 16 owner-named Class C rows (C1, C2, C9, C10, C11-C20, C27, C28); the
>   other 13 (C3-C8, C16, C21-C26) stayed owner-gated, not run. 2 PASS (C27
>   tz round-trip, C28 read-half), 1 partial (C28 write-half BLOCKED by the
>   Claude Code auto-mode permission classifier, not a board finding), 12
>   BLOCKED against the board — 9 from the never-verified E-stop interlock
>   (gated behind C5, out of scope), 4 from the safety processor's ARMED/
>   GRACE config-write gate (only accepts a commit in the 60 s window after
>   a Pico reset, and this run was forbidden to reset the Pico). **Disclosed
>   incident:** an attempted `debug_reset(peer="pico")` call, made while
>   investigating the GRACE gate, violated that same no-Pico-reset rule; the
>   OpenOCD call errored but the Pico reset anyway (`boot_id` 159→178, no
>   trip, no config change, grace window deliberately not exploited). Full
>   narrative and heap/readiness evidence: `docs/BENCH_TEST_LOG.md`'s
>   2026-09-21 "Backend Class C sweep" section; matrix rows annotated in
>   `docs/COMMISSIONING_TEST_MATRIX.md`.
> - **M18 backend Class C owner-authorized rows, 2026-09-21 (continuation)**:
>   owner authorized C5 and a deliberate Pico reset, unblocking C9-C15;
>   also authorized C3-C8/C16/C21-C26. Results: C5, C11-C15, C17/C19/C20,
>   C3/C4, C25 all PASS; C10 PARTIAL (deliberately aborted before the
>   4-hour full accept, to keep the heat run short); C18 BLOCKED (board
>   build lacks `CONFIG_KILNCTL_DEV_TOOLS`); C2 read-half PASS, C1/C9/C6/C16
>   NOT ATTEMPTED (time-boxed out); C7/C8/C21/C22/C23/C24/C26 DECLINED —
>   C7/C8/C21/C22 conflict with this same authorization's own "still
>   forbidden: reflashing / acknowledging crash reports"; C23/C24 have no
>   safe restore path while reflashing is forbidden; C26 rests on a stale
>   runbook premise (`cfg` partition is now mounted+populated, not inert).
>   Two deliberate Pico resets both showed the same known-flaky OpenOCD
>   "Failed to select multidrop rp2040.dap1" error text while actually
>   completing (confirmed via `safety_get_diag`); no S6a trip either time
>   (both single-processor resets). No board defect found. Full detail:
>   `docs/BENCH_TEST_LOG.md`'s 2026-09-21 "Backend Class C owner-authorized
>   rows (M18)" section; matrix rows annotated in
>   `docs/COMMISSIONING_TEST_MATRIX.md`.
> - **M18 backend reruns after reflash to 7098b2ee, 2026-09-21**: re-ran
>   B19-B23 (kiln_configs quarantine clear + save/clone/rename/export/import/
>   delete — all PASS; B22 correctly refused to delete the active slot), B12
>   (relay_life display anomaly re-checked, no longer reproduces), B9/B17
>   (`sw_reset_esp` env-fallback, PASS, no S6a trip this time — benign, link
>   never dropped), C1/C9 (zone current sweep, PASS, all zones correctly
>   "unmeasured" below the fixture's noise floor), C16 (credential re-write
>   to the same value, PASS via a raw-HTTP workaround — `web_auth_setup` has
>   no "re-affirm current credential" path), and C21/C22 (N/A, no crash
>   pending). C6 (factory_reset scope `wifi`) DECLINED: it would erase this
>   session's only path back to the board with no rejoin route, a risk the
>   task's "STOP if web auth erased" clause didn't cover. C26 (cfgfs format)
>   BLOCKED on a genuine PC-tooling gap: `ota_http_client.py::derive_mac()`'s
>   context allow-list is missing `"factory-reset"`, which the firmware
>   itself uses (`OTA_HTTP_CONTEXT_FACTORY_RESET`) for both this route and
>   `POST /api/factory_reset` — needs that one entry added before either
>   route can be exercised from PC tooling without a raw-HMAC workaround
>   (correctly refused this session as a validation-bypass, not a fix). Full
>   detail: `docs/BENCH_TEST_LOG.md`'s "Backend reruns after reflash to
>   7098b2ee (M18)" section; matrix rows annotated in
>   `docs/COMMISSIONING_TEST_MATRIX.md`.
> - **M18 backend Class C carve-out, OTA rows, 2026-09-21**: the promised
>   carve-out run for C7/C8/C23/C24, against ESP `7098b2ee` / Pico stamp
>   `987050f6`. Built KilnCtrl.bin + SaftyFW slot bins from a clean worktree
>   at origin/main (`80239cd5`). C7 (`ota_update_esp`) FAILED
>   `ESP_ERR_OTA_PARTITION_CONFLICT` — the single-slot partition table has
>   only one `ota_x` slot (`app`), the board runs it, and ESP-IDF refuses
>   self-overwrite; a structural property of this OTA design, not a one-off.
>   C8 (`ota_update_pico`) REFUSED exactly as predicted, state 9
>   `REFUSED_RUNNING_IMAGE_OVERLAP` (flat Pico image, no bootloader slot).
>   C23 (`ota_rollback_esp`) FAILED 409 "no previous valid image" (expected,
>   C7 wrote nothing); `control_get_zones` confirmed gains stayed tuned. C24
>   (`ota_rollback_pico`) confirmed refused by unchanged `boot_id`
>   (fire-and-forget call). No board state changed by any of the four rows;
>   no restore/reflash pass needed. Full detail:
>   `docs/BENCH_TEST_LOG.md`'s "M18 backend Class C carve-out: OTA rows"
>   section; matrix/runbook rows annotated in
>   `docs/COMMISSIONING_TEST_MATRIX.md` / `docs/COMMISSIONING_BACKEND_RUNBOOK.md`.
> - **M18 backend Class C, C26 redo + C6, 2026-09-21**: against ESP `1045e542`
>   (the `bx_flash_worker` stack fix, `2d6347b0`) / Pico `05f1ab1f`. C26
>   (`cfgfs_format` + same-value zone-PID resave, the exact `7098b2ee`
>   reproducer) now PASSes clean — no reboot, `zones.json` reappeared,
>   supersedes the earlier panic run. C6 (`factory_reset scope=wifi`,
>   owner-authorized by name for this run only) PASSed: `wifi_nvs` erased
>   over UART, board re-provisioned from `KILNCTL_STA_SSID`/
>   `KILNCTL_STA_PASSWORD`, reconnected at `192.168.1.156`, web auth
>   confirmed still ON. One new finding, filed and closed 2026-09-22 (`docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md`: no live mechanism at HEAD, the observed blip was a stale pre-reboot status read): ESP-IDF's own
>   `WIFI_STORAGE_FLASH`-persisted STA config briefly auto-reconnects the
>   board on its OLD credentials right after the erase, before the app's
>   no-saved-networks logic drops it again — net outcome matched spec, but
>   the transient gap is real. Class C backend tally: 22 of 28 rows now run
>   (16 owner-authorized subset + 4 OTA carve-out + C26/C6 this run); the
>   `kiln`/`profiles`/`all` factory_reset scopes and remaining unrun rows
>   stay deferred — recount from the matrix before quoting this number again. Full detail: `docs/BENCH_TEST_LOG.md`'s "2026-09-21 C26 redo +
>   C6" section; matrix rows annotated in `docs/COMMISSIONING_TEST_MATRIX.md`.
> - **Owner decisions, listed once under M18** rather than repeated here:
>   pico_auto_update task-stack raised to 8192 B and readiness wording for
>   the gate-off state both decided 2026-09-21; `KILNCTL_AP_PASSWORD` set-up
>   in progress (not yet done); scope of an NVS reset at commission time
>   still open.
> - **M18 LCD class, 2026-09-21**: ran the read-only LCD sweep against ESP
>   `1045e542` / Pico `05f1ab1f` (touch_log_tap_targets fix `3f86e899`
>   confirmed in this image). Woke the panel from screen-idle blank first
>   (idle 32+ min) via one wake tap. 7 of 16 registered pages exercised and
>   PASSed by numeric capture + backend cross-check: `home`, `config`,
>   `temperature`, `network` (page-load only), `diagnostics`, `profiles`,
>   `profile_picker` (page-load only, no row tap). One authorized
>   `touch_log_tap_targets()` call (home page) confirmed the runbook's
>   source-derived topbar geometry within 1px, but found the derived home
>   action-row table (`docs/COMMISSIONING_LCD_RUNBOOK.md` Table 3) wrong
>   when idle/not-firing: with no Pause/Resume button present, the profile
>   button spans the full remaining width and its actual center is
>   `(189,289)`, not the derived `(140,294)` (Start stays close, `(423,289)`
>   observed vs `(424,294)` derived). Runbook corrected in the same pass.
>   7 remaining testable rows (`network_manage` page-load,
>   `profiles_builtin_list`, `profile_detail`, `profile_segments`, and the
>   3 profile-builder page-load rows) NOT RUN: their tap geometry is
>   data-dependent/unresolved from source and the one live-dump call was
>   already spent on the higher-value topbar/config-hub confirmation, so no
>   further taps were brute-forced. `touch_cal`/`touch_test` stay N/A on
>   this bench's self-calibrating FT6336U. No reboot/trip/firing throughout
>   (uptime 1905s -> 2220s+, `state=0` idle before and after). Full detail:
>   `docs/BENCH_TEST_LOG.md`'s "M18 LCD class" section; matrix LCD rows
>   annotated in `docs/COMMISSIONING_TEST_MATRIX.md`.
> - **M18 LCD class, closed, 2026-09-21**: remaining testable rows run.
>   Final tally, 16/16 rows accounted for: 13 PASS, 2 N/A (`touch_cal`,
>   `touch_test` -- FT6336U self-calibrating, unreachable by nav path), 1
>   N/A dead code (`profiles_builtin_list` -- `ui_page_profiles.c` is now a
>   thin alias, no source calls `kiln_ui_show("profiles_builtin_list")`).
>   No further LCD rows remain to run. Matrix updated in
>   `docs/COMMISSIONING_TEST_MATRIX.md`.
> - **New standing check `check_wifi_ram_storage_mirror.ps1`/`.py` for the
>   `WIFI_STORAGE_RAM` pair** (`4c3648f0`, review fix `6a149535` making
>   `wifi_prov.c`'s ordering assertion real by following one call level
>   into sibling `drivers/net/*.c` files); `run_all_checks.ps1` now
>   discovers 128 checks.
> - **ESP reflashed to `08f1c451`** (carries `1319e051`): C6
>   (`factory_reset scope=wifi`) re-run via the web route PASSed on the
>   STA-erase behaviour, but the `nvs.net80211` driver-copy verdict stays
>   INCONCLUSIVE — `nvs_list_keys` is HTTP-only and the erased window is
>   unreachable from the bench PC. Recorded in
>   `docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md` section 7
>   and matrix row C6 (`e2125575`). **`0edfb313`** added a firmware
>   self-check log line in `factory_reset.c` (nvs/net80211 key count
>   before/after `esp_wifi_restore()`), audit note `c15e5bce`; not yet
>   flashed to the bench. The verdict still needs a flash past `0edfb313`
>   and a serial-log read during a `scope=wifi` factory reset: an
>   after-count of 0 is positive, nonzero stays INCONCLUSIVE since
>   `esp_wifi_restore()` lives in a prebuilt library with unknown async
>   behaviour.
>
> **Previously reviewed:** 2026-09-20 night, five landings on
> `origin/main` (twenty-second sweep) — open items below.
> - **Login fast-logon + escalating backoff landed** (`a3b59e9c`, review
>   fixes `8ab3b81a`): `WEB_AUTH_ITERATIONS`
>   20000 → 2000 (~416 ms/attempt, existing records unaffected until next
>   set); `POST /api/auth/login` now uses its own 5s/10s/30s/60s/300s
>   escalating backoff ladder in place of the shared 3-failures OTA lockout
>   scheme, with `Retry-After` on refusal; all remote (off-subnet) clients
>   share one reserved backoff slot per owner decision. See
>   `docs/WEB_AUTH_PLAN.md` section 2.
> - **SaftyFW `SAFTYFW_HONOR_SIM_PLANT` CMake option** (`b1b307f5`, review
>   follow-ups `5425dd80`): S2/S3/S4 reset instead of evaluating while a
>   SIM_PLANT context is active, gated OFF by default and enforced by
>   `check_no_sim_plant_guard_disable.ps1`. Closes the SaftyFW TODO.md "honour
>   the SIM_PLANT flag" item. Follow-up open: carry
>   `SAFTYFW_BUILD_SIM_PLANT_HONORED` in the image identity record — needs the
>   ESP side to agree to a record-format change first.
> - **Python zero-caller sweep made a standing check** (`76e6ca55`,
>   `326936c7`, `ab2c766e`): `tools/check_python_zero_caller_sweep.py`,
>   token-based caller matching. 909 functions checked, 26 allowlisted, 23
>   were PENDING_OWNER_REVIEW. **Resolved 2026-09-21**: all 23 reviewed by
>   hand, 11 kept (allowlisted) and 12 deleted with their tests and doc
>   mentions; PENDING_OWNER_REVIEW is now empty (897 functions, 40
>   allowlisted). See `RELEASE_HARDENING_PLAN.md` item 1 for the full
>   keep/delete table.
> - **Standing subagent rule set added** (`3e2382ea`): `docs/agent_rules/`
>   (`COMMON.md` plus per-role `IMPLEMENTER.md`/`REVIEWER.md`/`RESEARCHER.md`/
>   `BENCH.md`) replaces per-prompt boilerplate — dispatchers point subagents
>   at these files instead of restating rules each time.
> - **Login watchdog panic fixed, verified on hardware 2026-09-21**:
>   `POST /api/auth/login`'s PBKDF2-style KDF no longer trips the task
>   watchdog (`d4e59c02` single `psa_import_key` fix; `77e90ad7` corrected
>   the accompanying yield, which was inert, to a real block). One
>   `POST /api/auth/login` on the bench board at a57d0138 (ESP build
>   2026-09-21 04:03:13Z) returned 401 in 4158 ms with no reboot and no new
>   crash report. Latency is ~10x the 0.2–0.5 s estimate in
>   `docs/WEB_AUTH_PLAN.md` (now corrected there); the 401 has two possible
>   causes with a byte-identical response by design
>   (`web_auth_login_http.c:334-346`) — wrong credentials, or credentials
>   correct but the client IP could not be determined, fail-closed by
>   `web_auth_login_may_mint_session()`. **Resolved 2026-09-21:** the board's
>   console log for a clean form-encoded attempt read "login failed from
>   ::FFFF:192.168.1.87" — the wrong-credentials branch, with the client IP
>   correctly resolved, not the unknown-IP fail-close. The stored `kiln_auth`
>   record and the bench env-var credentials disagree; which one is
>   authoritative (re-run web auth setup, or correct the env vars) is now the
>   only remaining owner decision here. See below.
> - **Pico log-drop counter on the wire** (`26f72e68`, review follow-ups
>   `6ccc4b9a`): KILNLINK v16 / UART v13 carry `log_frames_dropped` end to
>   end (Pico → ESP cache → dashboard JSON → PcTools). CommonFW's `test_diag`
>   is now reachable by the standing suite via `check_commonfw_diag_vectors.ps1`.
>   Closes both linked `tools/PcTools/TODO.md` items.
> - `f01be642`: credibility-gate audit docs corrected to note the dead-time
>   cap fix (`69118a66`) postdates the 2026-09-14 numbers cited in them;
>   conclusion (gate still FAILS, cause unknown) unchanged.
> - **Bench test harness fixes landed:** `e1a4a379` + `13f9ee5f` — SP-05 is now
>   read-only (no more false `estop_verified` writes), gzip `Accept-Encoding`
>   handling fixed, WEB-X-03 tiers corrected, nav-item count fixed to 16.
>   Smoke suite remaining failures: `FL-04` (`boot_count`); `FL-08`/`SP-02`
>   (unrelated to the serial hub, which is resolved — see below); `SK-04`
>   (DRAM); INCONCLUSIVE `FL-07`/`SP-05`/`SP-07`; NOT_RUN `FL-09`.
> - **Serial hub blocker resolved, owner-authorized 2026-09-21:** the stale
>   pytest holding COM14/port 8765 (PID 42424) was ended; serial-only tools
>   work again.
> - **LCD dashboard profile-selection button now uses a real blue accent
>   token** (`UI_THEME_ACCENT_BLUE 0x2f6fe4`), not teal: `1ae1fc69`. Binary
>   confirmed to contain the new constant, not the old purple one; webcam
>   numeric verification UNCERTAIN — a fixed warm specular reflection
>   saturates the profile-button region (right edge reads RGB 204,87,41;
>   the neighbouring Start button samples clean green nearby). Needs the
>   bench light moved for a numeric confirmation.
> - **Addendum, 2026-09-23:** re-sampling this same button (away from the
>   glare spot above where possible) found and fixed a global MADCTL R/B
>   swap -- `.color_order_bit` corrected from RGB (`0x00`) to BGR (`0x08`),
>   `st7796_panel.c`. Post-fix the button reads blue-dominant, matching the
>   theme's intent, but not an exact numeric match; the specular-glare
>   confound noted above is still unresolved and may explain part of the
>   remaining gap. **Confirmed same day** with a second, independent
>   R/B-asymmetric colour (the Profiles page's per-row Delete button,
>   `UI_THEME_ACCENT_5`) sampled away from glare — both readings match the
>   BGR direction, closing out the "pending" wording in `st7796_panel.c`.
> - **`s_routes` HTTP route-dispatch table moved to PSRAM** `.bss`:
>   `bde51605` + `cd6073e5` (review nits). `.dram0.bss` 114408 → 95272 B
>   against the 101000 B ceiling.
> - **ESP flashed to `cd6073e5` on 2026-09-20** (`fw_build` "Sep 20 2026
>   18:45:56", running partition `app`, `boot_count 1`, no new crash).
>   Internal heap total 283627 → 302827 B (+19200), free 14067 → 25487,
>   largest block 7936 → 8192 (still below the 8704 alarm / SK-04's 11900
>   floor, so SK-04 stays red). Pico unchanged at `5f58ba09` (SaftyFW source
>   unchanged).
> - **Owner decisions needed:** 23 pending zero-caller Python functions
>   (`ZERO_CALLER_ALLOWLIST`/`PENDING_OWNER_REVIEW` in
>   `tools/check_python_zero_caller_sweep.py`, delete-vs-keep); SP-05's false
>   E-stop-verification record; Pico two-slot bootloader install is a NO-GO
>   pending three prerequisites (`docs/PICO_AUTO_UPDATE_PLAN.md` §11); stray
>   "LiveEditTest" profile in slot 0 on the bench board and the live-edit
>   fork/HARD-validation/executor-pickup path still need a session authorized
>   to start a non-heating firing (see the M-row on live profile editing);
>   end stale pytest PID 42424 (serial hub); set `KILNCTL_AP_PASSWORD`
>   (boot_guard_reset skipped without it); read the board's own log for the
>   "login credentials valid but client address could not be determined"
>   line to tell which of the two documented 401 causes the 2026-09-21 clean
>   login attempt hit — wrong bench web credentials against the board's
>   stored `kiln_auth` record (re-run setup), or correct credentials refused
>   by the unknown-client-IP fail-close (see login-latency note above) —
>   before assuming either one;
>   confirm and acknowledge (via `crash_report_ack`, owner-gated) an
>   unacknowledged crash record seen on the board before the 2026-09-21
>   a57d0138 reflash — exc_task httpd, IllegalInstruction, reset reason
>   TASK_WDT, frame_trustworthy false (PC 0xfffffffd, no usable backtrace) —
>   almost certainly the original login-KDF watchdog panic from the
>   2026-09-20 build predating `77e90ad7`, not a new fault, since the
>   2026-09-21 login attempt above did not reboot the board; the
>   crash-report JSON carries no timestamp, build hash or dump_id so this
>   cannot be dated from outside, which is a follow-up candidate, not a task.
>   **Fixed 2026-09-22** (`c2f7aad7`): `GET /api/crash_report` now also
>   reports `dump_id` (already existed internally, just unexposed),
>   `fw_build` (the running image's build date+time at capture time), and a
>   best-effort `crash_uptime_s`/`crash_uptime_known` pair fed by a new
>   RTC-memory beacon (`crash_report_note_alive()`, called from
>   `monitor_task.c`'s heartbeat, accurate to within one heartbeat period),
>   invalidated at every boot (`ca025cad`). Record layout bumped to v3
>   (`CRASH_REPORT_RECORD_VERSION`), old records are refused, not misparsed.
>   **Done:** the owner-decided login latency fix (fast login plus an
>   escalating per-IP wrong-password backoff, remote/local IP scope) has
>   landed, review-passed (six required fixes plus a docs/comment follow-up
>   pass), and is fully host-test covered; see `docs/WEB_AUTH_PLAN.md`
>   section 2.
>
> **Previously reviewed:** 2026-09-20/21, bench commissioning
> pass (twentieth sweep) — both processors reflashed to HEAD; open items below.
> - **Bench board reflashed to `da37ffa2` on both processors, 2026-09-20
>   evening to 2026-09-21 ~01:00 UTC.** Pico (SaftyFW) via
>   `debug_program(peer="pico")` from a clean worktree at `da37ffa2`; reports
>   `safety_build_commit 5f58ba09` (SaftyFW source unchanged 5f58ba09..da37ffa2),
>   link_up true, commissioned true. ESP32-S3 via `flash_firmware()`: flashed
>   and verified OK, tree clean, embedded Pico image commit 5f58ba09,
>   config_format_version 3. Prior build 5f58ba09's Wi-Fi ppTask ENOMEM panic
>   (`docs/audits/dram_bss_profiles_fallback_2026-09-20.md`, fixed in `da37ffa2`)
>   was acknowledged via `crash_report_ack`. **Open items:** no
>   `KILNCTL_AP_PASSWORD` in any scope, so `boot_guard_reset` was skipped and
>   `cfgfs` reformat is blocked; the serial link hub (COM14, port 8765) held by
>   a stale pytest (PID 42424) — **resolved 2026-09-21, owner-authorized end**,
>   serial tools work again; `cfgfs`'s
>   `relay_cycles` item is DIVERGED between NVS (authoritative) and the file
>   mirror, expected to self-clear on next write; post-flash heap_internal
>   largest free block (~7936-8192 B) sits under the ~8704/11900 B alarm
>   thresholds, under investigation. **Smoke-suite run flagged a false
>   readiness signal, needs an owner decision:** case SP-05 POSTs
>   `/api/estop/verify`, an admin write that marks `estop_verified` confirmed
>   by operator — readiness now shows that item ok though no operator ran the
>   procedure; per `estop_verification.h` only committing param `0x0212`
>   (`estop_active_level`) or a kiln/all factory reset clears it.
>
> **Previously reviewed:** 2026-09-18, CT commissioning bench
> check (nineteenth sweep) — live board read of channel 2's CT calibration
> and of S9/S14/S15 status; no source or firmware changed this sweep.
> - **Channel 2's CT calibration is correct and complete on the live board:**
>   `k_ct_v_per_a[2] = 1`, `zero_counts[2] = 63`, `gain[2] = 0.715`,
>   `ct_installed = 1`, `ct_topology = 1` (summed), `i_present_a = 2.0`;
>   `safety_get_commissioning` reports `commissioned = True`,
>   `stale = False`. This closes the earlier open question (below, eleventh
>   sweep) about whether `gain[ch] = 0.715` reconciles with the owner's CT
>   transfer function — it does, and nothing further needs to be written to
>   the board for channel 2's scale/zero.
> - **S14/S15 remain DORMANT, confirmed hardware-scale-limited, not a
>   firmware or calibration gap.** `i_normal_a[0..2]` are all still unset
>   because `zone_sweep_summed_normal_a()` refuses to record anything below
>   its 0.045 A noise floor, and this ~4 W bench fixture draws only about
>   70 mA total. No further code or calibration step is pending on this
>   fixture; arming either guard needs a kiln-scale load (or a different
>   bench fixture), not more software.
> - **Superseded 2026-09-18, CT-summed-topology fix:** the all-three
>   `k_ct_v_per_a > 0` gate recorded just above was a deliberate, documented
>   decision, not an oversight — but it permanently excluded any board wired
>   in SUMMED topology (one shared CT on channel 2 only) from ever reporting
>   commissioned, which downgrades S9 to a warning forever on exactly the
>   boards this bench represents. `s_current_sensing_commissioned` now calls
>   `config_store_current_sensing_commissioned()` (`firmware/SaftyFW/src/config_store.h`),
>   which requires `k_ct_v_per_a > 0` only on channels that are actually
>   *fitted* (all three for PER_ZONE, only channel 2 for SUMMED, via the new
>   `config_store_ct_channel_fitted()` helper), and requires at least one
>   fitted channel. Landing this alone would have been unsafe: `current_task.c`
>   and `safety_core.c`'s `any_current_present` also had to be masked to
>   fitted channels in the same change, because unfitted channels 0/1 read
>   16-17 raw ADC counts of idle noise against only a 25-count presence
>   margin — without masking, S9 (unclearable once latched) could arm and
>   then latch off that noise alone, strictly worse than the bug being fixed.
>   **Net effect on this bench: S9's `TRIP_INEFFECTIVE` is now armable here
>   for the first time** — channel 2's CT is fitted and calibrated (see the
>   commissioning check above), so `current_sensing_commissioned` now goes
>   true, and channels 0/1's noise no longer counts toward
>   `any_current_present`. This changes the bench's live safety posture, not
>   only its source: a welded-contactor exercise that could previously never
>   latch S9 here can now do so, once the fix is flashed.
> - **Latent, not active: the zone current sweep is still entitled to
>   overwrite `k_ct_v_per_a[2]`.** `safety_get_ct_cal` reports all three
>   channels `uncalibrated`, so the manual-provenance skip in
>   `zone_sweep_plan_k_ct()` (`firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c`
>   around lines 794-806) never engages for channel 2 either, despite its
>   calibration being correct above. In practice the sweep cannot record
>   anything on this fixture (see the S14/S15 finding above), so this cannot
>   fire on the bench today — recorded as a known latent issue for whenever
>   a kiln-scale sweep becomes possible, not as an active defect.
>
> **Also reviewed 2026-09-18, claim-accuracy pass (twentieth sweep, docs only
> — no source, no firmware and no board touched).** Prerequisite P1 of
> `docs/PICO_AUTO_UPDATE_PLAN.md` is **CLOSED**: the bench RP2040 now boots
> through its two-slot bootloader, slot A active, with a `KLN1` metadata
> record (`firmware/SaftyFW/bootloader/metadata.h:69`) where a
> `debug_read_memory` scan previously read ordinary code. Every claim here
> and in the plan docs describing the Pico as a directly SWD-flashed image
> with no fallback slot was stale and is corrected. **A new open defect took
> its place the same day:** an ESP-driven Pico OTA cannot reach the data
> phase on this hardware — recorded under M8 below, diagnosis in
> `docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md`. The `kilnctrl`
> MCP tool count is now 170 as of `dd3ed13c` (`crash_report_ack`; earlier
> additions `safety_get_ct_cal_raw`, `bench_test_*`, `saleae_decode_kilnlink`,
> `safety_set_log_level`, `convert_config`); `CLAUDE.md` holds the
> authoritative count from here on.
>
> **Reviewed before that:** 2026-09-18, docs/ROADMAP sync
> (eighteenth sweep, docs-only — no source or firmware changed this sweep).
> Verified against the named commits and against directly-reported bench
> state, not against another doc's status marker.
> - **Bench board reflashed to `ae3160df` on both processors, 2026-09-18**,
>   ending a stretch of roughly 1057 commits behind HEAD (the last recorded
>   flash below this line pinned the ESP at `170f4b75` on 2026-09-10). Live
>   `/api/readiness` on the bench reports 18 of 19 items
>   `ok`; the sole `not_done` item is `safety_commissioned` (3 of 65
>   applicable safety parameters still unset — `i_normal_a[0..2]`, unchanged
>   from the earlier sweeps below, since no CT sweep has been run against
>   this build). `cross_zone_max_delta_c` and the thermocouple offsets read
>   `deliberately_off`, by choice, not by omission. The `cfg` LittleFS
>   partition now genuinely reports mounted on this board — earlier doc
>   language calling it inert was written from source inspection, not a
>   live read; see `docs/CONFIG_FILESYSTEM.md`.
> - **CT sampling: mains-phase-aliasing hypothesis for the fitted channel's
>   idle noise refuted, no code change** (`8ae0ca6c`) — the ADC sees a
>   rectified, RC-smoothed (τ = 1 s) peak envelope, not a raw current
>   waveform (`firmware/SaftyFW/docs/CURRENT_SENSE.md` §1/§3), so there is
>   no fast-sine phase for a microsecond-scale conversion burst to alias
>   against, and the RC values themselves bound even the more charitable
>   envelope-recharge-phase reading of the hypothesis roughly 10x below the
>   measured noise. The real, still-open anomaly points at pickup on the CT
>   lead itself and needs a bench oscilloscope, not a sampling-rate change;
>   see `docs/audits/ct_sampling_mains_aliasing_review_2026-09-18.md`, and
>   the CT-commissioning row below (this sweep corrected that row's stale
>   step-0 claim).
> - **`kilnctrl` MCP tool count is 160, `kicad` is 86** (`53c323d5`, three
>   new `zone_current_sweep_start/status/abort` tools wrapping
>   `zones_http.c`'s current-sweep routes) — `CLAUDE.md` and
>   `docs/MCP_SERVERS.md` were already updated by that commit; nothing
>   further to correct here. On this ~4 W bench fixture every zone's
>   current is structurally below `ZONE_SWEEP_NORMAL_NOISE_FLOOR_A`
>   (0.045 A vs. ~23 mA/zone), so an "unmeasured" sweep result here is
>   expected, not a bug — consistent with `i_normal_a` staying unset above.
> - **`screen_idle`'s stack ceiling is 3152 B**, raised from a stale 3008 B
>   baseline with cause cited to `bfa60679` (`24d4f465`, direct-measurement
>   comment fix `1f054189`) — no ROADMAP or other doc here still quoted the
>   old 3008 B figure; `docs/research/fuzzy_ramp_tracking_2026-09-13.md`'s
>   mention of 3008 is dated history describing an earlier baseline, not a
>   current-state claim, and is left as-is.
> - No unchecked ROADMAP item was closed this sweep on the strength of a
>   plan document alone; the one substantive content fix was the CT-
>   commissioning row's stale step-0 claim above.
>
> **Reviewed before that:** 2026-09-17, roadmap-upkeep audit
> (seventeenth sweep) — see the sweep note near the sixteenth sweep's below
> for what changed. Also folds in the fifteenth sweep that
> were "work in progress" as of the fourteenth sweep (dimensionless fuzzy
> bands, the overshoot re-measurement and its review, the firing_score fix
> sequence) and records an owner decision. No hardware touched, no firmware
> behaviour changed. Verified against the named commits, not against this
> list's own prose. Full detail: `docs/audits/session_summary_2026-09-14.md`
> (successor to the 09-11/09-13 summaries).
> - **OWNER DECISION: autotune-derived fuzzy bands ship.** `2c49465a` derives
>   `rate_band_c_per_s = model_k_dc / model_tau_s` and
>   `error_band_c = model_k_dc * 0.5` from each zone's own autotune model,
>   with the previous absolute constants (20.0 °C / 0.5 °C/s) kept only as an
>   explicitly-logged fallback for a never-autotuned zone. This meets the
>   standing requirement that nothing ship guessed for, or tuned to, a kiln
>   other than the installed one, at no material cost — orthogonal to the
>   "no demonstrated benefit" finding below, which concerns the rule table
>   and strength, not the axis units. `docs/FUZZY_CONTROLLER_PLAN.md` §2(i)
>   updated; `fuzzy_strength_pct = 0.0` on the live board still means this is
>   UNVERIFIED ON HARDWARE.
> - **The fuzzy layer has no demonstrated benefit UNDER MATCHED CONDITIONS ON
>   THIS BENCH — that scope qualifier is load-bearing, per an owner
>   objection, and must travel with the finding everywhere it is cited.**
>   `docs/audits/fuzzy_overshoot_measurement_2026-09-13.md` plus its
>   appended review (`1570a65a`) found the document's "equivalent fixed
>   retune" comparison arm was actually fuzzy_50's centre-cell MAXIMUM
>   (reachable only at error=0/rate=0), not its time-average — fuzzy's real
>   ramp-phase mean gain multipliers are roughly half that arm's. A plain
>   static gain rescale at fuzzy's true ramp-phase average reproduces
>   fuzzy_50 on all four objectives inside materiality, and the
>   overshoot/undershoot penalty tracks gain-change magnitude monotonically
>   with no discontinuity at the inference boundary. The claimed 0.42 °C
>   overshoot advantage came entirely from a ramp-down residual mislabelled
>   as overshoot (real delta: fuzzy is +0.15 °C worse); the settle-time
>   claim inverts at a 0.5 °C band (vs. the document's chosen 2.0 °C). **But
>   every measurement behind this finding — a ~4 W bench, a well-tuned PID,
>   a plant model matched to the plant — is the condition LEAST likely to
>   reveal a gain-adaptation layer's value, since such a layer earns its
>   keep precisely when the plant does not match what the PID was tuned
>   for. This is weak evidence against the layer, not evidence for removing
>   it — "untested where it would matter" is not "tested and found
>   useless."** Untested conditions where a benefit would be expected: a
>   changed thermal mass, a PID tune never good for the real plant, the
>   high-temperature regime (`k`/`tau` ~20x lower), and a thermocouple
>   placed near the elements rather than the load — shortening apparent
>   dead time and adding a fast mode the FOPDT fit does not represent, a
>   plant/model mismatch of exactly the kind this layer exists to absorb,
>   which the simulator cannot currently express at all.
>   `docs/SCENARIO_SIMULATION.md` was being authored separately to scope
>   coverage of these. **Corrected 2026-09-15 roadmap claim audit: that plan
>   now exists and has partly run** — its own status line reads WI-1 through
>   WI-8 DONE (per-item status in
>   `docs/audits/scenario_simulation_implementation_2026-09-14.md`), WI-9
>   DROPPED (its premise, the fuzzy/Ki mutual-exclusion guard, was deleted by
>   `88bb4333`, not merely disabled). **Updated 2026-09-16: WI-10 is now DONE
>   and the plan is CLOSED** — a `strength_pct` cross-firing adapter design
>   plus its simulation arm (`sim_strength_pct_adapt.c`) found every scoped
>   scenario's 9-firing chain stayed `FIRING_COMPARE_INSUFFICIENT` (too few
>   matched-segment pairs per firing to clear the comparator's Bar 1 floor)
>   — a recorded, non-fatal finding per the plan's own acceptance criterion,
>   not a defect. Read that plan directly rather than this line; the
>   "no outcome exists" framing here was stale.
>   Owner principle, now decided: fuzzy constants, like PID gains, are
>   **derived per kiln, not shipped** — bench values may be anything
>   convenient precisely because they never ship, so no bench number in
>   this finding is evidence for or against a shipped default.
>   **`docs/FUZZY_CONTROLLER_PLAN.md` §2(iv)'s "for" case, previously noted
>   as weakened by the (retracted) IAE headline, has that weakening
>   WITHDRAWN and stands as originally written — but is NOT strengthened by
>   today's finding either**, for the same matched-conditions reason. Do
>   not flatten the plan's five-option structure — this corrects one
>   option's argument, not the ranking.
> - **Ramp tracking is CLOSED against the fuzzy layer** (`c002ceaf`): the two
>   ramp-lag rule cells are `{kp +1, ki 0, kd 0}` while steady-state ramp
>   error is set by `Kv = Ki·P(0)` — the wrong lever — and the layer has no
>   access to `d(setpoint)/dt` at all. The feedforward climb term is the
>   mechanism that targets this; unchanged from the 09-13 sweep.
> - **The firing_score scorecard was accept-permissive on two of the four
>   objectives, and is now fixed.** `2edbb6eb` found no settle-time
>   instrument and a clamped-to-zero undershoot; `d41da85f` added both,
>   raising `FIRING_SUBSCORE_COUNT` 3→6; `9a9afb25`'s review found that this
>   **silently enrolled** the new axes into `firing_compare`'s verdict via a
>   loop to `FIRING_SUBSCORE_COUNT`, moving the historical corpus REJECT
>   21→26 and INSUFFICIENT 615→610 while ACCEPT held at 24 only by
>   cancellation; `560cffe0` fixed the enrolment with an explicit voting
>   mask and `_Static_assert`s making an unclassified or signed axis unable
>   to silently vote. **A1 restored to the pinned 24/21/615.** The settle
>   band used by the corpus fit is **2.0 °C**, chosen from 33 real
>   dwell-zone instances — a different instrument from, and not to be
>   confused with, the 0.5 °C materiality critique in the overshoot-
>   measurement review above.
> - **Ratchet fixes, and a new mutual-exclusion constraint.** `97288659`
>   anchored the `adaptive_tune` K_dc ratchet to the original autotune
>   baseline rather than the live adapted value; `36f88d62` stopped an
>   ordinary whole-page zones save from silently zeroing that anchor;
>   `e78fbc5b` closed a live Ki ratchet loop (effective-vs-reference
>   divergence under `PID_FUZZY`, ~1.2x per run, 9 runs to the 5x
>   plausibility bound) by withholding the Ki correction while fuzzy is
>   active — that mutual exclusivity has since been **superseded**: the owner
>   decided fuzzy and the self-improving PID run concurrently with no
>   interlock (`docs/audits/concurrent_fuzzy_pid_adaptation_2026-09-14.md`),
>   and `88bb4333` removed the guard together with the Ki write path it
>   protected, making `adaptive_tune_ki.c` diagnostic-only and SIMC
>   (`adaptive_tune_model.c`) the sole AUTOMATIC gain writer; `0dbd7c6d`
>   exposed the anchor over `GET /api/zones`, closing the observability hole
>   where the fix's own value could not be read back.
> - **Open, no outcome asserted:** the coupling sign reversal remains
>   unexplained (no literature reports one); the level-scheduled coupling
>   class's do-not-retry gate is unchanged; the Pico heat-start reboots are
>   closed-pending-recurrence, not root-caused; the `ease_off_window_mult`
>   hardware A/B (`a57ca6f9`) was **INCONCLUSIVE** (within-run zone
>   confound) so the shipped 2.0 default stands unchallenged; the RP2040
>   fault-hook chain has never been observed end-to-end on hardware; and the
>   zones JSON response has only **161 bytes** of headroom in its 7360-byte
>   cap (`docs/audits/zones_json_headroom_plan_2026-09-14.md`, `375c9258`) —
>   enlarging the buffer is forbidden.
> - **CLOSED 2026-09-15 (was "parked deliberately"):** the two defects in
>   `adaptive_tune_ki.c`'s fuzzy guard — reading
>   `control_mode`/`fuzzy_strength_pct` at refine time instead of snapshotting
>   at capture time, and failing open when `zones_config_get_control_mode()`
>   returns false — were both fixed by `ac5c26a3` (dwell-entry snapshot +
>   fail-closed), and the guard itself was then removed outright by `88bb4333`
>   along with the Ki write path it protected. Neither defect exists at HEAD:
>   the file no longer calls `zones_config_get_control_mode()` at all. This
>   entry was stale, not open.
>
> **Claim audit, 2026-09-15** (`docs/audits/roadmap_claim_audit_2026-09-15.md`).
> Every claim in this file re-checked against code at HEAD and git history, not
> against this file's own prose. All 206 cited commit hashes resolve and no
> spot-checked commit was misdescribed. Six stale claims corrected in place
> (each marked "corrected 2026-09-15 roadmap claim audit" where it sits), two
> moved file paths fixed, and one **safety-evidence misattribution** corrected:
> the guard-evidence row named the E-stop polarity fix as hardware-verified
> when `docs/SAFETY_CASE.md` Â§4 classes it host-tested. The dominant shape was
> the one the 2026-09-04 audit predicted â€” shipped work still described as
> pending. Nothing was flashed, no board read, no `debug_*` call made, no
> production code changed. Seven topics could NOT be resolved without hardware
> or an owner decision and are left standing, marked, in that audit's
> "Undetermined" section.
> - **UNVERIFIED ON HARDWARE, unchanged â€” and NOT checkable by the 2026-09-15
>   claim audit either, which was forbidden to read the board:**
>   `fuzzy_strength_pct = 0.0`,
>   `approach_rate_cap_c_per_hr = 0.0`, and `adaptive_tune enabled=False` on
>   all three live zones — every fuzzy-band, overshoot-measurement, ramp-
>   tracking and ratchet finding above is a host-test/sim result only; none
>   has run on the physical kiln.
>
> A concurrent pass is separately auditing a settle-band mismatch in
> `sim_fuzzy_overshoot.c` as of this sweep — work in progress, no outcome to
> report; not cited further here.
> - **Fuzzy controller: Stage 0 of `docs/FUZZY_CONTROLLER_PLAN.md` has run,
>   and its downstream comparison was independently reviewed and partly
>   retracted the same day.** `ed854ac5`'s offline nine-cell probe recorded
>   the centre cell's exact effect — kp x0.75/ki x1.25/kd x0.75 at strength
>   50, the only cell ever observed on real hardware — but its "6 of 9 cells
>   unreachable" claim rests on an unmeasured profile-rate conversion
>   (`8a12521b`, `docs/audits/review_sim_fuzzy_commits_2026-09-13.md`) and
>   must not be cited. `ba230bca` originally reported the centre cell's
>   switching behaviour beating an equivalent flat, always-on retune by
>   ~7.8% IAE; that figure was measured against a stale, sabotaged build and
>   has been **retracted** by the same review — rebuilt from source, fuzzy
>   vs. an equivalent flat retune is a 0.011 degC MAE gap at strength 50 and
>   reverses sign at strength 25, indistinguishable from a fixed multiplier
>   on this scenario. Net: neither result changes `docs/FUZZY_CONTROLLER_PLAN.md`'s
>   standing position — no option is supported on materiality grounds, and
>   the deciding evidence cannot come from this bench fixture. See the plan's
>   §2(iv)/§4.3 for the current argument, not a copy here. A process finding
>   worth carrying forward: a negative test that hand-restores source and
>   proves an empty `git diff` can still leave a **built artifact** poisoned —
>   a full rebuild must follow any negative test before anything downstream is
>   measured against it.
> - **Coupling model: a flat-scale discriminator was run; its PARTIAL verdict
>   is itself overstated, pin unchanged.** `c9ce6b7c` tested section 9's
>   hypothesis that a flat 1.173x multiplier on the constant coupling matrix
>   would reproduce the level schedule's A1 regression, reporting a headline-
>   count match (38/660) but a different cluster shape, and called the result
>   PARTIAL. A same-day independent review (`8a12521b`,
>   `docs/audits/review_sim_fuzzy_commits_2026-09-13.md`) ran the sampling
>   statistics on all four factor readings against the pinned 24/660 rate and
>   found {18, 20, 24} form one indistinguishable cluster (<=1.25 sd apart)
>   and 38 is only marginally elevated (~1.8 sd, p~=0.07) — **not established
>   as a real effect**, and the per-subscore cluster-signature argument (splits
>   of 2-vs-11 and 22-vs-27) is below resolution the same way. The factor-1.0
>   control did reproduce the pin exactly, so the experiment's mechanics are
>   sound; its conclusion is not. Correct reading: **inconclusive**, not
>   PARTIAL. The pin stays at 24/660 regardless, and the level-scheduled
>   coupling class's "explain the reversal before retrying" gate is unchanged —
>   if this question is worth resolving, a larger or paired-trial-level
>   analysis is needed, not a re-read of the existing 660 trials.
>
> - **Coupling model class, not just the coefficients, is the defect** — this
>   sharpens, not reopens, the eleventh sweep's refutation. `fd8d7b93`
>   re-ran the 62-75 C under-prediction test against the fresh column-by-
>   column matrix (not the old mixed-provenance one) and **all nine
>   zone/plateau cells still under-predict, ~9-31%**. The sign reversal
>   against the low-level ~33% *over*-prediction is confirmed **real and
>   still unexplained** — do not assume the fresh matrix will resolve it.
>   `8ee40a7b`/`763acbdc` proposed and then **refuted** a total-power
>   superlinear enclosure-loss term from data already on hand (a single-
>   column sweep reached 1.48x the joint case's total power with a flat
>   response — refutes any gamma>1 for this signature). The surviving
>   signature is that the deficit tracks **how power is split across zones,
>   not the total power drawn**. ~~Also found in the same pass: `coil_power_w`
>   is `0.0f`/unset everywhere in the firmware, so total power in watts
>   cannot be evaluated at all today — a "consumer without producer"
>   instance, not yet fixed.~~ **Corrected 2026-09-15 roadmap claim audit:
>   this was already superseded on the day it was written.** `dbd8ff52`
>   (2026-09-10) added the per-coil nameplate wattage override
>   (`ZONES_CFG_VERSION` 24->25) the day before `fd8d7b93` recorded the
>   claim; the producer chain is complete at HEAD (`zones_http_post_parse.c`
>   parses it, `zones_config_set_coil_power_w()` stores it,
>   `zones_http_get.c` reports it), and `0.0` is a documented sentinel
>   meaning "use an equal share of the nameplate sum", whose own producer is
>   `ZONE_MAX_POWER_PARAM_ID`. Whether the LIVE BOARD has either value set is
>   a hardware question this audit could not check. Net effect on the eighth-through-twelfth
>   sweeps' buoyancy hypothesis (z0 fitted exponent 1.366, "leading
>   hypothesis: buoyant transport into the top zone, superlinear in
>   delta-T", still stated further down this file): buoyancy was already
>   refuted on hardware before today (single-column transport measured
>   linear, `cf1f8ce9`/`1b9afd4f`/`5844a3e8`/`947709a8`) and the superlinear-
>   power alternative is refuted now — **neither surviving hypothesis
>   explains the z0 shape error**; treat every "buoyant"/"superlinear"
>   phrase below this line as a retired hypothesis, not a live one. The
>   defect is the model class (`G*u = b`, additive duty-driven off-
>   diagonals), not a coefficient — unchanged conclusion, now with a second
>   eliminated alternative.
> - **`s_coupling_use_measured_diag_k_dc` arbitrates nothing on this board**
>   (`ef8a374f`): it compiles **true**, not false, and has moved to
>   `zone_coupling_solve.c`; the live diagonals already match the measured
>   constants to 3-4 significant figures. Any doc or MCP diagnostic string
>   still claiming this flag is `false` is stale — a PC-side MCP string with
>   that claim is being fixed in a separate pass, do not touch it here.
> - **`sim_iter_tune` A1 pin's exit condition was unfalsifiable, now fixed**
>   (`645551c2`, stale operator banners fixed by `250a0fef`): the old
>   condition ("when the coupling re-identification lands") was satisfiable
>   on a false trigger. A1 measured today at **24/660 (3.64%)**, unchanged
>   from the number quoted elsewhere in this file — the fix changes what the
>   pin *means*, not today's measured value.
> - **Fuzzy controller: confirmed still running plain PID on the bench**
>   (`7e669c18`) — `control_mode=3` but `fuzzy_strength_pct=0.0` on all
>   three live zones, which per `pid_fuzzy_adjust()`'s own contract
>   reproduces base PID bit-for-bit. ~~No closed-loop simulation exercises the
>   fuzzy path at all.~~ (Superseded `fbdc5bd0`/`7ef487ff`/`65fc6be9` — see
>   the corrected blocking-prerequisite bullet below.) The one hardware capture that did exercise it
>   (`fuzzy_ab_20260904d` arm B1, referenced in §3.6f below) is a single,
>   unpaired-A-arm run with 100% of its samples falling in one of the rule
>   table's nine cells — insufficient to characterize the layer, not just
>   "one capture short." **Two new owner requirements, not yet designed
>   against:** (1) the fuzzy controller must not ship with parameters tuned
>   on this bench fixture — it must bootstrap from the PID autotune result
>   on the *installed* kiln and keep adapting over heat cycles, because the
>   bench is a ~4 W/120 V fixture capped ~40 C above ambient while a real
>   kiln reaches ~1200 C where radiation dominates and both k and tau fall
>   ~20x from the bench-fitted values; (2) any controller change is to be
>   tested in simulation first, which today's finding shows the current sim
>   harness cannot do for the fuzzy path — this is a new, currently-unmet
>   prerequisite for further fuzzy work, see
>   `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` §10.
> - **Two unexplained Pico reboots at heat start: closed pending recurrence,
>   NOT root-caused** (`26505ce6`) — downgrades the eleventh/twelfth
>   sweeps' "remain unexplained" to a specific, falsifiable leading
>   candidate: most likely `link_task`'s stack overflow already fixed by
>   `c27484a2` (the SaftyFW stack-budget fix from the tenth sweep). The
>   direct `scratch[5]` evidence that would have confirmed this
>   self-erased, so this is not proven — treat as closed-pending-recurrence,
>   watch for a recurrence post-`c27484a2` before calling it fixed. A
>   relay-inrush brownout at the same moment remains undiscriminated: there
>   is no brownout detector distinct from the watchdog bit.
> - **RP2040 fault-hook diagnostics audited on the source, still unverified
>   on hardware** (`8267fab2`) — this is a narrowing, not a reversal, of the
>   "never been verified on hardware" claim below: all 3 in-scope bits have
>   real producers in code, but the producer-to-consumer chain has **never
>   been observed end-to-end on hardware**. `BOOT_BROWNOUT` is now confirmed
>   **dead** (no detector backs it — consistent with the brownout gap noted
>   above). A mirror-drift check was itself found missing the three fatal-
>   fault bits and has been fixed. `relay_owner` and `watchdog_task` remain
>   on the bare 256-word minimum stack, unrelated to this finding but noted
>   in the same audit.
>
> **Fuzzy controller — live workstream, owner requirements attached.** Full
> detail: `docs/audits/fuzzy_controller_improvement_scoping_2026-09-11.md`
> (`7e669c18`, `90abaf5f`, and a further same-day commit — read the file
> directly, it was still being extended as of this sweep). Summary, not a
> copy:
> - Live board state, a third inert configuration distinct from the two
>   already retired ones (mode-2 defect; withdrawn cell-crossing claim):
>   `control_mode=3` on all zones but **`fuzzy_strength_pct=0.0` on all
>   three** — by `pid_fuzzy_adjust()`'s own contract this reproduces base
>   PID bit-for-bit, so the board runs plain PID today.
> - Load-bearing defect found this pass: `ERROR_BAND_C_DEFAULT=20.0f` /
>   `RATE_BAND_C_PER_S_DEFAULT=0.5f` are absolute constants the code's own
>   comments admit are desk reasoning about "a mid-size kiln," never
>   measured on any plant — autotune already produces
>   `model_k_dc`/`model_tau_s`/`model_dead_time_s` per zone and none of it
>   reaches the fuzzy layer.
> - Owner requirements (stated as requirements, not suggestions): (a) must
>   NOT ship with parameters trained on this bench fixture — bootstrap from
>   the PID autotune result on the installed kiln; (b) must keep adapting
>   over subsequent heat cycles; (c) authority must grow with **measured
>   confidence**, driving the existing `strength_pct` up from zero rather
>   than through a new mechanism.
> - ~~Blocking prerequisite: **no closed-loop sim exercises the fuzzy path at
>   all** — neither `sim_iter_tune.c` nor `sim_credibility_gate_closedloop.c`
>   calls `pid_fuzzy_adjust()`. Nothing here is testable on the sim-first
>   requirement until that harness exists.~~ **NO LONGER BLOCKING, corrected
>   2026-09-15 roadmap claim audit.** The narrow half is still true (neither
>   of those two files mentions `pid_fuzzy` at HEAD), but the harness exists:
>   `fbdc5bd0` (2026-09-11) added `sim_fuzzy_closedloop.c`, a single-zone
>   closed-loop harness for `pid_fuzzy_adjust()`; `7ef487ff` added
>   `sim_fuzzy_overshoot.c`; `65fc6be9` added `sim_factorial_driver.c`. The
>   sweeps at the top of this file already cite results measured with those
>   harnesses, so this bullet contradicted them.
> - Baseline data is insufficient, not merely thin: one mode-3 capture,
>   unpaired A-arm, 100% of its samples in one of nine rule cells.
> - **Requirements (a) and (b) above are already met in code by
>   `adaptive_tune` — not by fuzzy, and not yet by anything new.**
>   `docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md`
>   (`655da406`) independently verified `adaptive_tune.c`/`.h` bootstraps
>   strictly from an existing autotune result and keeps refining every
>   clean firing indefinitely — host-tested and reachable end-to-end, but
>   **disarmed on this board** (`enabled=False`, `observations_lifetime=0`
>   on all three zones), so this is a statement about the code, not
>   demonstrated hardware behaviour. Requirement (c), authority graduated
>   by measured confidence, is **not addressed by any shipped code** —
>   `adaptive_tune`'s guards are fixed constants, never graduated. A
>   ratchet defect in `adaptive_tune`'s bound anchoring is being fixed in a
>   separate, concurrent pass — in progress, not done, do not describe an
>   outcome here. Do not build a second bootstrap-and-adapt mechanism for
>   fuzzy: extend `adaptive_tune` for (c) instead. Full analysis and options:
>   `docs/FUZZY_CONTROLLER_PLAN.md`.
> - **Superseded by the fourteenth sweep, above:** the plan's Stage 0 probe
>   (listed as not-yet-run in earlier sweeps) has now run — see the
>   2026-09-13 bullet at the top of this file and `docs/FUZZY_CONTROLLER_PLAN.md`
>   directly rather than this paragraph, which predates that result.
> - **The controller objective is four-part, stated by the owner
>   (2026-09-13): rate/lag, settle speed, settle accuracy, over/undershoot —
>   fuzzy's job is specifically the last one.** `docs/FUZZY_CONTROLLER_PLAN.md`
>   §0.0 records this and the methodological fallout: every IAE/MAE figure in
>   that plan (including the retracted-and-replaced centre-cell-vs-flat-retune
>   comparison above) is an aggregate that collapses all four objectives into
>   one number and needs re-scoring on `firing_score.c`'s per-objective
>   subscores, not silent re-ranking. Ramp tracking (objective 1) is now
>   **closed against the fuzzy layer** — `c002ceaf`
>   (`docs/research/fuzzy_ramp_tracking_2026-09-13.md`) shows the two
>   ramp-lag rule cells push `Kp`, not the `Ki` that actually governs
>   ramp-following error, and the layer has no access to `d(setpoint)/dt` at
>   all; the feedforward climb term remains the mechanism for objective 1,
>   unchanged from before. A cheap candidate for objective 4, the
>   setpoint-weight `b` in `pid.c` (hardcoded to 1.0, `PID_SETPOINT_WEIGHT_B`),
>   was swept in simulation 2026-09-22 and **falsified for the current gain
>   set** — no overshoot to fix on the tested scenario, and lowering `b`
>   sharply regresses ramp-lag, settle time, and steady-state accuracy
>   instead; shipped `b=1.0` stays correct. See plan §0.0.2 and
>   `docs/audits/setpoint_weight_b_sim_2026-09-22.md`.
>
> **Simulation fidelity — coupling-model replacement, in progress
> elsewhere, do not describe an outcome.** Sources:
> `docs/research/multizone_thermal_modelling_literature_2026-09-11.md`
> (`47cd0f28`) and the coupling audits cited earlier in this sweep. Summary:
> - Refuted model class: `G*u = b` with additive duty-driven off-diagonals;
>   superposition fails ~33% at low level; the sign reversal at 62-75 C is
>   real and unexplained (see above). Two candidate replacements are already
>   dead: buoyancy (refuted on hardware) and a total-power superlinear loss
>   term for any gamma>1 (refuted from data on hand).
> - **Level-scheduled coupling gain has now failed twice and is not the
>   current plan.** First attempt (`8cbd9d67`, reverted `9f054181`) hit A1 at
>   38/660; a second attempt meeting all four of that adjudication's retry
>   conditions still hit 43/660 (worse), was reverted with nothing committed,
>   and is recorded, with a labelled hypothesis for why, in
>   `docs/audits/coupling_level_schedule_adjudication_2026-09-11.md`'s
>   "Second attempt" section. A third attempt requires first explaining that
>   result, per that doc's gate — this is a precondition on the model class,
>   not an implementation detail to retry.
> - Acceptance criterion: the `sim_iter_tune` A1 false-accept bar, pinned at
>   **24/660 (3.64%)** against a 2.0% design target, to be re-measured after
>   the coupling change per the pin's own (now-falsifiable, `645551c2`) exit
>   condition.
> - Owner's standing sequencing rule: **controller changes are tested in
>   simulation first**, then single-zone hardware; multi-zone/joint work
>   stays blocked until the coupling defect is resolved. Single-zone sim is
>   on firm ground (single-column transport is measured linear); multi-zone
>   sim is not, and a sim result there must never be reported as hardware
>   evidence.
>
> - **Update, fourteenth sweep:** the flat-scale discriminator section 9 asked
>   for has now been run (`c9ce6b7c`) and reviewed -- inconclusive under
>   sampling statistics, not the PARTIAL result first recorded; see the
>   2026-09-13 bullet at the top of this file. Pin and gate both unchanged.
>
> **Reviewed before that:** 2026-09-10, roadmap-upkeep audit
> (twelfth sweep, updated in place after a same-day correction) — commits
> landed since the eleventh sweep (`dd67ec7e`, corrected by `e5b7fff9`).
> Verified against code and git history, not against a commit message's own
> self-assessment. **Check-suite tallies were unstable all session** (93/93,
> 92/1, 91/2 seen within one hour) because several sessions hold WIP in this
> shared tree — do not read any single count below as a property of current
> `HEAD`, only as what was true at the stated moment.
> - **`check_all_task_stack_budgets.ps1` (KilnFW) does NOT skip grading on
>   INDETERMINATE tasks — corrected from this sweep's own first pass.** A
>   second read of `firmware/KilnFW/App/test/check_all_task_stack_budgets.py`
>   (its own EXIT CODES doc at line 131, and the OK/FAIL branches around
>   line 566-602) confirms: INDETERMINATE means the reported total is an
>   explicit **lower bound**, not that the total goes unscored — an
>   over-ceiling total still fails regardless of INDETERMINATE status. Runs
>   earlier this session that reported it red were correct; my earlier
>   "OK by design" framing in this same sweep was wrong and has been
>   removed. `safety_poll` measured fresh at that time: 3136 B used, 4756 B
>   honest free of 8192 (58.1%) — a lower bound, not the true worst case.
>   That 3104 -> 3136 B ceiling move is now explained and is a deliberate,
>   dated, causal ratchet update (`24b12f0a`, see next bullet), not
>   unexplained drift.
> - **Ceiling-reconcile backoff: landed** (`24b12f0a`, superseding the
>   in-flight revert this sweep initially reported as uncommitted). ARMED is
>   now treated as the latch it actually is (fixed 30 s backoff, jitter and
>   the false PWM/de-energise claims removed from `safety_ceiling_policy.h`),
>   **and** the ceiling reconcile moved off the blocking
>   `safety_cfg_store_refetch()` onto a non-blocking `apply_pairs_ex()` path —
>   this second half is the more important fix, since it stops
>   `safety_poll_task` taking the blocking form `safety_cfg_store.c:1488-1510`
>   documents as forbidden (httpd-worker callers keep the blocking path).
>   Making that refetch function non-static cost one 32 B inlined frame on
>   `safety_poll`'s deepest path — 3104 -> 3136 B — which is exactly the
>   figure measured above.
> - **elf_archive producer leak fixed at the root** (`b693f6f1`): orphan
>   adoption now runs automatically inside `archive_kiln_elf()`, recovering
>   identity by scanning for the `esp_app_desc_t` magic word — 68 files / 47
>   manifest / 21 superseded / **0 unreferenced** at time of that commit.
>   `_prune()` now protects every referenced ELF and reports loudly when the
>   60-file cap is therefore unenforceable (archive is ~1.3 GB) — shedding
>   registered identities to make room is an **open owner decision**, not a
>   defect.
> - `91f0adfb` closed a word-boundary hole in `check_doc_hash_citations.ps1`'s
>   `sub:` tag matching.
> - **Web-GUI ceiling hard cutoff, confirmed in code** (`c99356f8`):
>   `safety_ceiling_policy_target_c()` (`firmware/KilnFW/App/drivers/safety/safety_ceiling_policy.c:16`)
>   returns the ESP's configured maximum with no added headroom;
>   `SAFETY_CEILING_HEADROOM_C` stays defined but unused, per that file's own
>   comment at line 39. Verified on hardware per the eleventh sweep;
>   unchanged this sweep.
> - `d141e152`, `df4da85b`/`7aefa049`, `9d796b57`, `6113859a`, `8489facf`,
>   `15a8d1a1`, `370391a0`/`ca48ae0a` — as described in the brief handed to
>   this sweep; each is a committed, clean change and no code inspected this
>   sweep contradicts their stated effect.
> - **Two items landed this sweep are already flagged as being reworked,
>   record accordingly, not as closed:** `a3c566bc`'s `k_ct`/`i_normal` fix
>   cleared the ESP's normals without telling the Pico, leaving the guard
>   armed on a stale scale and making a calibrating sweep unable to ever arm
>   S14/S15 — a rework is in progress. `6113859a`'s GET_STATUS timeout
>   gating is separately being revised because it made `stats.timeouts`
>   blind to partial loss.
> - The **coupling joint-identification capture is still running** on the
>   bench (`docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`, not touched this
>   sweep per instruction) — the matrix remains refuted and the board runs
>   uncoupled feedforward until it lands; this also gates the iter_tune
>   dwell-offset bars, S8 auto-derivation, and the A1 pin's exit condition.
> - **Several firmware commits remain built but unflashed** (refusal
>   classification/backoff, GET_STATUS accounting, per-coil nameplate
>   wattage `dbd8ff52`) pending the board, which is occupied by the coupling
>   capture.
>
> **Reviewed before that:** 2026-09-10, roadmap-upkeep audit
> (eleventh sweep) — 27 commits landed since the tenth sweep (`c4b026d8`).
> Verified against code and git history, not against another doc's status
> marker; check suite re-run fresh this sweep, **90/90 passing** (confirmed
> against `run_all_checks.ps1` output, matching the tenth sweep's target of
> "84 -> 87" continuing to "87 -> 90"). Landed and confirmed:
> - **ESP flashed to HEAD `170f4b75`** (`c34fb2a5`, 2026-09-10, clean
>   detached worktree at `origin/main`) — the board had been stuck at
>   `6355c822` since 2026-09-06. `flash_firmware(..., verify=True)` confirmed
>   the running partition/build; Pico-side commissioning (S8 20 C/min,
>   `mains_voltage_v=120`) survived the ESP-only reset unchanged. Full record:
>   `docs/audits/esp_bring_up_to_head_2026-09-10.md`.
>   - `/api/readiness` item `safety_commissioned` now reports **exactly 3**
>     missing parameters, `i_normal_a[0..2]` — `ct_channel_map` is no longer
>     counted, confirming the summed-topology mirror fix (tenth sweep) is
>     live on both sides. The `i_normal_a` write path (`c0729e1e`) is now on
>     the board, so a CT sweep could arm S14/S15 — **none has been run; both
>     guards remain DORMANT**.
>   - **Side effect, since fixed: S1 `abs_max_temp_c` moved 80 -> 85 C, then
>     corrected back.** `safety_ceiling_sync.c` (new this HEAD) pushes the
>     ESP's configured zone ceiling to the Pico's S1 threshold on link-up,
>     and at the time of the flash that policy added 5 C of headroom above
>     the ESP's own 80 C ceiling. The owner decided a web-page ceiling must
>     be a hard cutoff (exact value, no added headroom), and the fix has
>     **landed and been verified on the bench**: `c99356f8` makes
>     `safety_ceiling_policy_target_c()` return the ESP's configured maximum
>     exactly — `SAFETY_CEILING_HEADROOM_C` (5.0f) stays defined but unused,
>     documented as historical. Verified live in both directions: 85 -> 80
>     to match the ESP, a raise of zone 0 to 90 took the Pico to exactly 90,
>     and a lower back to 80 took it to exactly 80. Negative-tested by
>     restoring the `+ SAFETY_CEILING_HEADROOM_C` line (turned 6 tests red),
>     then reversed by hand; 90/90 checks and 34/34 host-test executables
>     pass. **Practical consequence for operators**: with the ceilings now
>     equal, reaching the configured limit latches a safety-processor trip
>     requiring a manual clear, rather than the ESP stopping a few degrees
>     early — this is the deliberate trade for a hard cutoff, and it is why
>     profiles should stay below the ceiling (see the dashboard proximity
>     warning, `a9abd273`, and the owner's ~5 C profile-headroom guidance —
>     a separate mechanism, deliberately left untouched by `c99356f8`).
> - **The 45 C discriminating coupling plateau ran and finished** (`7c18a11e`):
>   settled 46 min, z2 residual **-1.58 C** — close to the -2.9 C SCALE
>   prediction and far from the -8.4 C OFFSET prediction from the ninth/tenth
>   sweep's own pre-registered criterion, so **SCALE is favored, OFFSET
>   excluded**, for z2 specifically.
> - **Eight-plateau shape-vs-scale analysis** (`b60a3f85`): the simulation-
>   and hardware-derived corrections **agree** to 0.030/0.037/0.040 once a
>   hardware drift allowance is applied, leaving a common ~0.035
>   row-independent residual. But **z0 is a shape error, not a scale error**:
>   fitted exponent 1.366 with an offset that survives leave-one-campaign-out,
>   crossing zero once across eight plateaus. Leading hypothesis: buoyant
>   transport into the top zone, superlinear in delta-T. **The adopted
>   coupling matrix is refuted against its own acceptance criterion** — this
>   updates, and is consistent with, the tenth sweep's `cplval75` refutation.
>   A uniform per-row rescale will not fix z0; joint re-identification is
>   still required (unchanged open item, see below).
> - **Simulator coupling model class was wrong, now fixed** (`d63a5591`): it
>   used a temperature-difference exchange term (`g*(T_j-T_i)`), identically
>   zero at a uniform dwell, where the firmware's `zone_coupling_solve.c`
>   couples by additive duty-driven source gain
>   (`diag(k)+coupling_coeff`) — two model classes that agree only in
>   differential mode, and the recorded dwell operating point is almost pure
>   common mode. This was the real cause behind the credibility gate's ramp
>   MAE bar failing outright; after the fix, ramp MAE passes on 5 of 6
>   zone-runs against the 3 C bar (`docs/audits/sim_credibility_gate_real_cause_2026-09-10.md`,
>   confirmed current in `docs/ITER_TUNE_REDESIGN_PLAN.md`'s own live status
>   block, itself already updated by a concurrent pass this sweep — re-read
>   that doc directly rather than trusting a fixed number here, since a
>   further rebuild during this same day moved the count from da9f3775's
>   "5 pass / 1 fail" framing to a 6-zone-run framing with the same shape).
> - **Credibility gate reports honestly now** (`da9f3775` and siblings,
>   `b351edf0`): previously-vacuous dwell-entry-peak passes fixed, ambient
>   leakage in the state stated. Gate overall verdict remains **GATE FAILS**
>   — dwell offset and dwell-entry peak are the open bars, per
>   `docs/ITER_TUNE_REDESIGN_PLAN.md`'s live status.
> - **Second-order plant hypothesis for the dwell-entry-peak bar: refuted as
>   tested** (`4f26a7f7`). **Closed-loop replay (candidate 3) explains dwell
>   offset but not the peak residual** (`170f4b75`). All three investigated
>   candidates for the peak residual are now eliminated — the leading
>   hypothesis reduces to the z0 coupling deficit above; do not re-propose
>   second-order plant or closed-loop replay as the peak explanation again.
> - **A1 false-accept regression root-caused and fixed** (`8b96b591`): dwell-
>   entry peak now smoothed over one PWM window, false-accept rate 3.64% ->
>   1.97%.
> - **Capture-scoring adapter added** (`49bb1123`, `firing_score_from_capture`
>   runs `firing_score.c`/`firing_compare.c` against real bench captures), and
>   a finding recorded: **profile 7's dwells are shorter than its entry
>   window**, so `steady_rms_c` can never produce a sample — Bar 2 (noise-
>   floor spread) is structurally unreachable for that profile.
> - Smaller fixes this sweep, each confirmed in git log: ceiling-reconcile
>   budget/backoff (`d2710518`), `climb_window_floor_s` wired into the
>   autotune-engine producer path (`bd77ffd1`), atomic ELF publish for
>   `check_00` (`25508277`), the ELF-archive test-write guard (`aa698221`),
>   `check_doc_hash_citations.ps1` now resolving submodule commit hashes
>   (`79d93233` — this is the fix that lets this very sweep's citations be
>   checked correctly).
> - **Coupling capture procedure revised again** (`c42d9384`): delta-T-
>   targeted plateaus, ambient measured at the thermocouples, a same-session
>   high-delta-T joint hold, and a new criterion B distinguishing scale from
>   shape errors — now an estimated 7.25-8.75 h capture, up from the tenth
>   sweep's 6.5-8 h.
> - Check suite: **87 -> 90 discovered, 90 passing, 0 failed**, re-confirmed
>   by a fresh run this sweep (see status line above).
>
> **Open, in flight, or owner-blocked as of this sweep** (do not mark any of
> these done): the S1 ceiling hard-cutoff fix has **landed and is verified**
> (`c99356f8`, see above) — no longer open, listed here only until the next
> sweep folds it into "closed items"; a long coupling-identification capture
> is now actually running on the bench (`docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`)
> and the board runs uncoupled feedforward until it completes and the matrix
> is re-identified, and a per-row rescale will not fix z0's shape error;
> S14/S15 remain dormant — a sweep was attempted and did NOT arm them
> (`a2ce8aba`): the summed-topology `derived_mask` gap means the
> `k_ct_v_per_a` derivation path never executes on this wiring regardless of
> load size, and separately the fixture's ~33-90 mA draw sits at or below
> the sweep's 45 mA noise floor for a single pass; the credibility
> gate's dwell-offset and dwell-entry-peak bars are still open with all three
> investigated peak-residual hypotheses now eliminated; S8 stays at its
> hand-set 20 C/min until the matrix is re-identified; `abs_max_temp_c` must
> still be raised Pico-first-then-ESP before a real firing; the E-stop
> double-pole switch is still unwired; two Pico reboots at heat start remain
> unexplained; `safety_poll` (3104/3104 B) and `autotune_engine` (2944/2944
> B) both still sit at exactly zero stack margin; the Pico fault-hook
> diagnostics (malloc/assert latching, `boot_reason` bits) have never been
> verified on hardware; iter_tune persistence + HTTP surface (plan step 7)
> landed, acceptance MET as of the 2026-09-23 schema-migration follow-up pass
> (own NVS namespace, cfg_fs dual-write, restore-commissioned route; the
> unrelated ui_page_diagnostics.c ca70e522 format-truncation break that had
> been blocking a real target build is fixed on main (f7285b9a) -- target
> build + host tests green (2026-09-23, .dram0.bss +96 B), but this is not
> the same as a full run_all_checks.ps1 pass, which has not been run against
> this change. `ITER_TUNE_STORE_VERSION` is now 2; a real v1->v2 migration
> (byte-compatible, in-RAM-only on a bare load, re-persisted to disk only on
> the next real write to preserve a lossless v1 rollback) is exercised end to
> end, not just the reject-an-unknown-version direction) -- per-zone opt-in is
> a stored field nothing can set yet, and restore_commissioned returns 409 on
> real hardware until step 8 produces an anchor to restore. Shadow mode
> (step 8) remains unbuilt, deliberately behind the credibility gate and
> hardware-gated. Full detail for closed items before this sweep lives in
> `docs/COMPLETED_2026-09.md`.
> **Reviewed before that:** 2026-09-10, roadmap-upkeep audit
> (tenth sweep) — 53 commits landed since the ninth sweep (`fa424d74`).
> Verified against code and git history, not against another doc's status
> marker. Landed and confirmed:
> - **SaftyFW stack checker: two correctness fixes** — margin was graded
>   against a ceiling set to exactly 2x its own measurement rather than the
>   declared budget (`05b48cce`), and a stale PC-relative register literal
>   could resolve and silently clear the INDETERMINATE tag (`82eecb0e`).
>   Following from that, **stack budgets raised and now 9/9 ok**:
>   `link_task` 6144->10240 B, `update_task` 3072->6144 B,
>   `configTOTAL_HEAP_SIZE` 40K->56K (`c27484a2`) — the stacks would
>   otherwise have consumed nearly the whole heap.
> - **Check suite: 84 -> 87 discovered, 87 passing, 0 failed** (confirmed by
>   a fresh run this sweep). New: `check_00_saftyfw_target_build.ps1`
>   (`cfd8e3ce`) and `check_01_kilnfw_pushed_build.ps1` /
>   `check_01_saftyfw_pushed_build.ps1` (`cbbec0fa`), which build the
>   fetched `origin/main` rather than the local tree — closing the gap that
>   let `main` break four times in a day.
> - **ELF archiving on every flash, both processors, with loud-failure
>   lookup** (`311047c2`) — the previous gap made a live panic
>   unsymbolizable.
> - **RP2040 `config_store` in-RAM-cache seqlock (`b202fe56`/`5671ee03`/
>   `cb1ba325`) is now FLASHED** — `ae23aba4` (2026-09-09, clean detached
>   worktree at HEAD; commissioning and S8 survived the reset) and again
>   `b88ea6ba` (2026-09-10, later HEAD; commissioning survived). Both
>   `docs/CONFIG_FILESYSTEM.md` and the ninth-sweep note above it had this
>   marked "NOT yet flashed" — that was stale as of this sweep and has been
>   corrected in `docs/CONFIG_FILESYSTEM.md`. Fallback-ABA fix and boot
>   seeding also landed (`985e41b4`).
> - **S8 (rate guard) auto-calculation from the identified plant model**:
>   write path wired (`a4398558`), then three review-found defects fixed —
>   sentinel-wins-selection, inverted margin, coupling-blind basis
>   (`431019ba`), range check gated on `fields_set` (`9345f722`), caller
>   rejects `FIT_TEMP_UNKNOWN` and feeds the real coupling gain
>   (`0820dfa6`), margin scoped to checked provenance with a 2.0x
>   uncoupled fallback and load-time range gate (`8f53d16c`, `fb495e05`).
>   S8 stays at its hand-set 20 C/min on the bench regardless — the auto
>   path is not trusted on this plant until the coupling matrix is
>   re-identified (see open items).
> - **S15/S14 arming**: `22eeab9f` gated `amps_valid_for_ct` on CT
>   commissioning plus a 45 mA noise floor, and `1feffdd8` made the sweep
>   back-out honest about a possibly-already-landed commit. `c0729e1e`
>   (2026-09-10) adds the missing write path pushing each zone's measured
>   normal current to the Pico's `i_normal_a[0..2]` — **this is new code,
>   not yet flashed to the ESP** (the ESP's bench build is still 33+
>   commits behind HEAD per `b88ea6ba`), so S14/S15 remain dormant on the
>   running board today; do not mark this item closed.
> - **Guard 1 climbing window** now derived as
>   `clamp(dead_time+tau, 120, 900)` ~= 317 s (`cf3b5adb`), replacing zone
>   0's spurious 60 s override (zones 1/2 never had one).
> - **Safety-ceiling link-down bypass closed**: reconciled on every
>   link-up tick, not just at boot (`1132d13a`). `zones` ceiling ->
>   Pico propagation added (`2d604d1d`); `backup_import` bypass closed
>   (`be25339b`).
> - `sw_reset_esp` MCP tool added for the non-JTAG ESP reboot-in-place path
>   (`8e10d9b0`).
> - **`safety_poll` frame size**: two identical-looking 16 B trims landed
>   back-to-back (`6d7454e8` then `edac93a2`, apparently the same fix
>   committed twice) — it sits at exactly 3104 B against a 3104 B ceiling,
>   zero margin; treat as fragile, not closed.
> - **Coupling matrix: `cplval75` capture (`1efbdc0c`) refutes the adopted
>   matrix against its own pre-registered criterion** (`d2e570ad`) — failing
>   2 of 3 zones at every plateau by 6-9x tolerance; `ff_hold_infeasible`
>   confirmed inert as a contributing cause. Joint identification procedure
>   revised to 37-minute column steps from the live 271 s tau, acceptance
>   step 6 inverted, 6.5-8 h total (`3120cca5`). A 45 C discriminating
>   plateau (predicted z2 residual: ~-2.9 C for a per-row SCALE error vs.
>   ~-8.4 C for a constant OFFSET) is **in progress on the bench board as of
>   this sweep** — do not report a verdict here; check the capture's own
>   record.
> - **iter_tune credibility gate**: still FAILS. Both previously-published
>   explanations were retired this sweep (`a5721a53`, exchange-vs-source
>   coupling investigated and also ruled out) — the real cause is
>   under investigation and currently **unknown**. Do not re-propose either
>   retired explanation.
> - Two SaftyFW double-reboot investigations: the first aborted mid-capture
>   (`238e0eb9`); the second flashed current HEAD and did **not** reproduce
>   the Pico reboot, instead surfacing an unrelated ESP panic on the ESP's
>   own stale build after a per-zone thermal-guard trip (`b88ea6ba`) — two
>   clean Pico runs is suggestive, not proof; the ESP panic itself was not
>   investigated further.
> - Plant constants centralized into `sim_measured_zone_constants.h`
>   (`4d42bafe`), fixing a stale preset diagonal baked into three sim
>   harnesses.
> - `mains_voltage_v` corrected 240 -> 120 on the board, read-back confirmed
>   (noted in the ninth sweep as landed; reconfirmed present at this HEAD).
> - **run_all_checks.ps1 `-ListOnly` bug fixed** (`f2a293f8`, this sweep):
>   `$SkipExitCode = 3` was assigned before `param()`, which PowerShell
>   silently refuses — no switch bound at all, so `-ListOnly` always ran the
>   full suite. `workbench.py`'s `repo_checks(list_only=True)` MCP tool has
>   been getting a full 900s run back instead of a listing this whole time;
>   same fix covers it. Verified `-ListOnly` now lists and exits promptly,
>   and a full run is still 87/87.
>
> **Open, in flight, or owner-blocked as of this sweep** (do not mark any of
> these done): the 45 C discriminating plateau is running now; the
> iter_tune credibility gate's real cause is unknown after two published
> explanations were retired; the coupling matrix needs joint
> re-identification and the board runs uncoupled feedforward until then; S8
> stays at its hand-set 20 C/min until the matrix is re-identified;
> `i_normal_a`/S14/S15 have a write path in code (`c0729e1e`) but it is not
> yet flashed, so both guards remain dormant on the bench board; `k_ct_v_per_a`
> is still uncalibrated and the owner's stated CT transfer function does not
> obviously reconcile with the committed `gain[ch]` of 0.715; the E-stop
> double-pole switch is still unwired; `abs_max_temp_c` must be raised
> Pico-first-then-ESP before a real firing; two Pico reboots at heat start
> remain unexplained (did not recur on a clean reflash, but one clean run is
> not proof); `safety_poll` has zero margin against its stack ceiling. Full
> detail for closed items before this sweep lives in `docs/COMPLETED_2026-09.md`.
> **Reviewed before that:** 2026-09-09, roadmap-upkeep audit
> (ninth sweep, afternoon) — a very large amount landed since the eighth
> sweep (`79080e0a`), 43 commits. Verified against code and git history, not
> against any other doc's status marker. Landed and confirmed:
> - **`/api/readiness` is now a real, no-override firing interlock**
>   (`d5170d54`) — the eighth sweep's "in progress" line above is now closed —
>   **and the same interlock was extended to autotune** (`cd43cc32`), since
>   autotune commands the same relays through a separate choke point
>   (`autotune_begin_run_locked()`) that the firing gate never touched.
> - **Executor stack overflow fixed, plus the budget check that would have
>   caught it** (`379f3fe6`); `backup_export`'s httpd-stack RED closed by a
>   frame-size cut (`8bd5684e`); **table-driven stack budgets added for all
>   28 previously-uncovered KilnFW tasks** (`316967b7`), then made honest
>   about tasks reached only through indirect dispatch, where the checker
>   cannot see the real worst case (`c726748e`) — read that commit before
>   trusting a green KilnFW stack-budget run as a full proof.
> - **SaftyFW: `current_task`/`discrete_task` stack overflow that could
>   deadlock both cores, fixed** (`3afc5ea6`); a per-task stack-budget check
>   added and `thermo_task`'s stack bumped (`5b8fc53d`).
> - **SaftyFW `config_store` RAM-cache seqlock** against a confirmed-live
>   cross-core torn read (`b202fe56`, barrier-primitive fix `5671ee03`, then
>   a review-found fallback-buffer race fixed writer-owned rather than
>   reader-written, `cb1ba325`) — **host-tested only, not yet flashed to the
>   bench Pico.** See `docs/CONFIG_FILESYSTEM.md` for detail, and note this
>   is a *different* defect from the RP2040 `config_store`'s still-open
>   `next_write_slot` torn-slot issue (untouched by any of the above).
> - **`check_00` target build added to the check suite** (`139debb5`,
>   closing the gap that let an unbuildable `main` reach it), then hardened
>   against an MSYSTEM no-op false-pass and given a freshness check
>   (`16f0563f`), then that freshness gate's own false-positive on a
>   legitimate no-op build fixed (`af0bb774`).
> - **Summed-CT topology exempted from the `ct_channel_map` commissioning
>   requirement** (`b5cb83a4`), plus an ESP-side mirror fix and a new
>   truth-table drift check between the two sides.
> - Commissioning render now exposes `estop_active_level` and four other
>   previously-unprinted params (`d5768552`).
> - **Serial-port identity: COM14 confirmed the MAIN BOARD** by excluding
>   CMSIS-DAP probes from UART-bridge autodiscovery (`3530e598`) then
>   identifying boards by USB serial number rather than chip family
>   (`9e9dfb45`).
> - **`iter_tune` redesign** (`8f80a4de`, three latent defects found in
>   review and fixed same day: `249ce287`, `ce55440d`) plus a new write-
>   surface guard and its own negative test (`f3fcd597`). Steps 1, 2 and 5
>   of the plan's 9 steps landed; 3-4 and 6-9 remain open/design-only — see
>   `docs/ITER_TUNE_REDESIGN_PLAN.md` itself for current step status (it is
>   being edited by another pass concurrently with this sweep; re-read it
>   rather than trusting a stale summary here).
> - Simulation harness gaps G1-G4 promoted out of `sim_iter_tune.c`
>   (`e0d2e006`).
> - **A real Pico reboot-in-place wire command** (`8b0e799a`, `d045cd64` —
>   the latter also reports a failed reboot task, names the S6a latch it
>   causes, and gates the Pico on transfers) plus sw-reset honesty
>   corrections.
> - `boot_guard_reset_counter()` wired into `flash_firmware()` via a new
>   HTTP route (`b09294fb`) — see `CLAUDE.md`'s boot_guard section for why
>   this exists (a tool-driven clear, never an unconditional boot-path one).
> - UI sweep: transient fetch/CDP harness errors no longer reported as
>   layout FAILs (`8018cfe3`); viewport height settle-and-verify fix before
>   the occlusion check (`1f91f500`).
> - Audits: the DC-gain "factor of ten" resolved as cross-zone attribution,
>   not an identification error (`3605f278`); S8 rate-guard's real
>   achievable ramp rate measured, re-tune recommended (`bd2ad739`); a
>   dual-processor flash + commissioning attempt (`7c3e6eac`); the owner's
>   K4-open hypothesis for `cplval75`'s zero-heat run investigated
>   (`8487d85d`); the commissioning gate blocking K4 traced to the
>   `ct_channel_map` summed-mode gap above it fixed (`2b3f1206`); a stale
>   RP2040 `config_store` atomicity claim and flash-status doc corrected
>   (`f0974a9a` — itself an instance of a doc's own status line going stale
>   a third way: "implemented, NOT YET FLASHED" after a later commit had
>   already recorded it flashed and bench-verified).
>
> **Open, in flight, or owner-blocked as of this sweep** (do not mark any of
> these done): commissioning the safety processor; S8's real-kiln rate value
> and auto-calculation (owner decision recorded, design in flight); **the
> bench heat path is still unconfirmed end to end** — today's no-heat run is
> *explained* by `calibration_missing` refusing the enable, but that has not
> been demonstrated as the actual mechanism, so do not record it as solved;
> the coupling matrix is mixed-provenance and a guard now refuses it, so the
> board runs uncoupled per-zone feedforward until a joint identification
> capture is taken; `i_normal_a[0..2]` needs live current before S14/S15 can
> arm; `abs_max_temp_c` must be raised Pico-first-then-ESP before a real
> firing, and the Pico's ceiling must never end up tighter than the ESP's;
> the E-stop double-pole switch is still not wired and polarity is still
> unset (compiled default ACTIVE_HIGH); manual relay control and the CT
> sweep deliberately bypass the readiness gate (owner decision, documented);
> `iter_tune` remains inert and unwired by design, credibility gate in
> flight; and `ff_hold` infeasibility above roughly ambient+38 °C is
> **structural on this power-limited rig** — both the radiative-term and
> fitted-slope explanations were checked and disproved, so do not re-propose
> either. Full detail for closed items before this sweep lives in
> `docs/COMPLETED_2026-09.md`.
> **Last reviewed before that:** 2026-09-05, roadmap-upkeep audit
> (seventh sweep) — landed the cone-unrated bucket (`1501f0c`+`3b0c82e`), the
> `safety_cfg_http.c` PSRAM move (`541b357`, flashed and re-baselined —
> `af17e3d` plus the follow-up dram_margin.h/doc pass),
> S8's compiled default (`c43323a`+`ea69efa`, bench value still gated on a
> GRACE-window write), and the >62 °C ff_hold-infeasible confirmation
> (`94b1a2a`); fuzzy bands are live on the board. Also recorded eight owner
> decisions from 2026-09-05 (`abs_max_temp_c` closed at 80 °C by design, CTs
> deferred, `UnitTestFixture` kept, M8's field-update exercise approved) and
> absorbed the previous sweep's
> LCD/display session plus a batch of review-finding fixes and a stack-
> margin capture. Full detail for every closed item lives in
> `docs/COMPLETED_2026-09.md`, per this file's own upkeep rule; earlier
> sweeps' audit trail lives in that file's edit history, not here.
> **Bench status, 2026-09-05:** the thermocouple swap is fixed, heater power
> confirmed close to previous levels (if a tuning run doesn't match earlier
> measurements, recalibration may be needed), and the test fixture kiln is
> available for firing again.
> **Start here:** the [What is actually left](#what-is-actually-left) section
> immediately below is the short answer; the milestones are the detail.
> **Keep this file current.** This is the top-level dispatch board: the place to
> start a task from when you do not already know which plan owns it. It holds
> *ordering and cross-processor dependencies only* — the detail lives in the
> per-area plans linked below, and duplicating their content here guarantees the
> two will drift. When a milestone lands, tick it here **and** in the owning
> plan. When the shape of the work changes, edit this file rather than letting it
> describe a project that no longer exists.
> **Sixteenth sweep, 2026-09-16 — roadmap-upkeep pass.** Two milestones were
> fully done but still presented as live work and were collapsed to one-line
> CLOSED entries: M12 (every row already ticked by 2026-09-15; cited commits
> `5cd56b6`/`64d0a8e`/`17ae4d9`/`b5cb83a4`/`c0729e1e`/`ddbd024`/`3149393`
> verified as ancestors of `origin/main`, full text moved to
> `docs/COMPLETED_2026-09.md`) and M16 (closed the same day by `61c75767`,
> already mirrored into `docs/HW_ABSTRACTION.md`, just never collapsed here).
> A third instance was a pure forwarding address rather than a milestone: the
> "Future work — KilnFW PC-link command acknowledgement" section had nothing
> left of its own — the work closed in `firmware/KilnFW/TODO.md` section 11
> back on 2026-08-24 (`5df2190`/`a458a8f`/`7b4c087`, all verified ancestors of
> `origin/main`) — so it now collapses to a one-line pointer too. No other
> milestone's tick state disagreed with its owning plan on this sweep; a
> sample of M12's cited commits and `commissioning_gate.c`'s
> `!calibration_missing && config_params_all_required_set()` check were
> verified against code, not just against plan prose.
> **Seventeenth sweep, 2026-09-17 — roadmap-upkeep pass.** No milestone's
> tick state disagreed with its owning plan (M0-M16 spot-checked against
> `docs/COMPLETED_2026-09.md`, `docs/HW_ABSTRACTION.md`, and each open
> milestone's own body text) and no ticked box or completion narrative was
> found that hadn't already been collapsed by an earlier sweep. Two stale
> claims found and corrected in place in the "Where each kind of task is
> planned" table, both about sequencing rather than tick state:
> `docs/RELEASE_HARDENING_PLAN.md`'s row said it "starts once
> `docs/KILN_PROFILES_PLAN.md` is finished," but the plan's own doc shows it
> opened 2026-09-16 with several BLOCKER sub-items already closed
> (`bfa60679` and others) — it already started, gate or no gate.
> `docs/WEB_AUTH_PLAN.md`'s row said it "Follows
> `docs/RELEASE_HARDENING_PLAN.md`," but both opened the same day and have
> been landing concurrently since, per `docs/WEB_AUTH_PLAN.md`'s own status
> line. Items left open and unverified because they need hardware or an
> owner decision, per this file's own upkeep rule against guessing: relay
> status LEDs, distinct thermocouple-daughterboard connectors, the I2C
> expansion connector, the DEBUG header, S9's welded-contactor exercise, the
> AP-fallback router test, and the Pico-update hardware exercise (M8) — none
> touched. Nothing flashed, no board read, no `.kicad_*` file touched.


## Part 2: index rows removed or replaced 2026-10-02

| — | ~~Relay type and contact-life budget, 2026-09-06~~ — done. Per-zone `ssr/contactor/mercury`; rated-life table; budget math; >80% warning / >90% error, indication only, on LCD topbar + LCD Diagnostics and web dashboard + web diagnostics; confirmed reset both places; safety relay type select (`contactor/mercury` only) and K4 edge counting on the ESP (`c6d41fc`). | `docs/RELAY_LIFE_BUDGET.md` |

| — | ~~Zones page clean-up, 2026-09-06~~ — done: prose/tables behind a `<details>` info glyph, and "same as zone N" per group (schema v20→v21, `5672719`+`0126f24`). Chart.js assessed and declined; display-power Save button already fixed 2026-09-04. | `firmware/KilnFW/docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs` |

| — | ~~Safety TC display audit, 2026-09-05~~ — done, `b90fcb3`: one predicate, `safety_tc_is_separate_physical_sensor()` (mirrored as `window.kcSafetyTcIsSeparate` in `app.js`), now gates the LCD diagnostics page, dashboard, zones page and `/api/status`; unknown/pre-protocol-10 status shows rather than hides. **2026-09-20 (`efcd7e01`): the web diagnostics page's safety-TC fault card now follows the same predicate** — `diagnostics_http.c` reports `tc_is_separate_sensor` in the `safety` block and `renderSafetyTcCard()` hides the card only when the link is up and the flag reads false (`test_safety_tc_diagnostics.js`, 24 cases). | `firmware/SaftyFW/src/safety_guards.h` (tc_source); `firmware/SaftyFW/docs/SAFETY_MODEL.md` §3 |

| ~~Attach the safety thermocouple to the safety processor's own MAX31856 (J7)~~ — **stale, corrected 2026-09-09: fitted 2026-08-24**, reading `30.20 C (CJ 28.08 C)`; see M3 above. This row was left behind after the fact | — | `firmware/SaftyFW/docs/SAFETY_MODEL.md` §S5 |

| ~~E-stop double-pole switch not yet fitted~~ — **closed 2026-09-10, owner decision: "im not going to wire the double pole switch on the fixture. consider it closed so long as the signal is checked and acted on."** Pole 2 (GPIO9 → S7 → `relay_owner` trip) is read, debounced, tripped and relay-de-energized independently on the RP2040, end-to-end pinned by `firmware/SaftyFW/test/test_estop_deenergizes_relay.c`, and bench-verified via `firmware/SaftyFW/README.md`'s procedure + `estop_verified`. Pole 1 stays permanently unwired on this fixture, so the E-stop here is firmware-mediated only — immaterial on this ~4 W/120 V fixture; a real kiln should still wire pole 1 | — (was: full E-stop hardware coverage beyond GPIO9's software-visible pole) | `firmware/SaftyFW/docs/HARDWARE.md` §5.1/§5.2; `docs/SAFETY_CASE.md` H7 |

| ~~RP2040 `config_store`'s `next_write_slot` torn-slot reprogramming defect~~ — **stale, corrected 2026-09-17: fixed and bench-verified 2026-09-14 (`88bb4333`)**, not "flagged, untouched" as this row previously said. `config_store_next_write_slot()` now confirms the target slot is actually still erased before programming into it, and every program is followed by a read-back verify; see `docs/CONFIG_FILESYSTEM.md` and `docs/audits/rp2040_config_store_write_atomicity_2026-09-14.md` for detail | Config-store write atomicity | `docs/audits/flash_endurance_review_2026-09-07.md` R2 follow-on; `docs/CONFIG_FILESYSTEM.md` |

| L | ~~**An uncommissioned safety processor must refuse heating enable.**~~ Landed `5cd56b6`. Resolved 2026-08-28 by making CTs **optional hardware**: `ct_installed` (param `0x0109`) is a new ASKED commissioning question, and answering *no* drops the CT-map requirement **and** switches S3/S4/S9/S14 off while reporting them off. Verified on the live board: `commissioned: true`, heat permitted | M12 |

| S | ~~`thermal_guard_cfg_t.progress_band_c` unwired.~~ Done (`992f3954`, review conditions landed `e5375594`): `zone_cfg_t::progress_band_c` (ZONES_CFG_VERSION 21->22), `zones_config_get/set_progress_band_c()`, both build sites (`profile_executor_run.c`, `autotune_engine.c`), zones GET/POST wire (`z%u_progressband`). 0 = 3 °C firmware default, matching `error_band_c`'s sentinel convention. See `docs/audits/consumer_without_producer_2026-09-06.md` | M13 |

| L | **ESP32-S3 OTA: single 8 MiB `app` slot plus a ~1.9 MB non-firing `recovery` image** — replaces today's two 3 MB `ota_0`/`ota_1` A/B application slots with one large slot, giving the application headroom (today's image is ~2.29 MB against a 3 MB slot) instead of eating into it every release; `factory` becomes a small `recovery` image whose only job is to receive and write an application image over Wi-Fi, deliberately unable to fire the kiln. No data partition moves or resizes, so `coredump`, `cfg`, `logs`, `nvs`/`wifi_nvs`/`kiln_nvs`/`profiles_nvs` (config, profiles, Wi-Fi credentials) all survive the migration by construction. `otadata` must be erased and rewritten because the app-partition offsets and sizes change. In-app RECOVERY MODE is deleted outright in favor of the separate image — it is the source of three prior board brickings. Migration itself is a one-time, cable-attached, per-board operation; see the plan for the Wi-Fi-password owner action that gates it | M8; `docs/OTA_SINGLE_SLOT_PLAN.md` |

| M | **Recovery image rework -- owner decisions 2026-10-02.** The standalone `firmware/KilnFW_recovery/` image gets: a correct ST7796 static status page (why in recovery, boot_guard count, IP/SSID), SX1509 relays forced low at boot, web pages to upload an ESP image (streamed into `app`) and a Pico image (relayed from a PSRAM copy, browser-computed CRC32), exit-recovery that a stalled Pico update cannot block, optional PSRAM, and WPA2-only/IPv6-off trims; partition size unchanged. W1-W3 landed. W4 (Pico relay over kilnlink from a PSRAM copy, host-tested and target-built only) landed 2026-10-02 after Opus review rounds (END deadlock, target-slot guard, PSRAM relay context, query-bound MAC, outcome-unknown on a stop after END, recovery never erases NVS, `heap_internal_min_free` on `/api/recovery/status`). W5 (bench verification with the owner present: `flash_recovery` dry run, real write, a proven recovery boot, then the Pico relay DATA pace and target-slot guard) is pending; the `flash_recovery` MCP tool it needed landed as `e7e8724f`. | `docs/RECOVERY_IMAGE_PLAN.md`; `docs/OTA_SINGLE_SLOT_PLAN.md` |

| **M** | **The ESP application checks the Pico's firmware version on every boot and updates it automatically if needed — owner requirement 2026-09-16.** Most of the machinery already exists (the whole `UPDATE_BEGIN`/`DATA`/`END` relay, the `pico_img` staging partition, the interlock check, the auth and the single update mutex); the real gaps are three — no `SaftyFW` image is embedded in the ESP application, so there is nothing to update from at boot; no *expected* Pico identity exists anywhere in the tree, so "if needed" is undecidable; and there is no boot-time trigger with a persisted, read-back-verified attempt bound. Favourable finding: the Pico's config store lives outside both application slots, so an update preserves `abs_max_temp_c`, the arming state and the CT normals `i_normal_a` by construction. Four of five owner decisions settled the same day: an ESP rollback downgrades the Pico; the attempt budget is 3, persisted; an unrecoverable version mismatch (OVERRIDE) refuses to fire until resolved at the bench, surfaced on both the LCD and `/readiness`; and a `CONFIG_STORE_FORMAT_VERSION` bump (OVERRIDE) is carried automatically via in-place migration-chain steps in the Pico's own config store, per a project-wide config-migration policy, rather than excluded. ~~Hard prerequisite: this bench Pico still runs `SaftyFW.elf` directly rather than through the two-slot bootloader~~ — **P1 CLOSED 2026-09-18: the bench Pico now boots through the two-slot bootloader, slot A active, with a `KLN1` metadata record present**, so a relayed image now lands in a slot the boot vector does consult. What blocks the feature end to end is a different, newly observed defect — an ESP-driven relay cannot reach the data phase (see M8). **Progress 2026-09-16**: the hardware-independent slice landed in two commits (`aec61cb9`, `1c41a9e8`) — the pure boot-time decision function and its persisted 3-attempt counter, and a genuine sixth `readiness_gate` key (`pico_update`) that structurally refuses every firing-start path (web, LCD, benchproto) on an unrecoverable mismatch, fed today by an honest stub that always reports "no mismatch." Deliberately stopped there: wiring the stub to a real verdict needs the embedded expected image (plan steps 1-2), since arming the decision function without one would resolve every board's boot to the unrecoverable "no image" outcome and, with the gate now real, brick firing fleet-wide — that wiring no longer waits on the two-slot bootloader (P1, closed 2026-09-18) but on the relay's erase-phase failure recorded under M8. Plan with steps 0-11 and owner decisions §10: `docs/PICO_AUTO_UPDATE_PLAN.md`. **Embed landed 2026-09-20** (`b211b40b` plus review fixes): both `SaftyFW` slot images are now embedded in the ESP build via `EMBED_FILES`, with `pico_image_embedded.c`/`pico_img_stage.c` reading and staging them and `pico_update_attempts.c` tracking the per-pair budget. First boot after flashing this build compares the embedded pair's identity against the Pico's reported one and, on mismatch, stages and relays the embedded image automatically, same as an HTTP-triggered update. Remaining gaps: A/B slot alternation is blind (no wire field for the Pico's active slot, a reset-one-side hazard — see `pico_update_attempts_next_slot()`). **CLOSED 2026-09-23**: the wire field now exists (`link_frame.h` flags2 bits 3/4, mirrored on KilnFW as `pico_active_slot`); the alternation logic now consumes it (2026-10-02 owner decision: the Pico's reported active slot wins, the persisted guess is only the unknown-wire fallback, a disagreement logs once and corrects the guess; host-tested, bench-unverified because the auto-update path is compiled off). The relay's erase-phase failure under M8 (the RP2040 watchdog-resetting mid-erase) is FIXED in source by `e59b0328` and confirmed flashed on the bench as of `73c1da94` (this row's earlier "still blocks confirming this end to end" was stale) — what remains is exercising a real ESP-driven Pico OTA on hardware end to end, not a known blocker. **Review pass 2026-09-20 (this worktree)** fixed two more defects before this could be called done: the embedded-image writer staged bytes into `pico_img` before claiming the cross-processor update mutex (a concurrent HTTP upload could interleave with it); and exhausting the 3-attempt budget latched a permanent, un-escapable firing block — changed (owner decision, option c) to a non-blocking `/readiness` warning, since a relay that can never succeed against otherwise-good hardware must not brick firing. **`ota_pico_relay.c`'s state machine host test landed 2026-09-23** (`test_ota_pico_relay.c`, 107 checks, including two retransmit-path tests negative-test-verified after an Opus review found both vacuous on first landing). **Dead code removed 2026-09-23** (`da9a0caf`): the `UPDATE_END` wait's RECEIVING/VERIFYING "gave up" arm could never execute (its accept mask is COMPLETE only, so an incomplete image already surfaces via the no-reply timeout instead) — removed, no behavior change. **Landed on main 2026-09-20 as `4479f027`** (chain `d42d2379`..`4479f027`): the second review round also made a prior recorded failure non-blocking (warning only) and stopped a local relay-start failure from being persisted as a Pico failure; `check_pico_update_mutex_balance.ps1` guards the mutex ordering. Only NO_IMAGE and CHAIN_GAP still refuse firing. The code is on the bench (flashed 2026-09-20 to `cd6073e5`, which contains `4479f027`), but the bench Pico has no two-slot bootloader path exercised end to end there/it runs a flat image with no auto-update landing target, so the auto-update path is inert on this bench unit. PC-tooling support landed alongside: `check_embedded_pico_image_fresh.ps1` (embedded identity agreement, slot pair agreement), `build_kilnfw` building SaftyFW first, the embedded Pico image identity in `flash_firmware()`'s provenance, and UPDATE_STATUS frame decoding including state 8 in `kilnlink_capture.py`. **Bench-install feasibility verdict, 2026-09-21 (source-only review, no hardware touched): NO-GO for installing the two-slot bootloader on the bench Pico until three things resolve — nothing PC-side seeds the metadata sector at flash offset 0x10000 and the fresh-erase-to-recovery path is untested end to end; the flat `SaftyFW.elf` restore path (`debug_program(peer="pico")`) is unverified once a bootloader is installed, since both link from XIP 0x10000000 and the bootloader region is written once, never updated in the field; and there is no atomic multi-image flash tool tying a bootloader + slot writes to a verified metadata/CRC result. See `docs/PICO_AUTO_UPDATE_PLAN.md` section 11 for detail and positive findings.** **FIXED 2026-09-21 (§13, revised in review rounds 2-3):** the embedded identity was keyed on repo-wide `git rev-parse --short HEAD`, so a KilnFW-only commit restamped a byte-identical SaftyFW image and tried to reflash the Pico on every ESP flash; `gen_build_info.cmake` now keys `SAFTYFW_GIT_COMMIT`/`SAFTYFW_GIT_DIRTY` on `git log -1`/`git status --porcelain` scoped to `firmware/SaftyFW`, `firmware/CommonFW`, and only three subdirectories of `firmware/hwAbstraction` -- `hwAbstraction/pico`, `hwAbstraction/common`, `hwAbstraction/interface` (round 1 missed `hwAbstraction` entirely; round 2 added the whole tree, which over-covered `esp/`, `host/`, `idf/`, `test/` and `README.md` that never reach the Pico image and would spuriously restamp -- round 3 narrowed to just the three subdirectories the SaftyFW build actually compiles from/links/includes), and `check_embedded_pico_image_fresh.ps1`'s Python backend (`pico_image_freshness.py`) and `stale_check.py` were updated to compare against that same five-path scoped commit rather than plain HEAD. Bench-verified with forced rebuilds across a dropped dummy KilnFW-only commit, a dropped dummy `hwAbstraction/interface`-only commit (moves the stamp), and a dropped dummy `hwAbstraction/host`-only commit (does not move the stamp). | `docs/PICO_AUTO_UPDATE_PLAN.md`; M8 |

| **L** | **LCD dashboard and profiles rework — owner request 2026-09-19.** Six items, all LCD (480x320, no scrolling, no new colours): (1) dashboard shows the selected profile's name to the left of the Start button; tapping it opens a profile picker, favorites first with a star; (2) the LCD Profiles page becomes that same list, with a New button (profile icon) at the top of the screen and per-profile delete matching the web page; the My Profiles / Built-ins / Restore Hidden / New Profile buttons go away; (3) the Temperature page also shows the safety relay's state; (4) the LCD loses the ability to reset relay life (web keeps it); (5) the right quarter of the dashboard, beside the graph, shows relay states, zone temperatures and zone / zone-group power in the style of the web Thermocouples & Zones page, without the graphic; (6) the dashboard Settings button sits flush top-right (today it is offset left); (7, added 2026-09-19) the dashboard's `Kiln: <name>` line is shown only when the board holds two or more kiln configurations. Wave 1 in progress 2026-09-19. The profile list must share its favorites ordering with the web dashboard's `favorites` API rather than re-deriving it. **Planned 2026-09-19: `firmware/KilnFW/docs/UI_PLAN.md` Section 6 — "LCD dashboard and profiles rework, owner request 2026-09-19"** carries the per-item detail: files and functions, the data source and lock status for each (items 1, 3 and 5 read the two snapshots `ui_page_home_refresh.c` already fetches, so no new task, timer or HTTP route is needed), pixel arithmetic against `UI_THEME_PAGE_CONTENT_BUDGET_PX`, the host tests and `check_*.ps1` entries each item owes, a numeric `capture_lcd.ps1` verification recipe per item, a three-wave parallel split with the file collisions named, and five owner decisions, all answered 2026-09-19 — including the owner's override to four 64px rows per profile page, which fits the 268px budget exactly (`4*64 + 3*4 = 268`) at the cost of the page's header row, moving the New button and the page indicator into the topbar. Item 6 is root-caused there, not guessed: the topbar's hidden warning indicator is built after the gear and collapses to zero width under `LV_FLEX_ALIGN_START`, leaving the gear 40px short of flush right. **All seven items are in source at HEAD as of 2026-09-20** (item 5's LVGL rail widget tree included; this row's earlier "still open" caveat was stale). Flashed 2026-09-20 to `cd6073e5`; bench verification 2026-09-20: items 5, 6, 7 confirmed by webcam capture (relay/zone-temp/power rail in the right quarter, settings gear flush top-right, no "Kiln:" line with a single config); item 1 partial (name bar present left of Start, text not legible at camera resolution); items 1 tap, 2, 3, 4 still need verification once LCD touch injection is available (serial hub was held by a stale process this session). | `firmware/KilnFW/docs/UI_PLAN.md` |

| S | ~~Zone 0 gets the same per-group "Same as zone N" selectors as the other zones — owner request 2026-09-19.~~ Done: `zones_page.html` now renders the whole-zone `settingssrc` select and five `groupsrc` selects for zone 0 exactly as for zones 1..N-1 (`resolveTerminal()`/`resolveGroupTerminal()` no longer hardcode zone 0 as the fixed root); the server side needed no changes (`zones_http_post_parse.c`'s parser and the shared chain-walk were already generic per zone index). The one real fix was the cycle tie-break: `zones_config_json_normalize_settings_source_cycles()` now resets only the highest-indexed zone on a detected cycle (was: every member), so zone 0's own link survives a cycle it's part of. See `firmware/KilnFW/docs/ARCHITECTURE_DECISIONS.md`'s "Zone 0 gets the same per-group selectors" entry | `firmware/KilnFW/App/drivers/http/zones_page.html`; `firmware/KilnFW/App/test/test_zones_group_frames.js`; `firmware/KilnFW/docs/ARCHITECTURE_DECISIONS.md` |

| **M** | ~~**Visually simplify the Thermocouples & Zones web page — owner request 2026-09-19.**~~ **Done, 2026-09-19.** (a) Show-when-enabled landed for all five sections (Continuous Tuning, Cross-zone coupling matrix, Tuning quality, PID Autotune, Measure Zone Normal Current), gated on the `hidden` DOM property, never inline display. **Open owner question answered: yes** — each hidden section shows one muted `class="hint"` line in its place (e.g. "Enable continuous tuning on a zone to see learning results here"). (b) Relay-feedback test removed from the page. **Correction to this row's wire claim: no URI-handler slot was freed** — `/api/autotune/start` was always one shared route for both `method=step` and `method=relay`, so there was never a separate relay-only route to remove; cap stays 150/151. The firmware engine (`autotune_engine_relay.c`) is left in place, untouched, because `test_autotune_engine_prestart.c` host-tests it directly — dead-coded-only retirement did not apply since it is not dead, it is still tested. (c) Step test and PID Autotune collapsed into one panel; the tuning-method recommendation panel moved into that section's `<details>` info glyph. (d) Noise-floor section and its renderers removed outright (`test_noise_floor_panel.js` deleted with it); `tuning_recommendations.json`'s `noise_floor` field is untouched. Positive+negative gate tests added (`test_zones_page_visibility_gates.js`, 33 cases). See `firmware/KilnFW/docs/ARCHITECTURE_DECISIONS.md`'s "Zones page visual simplification and prose shortening (2026-09-19)" section | `firmware/KilnFW/App/drivers/http/zones_page.html`; `firmware/KilnFW/docs/ARCHITECTURE_DECISIONS.md` |

| **S** | ~~**Shorten and simplify the prose on the Thermocouples & Zones web page — owner request 2026-09-19.**~~ **Done, 2026-09-19**, shipped together with the show-when-enabled row above in the same pass: top-level section descriptions across the page were cut to one or two plain sentences, repository notes (commit hashes, plan-doc names, audit dates, bench-provenance "honesty note" paragraphs) were dropped from operator-visible text, the guard-suite/empty-kiln warning is now stated once at the top of the PID Autotune section instead of per sub-section, and existing `<details>` info glyphs were kept for the long form. Source-level HTML provenance comments were shrunk to one-line pointers at their owning doc. This pass covered the page's top-level sections that were touched by the removals/collapses above; per-zone/per-field prose elsewhere on the page (e.g. `INFO_HTML` entries, zone-type descriptions) was out of scope for this change | `firmware/KilnFW/App/drivers/http/zones_page.html` |

| **L** | **Thorough OTA testing of both processors — owner request 2026-09-19.** Unblocked; ESP half (OT-E01..12) and, as of 2026-09-21, the Pico case bodies (OT-P01..05) are implemented in `cases_ota.py`, unit/mock only -- no case in suite OT has yet run against the bench board. The two RP2040 defects that blocked the matrix (erase watchdog `e59b0328`, CRC variant `ota_image_crc.c`) are both fixed and confirmed flashed as of `73c1da94`. Exercise every field-update path end to end on the bench and record the evidence, rather than the single ESP round trip and the two failed Pico attempts recorded so far. ESP32-S3: update into the `app` slot over Wi-Fi from a real `.bin`, rollback, a deliberately corrupted image (bad CRC, wrong build, truncated), a power loss mid-write, an update attempted during a firing (must be refused), an update under web auth with and without credentials, the recovery image receiving an application image, and `otadata` state after each case; confirm PID gains, zones config, profiles, favorites and Wi-Fi credentials are byte-identical before and after each run. RP2040: the relayed `UPDATE_BEGIN`/data/commit sequence over the isolated link into the inactive slot, boot from that slot, fallback to the previous slot on a bad image, the erase-time watchdog case (`docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md`) and the CRC-variant defect that blocked every image, an update attempted with a trip pending (must be refused, already observed), and commissioning config read back byte-for-byte afterwards. **OT-P03 fixed 2026-09-21** (`529e4c31`): it now gates on Pico `trip_pending` the same way its sibling Pico cases do; `tools/PcTools/tests/test_bench_test_cases_ota.py` is 108 tests. **OT-P01/OT-P03 fixed again 2026-09-22** (`7dcde0dd`): an unreadable Pico trip status is now reported INCONCLUSIVE, never read as no-trip. Carried advisories `cfe1cacc` (same day) also name the ack header (`X-Kiln-Ack-Hardware-Differs`) in the 428 response body and correct the stack-size rule in `docs/agent_rules/IMPLEMENTER.md`. Both: the dual-reflash S6a handshake trip and its `safety_clear_trip()` recovery, and a scripted `run_pctools_tests`-style regression so the whole matrix can be re-run from PcTools rather than by hand. Both defects are fixed and flashed, so the whole matrix -- ESP and RP2040 -- can start now. Record each case as PASS / FAIL / NOT RUN with the commit flashed, in a single audit doc, and update the M8 rows to match. Home for this matrix is now suite OT of `docs/BENCH_TEST_SYSTEM_PLAN.md` (Section 3.4), whose `summary.json` is the PASS / FAIL / NOT RUN record. **The scripted regression itself landed 2026-09-23**: `ota_matrix_run` (MCP tool, `tools/PcTools/src/kilnctrl/mcp_server_ota_matrix.py`) is a thin front door onto the same `BenchTestRunner` engine `bench_test_run(suite="ota")` already drives, adding a hard `confirm is True` gate (exactly `True`, not merely truthy -- this matrix can flash both processors, roll back a slot, and reset the safety link) and a `dry_run=True` mode that lists suite `ota`'s cases and preconditions with zero board access. Before `BenchTestRunner` is even constructed it runs its own fail-closed run-level gate (ARMED, safety link up, executor not running or paused, OTA interlock ok -- reusing `coordinated_gpio_test`'s probe, unreadable counts as refusal) plus a `capability_preflight` read, then `BenchTestRunner.preflight()` runs its own checks on top; each case's own idle/interlock check (`cases_ota.py`) still gates immediately before it acts, with one deliberate exception -- OT-E07/OT-E08 intentionally push during a firing/autotune run to prove the push is refused, and only run at all when `allow_heat=True` is passed (default `False`); OT-B01 checks executor-idle but not the OTA interlock itself, covered instead by this tool's run-level gate. **Without an `ota_*` image path/commit parameter set, only OT-B01 actually executes** -- every other case SKIPs for lack of an image, same as calling `bench_test_run(suite="ota")` with none set; the relevant image parameters let more of the matrix run. Unit-tested only (`tools/PcTools/tests/test_mcp_server_ota_matrix.py`, 24 tests, all mocked) -- **not yet run against real hardware**. The actual bench exercise of the matrix (every case above, PASS / FAIL / NOT RUN, evidence recorded) is still the open work on this row. **First live no-image run 2026-09-30 (run `20261001T062928Z_ota`):** the run-level gate passed with the Pico ARMED and idle; OT-B01 FAILED (`trip_reason=0, expected 6`, 0.24 s -- no S6a trip observed; both processors did reset, board uptime 11 s afterwards), every image-dependent case SKIPped, OT-E07/E08 SKIPped (`allow_heat=False`). **Rerun 2026-10-01 after the OT-B01 fix (run `20261001T072647Z_ota`):** OT-B01 PASS, `outcome=no_trip` -- reset confirmed (ESP uptime 3436 s then restart, Pico boot_id 85 -> 42), trip_reason 0/mask 0, so `sw_reset` of both processors does not latch S6a on this bench; the earlier FAIL was a wrong expectation, not a late sample. Image-dependent cases still SKIP; see `docs/BENCH_TEST_LOG.md`. | M8; `docs/BENCH_TEST_SYSTEM_PLAN.md`; `firmware/CommonFW/docs/UPDATE_PROTOCOL.md`; `docs/OTA_SINGLE_SLOT_PLAN.md` |

| **L** | **Standardized bench test system — owner request 2026-09-19.** Waves 0-4 are all implemented in source (`tools/PcTools/src/kilnctrl/bench_test/`): `bench_test_run`/`bench_test_list`/`bench_test_last` MCP tools, `tools/bench_test.ps1`, registry/runner/report scaffolding, `tools/check_bench_test_registry.ps1`, and case bodies for every suite (ST/FL/SK/OT/AT/HP/WEB/LCD/SP) with `smoke`, `nightly` and `full` subsets, the fixed ordering, and the twelve harness-must-never-do rules from §6. **First real-hardware runs, 2026-09-24** (ESP `351304cb`, Pico `6bb41fe1`): heat suite `20260924T072516Z_heat` — HP-02/04/05/06 PASS, HP-01/03/07/08 FAILed on harness defects (wrong-order zone-limit sequencing, missing zone-override fields, target-vs-limit pinning, firing-history erased by teardown before it was read), all fixed and landed same day (`0567bf09`, `35d407df`); LCD suite `20260924T080746Z_lcd`/`20260924T084342Z_lcd` — LCD-21 PASS both runs, LCD-01/08/09/14/16 FAILed both on a screen-idle touch-swallow race, fixed in `cases_lcd.py` (`866003ea`), then further review-fixed 2026-09-25 (`d4e7ff29`, `f40e8d37` in `cases_lcd.py`; `0ef18917`/`466b29b2` in firmware, not yet flashed to the bench -- ESP still `111b1b6f`) but still not re-run since any of these fixes landed at that point. **LCD-19 is no longer NOT_RUN/needs-`KILNCTL_LCD_PIN`: it was root-caused and PASSed 2026-09-30** (run `20260930T212155Z_lcd`, fix `e388752c`, `stop_gated=true`, exit 0; see `docs/BENCH_TEST_LOG.md`'s "LCD-19 root-caused and fixed" entry). That same day's run also found a LCD-09/LCD-16 regression (`click_by_name('settings')` returned `not_found`), since **resolved**: the real cause was a `lvgl_port_task` dispatch-timeout-reported-as-not_found bug (not `e388752c`), fixed by `82de0234`'s harness retry plus `232e668f`/`c471101c`'s firmware `KILN_UI_CLICK_WALK_BUSY` signal and PC-side busy derivation, verified clean on `c471101c` (run `20261001T011155Z_lcd_walk_busy_verify`: LCD-08/09/14/16/21 PASS; run `20261001T011311Z_lcd_walk_busy_lcd19`: LCD-19 PASS) -- see the ROADMAP item above. Stack suite `20260924T080808Z_stack` (SK-01/02 FAIL, SK-03/04 PASS, before the SK-02 noise-tolerance/fw_commit-gate fix) then `20260924T084524Z_stack` (preflight refused on a stale MCP server, rerun pending); web suite `20260924T080814Z_web` ran its render-only rows clean (24 PASS, 1 SKIP needing an operator). No firmware defect found — every FAIL traced to the runner and is fixed or pending a rerun; full detail in `docs/BENCH_TEST_LOG.md`. Stack suite rerun on `eb83c1ac` (`20261001T050213Z_stack`, 2026-10-01): SK-01/03/04 PASS, SK-02 INCONCLUSIVE; the `eb83c1ac` idle (`045510Z`), `web_ui_open` (`050508Z`) and `mid_firing` (`050631Z`) stack-margin baselines are all committed, and the rerun `20261001T050807Z_stack` is SK-01..04 PASS (exit 0). Still open from before: OT-E11 and LCD-20 (wait on `ota_recovery_boot_esp()`, single-slot plan step 5), and suite OT/AT against real hardware. All ten owner open questions are decided (`docs/BENCH_TEST_SYSTEM_PLAN.md` §7) — none remain open. Not meant to run whole often; pieces are for routine use. | `docs/BENCH_TEST_SYSTEM_PLAN.md`; `docs/BENCH_TEST_LOG.md` |

| **M** | **Edit the running profile mid-firing, from the web UI — owner request 2026-09-18. DELIVERED 2026-09-19 (pass 2): `live_profile_page.html`, five ADMIN routes in `profiles_live_http.c`, fork-on-edit, HARD-mode validation, executor pickup, end-of-firing prompt; `test_profiles_live_http.c` 104/104. This row's earlier "groundwork only" text was stale — see `docs/LIVE_PROFILE_EDIT_PLAN.md` §10 for what landed and its four handler-bug fixes. Duplicate-name refusal shared by both profile save paths (`live_edit_name_collides()`) landed on main 2026-09-20 as `d1f43870` (four commits, two Opus review rounds). Flashed 2026-09-20 to `cd6073e5`; bench verification 2026-09-20: fork refused when idle confirmed (409 "no active firing to fork from"); fork, HARD validation, and executor pickup NOT RUN because the tooling permission layer refused `POST /api/profile_exec/start` and `POST /api/profile/delete` in that session, not a firmware defect; needs a session authorized to start a non-heating firing. Two open bench items: a stray test profile "LiveEditTest" in slot 0 on the bench board needs deletion, and `POST /api/auth/login` hung twice and reset after 60 s on `cd6073e5` — root-caused and fixed in source 2026-09-20 (`d4e59c02`/`77e90ad7`, task-watchdog panic in the login KDF; see the top-of-file status log), verified on hardware 2026-09-21 (no reboot, 4.2 s); the 2026-09-21 clean login attempt still returned 401 — resolved: the board's console log shows the wrong-credentials branch, client IP correctly resolved, so the stored `kiln_auth` record and the bench env-var credentials disagree; which is authoritative is now the only remaining owner decision here — see the top-of-file status log. Login latency itself is now an owner-decided, in-progress fix (fast login plus an escalating per-IP backoff, `docs/WEB_AUTH_PLAN.md` §2, owned elsewhere).** Design record kept below. A web-only page that changes the profile a firing is currently executing, so a firing can be improved in progress. Editing forks immediately into a new profile (an ordinary user slot, so it inherits NVS, the `cfg` dual-write and migration, and survives a reboot mid-firing), leaving the original untouched on disk; at the end of the firing the operator is asked to name-and-save the copy or overwrite the original, with overwriting a shipped/builtin schedule refused outright server-side — structurally, since builtin ids are `>= PROFILE_BUILTIN_ID_BASE` and back a `const` table with no writable storage at all, so a forged request cannot express the attack. **Two hard requirements shape it:** an edit that would exceed the running zones' `max_temp_c` (or their ramp ceiling) is *rejected* server-side before the working copy is written, not warned about in the browser — deliberately stricter than ordinary save-time validation, which is advisory because profiles are portable while a live edit is not — and re-checked inside the executor before the swap is adopted, closing the ceiling-changed-underneath window; and both profile editors must share one implementation, which means factoring `parse_profile_fields()` and one `profiles_validate_candidate()` out of `profiles_edit_http.c`/`profiles_http.c` and the segment editor out of `profiles_page.html` into a served `profile_editor.js`. **The shared-validator half of that factoring has landed (`73c322bb`): `profiles_validate_candidate()` now single-sources ceiling/ramp validation, with a HARD mode added for the coming accept/pickup paths while ADVISORY mode preserves existing save-time behavior byte-for-byte** — this is groundwork only, not the live-edit feature, which still has no UI, no fork-on-edit, and no pickup path. The Pico's `abs_max_temp_c` is never written, read as a limit, or derived from — there is no code path from this feature to any Pico parameter, which is how "never exceeded, never tightened" is met. Pickup is a generation counter polled by the control task, the same shape as the existing mid-run zones-config reload, and is continuous by construction because the ramp state lives in `s_exec`, not in the profile. **Sequencing dependency:** the shared factoring rewrites files the kiln-profiles work also touches; the profile-favorites work it also used to depend on has since landed (`acf36720`, `18e8b60a`). Five owner decisions were open and are now recorded as resolved in the plan doc (slot budget, a ceiling lowered mid-firing, name collisions, editing while PAUSED/FAULTED, the prompt under web auth) — none of them blocked starting. **LCD Edit-firing page first bench result, 2026-10-01:** PASS on `eb83c1ac` -- Segment 2 target +5 C and dwell +5 min via the steppers plus Apply were adopted by the running firing (`profile_live_get` content and the executor's "live profile edit adopted" log line), page fits 480x320 with no scroll | `docs/LIVE_PROFILE_EDIT_PLAN.md` |

| **L** | **100 user profile slots plus a live-edit slot — owner request 2026-09-19.** In source at HEAD 2026-09-20: `PROFILES_MAX_COUNT` 8 -> 100, `LIVE_EDIT_WORKING_SLOT_ID` = 100, hidden bench slot `PROFILE_BENCH_SLOT_ID` = 101, `PROFILE_BUILTIN_ID_BASE` 128 as the hard ceiling; 4-word slot bitmaps with legacy-blob read migration; chunked list handlers; `s_profiles.profiles[]` moved to PSRAM; `cfg` partition grown to 0x250000 (source only, not yet flashed); backup import PSRAM-buffered with 100/101-profile tests; web name filter plus Favorites / Recently fired grouping; LCD picker shares the web `favorites` ordering; `tools/check_profiles_capacity.ps1`. Plan tasks 1-11 done. **Task 12, the bench migration, done 2026-09-20**: the ESP was flashed to `5f58ba09` with the new partition table (`cfg` now 0xDB0000/0x250000, NVS untouched, relay cycle counters continuous). That first flash exposed a real regression the plan's review missed: `profiles_http.c`'s never-used `s_profiles_fallback` static grew to 42416 B of INTERNAL `.dram0.bss` with the slot count, internal heap fell to 8447 B free / 263 B low-water and the Wi-Fi `ppTask` aborted once on `esp_timer_create() == ESP_ERR_NO_MEM`. Fixed by `EXT_RAM_BSS_ATTR` (`.dram0.bss` 156824 -> 114408 B), guarded by the new `check_kilnfw_dram_bss_budget.ps1` (120000 B ceiling); `docs/audits/dram_bss_profiles_fallback_2026-09-20.md`. One correction to the plan's own premise: `cfg` was NOT unmounted on the bench -- it was already mounted (7 files, dual-write mirror live), and the LittleFS volume keeps its old 512 KiB geometry on the new 2.3 MiB partition until reformatted, which `POST /api/cfgfs/format_confirm` gates on the AP-password HMAC (no `KILNCTL_AP_PASSWORD` available to any session). NVS stays authoritative, so this costs unused space, not data. | `docs/PROFILE_SLOTS_100.md` |

| S | ~~Off-theme white button backgrounds on the web diagnostics danger zone~~ — done `e8772211` (2026-09-20): `background:#fff` -> `var(--bg)`, and `lint_pages.js` gained a `hardcoded_background_literals()` rule (enforced via `check_lint_pages.ps1`) so a literal background colour outside a `lint-color-ok`-annotated line fails the suite; negative-tested. | `firmware/KilnFW/App/test/lint_pages.js` |

| S | ~~Credit the research the control design draws on~~ — done `d833161c` (2026-09-20): `CREDITS.md` "Research and control literature" section plus `docs/research/README.md` mapping each source to the code it influenced (three-node sensor model, `zone_coupling_solve.c` candidate 3) and listing what was surveyed but not used. | `CREDITS.md`; `docs/research/README.md` |

