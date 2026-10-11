# Review: lcdfx (LCD UI review L1/L2/L3 fixes), 2026-10-10

Adversarial review of the lcdfx work on origin/dev:

- `3eb2f983a`: L1 (`ui_num_pad_close()` on the relock edge) and L3 (Config hub
  cells gated on `LCD_PIN_ROLE_USER`), plus host-test stub counter.
- `7d39d7f43`, `29c52d6be`: edits to `docs/audits/LCD_UI_REVIEW_2026-10-10.md`.
- L2 is claimed covered by `062456379` (`live_profile_save_working_if_gen`).

No code was changed by this review. No board access.

## Verdict

No HIGH or MED findings. L1 and L3 are correct as written; L2 is closed for
the web path and, by inspection, for the LCD path except one narrow residual
window; the LCD guard is untested. Four
LOW and five INFO findings below.

## L1: number pad closed on relock

`ui_num_pad_close()` (`firmware/KilnFW/App/drivers/ui/ui_num_pad.c:181-188`)
clears `s_on_done`/`s_user_data` **before** `close_modal()`, so no done
callback can fire from the hide (LVGL delete events cannot reach a committed
write). `close_modal()` is NULL-safe, so the unconditional call on every
relock edge is fine. The only caller is the relock edge in
`ui_lcd_lock.c:286`, which runs in `tick_timer_cb` on the LVGL timer;
`ui_lcd_lock_force_lock()` from httpd only sets the atomic request, so the
close is LVGL-task-only. The four num-pad users
(`ui_page_profile_builder_segment.c:277/299/316`,
`ui_page_profile_builder_zones.c:127`) keep no "pad open" state that a skipped
Cancel would leave stale. Ordering at the edge (prompt, keypad unless pending
lock gate, `ui_confirm_close_open()`, num pad, relock callback to home) is
sound.

### LOW-1: page-local modals survive relock on cached page screens -- FIXED (lcdfx2)

- `ui_page_network.c:729` builds the AP edit modal on the page screen
  (`lv_obj_create(scr)`), prefilled with the AP SSID and the clear AP password
  (textarea in password mode, `:755`). It is closed only by cancel
  (`ap_edit_modal_close`, `:588`) or job success (`:358`).
- `ui_page_network_manage.c:735` builds the Wi-Fi connect modal the same way
  (password mode, `:758`); closed only by cancel or success.

Scenario: an ADMIN opens either modal, walks away, the session times out. The
relock edge closes keypad/confirm/num pad and shows home, but `kiln_ui_show()`
caches page screens, so the modal object stays on the network page. A later
USER-role operator who re-enters via Config -> Network sees the admin's
half-filled modal (masked password, SSID, and the reveal control if any).
Writes are not exposed: AP save goes `ap_edit_save_cb` (`:663`) ->
`ui_confirm` -> `ap_edit_confirm_apply_cb` -> `run_gated(ADMIN)` (`:623`),
and connect submit is ADMIN-gated (`ui_page_network_manage.c:406`). So this is
stale-UI / information exposure, not a write bypass.

Fix: close both modals from the relock path (a per-page relock hook, or have
`handle_lcd_relock_to_home` call `ap_edit_modal_close()` /
`connect_modal_close()`), or close them on page show. Same class as L1, just
not on `lv_layer_top()`.

### INFO-1: pending-lock-gate keypad outlives relock (pre-existing)

`s_keypad_is_pending_lock_gate` (`ui_lcd_lock.c:69`, `:273`, `:412`) keeps a
keypad raised as the PIN gate for this lock open across the relock edge. After
the PIN it runs the gated page action (for example `ap_edit_apply_gated`)
while home is showing. The action's role is re-checked by `run_gated`, so no
bypass; noted only because it is the one keypad that intentionally survives
the edge.

## L3: Config hub cells gated on USER

