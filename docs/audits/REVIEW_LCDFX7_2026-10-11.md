# Review LCDFX7: check_lcd_admin_gates.ps1 LCDFX6 fixes (2026-10-11)

Scope: commit 3a39cd1ac on origin/dev (lcdfx7). It touches only `tools/check_lcd_admin_gates.ps1` and
`docs/audits/REVIEW_LCDFX6_2026-10-11.md`, and claims fixes for LCDFX6 HIGH-1, MED-1, MED-2, LOW-1 and LOW-2.
X18, INFO-3 and INFO-4 were left open.

The owner decisions the check enforces:

- (D1) the LCD Safety page needs a USER PIN;
- (D2) an LCD trip clear needs an admin login and follows the web clear;
- LCD reads stay USER while the web needs ADMIN.

Method: read-only. I read the check, `ui_page_safety.c`, `ui_page_safety_logic.c` and the call graph of
`dashboard_safety_clear_trip`. Every run below used `tools\negtest.ps1 -Preset check -PresetArg
tools/check_lcd_admin_gates.ps1` against base 3a39cd1ac. Each run passed its baseline and reported
`real_tree_unchanged: true`.

## Summary

| Id | Finding |
|----|---------|
| HIGH-1 | The new P6 lookbehind blinds every use count after a brace-less `if (f(x))`. An ungated `clear_action(NULL)` or `dashboard_safety_clear_trip()` in `ui_page_safety.c` now passes. This is a regression in this commit. |
| MED-1 | Only clear_trip has a sink inventory. Ungated `dashboard_set_relay`, `wifi_prov_forget_network`, `delete_do` or `crash_ack_do` calls pass anywhere outside the anchored gate statements. |
| MED-2 | The pre-gate rule strips braced blocks and `if (...) return;` guards without inspecting them. A sink call inside either one passes. |
| LOW-1 | In `clear_action`, the NEEDS_ADMIN `[^}]*?\breturn` accepts a conditional return that falls through to the send. |
| LOW-2 | X18 is cheap to close: every gated callback and apply is `static` today, so the check can require it. |
| LOW-3 | Formatting brittleness (loud false-fails): a clang-format break after `(`, a nested-call registration target, the verdict held in a local, and an unrelated `*clear_trip*` helper name in another UI file. |
| INFO-1 | The check is wired into run_all_checks: auto-discovered, carries `# checkcache: ok`, passes on 3a39cd1ac. |
| INFO-2 | Fixer mutation set re-run: X1-X17, X19, H1-H14 and N1-N5 CAUGHT; P1-P7 pass as intended; X18 MISSED. Vacuity sweep 49/49 CAUGHT. |
| INFO-3 | The verdict and gate semantics still rest on host tests. This is LCDFX6 INFO-4, unchanged. |

## Answers to the review questions

1. **Does the check prove the trip-clear path is admin-gated?** Only for the code as written today:
   - The button registers `btn_clicked_cb` once.
   - `btn_clicked_cb` is fully anchored: `(void)e;` and then the ADMIN gate on `clear_action`, with nothing else.
   - `clear_action` re-checks the verdict with `ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)`, and its only send is after the switch.
   - Other `drivers/ui` files may not call a clear_trip function.
   - `dashboard_safety_clear_trip` has two callers, the ROUTE_TIER_ADMIN HTTP handler and the LCD.
   - The UI cannot reach `safety_link_send_clear_trip` without a new accessor.

   It does **not** prove the path holds under edits, because of HIGH-1, LOW-1 and X18 (LOW-2).
2. **Is there a remaining non-adversarial bypass beyond X18?** Yes. HIGH-1 (a common C idiom), MED-1 and MED-2.
3. **Is X18 cheap to close?** Yes. See LOW-2.
4. **Is the check over-broad or brittle?** It is brittle against formatting in four ways (LOW-3). Every one is a loud
   false-fail, never a silent pass. Benign log lines (R11) and a same-file helper with a clear_trip name (R13) pass.
5. **Mutation runs.** See INFO-2 and the tables below.
6. **Wired into run_all_checks?** Yes (INFO-1).

## Findings

### HIGH-1: the P6 lookbehind hides real uses (regression)

The fix for P6 (an attribute on a gated callback definition) changed `Count-Uses` (check line 137):

```
'(?<!void\s+)(?<!\)\)\s+)\b' + name + '\b'
```

The new `(?<!\)\)\s+)` skips any occurrence that follows `))` and whitespace. That covers the intended
`__attribute__((noinline)) name`. It also covers every brace-less `if (f(x)) name(...)`, which is ordinary C.

| Mutation | Result |
|----------|--------|
| R5: in `refresh_cb`, add `if (view.tripped_live && lv_obj_has_state(s_btn, LV_STATE_CHECKED)) dashboard_safety_clear_trip();` | MISSED |
| R6: in `refresh_cb`, add `if (lv_obj_has_state(s_btn, LV_STATE_CHECKED)) clear_action(NULL);` | MISSED |
| R7: after the relay gate, add `if (lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)) relay_toggle_apply(...);` | MISSED |
| L-R5, L-R6, L-R7: the same three mutations with only the `(?<!\)\)\s+)` lookbehind removed | all CAUGHT ("referenced 2 time(s)") |

The L-R runs isolate the cause to this one lookbehind. The parent commit's `Count-Uses` had no such lookbehind and
counted these uses.

This defeats the main D2 protection, "clear_action and dashboard_safety_clear_trip are each used exactly once in
`ui_page_safety.c`". It also defeats every other per-row use count, without any adversarial intent.

**Fix:**
- Option 1: narrow the lookbehind to the attribute form, for example `(?<!__attribute__\s*\(\([^()]*\)\)\s+)`.
  .NET supports variable-length lookbehind.
