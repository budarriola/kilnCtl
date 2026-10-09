# Release gate vacuity audit — 2026-09-16f

Sixth slice of blocker 3 (`docs/RELEASE_HARDENING_PLAN.md`). Continues from
`docs/audits/release_gate_vacuity_audit_2026-09-16e.md`, which must not be
redone — its gates (and those of `_2026-09-16.md`/`b`/`c`/`d`) are load-bearing
and closed. This pass worked `-16e.md`'s "Gates not examined" list: eight
gates closed, one stale documented negative-test example fixed, and one real
vacuity gap found (not yet fixed) in `check_duplicate_symbols.ps1` itself.

Worktree: `C:\wt\vacuity6_xfthp7` (`git worktree add --detach` at
`origin/main`, minted via `tools/worktree_mint.ps1 -Label vacuity6 -RunSetup`),
submodules initialized, `tools/PcTools/.venv` provisioned automatically by the
`-RunSetup` switch. Concurrent agents were actively pushing during this pass —
origin/main advanced `61c75767` (start) to `24d4f465` (a fix for
`-Werror=format-truncation` in `backup_import.c`, merged mid-pass to unblock a
clean KilnFW build baseline) and again to a `screen_idle` stack-ceiling fix,
both fast-forward-merged into this worktree with `git merge --ff-only
origin/main` before committing. SaftyFW/PcTools tests ran via the Bash tool
from this `C:\wt\` path; the ESP-IDF KilnFW target build ran via the
PowerShell tool. No JTAG flash-bank probing, no flashing, no board contact, no
`.kicad_*` file touched. Every sabotage below was restored by hand (never
`git checkout --`/`git restore`/`git stash`) and confirmed both by an empty
`git diff` and a `git hash-object <path>` match against `git rev-parse
HEAD:<path>`.

## 1. `tools/check_mcp_facade_coverage.py` — stale documented example fixed

The module docstring's negative-test example named `plant_sim_compare` as a
tool that would fail coverage if its `KEYWORDS` entry were removed. That tool
is independently matched by `GROUP_PREFIXES` today, so the documented example
is vacuous: removing its `KEYWORDS` entry no longer fails anything, and a
future maintainer following the docstring's own recipe verbatim would get a
false "the check is broken" reading.

**Fix.** Queried `mcp_facade.py`'s own `GROUP_OVERRIDES`/`KEYWORDS`/
`GROUP_PREFIXES` structures for a tool matched by `KEYWORDS` alone (no
override, no prefix match) and found `ramp_assist_set_enabled`
(`tools/PcTools/src/kilnctrl/mcp_server_ramp_assist.py:79`, a real registered
tool). Replaced the stale example in the docstring with this one, and added a
short note on why the old example went stale and how to re-derive a fresh one
in future (query the three dicts directly rather than trusting a fixed name to
stay uncovered forever).

**Verified live**, not just documented: removed the `ramp_assist_set_enabled`
`KEYWORDS` entry from `tools/PcTools/src/kilnctrl/mcp_facade.py` —

```python
"ramp_assist_set_enabled": ("ramp", "assist", "stretch", "dwell", "credit", "cone",
                            "heat-work", "pin", "toggle", "enable", "disable"),