All five cells (`ui_page_config.c:80` temperature, `:92` network, `:123`
diagnostics, `:135` safety via `ui_page_safety_open(true)`, `:152` profiles)
now go through `ui_lcd_lock_run_gated(..., LCD_PIN_ROLE_USER, ...)`. Touch
calibration (`:104-111`) stays ungated only before first calibration
(owner rule (d)), ADMIN after.

Entry paths checked for a bypass:

- Home Menu (`ui_page_home_actions.c:412-434`): `run_gated(USER)` before
  `kiln_ui_show("config")`. Picker `:452` USER, Edit `:404` ADMIN.
- Home trip strip (`ui_page_home.c:221-238`): `lcd_safety_strip_needs_pin`
  plus `run_gated(USER)` before `ui_page_safety_open(false)` (rule (b), N5).
- Topbar Back targets are ungated `kiln_ui_show(back_page)`, but every source
  page (diagnostics `:1634`, network `:828`, temperature `:563` -> config;
  network_manage `:823` -> network; profile_picker `:493`; builder_zones
  `:185`; profile_detail `s_back_target`; safety `s_back_page` set at
  `ui_page_safety.c:241-245`) is itself reachable only with a held role.
- No `kiln_ui_show` caller outside `drivers/ui`; the UART bridge only
  mentions it in comments.
- Boot: `ui_lcd_lock_init()` / `ui_lcd_lock_set_relock_cb()`
  (`kiln_ui.c:342-343`) run before the boot touch-cal show (`:349`), so the
  relock-to-home callback is armed before any page appears.

No bypass found. Rules (a)-(d) hold.

### LOW-2: L3 has no test or check -- FIXED (lcdfx2)

Reverting any of the five `LCD_PIN_ROLE_USER` gates passes every existing
test and check: `check_lcd_admin_gates.ps1` and `check_lcd_home_nav_gated.ps1`
are regex checks that cover ADMIN writes and the home Menu, not these cells,
and `test_ui_lcd_lock.c` tests the lock module, not the page.

Cheap seam: add five rules to `check_lcd_admin_gates.ps1` (or a sibling
check) matching
`run_gated\("[^"]*",\s*LCD_PIN_ROLE_USER,\s*(temperature|network|diagnostics|safety|profiles)_open_apply`
in `ui_page_config.c`, and additionally refuse any direct
`kiln_ui_show("(temperature|network|diagnostics|safety|profile_picker)")`
in that file outside an `*_open_apply` function. A source-text guard in a
host test (the D2 pattern in `test_ui_profile_builder_segment_logic.c`) is an
equivalent alternative. Negative-test it by dropping one gate.

### INFO-2: Back to Config in the force-lock gap

`ui_lcd_lock_force_lock()` from httpd sets a flag applied on the next LVGL
tick. A Back tap landing in that single tick shows the Config hub until the
relock edge sends home on the next tick. Hub cells are themselves gated
(`has_role` returns false while force-lock is pending), so nothing opens.

## L2: live edit lost update

`live_profile_save_working_if_gen()` (`live_profile.c:553-568`) does the
generation compare, save and `out_gen` publish under `s_live_save_lock`, and
returns OK/STALE/FAILED.

- Web: `profiles_live_http.c:517` calls it with `gen_present`/`gen_val`
  parsed by `live_gen_parse`; the response reports `saved_gen`. An absent
  `gen` is ungated by design (documented).
- LCD: `ui_edit_firing_apply.c:252` calls it with `check_gen=true` and
  `expect_gen`, maps STALE to "edited elsewhere -- reopen to reload", and
  sets `ctx->generation = saved_gen` (`:263`).

Covered on both paths, with one residual window:

### LOW-3: LCD re-bases its expected generation from an unlocked read after fork -- FIXED (lcdfx2)

`ui_edit_firing_apply.c:214` starts with `expect_gen = ctx->generation`, then
after a successful fork does `expect_gen = live_profile_generation();`
(`:245`), an unlocked read taken after the fork returned. Two interleavings
make the LCD adopt a foreign generation and overwrite a web edit:

