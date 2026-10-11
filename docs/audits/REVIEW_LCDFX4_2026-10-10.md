# Review: lcdfx4 (70e79815f, 4ec5ba5cf), 2026-10-10

Adversarial review of the fixes for LOW-1..3 and INFO-7 in
`docs/audits/REVIEW_LCDFX3_2026-10-10.md`:
- `tools/check_source_bytes.ps1`: git exit codes are checked, there is a
  1000-file floor and an anchor file, and stale temp directories are swept.
- `tools/check_lcd_admin_gates.ps1`: rule C3 now enumerates the callbacks
  from `build_nav_item`, refuses code around a USER gate and refuses `#if 0`.
- `firmware/SaftyFW/tools/check_bootloader_builds.ps1`: the `TrimEnd('\')` fix.

Reviewed at dev tip `4ec5ba5cf` in a minted `-NoSubmodules` worktree. No code
was changed.

## Runs

| Command | Result |
|---|---|
| `tools/check_lcd_admin_gates.ps1` | PASS (19 gates) |
| `tools/check_source_bytes.ps1` | PASS (2583 tracked text files; `-NoSubmodules` worktree) |
| `firmware/SaftyFW/tools/check_bootloader_builds.ps1` | PASS (118/118, `saftyfw_bootloader.elf` built; first run in this worktree, so it did a fresh configure) |
| `tools/check_gate_negative_test_table.ps1` | PASS (198 discovered checks, 199 rows; f65950541's rows close LCDFX3 LOW-4) |

Negtest: `tools\negtest.ps1 -Preset check -PresetArg tools\check_lcd_admin_gates.ps1`
with 8 mutations. The baseline passed, `real_tree_unchanged: true`, and the
copies were removed.

| Mutation (`ui_page_config.c` unless noted) | Verdict |
|---|---|
| N1: the gate statement gains a comma operator: `ui_lcd_lock_run_gated(..., USER, safety_open_apply, NULL), safety_open_apply(NULL);` | **MISSED** |
| N2: the role argument is weakened to `LCD_PIN_ROLE_USER - 1` (NONE), with a dead `static void dead_cb(void)` holding the exact USER gate text | **MISSED** |
| N3: the gated `safety_nav_cb` is under `#ifdef KILNCTL_NEVER_DEFINED`, with an ungated twin under `#else` | **MISSED** |
| N4: the same as N3 with `#if (0)` | **MISSED** |
| N5: `touch_cal_nav_cb` (ADMIN) gets `kiln_ui_show("diagnostics");` after its gate | **MISSED** |
| N6: Safety registered as `build_nav_item(grid, "Safety", (lv_event_cb_t)safety_direct_cb)`, with an ungated `safety_direct_cb` | CAUGHT, but only by the `navN >= 5` floor |
| N7 (`ui_page_home.c`): the trip-strip PIN condition is replaced by `if (0) {`, so the strip opens Safety directly | **MISSED** |
| N8 (control): the gate is moved into a helper (`safety_gated_helper();`) | CAUGHT (per-target rule, C3 no-gate, floor) |

## Findings

### LOW-1: C3 still misses the preprocessor bypass when it is not spelled `#if 0` (N3, N4)

The new refusal matches only `^\s*#\s*if\s+0\b`. `#if (0)`, `#ifdef NEVER`,
`#if defined(X)` for an undefined X, `#if !1` and `#if 0x0` all get past it.
C3 then takes the **first** definition of `safety_nav_cb`, which is the dead,
gated copy. The per-target text rule is satisfied by that same dead copy. N3
and N4 pass with an ungated live callback. This is the M3 class from LCDFX3
LOW-3, in a different spelling.

`ui_page_config.c` has no preprocessor conditionals today, since the grep for
`#if`, `#ifdef` and `#else` is empty. Fix: refuse every `#if`, `#ifdef`,
`#ifndef`, `#elif` and `#else` in `ui_page_config.c`. Also fail when a nav
callback name has more than one definition.

### LOW-2: the USER-gate statement match accepts anything up to the `;` (N1, N2)

The gate is matched as `ui_lcd_lock_run_gated\([^;]*;`, and then:
- the code only tests whether `LCD_PIN_ROLE_USER` appears anywhere in that
  text;
- `$post` begins after the `;`.

This leaves two gaps:
- **N1:** a comma operator, `gate(...), safety_open_apply(NULL);`, puts a
  direct open inside the "gate statement", and the check passes.
- **N2:** the role argument can be weakened, for example to
  `LCD_PIN_ROLE_USER - 1`, a ternary, or a string containing the token. The
  file-wide per-target rule still passes, because a dead function anywhere in
  the file carries the exact text.

N2 is deliberate. N1 is unlikely to happen by accident. Both pass the check
while the page opens without a PIN.

Fix: require each USER-gated body to match one exact shape:

```
^\s*(\(void\)\s*e\s*;)?\s*ui_lcd_lock_run_gated\("[^"]*",\s*LCD_PIN_ROLE_USER,\s*(\w+_open_apply),\s*NULL\);\s*$
```

Then tie each per-target rule to that callback's own body, not to the whole
file.

### LOW-3: the ADMIN allowance is unchecked; any code around an ADMIN gate passes (N5)

`elseif ($g.Value -match 'LCD_PIN_ROLE_ADMIN') { continue }` skips every check
for a callback whose first gate is ADMIN. Two bypasses follow:
- **N5:** a direct `kiln_ui_show("diagnostics")` after the touch-cal ADMIN
  gate opens a USER page with no PIN.
- **Untested, same code path:** a new nav callback whose first statement is
  `if (0) ui_lcd_lock_run_gated(..., LCD_PIN_ROLE_ADMIN, ...);`, followed by
  `safety_open_apply(NULL);`, also passes. It is the ADMIN form of the
  LCDFX3 M2 case.

An existing USER callback cannot be converted this way: the five USER
callbacks are exactly at the `navN >= 5` floor, so a conversion is caught. But
an *added* callback, or extra code in `touch_cal_nav_cb`, gets through.

Fix: allowlist the ADMIN callback by name (`touch_cal_nav_cb`). Pin its whole
body to the known shape:

```
(void)e; if (!touch_cal_store_is_calibrated()) { kiln_ui_show("touch_cal"); return; } <ADMIN gate to touch_cal_open_apply>;
```

Then fail any other ADMIN-gated callback, or apply the same exact-shape rule
used for USER.

### LOW-4: the Home trip strip, the other path into Safety, is not pinned (N7)

`ui_page_home.c` `trip_strip_clicked_cb()` opens Safety through
`lcd_safety_strip_needs_pin(ui_lcd_lock_has_role(LCD_PIN_ROLE_USER))`. That
predicate has host tests in `test_lcd_auth_state.c`, but no check ties the
call site to it. Replacing the condition with `if (0)` passes every check
(N7), which leaves an ungated Safety open on the trip strip. The bench LCD
cases (`cases_lcd.py`) exercise the strip only on hardware.

Fix: add a rule to `check_lcd_admin_gates.ps1`. In `trip_strip_clicked_cb`,
`trip_strip_gated_open_cb` must be reached only through the
`lcd_safety_strip_needs_pin(ui_lcd_lock_has_role(LCD_PIN_ROLE_USER))` branch,
with the gated call `ui_lcd_lock_run_gated(..., LCD_PIN_ROLE_USER,
trip_strip_gated_open_cb, NULL)`. A simple form: require both texts, and
require that no other `ui_page_safety_open(` call exists outside
`trip_strip_gated_open_cb` and `safety_open_apply`.

The other openers were checked by grep: `kiln_ui_show("temperature"|"network"|"diagnostics"|"profiles"|"safety")`
outside the hub. Only `ui_page_profile_builder_review.c:133` (after an
ADMIN-gated save) and `ui_page_safety.c` itself show these pages, and both
are fine.

### INFO-1: enumeration gaps are covered only by the floor (N6)

The enumeration regex needs `build_nav_item(<ident>, "<literal>", <ident>)`.
These forms drop out of the enumeration:
- a cast callback;
- a non-literal label;
- a table or array callback (`cbs[i]`);
- a cell built with a direct `lv_obj_add_event_cb`.

N6 was caught only because the five USER callbacks sit exactly at the
`navN >= 5` floor. The day a sixth USER cell is added without raising the
floor, the same mutation passes.

Fix (optional): fail when the number of `build_nav_item(` and
`lv_obj_add_event_cb(` occurrences differs from the number of enumerated
callbacks plus the known non-nav handlers. Alternatively, make the floor
equal the count of USER `build_nav_item` calls. A macro wrapping
`build_nav_item(grid, "...", c)` is caught by accident, because the macro
body is enumerated as a callback named `c`, which is "not found".

### INFO-2: the source-bytes fixes are sound

- **Git exit codes.** Every git call before the scan goes through
  `Invoke-GitChecked`: `rev-parse`, `add -u` and both `ls-files`. A nonzero
  exit prints FAIL with git's stderr and exits 1. `cat-file --batch` was
  already checked. `exit` inside `try` still runs `finally`, which unsets
  `GIT_INDEX_FILE` and removes the temp directory.
- **Floor and anchor with `-NoSubmodules`.** Submodule content is never
  listed by the superproject's `ls-files`; gitlinks are mode 160000 with no
  text extension. So the count is the same with or without submodules: 2583
  here. Nothing in the repo runs the check against a scratch repo, so the
  floor breaks no caller. Only a `-Repo` pointed at a submodule or a small
  repo fails, which is intended.
- **Temp sweep.** The sweep deletes only `%TEMP%\srcbytes_*` directories
  whose LastWriteTime is more than an hour old. No other tool uses that
  prefix (grep of `tools/`). A live run's directory is created and written
  (index copy, `git_err.txt` on every git call, `in.txt`, `out.bin`) within
  seconds of start, and a run takes seconds. Only a run hung for over an hour
  could be swept, and then it is long past useful. A file still held open
  makes `Remove-Item` fail silently (`SilentlyContinue`), so the sweep cannot
  break a run.

### INFO-3: the bootloader TrimEnd fix is correct and inert

`GetFullPath` of the `Join-Path` result never ends in `\`, so the lock name
(SHA-1 of the lower-cased path) is unchanged. A run that predates the fix and
a run that follows it still serialize on the same mutex. The check passes.

### INFO-4: cosmetic

- In `check_lcd_admin_gates.ps1`, the floor line and the `# L9:` comment are
  now joined on one line: `...$fail++ }# L9: ...`. This is valid PowerShell,
  but it reads as an editing accident.
- The pass message still says "N gates plus L9 init order" and does not
  mention C3 or the relock rule (LCDFX3 INFO-6).
- The LCDFX3 fix-status note cites the fix commit by subject, not by SHA
  (`70e79815f`).