```

— re-ran `check_mcp_facade_coverage.py`: **FAILS**, naming
`ramp_assist_set_enabled` as uncovered. Restored the two-line entry by hand;
`git diff` empty; `git hash-object` on `mcp_facade.py` matched
`git rev-parse HEAD:tools/PcTools/src/kilnctrl/mcp_facade.py`
(`blob:fc47fb26af29318a63f4c39292632ad400477ce`). Re-ran: **PASSES**.

## 2-5. `tools/check_stack_margin_registration.ps1` — all four untested sub-checks

Only the create-vs-register sub-check had ever been exercised before this
pass. The script `throw`s at the first failing sub-check, so each of the
following four had to be isolated one at a time without tripping an earlier
one.

**2. Required-name-missing.** `firmware/KilnFW/App/drivers/ui/screen_idle.c`
line 333, `stack_margin_register("screen_idle", &s_task_handle, 6144);`
temporarily renamed to `"screen_idle_x"`. **FAILS**, naming `screen_idle` as
missing its required registration. Restored by hand; hash
`blob:0d9356f10906b2c1bcbb7a4278fc5fd9b4fd14a9` matched HEAD. **PASSES.**

**3. Duplicate-name.** Renaming an existing call site (tried first, on
`telemetry_log.c`) instead trips sub-check 2 first, since it removes the
original name's only registration — the two failure modes have to be
disentangled by *adding* a stray call rather than *renaming* one. Appended a
second `stack_margin_register("screen_idle", &s_task, 6144);` call
immediately after `telemetry_log.c`'s own (unmodified) registration at line
324. **FAILS**, naming `screen_idle` as registered more than once. Restored by
hand (stray line removed); hash
`blob:08863b5710b31b7931996f439c714c7e1e1b5978` matched HEAD. **PASSES.**

**4. Cap-vs-count.** `firmware/KilnFW/App/drivers/common/stack_margin.h` line
105, `#define STACK_MARGIN_MAX_TASKS 48u` temporarily lowered to `20u`
(below the real registered-task count). **FAILS**, naming the overflow.
Restored to `48u`; hash `blob:6485e878beddfe3d16dcd641cd5cee8ce51e99c3` matched
HEAD. **PASSES.**

**5. hwAbstraction accessor/boundary check.** `firmware/hwAbstraction/esp/
gpio/hal_gpio_esp.c` — added `#include "stack_margin.h"` after the existing
`#include "hal_esp_common.h"` line (~line 38), violating the one-way rule that
HAL backend files must never include KilnFW/SaftyFW headers. **FAILS**, naming
the illegal include. Restored by hand (include line removed); hash
`blob:4ec2bd2b15e81f370e864a5cdb87aff7d9591b7b` matched HEAD. **PASSES.**

All four sub-checks are load-bearing — none vacuous.

## 6. `tools/check_duplicate_symbols.ps1` — FAIL path exercised for the first time, plus a real vacuity gap found

This check had previously only been verified clean-baseline (263 objects, no
duplicates). Exercising its FAIL path requires a real ESP-IDF build, which at
pass start failed on the pre-existing, out-of-scope
`-Werror=format-truncation` break at `firmware/KilnFW/App/drivers/http/
backup_import.c:1284` (owned by another agent, named in the task brief).
Merged the upstream fix (`24d4f465`'s ancestor, landed as commit
`7ad48c62 Fix -Werror=format-truncation in backup_import.c's safety_i_normal_a
restore`) via `git merge --ff-only origin/main`, then rebuilt clean.

**Sabotage.** Appended `int vacuity_audit_dup_symbol_probe(void) { return 1;
}` to `firmware/KilnFW/App/drivers/ui/screen_idle.c`, and the same symbol
returning `2` to `firmware/KilnFW/App/drivers/persist/telemetry_log.c` (an
externally-linked duplicate definition across two translation units in the
same component). Rebuilt (`idf.py -C firmware/KilnFW build`) — compilation of
both objects succeeds; the final link step fails as expected (duplicate
symbol at link time), confirming the check inspects `.obj` files directly
rather than depending on link success. Ran `check_duplicate_symbols.ps1`:
**FAILS**, naming `vacuity_audit_dup_symbol_probe` as duplicated across
`screen_idle.c.obj` and `telemetry_log.c.obj`.

Restored both files by hand (probe function removed from each); hashes
`blob:0d9356f10906b2c1bcbb7a4278fc5fd9b4fd14a9` (`screen_idle.c`) and
`blob:08863b5710b31b7931996f439c714c7e1e1b5978` (`telemetry_log.c`) matched
HEAD. Forced a full clean rebuild into a fresh output (`idf.py -C
firmware/KilnFW fullclean` then `build`): **SUCCESS**. Re-ran
`check_duplicate_symbols.ps1` against the fresh build: **PASSES**, 263 objects,
no duplicates — matching the previously-recorded clean baseline exactly.

