# Review: lcdfx5 (3ba63d59d, 58a938428), 2026-10-10

Adversarial review of the `tools/check_lcd_admin_gates.ps1` hardening that
closes LOW-1..4, INFO-1 and INFO-4 in `docs/audits/REVIEW_LCDFX4_2026-10-10.md`.
The fixer's claims were:

- each USER nav callback body is exactly one gated call, and there are exactly 5;
- the ADMIN exemption is `touch_cal_nav_cb` only, with its body pinned;
- new rule C4 pins `ui_page_home.c` `trip_strip_clicked_cb`;
- preprocessor conditionals and duplicate definitions are refused;
- `build_nav_item` counts are matched, and only known `lv_obj_add_event_cb` forms pass.

Reviewed at dev tip `66aab5fe9`, in a minted `-NoSubmodules` worktree. No code
was changed.

**Verdict:** every claim holds for the code shapes it names. All eight LCDFX4
mutations (N1-N8) are now closed. But the check still pins only call sites it
already knows about. An ungated opener added anywhere else passes: elsewhere in
`ui_page_config.c`, elsewhere in `ui_page_home.c`, or in another file. A macro
can also rename the tokens the regexes trust. Ten new bypasses all pass the
check. Each needs a deliberate edit, so all are LOW.

## Runs

| Command | Result |
|---|---|
| `tools/check_lcd_admin_gates.ps1` | PASS (14 gates plus C3/C4, L9, relock) |
| `tools/negtest.ps1 -Preset check -PresetArg tools\check_lcd_admin_gates.ps1 -Mutations <14>` | Baseline PASS. 10 bypasses MISSED. 4 brittleness probes CAUGHT. `real_tree_unchanged: true`, `copies_removed: true` |

Working copies are CRLF (`w/crlf`). The check passes on them because `Get-Code`
strips `\r` before matching, so CRLF is not a gap.

| Mutation | File | Verdict |
|---|---|---|
| B1: `#define ui_lcd_lock_run_gated(p, r, f, u) ((void)(p), (void)(r), (f)(u))` above the callbacks | config | **MISSED** |
| B2: `#define LCD_PIN_ROLE_USER LCD_PIN_ROLE_NONE` above the callbacks | config | **MISSED** |
| B3: `safety_open_apply(NULL);` added before the ADMIN gate in `units_toggle_cb` | config | **MISSED** |
| B4: `lv_obj_add_event_cb(lv_obj_get_child(grid, 0), (lv_event_cb_t)safety_open_apply, ...)` in the hub build | config | **MISSED** |
| B5: `lv_timer_create((lv_timer_cb_t)diagnostics_open_apply, 10, NULL)` in the hub build | config | **MISSED** |
| B6: pinned `trip_strip_clicked_cb` under `#ifdef KILNCTL_NEVER_DEFINED`, with an ungated twin under `#else` | home | **MISSED** |
| B7: the strip also registers `(lv_event_cb_t)trip_strip_gated_open_cb` directly | home | **MISSED** |
| B8: a second strip handler calling `kiln_ui_show("safety")` | home | **MISSED** |
| B9: `.back_page = NULL` changed to `"safety"` on `ui_page_touch_test.c`, which opens with no role | touch_test | **MISSED** |
| B10: the relay gate moved into a dead `dead_relay_gate()`, with the live `relay_toggle_cb` calling `relay_toggle_apply` directly | temperature | **MISSED** |
| F1: forward declaration `static void safety_nav_cb(lv_event_t *e);` | config | CAUGHT ("2 definitions") |
| F2: `lv_event_t* e` spacing on `safety_nav_cb` | config | CAUGHT ("not found") |
| F4: `build_nav_item` prototype | config | CAUGHT ("7 calls, 6 recognised") |
| F5: an `ESP_LOGI` line in `trip_strip_clicked_cb` | home | CAUGHT (C4 shape) |

F1, F2, F4 and F5 are all legitimate C, so each CAUGHT is a false positive. See INFO-1.

## Claims verified

- **USER body shape.** The body regex is anchored to the whole captured body,
  with no `(?m)`. The comma operator (LCDFX4 N1), role arithmetic (N2) and code
  around the gate all fail it now. The capture ends at the first column-0 `}`.
  For a column-0 `}` to sit inside the gate, it would need an opening brace or
  quote that itself breaks the shape. Comments are blanked before matching. I
  found no way to end the capture early while the gate shape still matches.
- **Exactly 5, and one per apply.** `$navN -ne 5`, plus the per-apply table,
  close the floor gap from LCDFX4 INFO-1. N6 (a cast registration) fails the
  `navCalls` versus `navRe` count.
- **ADMIN exemption.** Only `touch_cal_nav_cb` gets it, and its body is pinned
  (N5 closed). Any other ADMIN-first callback falls through to the
  "not exactly one USER gate" failure.
- **Function pointers.** `build_nav_item(grid, "Safety", p)` with
  `lv_event_cb_t p = ...` is caught, because `p` has 0 definitions.
