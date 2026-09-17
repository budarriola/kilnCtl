# Release-gate vacuity audit, 2026-09-17

Continues the series started `docs/audits/release_gate_vacuity_audit_2026-09-16.md`,
blocker 3 of `docs/RELEASE_HARDENING_PLAN.md`. This pass closes the one item
explicitly carried forward, unexercised, across `...16e.md`, `...16f.md`, and
`...16g.md`:

> `check_00_kilnfw_target_build.ps1` / `check_01_kilnfw_pushed_build.ps1` and
> `check_00_saftyfw_target_build.ps1` / `check_01_saftyfw_pushed_build.ps1`
> FAIL paths were not deliberately sabotaged this pass (only exercised at a
> now-clean baseline, incidentally, via the rebuilds gate 6 required).

This pass exercises `firmware/SaftyFW/test/check_00_saftyfw_target_build.ps1`'s
FAIL path specifically (the KilnFW-side pair remains open for a follow-on
pass).

Work was done in a dedicated worktree, `C:\wt\wt_blocker6_1789671609`, checked
out at `origin/main` `12d3093fa3e2e3526ba35e241de977b69acfb75b`, submodules
initialized, `tools/PcTools` uv-synced, arm-none-eabi-gcc 14.2 rel1 and
`C:\pico-tools\pico-sdk` already present on this machine.

## `firmware/SaftyFW/test/check_00_saftyfw_target_build.ps1`

**Claim:** the check reconfigures and builds SaftyFW for the real RP2040
target (cmake + ninja, arm-none-eabi-gcc), fails loud (exit 1) on any
reconfigure/build failure or a missing `SaftyFW.elf` after a reported
success, and exits 0 only once a real `SaftyFW.elf` exists. Never
independently sabotage-tested before this pass — only ever seen at a clean
baseline (PASS) incidentally, via other gates' forced rebuilds.

**Baseline:** ran the check against the clean worktree. PASS — full 477-step
ninja build, `SaftyFW.elf`/`SaftyFW_slotA.elf`/`SaftyFW_slotB.elf` produced,
exit 0.

**Negative test:** sabotaged real production code,
`firmware/SaftyFW/src/tasks/link_frame.c` (pure byte-packing module, no
RTOS/SDK dependency, compiled into every SaftyFW image), inserting a garbage
top-level token right after the includes:

```c
#include "link_frame.h"

#include <math.h>
#include <string.h>

vacuity_audit_saftyfw_target_build_negative_test garbage syntax token;
```

Ran the check: **FAILED**, exit 1. Exact compiler errors:

```
C:/wt/wt_blocker6_1789671609/firmware/SaftyFW/src/tasks/link_frame.c:8:1: error: unknown type name 'vacuity_audit_saftyfw_target_build_negative_test'
C:/wt/wt_blocker6_1789671609/firmware/SaftyFW/src/tasks/link_frame.c:8:58: error: expected '=', ',', ';', 'asm' or '__attribute__' before 'syntax'
```

`ninja` returned non-zero; the check's own `Fail "ninja build failed (exit
$LASTEXITCODE) -- see output above."` path fired and the script exited 1 —
confirming the FAIL contract (non-zero on a genuine build failure, not a
silent PASS or a SKIP).

**Restore:** removed the inserted line by hand (never `git checkout --`).
`git diff --stat firmware/SaftyFW/src/tasks/link_frame.c` — empty.
`git hash-object firmware/SaftyFW/src/tasks/link_frame.c` —
blob:firmware/SaftyFW/src/tasks/link_frame.c`c7aefbdf2c4a9349ae338f746d3d29f21605cddb`,
matching the pre-sabotage hash taken before the edit.

**Forced full rebuild:** deleted `firmware/SaftyFW/build` entirely (not a
reconfigure-in-place) and re-ran the check from scratch. **PASS**, exit 0,
477/477 steps, `SaftyFW.elf` produced — matching the original baseline.

**Verdict: load-bearing.** The check's FAIL path is real: it depends on
`ninja`'s actual exit code and the compiler's actual diagnostics, not on any
sentinel or mirrored copy, and correctly refuses to report PASS when the
real production source fails to compile.

## Gates not examined (carried forward)

- `check_00_kilnfw_target_build.ps1` / `check_01_kilnfw_pushed_build.ps1`
  (KilnFW side of the same pair) — FAIL path still not deliberately
  sabotaged as of this pass.
- `check_01_saftyfw_pushed_build.ps1` — FAIL path still not deliberately
  sabotaged as of this pass.
- The remaining UI/layout checks (`check_ui_responsive_sweep` and siblings)
  are out of scope for a non-LCD session per this task's exclusion rules.

## Summary

One gate closed this pass: `check_00_saftyfw_target_build.ps1`'s FAIL path,
found load-bearing via a real compile-error negative test against
production code (`link_frame.c`), hand-restored, verified via empty diff
plus matching blob hash, and re-confirmed PASS only after a forced full
clean rebuild into a fresh `build/` directory.