**Production bug found in the check itself (not fixed this pass).**
`firmware/hwAbstraction/idf/hwabstraction_esp/CMakeLists.txt` genuinely
compiles `../../common/hal_status.c` (i.e.
`firmware/hwAbstraction/common/hal_status.c`) into the `hwabstraction_esp`
component's SRCS. But `check_duplicate_symbols.ps1`'s `$componentSourceRoots`
mapping for that component only points at `firmware\hwAbstraction\esp`,
omitting `firmware\hwAbstraction\common` entirely. The check's stale-object
filter matches an `.obj`'s basename against source files found under its
component's registered root(s) to decide whether that `.obj` is current
build output or leftover cruft from a deleted/renamed source; since
`hal_status.c` is invisible to that root list for this component,
`hal_status.c.obj` is unconditionally classified "stale" and silently
excluded from duplicate-symbol scanning on **every** run — a live vacuity
gap. It has not caused a false pass to date because no actual duplicate
symbol currently originates from `hal_status.c`, but the check would miss one
if it existed. Left unfixed and flagged here rather than patched blind, since
correcting `$componentSourceRoots` touches the check's core stale-detection
logic and deserves its own reviewed change, not a drive-by edit inside an
audit pass already carrying five other sabotage/restore cycles.

## 7. `firmware/KilnFW/App/test/check_ui_budget_asserts.ps1`

Pure static grep check, no build required. `firmware/KilnFW/App/drivers/ui/
ui_page_temperature.c` lines 128-132's `_Static_assert` (`UI_PAGE_TEMPERATURE_
MAX_RELAYS_PER_ZONE == KILN_IO_RELAY_COUNT`) temporarily wrapped in a
`/* vacuity-audit: temporarily removed for negative test ... */` comment
block. **FAILS**, naming the missing assertion for that file. Restored by
hand; hash `blob:9f2f89932f7ed195a27fa0912c21e0df1498ccce` matched HEAD.
**PASSES.**

## 8. `firmware/KilnFW/App/test/check_ui_status_color.ps1`

Shells out to `ui_status_color_check.mjs` via Node; confirmed Node present so
this ran for real rather than SKIPping. `firmware/KilnFW/App/drivers/http/
main_page.html` line 26, `--ok: #1a7a1a;` temporarily changed to `#f0f0f0`
(fails the 3:1 contrast floor against the page's `--card-bg: #f7f7f7`).
**FAILS**, naming `--ok` in `main_page.html` as below the contrast floor in
(at least) one theme. Restored to `#1a7a1a`; hash
`blob:ce245f9b8b8b6e13e8753ba2b1ebb8d905cb65ab` matched HEAD. **PASSES.**

## Full-suite run

`tools/run_all_checks.ps1 -ExecutionPolicy Bypass` (foreground; the run
exceeded the tool's single-command timeout and was moved to background by the
tool infrastructure itself mid-run, not by choice — polled via direct log
reads afterward, never `Monitor`): **96 passed, 1 skipped, 1 failed.**

- The 1 skip is `check_recovery_image_size.ps1` (expected — no OTA recovery
  image built in this worktree).
- The 1 fail was, at the time the run was launched, `check_all_task_stack_
  budgets.ps1`'s pre-existing `screen_idle` 144-byte stack overage (explicitly
  named in this pass's task brief as owned by a different agent, not this
  pass's to fix). That fix landed upstream (`24d4f465`, "Raise screen_idle's
  stack ceiling to 3152 B with cited cause, not blanket-raised") and was
  fast-forward-merged into this worktree after the run completed; it is not
  re-run here since re-running the full suite a second time for a fix that
  belongs to another pass's ledger is out of scope for this audit.

No gate examined this pass was found vacuous. The one real defect found
(`check_duplicate_symbols.ps1`'s `hal_status.c`-misclassified-as-stale gap) is
in the checking tool, not production firmware, and is reported rather than
silently patched.

## Gates not examined (carried forward)

- Five of the seven UI/layout checks named in this pass's scope were not
  reached: `check_ui_shell_layout`, `check_stop_bar_body_padding`,
  `check_label_column_overflow_wrap`, `check_kv_narrow_stack`, and
  `check_ui_responsive_sweep`'s own dedicated negative test.
- `check_00_kilnfw_target_build.ps1` / `check_01_kilnfw_pushed_build.ps1` and
  `check_00_saftyfw_target_build.ps1` / `check_01_saftyfw_pushed_build.ps1`
  FAIL paths were not deliberately sabotaged this pass (only exercised at a
  now-clean baseline, incidentally, via the rebuilds gate 6 required).
- `check_duplicate_symbols.ps1`'s own `$componentSourceRoots` gap (see gate 6)
  is open and unfixed — a good first item for the next slice.
