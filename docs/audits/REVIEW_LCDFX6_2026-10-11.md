# Review LCDFX6: hardened check_lcd_admin_gates.ps1 (2026-10-11)

Scope: the REVIEW_LCDFX5 fix to `tools/check_lcd_admin_gates.ps1` (per-function gate rules, repo-wide macro
scan, opener inventory, relaxed brittleness) and its fix-status note. Reviewed at origin/dev `4efaf2cda`.

The check enforces two owner decisions:

- (D1) the LCD Safety page needs a USER PIN;
- (D2) LCD trip clear needs an admin login and follows the web clear rules.

## Commits

- The task named `50bfa5948`, but that sha is **not on origin/dev**. It is the pre-rebase sha.
- The code landed on dev as **`697f8f338`** ("check_lcd_admin_gates: per-function gate rules, macro scan, opener
  inventory, relaxed brittleness (REVIEW_LCDFX5)"). It touches only `tools/check_lcd_admin_gates.ps1`, and the
  script is byte-identical to the one in `50bfa5948`.
- `ae64fd230` adds the REVIEW_LCDFX5 "Fix status" section. That section cited `50bfa5948`; this review corrects
  it to `697f8f338`.

## Current state

- `check_lcd_admin_gates.ps1` passes on dev `4efaf2cda`, exit 0.
- **Vacuity sweep: 49/49 CAUGHT.** Every rule was mutated:
  - 24 rule-row weakenings (ADMIN to USER);
  - 13 extra direct calls of each counted apply;
  - 5 C3 USER nav callbacks (USER to NONE);
  - 4 C3 `*_open_apply` bodies gaining a side effect;
  - 2 C4 mutations (strip gate USER to NONE, and an inverted `lcd_safety_strip_needs_pin`);
  - 1 relock-order mutation.

  No rule is vacuous.

## Findings

### HIGH-1: D2 (LCD trip clear needs admin) has no mechanical coverage

- Nothing in the check mentions `ui_page_safety.c`'s clear path. The L4 inventory explicitly skips `ui_page_safety.c`.
- Git history shows it never covered the clear gate.
- `test_ui_page_safety_logic.c` tests only `ui_safety_clear_verdict()`. It does not test which role the caller
  passes or whether the button is gated.
- Both of these mutations pass the check:
  - **X16**: `btn_clicked_cb`'s `ui_lcd_lock_run_gated("Enter admin PIN to clear trip", LCD_PIN_ROLE_ADMIN, clear_action, NULL)`
    and `clear_action`'s `ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)` both changed to `LCD_PIN_ROLE_USER`. This is a
    USER-PIN trip clear, which contradicts D2.
  - **X17**: the button calls `clear_action(NULL)` directly, and the verdict role is forced to `true`. The trip
    clears with no PIN at all.
- Fix: add rows for `ui_page_safety.c`:
  - `btn_clicked_cb` is exactly the ADMIN gate on `clear_action`;
  - `clear_action` passes `ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)` to `ui_safety_clear_verdict`;
  - `clear_action` has exactly one reference outside its definition.

  Negative-test the new rows with X16 and X17.

### MED-1: dead-gate and twin-callback refactors pass the per-function rules

The `GateRx` rows search the function body unanchored, and the check never ties the gated callback to its widget
registration. Two mutations pass:

- **X13**: the apply is renamed `relay_toggle_do`. A one-line `relay_toggle_apply` wrapper keeps the counted name,
  and `relay_toggle_cb` calls `relay_toggle_do()` then `return;` before the intact gate. The gate is still present,
  so the check passes, but the gate is dead code. The apply-use count only follows the name the row pins.
- **X14**: the gated `relay_toggle_cb` stays defined and is referenced as `(void)relay_toggle_cb;`. The button's
  `lv_obj_add_event_cb` switches to a new ungated `relay_quick_cb`.

Both are plausible accidental refactors, not only deliberate evasion. Fixes:

- Anchor every `GateRx` body as C3 already does (the body is exactly the gate, or the gate is the first statement
  with no early return).
- Pin each L-row callback's registration: exactly one `lv_obj_add_event_cb(..., <cb>, ...)` per callback, as C3
  does for the config page.

### MED-2: D1 (Safety needs USER PIN) opener inventory is literal-text-only

Each of these opens Safety, or another USER page, from a no-role context, and the check still passes:

- **X6** `kiln_ui_show("saf" "ety")` and **X7** `.back_page = "saf" "ety"`, both in `ui_page_touch_test.c` (a
  no-role page). The literal regex does not see adjacent-literal concatenation.
- **X8** `#define GO kiln_ui_show` then `GO("diagnostics")`. The macro scan only refuses defining a trusted token,
  not aliasing one.
- **X9** `kiln_ui_show("safety")` from `drivers/bridge/uart_bridge_touch.c`. L4 scans `drivers/ui` only. Anything
  else under `App/` that includes `kiln_ui.h` is unchecked.
- **X10** a second registration, `kiln_ui_register_page("safety_view", ui_page_safety_build)` in `kiln_ui.c`, then
  `kiln_ui_show("safety_view")` from touch_test. The check pins page names, not builders.
- **X15** in C4, `ESP_LOGI(TAG, "...", (int)kiln_ui_show("saf" "ety"))` inside `trip_strip_clicked_cb`. C4 strips
  `ESP_LOG*(...)` statements before pinning the body shape, so a call inside the log arguments disappears.

Fixes, in order of value:

- Scan all of `App/` (not only `drivers/ui`) for `kiln_ui_show`, `ui_page_safety_open` and `back_page`.
- Require `ui_page_safety_build` to be referenced exactly once, in `kiln_ui.c`'s registration.
- Concatenate adjacent string literals before matching (a `"\s*"` collapse).
- Refuse `ESP_LOG*` arguments that contain `(`, other than the logging macro's own parentheses, when stripping
  logs from a pinned body.

### LOW-1: preprocessor-level evasions of the macro scan and identifier counts

All of these pass. Each is deliberate-looking, not a plausible accidental refactor, hence LOW.

- **X1** `#define \` then a newline, then `ui_lcd_lock_run_gated(p, r, f, u) ...`. Line splicing comes before
  tokenizing; the scan regex needs the name on the `#define` line.
- **X2** `#/**/define ui_lcd_lock_run_gated(...)` and **X3** `#define/**/LCD_PIN_ROLE_USER LCD_PIN_ROLE_NONE`. The
  scan runs on raw text, where the comment is not yet whitespace.
- **X4** `%:define LCD_PIN_ROLE_USER LCD_PIN_ROLE_NONE`. The digraph `%:` is `#` and is always enabled.
- **X5** `set_source_files_properties(ui/ui_page_config.c PROPERTIES COMPILE_DEFINITIONS "ui_lcd_lock_run_gated=ui_lcd_lock_run_open")`
  in `App/drivers/CMakeLists.txt`. The macro scan reads only `*.c` and `*.h`.
- **X19** `LCD_PIN_ROLE_USER = 0,` in `lcd_auth_state.h`'s enum. It compiles (no `switch` on the enum), makes USER
  equal NONE, and shifts ADMIN down. It is not a macro, so nothing looks at it.
- **X11** `safety_open_ap\` newline `ply(NULL);` in `units_toggle_cb`. A line splice inside an identifier hides an
  ungated Safety open from `Count-Uses`.
- **X12** `// L3 admin gate (see header) \` newline `ui_lcd_lock_run_gated(...relay_toggle_apply...)`, then
  `relay_toggle_ap\` newline `ply(...)`. The backslash continues the `//` comment over the gate, so the compiler
  never sees the gate. `Remove-CComments` strips `//` only to the physical end of line, so the check still counts
  the gate, and the split direct call is invisible.

Cheap fixes:

- Splice `\`+newline before all other processing (fixes X1, X11, X12).
- Run the macro scan on comment-stripped text and accept `%:` as well as `#` (fixes X2, X3, X4).
- Grep `CMakeLists.txt` and `*.cmake` under `firmware/KilnFW` for the trusted tokens (fixes X5).
- Pin the `lcd_pin_role_t` enum body text (fixes X19).

### LOW-2: brittleness — seven legitimate edits fail falsely (all fail loud)

Each probe below is a reasonable edit that fails the check. Every failure message names the rule and says what to
edit, so the cost is friction, not a silent false green. The exact messages are in the negtest logs.

- **P1** `ESP_LOGI(TAG, "safety cell tapped");` in `safety_nav_cb` fails C3 "not exactly one USER gate statement".
  This is inconsistent: C4 strips `ESP_LOG*` lines, but C3 does not.
- **P2** `ESP_LOGD(TAG, "relay_toggle_apply");` inside the apply fails with "referenced 2 time(s)".
  `Count-Uses` counts string literals. Fix: blank string literals before counting.
- **P3** a host-test stub `#define kiln_ui_show(name) test_stub_show(name)` in `App/test` fails L1-macro. The macro
  scan covers `App/test`, where stubbing is normal. Exclude `App/test` or allow-list it per file.
- **P4** `#define LCD_PIN_ROLE_COUNT 3` in `lcd_auth_state.h` fails L1-macro. The trusted token
  `LCD_PIN_ROLE_\w+` is too broad; list the three real enumerators instead.
- **P5** renaming `units_toggle_apply`'s parameter to `ud` fails the anchored `(void)user_data;` shape.
- **P6** `__attribute__((noinline))` on `relay_toggle_cb` makes the definition invisible ("0 definitions").
- **P7** a log line before `crash_ack_do();` fails the `$`-anchored crash-ack shape.

### INFO

- **INFO-1** `697f8f338` (and its pre-rebase twin `50bfa5948`) has an empty body and no `Co-Authored-By` trailer.
  `ae64fd230` has one. History is not rewritten.
- **INFO-2** Trigraphs are not a bypass: KilnFW sets no `-std`, so it gets the ESP-IDF default `gnu17`, where trigraphs are off.
- **INFO-3** **X18** (apply made non-static and called ungated from `ui_page_home.c`) passes because apply-use
  counts are per file. A `static`-only rule for every counted apply would close it. Any `static` to extern change
  is visible in review, so INFO.
- **INFO-4** Two pieces of logic still rest on host tests, not this check: `lcd_safety_strip_needs_pin()` (C4's
  predicate; the inverted call is caught, a body change is not) and `lcd_touch_cal_exit_target()`.

## Test summary

All runs used `tools/negtest.ps1 -Preset check -PresetArg tools/check_lcd_admin_gates.ps1` in worktree
`C:\wt\lcdfx6_hdtu0s` at `4efaf2cda`.

- **Bypass set** (X1-X19, P1-P7):
  - baseline PASS;
  - X1-X19 all MISSED (the findings above);
  - P1-P7 all CAUGHT (each one is a false positive, LOW-2);
  - real tree unchanged, copies removed.
- **Vacuity set**: 49/49 CAUGHT, baseline PASS, real tree unchanged.
- Direct run of the check on dev `4efaf2cda`: PASS, exit 0.
