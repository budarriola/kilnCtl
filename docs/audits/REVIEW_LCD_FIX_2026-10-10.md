# Review: LCD fix batch cc62edf2f (2026-10-10)

Reviewer: Claude Opus 5.5. Scope: commit `cc62edf2f` on origin/dev, which fixes
N1-N5 and N8 of `docs/audits/LCD_UI_REVIEW_2026-10-09.md`. Reviewed at dev tip
`1f9f0c0a3`. Review only; nothing here is fixed. No bench board was used.

Owner decisions taken as given: N6 kept (LCD read pages stay USER); N5 the Safety
page opened from the trip strip needs the USER PIN, and Clear stays ADMIN.

Paths below are relative to `firmware/KilnFW/App/drivers/` unless noted.

## Summary

| Sev | Id | Finding |
|-----|----|---------|
| MED | R1 | Danger mode and autotune still do not exclude each other |
| MED | R2 | Danger-mode start check is early only; open-danger vs start race |
| LOW | R3 | `danger_mode_active()` fails open on lock timeout |
| LOW | R4 | Builder pad converts with the unit read at done time, not at open time |
| LOW | R5 | Builder card titles still say "Target C" / "Ramp C/hr"; stale comments |
| LOW | R6 | First-boot calibration without a role skips touch_test verification |
| LOW | R7 | Unit-entry tests miss the lower clamp and the display rounding |
| NIT | R8 | Float scale drift on accept-unchanged (below noise floor) |
| NIT | R9 | Stale refusal log text in `profile_executor_run()` |

No HIGH finding. No remaining LCD path was found that opens a non-dashboard page
with no role held.

## Findings

### R1 (MED) Danger mode and autotune still do not exclude each other

`safety/system_mode_gate.c:193` refuses only `SYS_ACTION_START_PROFILE` while
danger mode is open. `autotune_engine_run()` (`control/autotune_engine.c:1033`)
and the web autotune route (`http/dashboard_autotune_http.c:244`) never fill or
test a danger field. In the other direction, `danger_mode_request_start()`
(`safety/danger_mode.c:52`, `state_refuses_start()`) refuses only when the
profile executor is RUNNING or PAUSED; it never checks autotune or the published
heat claim.

Failure scenario: an operator starts an autotune, then opens danger mode on the
diagnostics page (accepted), or opens danger mode and then starts an autotune
from the LCD or web (accepted). The autotune drives heaters while the danger
window bypasses `relay_on_blocked()` protections, and the window's
auto-expire-and-reboot then fires mid-autotune. `docs/SYSTEM_MODE_GATE.md:252-254`
already notes the reverse gap. N2 claimed a single choke point for "no firing
starts in danger mode"; autotune is a heating run and falls outside it.

Fix direction: gate `SYS_ACTION_START_AUTOTUNE` on `danger_mode_active` too, and
make `danger_mode_request_start()` refuse while the heat claim is held (any
owner), not only while the executor is RUNNING/PAUSED.

### R2 (MED) Danger-mode start check is early only

`control/profile_executor_run.c:258` samples `danger_mode_active()` at the top of
`profile_executor_run()`, before the `s_exec.lock` check, baseline SPI reads and
the RUNNING commit. The late commit re-check later in the same function (around
the `restore_in_flight` re-check) does not re-test danger mode. Because
`danger_mode_request_start()` checks executor state only, and the executor is
not RUNNING until the commit, the two can interleave:

1. LCD/UART/web start passes the early gate (danger closed).
2. Diagnostics opens danger mode; executor state is still IDLE, so accepted.
3. The start commits RUNNING. A firing now runs inside a danger window.

The race window is the start path's baseline reads (tens to hundreds of ms). It
existed before for the web path; N2 moved the check but did not close it.

Fix direction: re-test `danger_mode_active()` at the commit point, alongside the
`restore_in_flight` re-check, or have danger mode refuse once a start has
claimed heat (see R1).

### R3 (LOW) `danger_mode_active()` fails open

`danger_mode_active()` returns false when the `s_dm.lock` take times out after
50 ms. The new start gate therefore reads "unknown" as "not active". Under lock
contention a start is let through while danger mode may be open. Fail closed
(treat a timeout as active, refusing the start) is the safer default for a
start gate; callers that only display state can keep the current behavior via a
separate accessor.

### R4 (LOW) Builder pad unit read at done time

`ui/ui_page_profile_builder_segment.c:273` and `:297` (target and ramp cards)
convert the stored Celsius value to the display unit with the preference read
when the pad opens. The done callbacks at `:227` and `:238` call
`unit_pref_get()` again when the pad closes.

Failure scenario: unit is F, the pad shows 1832 (1000 C). While the pad is open,
someone switches the unit to C on the web settings page. The operator accepts
1832; done converts with C, so 1832 C is stored (clamped to 2015 C only past
the bound). A 1000 C segment silently becomes 1832 C. Rare, but the result is a
large, unflagged setpoint change. Fix: capture the unit in a static at open and
use it in done.

