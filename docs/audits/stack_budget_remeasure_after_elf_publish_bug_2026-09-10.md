# Stack-budget re-measurement after check_00's non-atomic ELF publish bug (2026-09-10)

## Background

`tools/check_00_kilnfw_target_build.ps1` published `build/KilnCtrl.elf` via
`Move-Item -Force`, which on PowerShell 5.1 is delete-then-rename, not atomic.
A publish around 14:35 today failed partway and left `KilnCtrl.elf.tmp_6288`
behind while the published `KilnCtrl.elf` stayed at its 15:45 version --
meaning any ELF-derived measurement taken in that window could have read a
stale ELF with no signal that it was stale. Fixed in `d141e152` (P/Invoke
`MoveFileEx` with bounded retry, loud failure on publish failure, temp-file
cleanup).

The specific claim flagged as possibly tainted: `bd77ffd1` (14:50:37, inside
the suspect window), "the plain locals measured 16 B over
`check_all_task_stack_budgets.ps1`'s ceiling for `autotune_engine`", which
motivated extracting `autotune_zone_climb_window_floor_s()` as a
`noinline` helper. `8b96b591` (14:55:17) was checked and ruled out as
irrelevant -- a sim/plant-model change with no ELF reads.

## Fresh build, verified

Built `firmware/KilnFW` via the `kilnctrl` MCP facade's `build_kilnfw` tool
(not the Bash tool -- git-bash's inherited `MSYSTEM=MINGW64` makes `idf.py`
silently no-op and leaves a stale ELF that then gets measured as current;
that failure mode is exactly what this task exists to avoid reproducing).

Freshness evidence for the ELF measured below (HEAD `df4da85b`, current at
the time, which already contains `bd77ffd1`'s fix and `d141e152`'s check_00
fix):

- `build/KilnCtrl.elf` mtime: 2026-09-10 16:34, immediately after the build
  tool call returned "OK in 79.4s".
- File size changed from the pre-build ELF (19,605,868 -> 19,667,512 bytes),
  confirming it was actually relinked, not left untouched.
- Embedded `esp_app_desc_t` build date/time inside the ELF read directly
  with a byte scan: `Sep 10 2026` / `16:33:34` -- matches the build run to
  the minute and is today, not a stale prior build.

## Numbers: autotune_engine and safety_poll, as of HEAD df4da85b

Both framings the checker prints are reported, per standing practice:

- **autotune_engine**: `total 2944 B; ceiling 2944 B; honest free 852 B
  (20.8% of 4096 B)`. The 2944 B is a **lower bound** (INDETERMINATE --
  unresolved `callx4/8/12` indirect dispatch in the call graph), not a
  measured maximum. Against the ratcheted ceiling, margin reads as zero;
  against the actual 4096 B hardware stack, honest free is 852 B (20.8%).
- **safety_poll**: `total 3104 B; ceiling 3104 B; honest free 4788 B
  (58.4% of 8192 B)`. Same caveat: 3104 B is a lower bound. Against the
  ceiling, margin reads as zero; against the 8192 B hardware stack, honest
  free is 4788 B (58.4%).

Overall re-run of `check_all_task_stack_budgets.ps1`: **0 of 28 tasks fully
measured and within budget; 28 INDETERMINATE** (every task, unresolved
indirect-call dispatch) -- unchanged in shape from the standing baseline;
no task failed on the freshly built, verified-fresh ELF.

## Does the 16 B claim reproduce?

**Yes, exactly**, verified independently rather than taken on faith from a
commit message written inside the suspect window.

Negative test performed against the PRODUCTION file (not a test-local copy):
in `firmware/KilnFW/App/drivers/control/autotune_engine.c`, temporarily
forced `AUTOTUNE_ENGINE_NOINLINE` to expand to nothing unconditionally
(defeating the `noinline` attribute that keeps
`autotune_zone_climb_window_floor_s()` in its own frame), rebuilt via
`build_kilnfw` (fresh ELF reconfirmed: mtime 16:37, size changed again),
and re-ran `check_all_task_stack_budgets.ps1`:

```
-- autotune_engine (root task_entry, declared 4096 B) --
    total 2960 B; ceiling 2944 B; honest free 836 B (20.4% of 4096 B)
    FAIL: 2960 B exceeds the 2944 B ceiling for autotune_engine.
```

Exactly 16 B over, exactly one task failing, everything else unchanged --
matching `bd77ffd1`'s commit message precisely. Restored the file by hand
(reverted the temporary `#if 1` back to `#if defined(_MSC_VER)` /
`#else __attribute__((noinline)) #endif`) and confirmed with an empty
`git diff firmware/KilnFW/App/drivers/control/autotune_engine.c`. Rebuilt
again afterward (fresh ELF archived as `KilnCtrl-eb05c6bb27d0.elf`) so the
tree's published ELF reflects the real, fixed HEAD, not the negative-test
variant.

## Conclusion

`bd77ffd1`'s 16 B claim is **not tainted** -- it reproduces byte-for-byte
against an independently, verifiably fresh ELF. The `noinline` extraction
was a real fix for a real 16-byte overage, not a fix for a phantom created
by the stale-ELF publish bug. `tools/run_all_checks.ps1 -ExecutionPolicy
Bypass`, foreground: **92 passed, 0 failed** (two more than the previously
reported 90 -- unrelated new checks landed on `main` since that count was
last quoted, not a regression).

`8b96b591` remains out of scope here as previously determined (no ELF
reads).

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