- **Duplicates and conditionals in `ui_page_config.c`.** N3/N4 are caught. A
  twin inside a string literal also counts as a definition, so it fails too.
- **INFO-4.** The joined `}# L9` line is split. The pass message names C3/C4.
  The LCDFX3 fix note now cites `70e79815f`.

## Findings

### LOW-1: a file-local macro can rename the tokens the regexes trust (B1, B2)

**Scenario:** `ui_page_config.c` gains
`#define ui_lcd_lock_run_gated(p, r, f, u) ((void)(p), (void)(r), (f)(u))`, or
`#define LCD_PIN_ROLE_USER LCD_PIN_ROLE_NONE`. Every nav callback still has the
exact pinned text, so C3 passes. But the compiler now opens Safety, Diagnostics
and the other pages with no PIN. The same edit in `ui_page_home.c` defeats C4,
and in any other UI file it defeats that file's L-rule. A macro in a new or
existing header (`ui_lcd_lock.h`, or a newly included one) does the same thing
without touching the checked file at all. Only `#if`/`#ifdef`/`#elif`/`#else`
are refused, not `#define`, `#undef` or `#include`.

**Fix:** fail if any `#define` or `#undef` under `firmware/KilnFW/App` names one
of the trusted identifiers: `ui_lcd_lock_run_gated`, `ui_lcd_lock_has_role`,
`LCD_PIN_ROLE_(NONE|USER|ADMIN)`, `kiln_ui_show`, `ui_page_safety_open`,
`lcd_safety_strip_needs_pin`, `touch_cal_store_is_calibrated`, and the
`*_open_apply` / `trip_strip_gated_open_cb` names. Scanning by name rather than
by file covers headers too. Optionally, also pin the `#include` list of the
two pinned files.

### LOW-2: other openers in `ui_page_config.c` are not inventoried (B3, B4, B5)

**Scenario:** C3 checks what each `build_nav_item` callback does. It does not
check whether some other code in the file opens a USER page directly:
- **B3:** `units_toggle_cb` is an allowlisted registration whose body is never
  read. A `safety_open_apply(NULL);` added there opens Safety from the Units
  cell with no PIN, and L11's units rule still matches.
- **B4:** the `lv_obj_add_event_cb` scan needs a bare identifier as the first
  argument (`\(\s*\w+\s*,`). `lv_obj_add_event_cb(lv_obj_get_child(grid, 0), ...)`
  is skipped, and so is a space before the `(`.
- **B5:** `lv_timer_create` is not scanned at, and neither is any other callback
  registrar (`lv_obj_add_event`, `lv_async_call`, `lv_anim_set_*_cb`).

**Fix:** inventory the openers instead of the registrars. In
`ui_page_config.c`, require that:
- `kiln_ui_show("temperature"|"network"|"diagnostics"|"profiles")` and
  `ui_page_safety_open(` each occur exactly once, inside their own
  `*_open_apply` body;
- `kiln_ui_show("touch_cal")` occurs exactly twice (in `touch_cal_open_apply`
  and in the pinned uncalibrated branch);
- each `*_open_apply` identifier occurs exactly twice: its definition and its
  one gated call.

Also widen the `lv_obj_add_event_cb` regex to `\blv_obj_add_event_cb\s*\(`, and
fail on any match it cannot parse.

### LOW-3: C4 lacks the LOW-1 and duplicate protections that C3 has (B6, B7, B8)

**Scenario:** the preprocessor and duplicate-definition refusals were added for
`ui_page_config.c` only.
- **B6:** in `ui_page_home.c`, an `#ifdef NEVER` around the pinned handler,
  with an ungated twin under `#else`, passes. C4's regex takes the first
  (dead) definition, and the twin calls `trip_strip_gated_open_cb(NULL)`, so
  the "exactly one `ui_page_safety_open(`" count still holds.
- **B7:** `trip_strip_gated_open_cb` can be registered directly on the strip,
  or called from any other handler. Its references are not counted.
- **B8:** `kiln_ui_show("safety")` opens the same page and is not counted at
  all.

**Fix:** apply C3's refusals to `ui_page_home.c` too: no `#if`/`#ifdef`/
`#ifndef`/`#elif`/`#else`, and exactly one `trip_strip_clicked_cb(lv_event_t`
definition. Require `trip_strip_gated_open_cb` to occur exactly 3 times: the
definition, the gated argument and the else-branch call. Require
`kiln_ui_show("safety")` to occur 0 times in the file. `ui_page_home.c` has
no conditionals today; it has `#define`s, which the LOW-1 fix handles by name.

### LOW-4: no repo-wide inventory of the ways into USER pages (B9)

**Scenario:** the check pins two files. Any other UI file can open Safety,
Diagnostics, Temperature, Network or Profiles without a role:
- through `kiln_ui_show("<page>")`;
- through `ui_page_safety_open(`;
- by setting `ui_topbar_cfg_t.back_page` to one of those names. The topbar's
  `nav_cb` calls `kiln_ui_show(page)` with the stored string.