1. `live_profile_has_pending_for_origin()` (`:217`) is false, a web fork plus
   edit lands, then the LCD's `live_profile_fork()` (`:233`) takes the
   idempotent path (`live_profile.c:735-751`: existing pending record for the
   same origin, no save, no bump) and returns true. `:245` reads the web
   edit's generation and the LCD save at `:252` passes.
2. The LCD fork really saves, then a web edit with no `gen` (a PcTools
   caller) or with a fresh GET lands before `:245`.

Both need a web write inside a few-millisecond window on an LCD Apply, so
LOW. Fix: have `live_profile_fork()` report the generation its own save
produced (or whether it actually forked); keep `ctx->generation` on the
idempotent path and use the returned value otherwise, never a fresh read.

### LOW-4: the LCD stale-generation guard has no test -- FIXED (lcdfx2)

Negtest (below) shows that changing `ui_edit_firing_apply.c:252` to
`check_gen=false`, or replacing the early check at `:168` with `if (0)`,
passes every host test. `test_ui_edit_firing_apply.c` covers fork, save,
generation advance (`:274-299`) and the foreign-origin record, but never a
foreign save landing between page open and Apply. So the LCD half of L2 is
correct by inspection only; a regression in either guard would land green.

Fix: add a test that opens the edit ctx, performs a `live_profile_save_working()`
(the web's path) to bump the generation, then calls `edit_firing_apply()` and
expects refusal with "edited elsewhere" and an unchanged working profile. To
cover the `if_gen` call itself (not just the early check), add a fake seam
that bumps the generation after the early check, for example from the fake
`live_profile_fork()` path or a test hook in the fake KV write. Negative-test
both mutations below against it.

### INFO-3: `live_profile_clear()` erases outside the save lock

`live_profile.c:776-822`: the NVS/cfg erase and read-back run unlocked; only
the generation bump (`:818-820`) takes `s_live_save_lock`. A
`save_working_if_gen()` interleaved between the erase and the bump compares
against the pre-clear generation, succeeds, and rewrites the working file with
no pending record (an orphan the next fork overwrites). Benign today; take
the lock around the whole clear if the record/working pair ever gains a
consumer that trusts the working file alone.

### INFO-4: web decide still pre-checks only

`profiles_live_http.c:810` (`live_gen_stale`) checks the generation before
decide, not atomically with the save-as/overwrite/discard. Out of L2's stated
scope (edit), noted for completeness.

### INFO-5: commit message of 3eb2f983a

The message claims "compare-and-save for live edit", but the commit contains
no L2 code; the real fix is `062456379`. `29c52d6be` corrected the audit doc,
so only the commit message is misleading.

## Tests run

Targeted KilnFW host tests on base `29c52d6be`
(`build_host_tests.ps1 -Only "live_profile|lcd_lock|edit_firing_apply|profiles_live_http"`):
all 6 executables built and passed.

`tools\negtest.ps1` over the same `-Only` set, with
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`
(baseline passed):

| Mutation | Verdict | Caught by |
|---|---|---|
| L1: drop `ui_num_pad_close()` from the relock edge | CAUGHT | `test_ui_lcd_lock.c:282` |
| L2: `save_working_if_gen` ignores the generation | CAUGHT | `test_live_profile.c:641-645`, `test_profiles_live_http.c:657-685` |
| L2: web POST edit passes `check_gen=false` | CAUGHT | `test_profiles_live_http.c:657-675` |
| L2: LCD apply passes `check_gen=false` | MISSED | none (LOW-4) |
| L2: LCD apply early gen check removed | MISSED | none (LOW-4) |

The run ended with negtest's "REAL TREE CHANGED" error because this review
file was written into the worktree while it ran. The mutated copies are
separate worktrees and were removed; the per-mutation verdicts are
unaffected.