- Option 2 (preferred): strip `__attribute__((...))` in `Remove-CComments` before any matching, and drop the
  lookbehind.
- Either way, add R6 to the regression set.

### MED-1: sink inventory exists only for clear_trip

Each gated row anchors the gate call and counts uses of the apply. Nothing inventories the underlying mutator or
sink, so a direct call to any of them passes:
- in the same callback after the gate (bare gate rows are not end-anchored; only `btn_clicked_cb` is);
- in a new helper in the same file;
- in a new callback.

| Mutation | Result |
|----------|--------|
| R3: `wifi_prov_forget_network(...)` directly after the forget gate in `forget_confirm_yes_cb` | MISSED |
| R15: a new callback in `ui_page_profile_picker.c` calling `delete_do(...)` (`delete_do` has no use count) | MISSED |
| R16: a new function in `ui_page_diagnostics.c` calling `crash_ack_do()` (no use count) | MISSED |
| R17: a new helper in `ui_page_temperature.c` calling `dashboard_set_relay(1, false, &src)` | MISSED |

**Fix:**
- Add use-count rows for `delete_do` and `crash_ack_do`. Each currently has 2 uses, one after each gate path.
- Add a per-file allow-list (file, function, count) for each sink, mirroring the clear_trip ban. The sinks are
  `dashboard_set_relay`, `wifi_prov_forget_network`, `unit_pref_set*` and `touch_cal_store_save`.
- End-anchor the gate statement in rows whose callback has nothing after the gate.

### MED-2: stripped pre-gate code is never inspected

The pre-gate rule removes `if (...) return;` guards and nested `{...}` blocks before forbidding calls, returns and
gotos. A call inside the removed text is never seen:

| Mutation | Result |
|----------|--------|
| R1: `if (s_relay_quick_mode) { ...dashboard_set_relay(...); return; }` before the relay gate | MISSED |
| R1b: the same block guarded by `if (!lcd_auth_is_enabled())` | CAUGHT, because the call is in the condition |
| R2: `if (dashboard_set_relay(1, true, NULL) == DASHBOARD_RELAY_OK) return;` before the gate | MISSED |

**Fix:**
- Before stripping, run the call allow-list over the guard condition and the block body.
- Or forbid any braced block before the gate.

### LOW-1: conditional return in the NEEDS_ADMIN case

`case UI_SAFETY_CLEAR_NEEDS_ADMIN\s*:[^}]*?\breturn\s*;` also matches `if (s_scr == NULL) return;`. With that
change the case falls through to `case UI_SAFETY_CLEAR_SEND` and sends without admin. R4 was MISSED.

**Fix:** require the case body to be the label call followed directly by an unconditional `return;`, for example
`(?:(?!\bif\b|\?)[^};])*;\s*return\s*;`. The same applies to the NOT_TRIPPED case.

### LOW-2: closing X18 is cheap

Every gated callback, apply and C3 apply is `static` today. That includes `clear_action`, `btn_clicked_cb`,
`delete_do`, `crash_ack_do`, the `*_apply` functions and the `*_open_apply` functions.

R8 made `clear_action` extern and called it from `ui_page_diagnostics.c`. It was MISSED (X18 for the trip clear).

**Fix:** have the definition lookup for every row require a leading `static` and fail otherwise. That is a
one-regex change in the definition match. The alternative is counting uses across all App files.

### LOW-3: formatting brittleness (fails loudly, never passes silently)

| Mutation | Why it fails |
|----------|--------------|
| R9: the gate call broken after `(`, as clang-format does at long lines | `GateRx` has no `\(\s*"`. Fix: allow `\(\s*"`. |
| R10: registration target written as `lv_obj_get_child(brow, 0)` | The registration regex forbids parentheses in the first argument. |
| R12: verdict held in a local before the switch | The re-check regex requires the call inside `switch (...)`. |
| R14: an unrelated `show_clear_trip_hint()` in `ui_page_diagnostics.c` | `\b\w*clear_trip\w*\s*\(` bans any name containing clear_trip. Fix: ban the real sinks (`dashboard_safety_clear_trip`, `safety_link_send_clear_trip`). |

R11 (an `ESP_LOGI` before the trip gate) and R13 (a same-file helper named with clear_trip) pass, as they should.

### INFO-1: wiring

- `run_all_checks.ps1` discovers every `check_*.ps1` recursively, so this check is included.
- The script carries `# checkcache: ok`.
- A direct run on 3a39cd1ac passes.

### INFO-2: the fixer's mutation set against the final commit

Fixer set, 45 mutations (log dir `negtest_logs\20261011_233139_et6x`):
- X1-X17, X19, H1-H14 and N1-N5: CAUGHT.
- P1-P7: pass, as intended.
- X18: MISSED (a known residual).

Vacuity sweep (log dir `negtest_logs\20261011_233140_234u`): 49/49 CAUGHT.

This review's own runs:

| Run | Mutations | Log dir |
|-----|-----------|---------|
| R1-R14 | 15 | `negtest_logs\20261011_233347_ur8t` |
| R15-R17 | 3 | `negtest_logs\20261011_233601_gnbh` |
| L-R5..7 | 3 | `negtest_logs\20261011_233725_s35v` |

The log dirs are under `%TEMP%`.

### INFO-3: semantics carried by host tests

The check pins the shape of the gate and verdict calls, not what they do:
- `ui_safety_clear_verdict` (NOT_TRIPPED, then NEEDS_ADMIN, then SEND) is tested by `test_ui_page_safety_logic.c`.
- `ui_lcd_lock_run_gated` and `ui_lcd_lock_has_role` are tested by `test_ui_lcd_lock.c`.

A regression there passes this check. This is LCDFX6 INFO-4, unchanged.