B9 sets `.back_page = "safety"` on `ui_page_touch_test.c`. By owner exception,
that page opens with no role after a calibration save, so its Back button
would open Safety with no PIN. A grep today finds only the expected call sites:
`ui_page_profile_builder_review.c:133` (after an ADMIN save), the
`ui_page_safety.c` self-show, and the pinned config and home sites. The
`back_page` values are `home`, `config` and in-flow pages. `kiln_ui_show` has
no callers outside `drivers/ui/`. Nothing keeps it that way.

**Fix:** add a rule over `drivers/ui/*.c`. Every
`kiln_ui_show("(safety|diagnostics|temperature|network|profiles)")`,
`ui_page_safety_open(` and `.back_page = "(those names)"` must appear on an
allowlist of file and function pairs, and the allowlist should be part of the
check. Fail on any non-literal `kiln_ui_show(<expr>)` outside `ui_topbar.c`
`nav_cb` and the `ui_page_touch_cal.c` exit-target calls, which are already
host-tested through `lcd_touch_cal_exit_target()`.

### LOW-5: the 14 L-rules are still file-wide text matches (B10)

**Scenario:** LCDFX4 LOW-2 (N2) was fixed for C3 only. Every `$rules` entry is
still a single `-match` against the whole file. In `ui_page_temperature.c`,
moving the `"Admin PIN to switch relay"` gate into a dead function, and having
`relay_toggle_cb` call `relay_toggle_apply` directly, passes. The relay then
switches with no admin PIN. The same works for the network, profile save,
live-decide, units and live-edit rules. The `has_role(LCD_PIN_ROLE_ADMIN)` rules
(touch-cal save, profile delete, crash ack, AP password mask) are weaker still:
any one occurrence anywhere in the file satisfies them.

**Fix:** give each rule a function name, and match the gate inside that
function's body using the same body extractor as C3. For `run_gated` rules,
require the gate to be the body's first statement and the apply to be named.
For `has_role` rules, require it to be the condition of the `if` that guards the
action.

### INFO-1: false-positive brittleness (F1, F2, F4, F5)

These legitimate edits fail the check:
- a forward declaration of any nav callback ("2 definitions");
- `lv_event_t* e`, `lv_event_t *ev`, or `static inline void`, all of which the
  `static void <cb>\(lv_event_t \*e\)` extractor rejects;
- a `build_nav_item` prototype;
- any added statement in `trip_strip_clicked_cb`, such as a log line or a tap
  action for the non-Safety strip state (`s_ui_home_trip_strip_is_safety ==
  false` currently does nothing);
- any `#if` in `ui_page_config.c`, such as a future `CONFIG_KILNCTL_SIM_PLANT`
  cell;
- a sixth USER cell, or a new allowlisted registration, until the check is
  edited.

All of these fail closed, and the messages name the rule. The failure messages
should also say how to fix the check:
- count definitions as `<cb>\s*\([^)]*\)\s*\{`, so a prototype is not counted;
- accept `lv_event_t\s*\*\s*\w+` in the extractor;
- exclude prototypes (`build_nav_item\([^)]*\)\s*;` with typed parameters) from
  `navCalls`;
- add a pointer to the line to edit when the cell count changes.

### INFO-2: `*_open_apply` bodies are not pinned

C3 proves that each gated apply name is passed with USER. It does not read the
apply bodies. `profiles_open_apply` already runs `ui_page_profiles_refresh()`.
An ADMIN-tier action placed in any apply would run with only a USER PIN. The
LOW-2 inventory covers the page-show half of this. Pinning each apply to its
known body, as done for `touch_cal_nav_cb`, would cover the rest.

### INFO-3: an unbalanced apostrophe in a directive desynchronizes the comment stripper

`Remove-CComments` treats every `'` as the start of a char literal. A
`#warning don't ...` or `#error` line with a single apostrophe (GCC accepts it
with a warning) makes the stripper copy everything up to the next `'`
verbatim. Comments inside that stretch then stay in the text, so a gate left in
a comment can satisfy a file-wide L-rule again. I did not build a
mutation for this; it needs an odd directive. Fix: blank `#` directive lines,
other than `#define`/`#include`, before tokenizing, or make LOW-5's
per-function rules ignore any text before the function.

## Severity summary

| ID | Severity | Mutations |
|---|---|---|
| LOW-1 | LOW | B1, B2 |
| LOW-2 | LOW | B3, B4, B5 |
| LOW-3 | LOW | B6, B7, B8 |
| LOW-4 | LOW | B9 |
| LOW-5 | LOW | B10 |
| INFO-1..3 | INFO | F1, F2, F4, F5 |

None of these is live in the current source: the shipped code is correctly
gated, and the check passes on it. Every finding needs a deliberate or careless
edit that the check would fail to flag.