### R5 (LOW) Card titles and comments still say Celsius

`ui/ui_page_profile_builder_segment.c:398-399` builds the cards as
`"Target C"` and `"Ramp C/hr"`, while the value and pad now show Fahrenheit
when the preference is F. An F user sees "Target C 1832". The comments at
`:86-96`, `:250-265` and `:288-291` still describe the pads as Celsius-only,
which invites a later revert of the N3/N4 conversion. Use
`unit_pref_suffix()` in the titles and update the comments.

### R6 (LOW) First-boot calibration skips touch_test without a role

`ui/ui_page_touch_cal.c:196` now routes a successful save through
`lcd_touch_cal_exit_target()`, which returns `"home"` when no USER role is held.
With web auth on and no PIN entered (first boot after enabling auth), the
operator calibrates and never sees the touch_test verification page. touch_test
only offers a Done button that goes home (`ui/ui_page_touch_test.c:140`), so it
exposes nothing beyond the dashboard and could be allowed without a role. Not a
security problem; a usability loss on the one path where verifying the new
calibration matters most.

### R7 (LOW) Unit-entry test gaps

`test/test_ui_page_home_rail.c` covers only the upper clamp of
`ui_unit_entry_to_celsius()`. There is no lower-clamp test and no test of the
rounding in `ui_unit_entry_to_display()`. The negative test below confirms
both mutations survive the suite. Page wiring (rail stale/unit pass-through,
touch_cal exits, the profile-detail Start gate) has no host test either; only
the pure helpers in `ui/lcd_auth_state.c` are tested.

### R8 (NIT) Float scale drift

`ui/ui_unit_entry.c:12-13` derives the scale as
`convert(1) - convert(0)`, about 1.7999992 in float. Accepting an unchanged
value drifts by up to about 0.28 C (1001 C displays as 1834 F and is stored as
1001.11 C). Below the 0.5 C noise floor, so no action needed; using the exact
constants for the F case would remove it. The bound clamp handles the edge
(3659 F converts to 2015.0009 C and is clamped to 2015). The ramp conversion
correctly applies scale only, no offset.

### R9 (NIT) Stale log text

`control/profile_executor_run.c:260` still logs "refused by the system mode gate
(recovery mode or restore)" when the refusal can now be danger mode. The
`err_msg` returned to the caller is correct; only the log line is stale.

## Checked and not a problem

- N1: every touch_cal exit (refused save `:183`, save success `:196`, cancel and
  unsupported-notice Back `:245`) goes through `lcd_touch_cal_exit_target()`.
  Profile-detail Start is wrapped in `ui_lcd_lock_run_gated` USER. Trip strip
  (`ui/ui_page_home.c:141-159`) is gated USER and opens the Safety page with
  Clear still ADMIN; Safety Back goes home. Home Menu/Profiles/Edit/Start/Pause
  are gated. The Config hub cells and the profile-detail Segments/Builder
  buttons are not gated individually, but every route to them passes a gate;
  defence in depth only.
- N2: stop, abort and halt are not gated. Resume is not gated, which is correct:
  danger mode cannot open while the executor is PAUSED. The UART bridge run path
  (`bridge/uart_bridge_ext_control.c:567`) goes through `profile_executor_run()`
  and is covered. There is no resume-after-power-loss path that opens during a
  danger window (danger mode does not survive a reboot).
- N3/N4/N8: clamping happens in Celsius after conversion; ramp uses scale only;
  the home rail uses `ds->temp_unit` (filled at `http/dashboard_http.c:586`) and
  passes `valid && stale` correctly.

## Verification

- Host tests at dev tip `1f9f0c0a3`: `build_host_tests.ps1` has
  `$totalExpected = 78`; full run built and passed 78/78. The fixer's reported
  77/77 predates its last rebase; the tip is self-consistent.
- Target build (`run_all_checks.ps1 -Only check_00_kilnfw_target_build -NoCache`):
  PASS on the second run. The first run failed on
  `safety/safety_link_frame.c.obj` with no compiler diagnostic in the log;
  compiling that file by hand with the identical command line minus `ccache`
  succeeded, so the first failure was a transient ccache fault, not a source
  break.
- Negative tests (`tools\negtest.ps1 -Preset kilnfw-host`):
  baseline passed; `unit_entry_no_lower_clamp` (drop the `c < min_c` clamp in
  `ui/ui_unit_entry.c:15`) MISSED; `unit_entry_display_no_round` (drop `roundf`
  in `ui_unit_entry_to_display()`) MISSED; `run_danger_snapshot_false` (set
  `mode_snap.danger_mode_active = false` at `control/profile_executor_run.c:258`)
  CAUGHT. The run ended with verdict ERROR only because this review file was
  created in the worktree during the run; no tracked file changed.
