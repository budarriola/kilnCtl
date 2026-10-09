# Check suite triage, 2026-09-14

Requested triage of the check suite after it drifted to roughly 89/94 during a
long stretch of heavy parallel agent work. Every check was verified directly
(not assumed from a previous report); the target build was force-rebuilt from
a clean worktree state before drawing any conclusion about it.

## Highest-value fix: the target build (`check_00_kilnfw_target_build.ps1`)

Two independent problems, both real, stacked on top of each other.

**1. Environmental: a dangling worktree registration.** `git worktree list`
showed `C:/wt/checkbuild` registered but the directory itself was gone from
disk ("missing but already registered worktree"), so `git worktree add`
failed before the check could even mirror source into it. This is exactly
the "pre-existing mismatch in the shared build/ directory" symptom reported
earlier, just one layer up (the worktree, not the CMake cache inside it).
Fix: `git worktree prune`, which only removes the stale registration for a
worktree whose directory no longer exists -- it does not touch any other
worktree (`C:/wt/checkbuild_origin_kilnfw`, `_origin_saftyfw`, or any of the
dozen-plus other agent-owned worktrees under `C:/wt/` were left alone). The
script's own logic then recreated `C:/wt/checkbuild` cleanly on the next run.
No `idf.py fullclean` was needed or attempted -- the check's own header
already argues carefully for keeping the persistent worktree/build dir
(measured ~138s cold vs ~27-29s incremental) and forcing `CCACHE_DISABLE=1`
instead of wiping every run; that reasoning still holds and nothing found
here contradicts it. **Do not add a routine `fullclean` here** -- the
dangling-worktree problem is fixed by `prune`, not by discarding the build
cache, and doing so would silently reintroduce the ~138s cost on every future
run for no benefit.

**2. Genuine, unclaimed regression:** once the worktree was rebuilt, the real
compiler ran and immediately failed on
`firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c:1478`:

```
error: 'snprintf' output truncated before the last format character [-Werror=format-truncation=]
```

`kiln_cfg_store_get_full_package()`'s half-package message used a 192-byte
`char msg[192]` for a fixed format string plus a name up to
`KILN_CFG_NAME_MAX_LEN` (23) characters -- GCC's format-truncation analysis
correctly determined the fixed text alone can require 193+ bytes. This is
the same failure class CLAUDE.md already documents for commit `9bc155ea`'s
`readiness_http.c` -Werror -- exactly what this check exists to catch.

`git status --porcelain` and `git log` confirm `kiln_cfg_store.c` has **no**
uncommitted edits and was last touched by `c2c9eff2` ("Add the two-processor
kiln-config apply transaction (plan item 5)"), the current HEAD -- this is
not another agent's in-flight WIP, it landed on `main` broken. Nobody had
claimed it as attributed to in-flight work, and it is small and clearly
correct to fix: widened the buffer to `char msg[256]`, matching the pattern
used for the near-identical `msg[192]` at line 1220 in the same file (left
untouched -- it was not implicated by the compiler and its own text is
shorter).

**Negative-tested:** reverted the buffer to 192 by hand, reran
`check_00_kilnfw_target_build.ps1`, confirmed it reproduces the identical
`-Werror=format-truncation=` failure, then restored the fix by hand and
reran to a clean `PASS` with a real, freshly-linked `KilnCtrl.bin`/`.elf`
(published to `firmware/KilnFW/build/`).

**Root cause, stated plainly:** the target build was broken by an
`-Werror=format-truncation` violation committed in `c2c9eff2`, compounded by
a stale worktree registration that made the check unable to even attempt a
build. Both are now fixed. `check_01_kilnfw_pushed_build.ps1` (below) reruns
this same build against the **pushed** `origin/main`, which still carries the
old, broken `kiln_cfg_store.c` until this fix is pushed -- it is the same
root cause, not a second defect, and clears once this commit is pushed.

## `tools/PcTools/selfcheck.py`

Not flapping against churn -- one concrete, genuine drift, confirmed by
running it directly under its own venv
(`tools/PcTools/.venv/Scripts/python.exe selfcheck.py`, not bare `python`,
which fails immediately with `ModuleNotFoundError: No module named
'kilnctrl'` for an unrelated environment reason and would have been a false
read of this check's real state).

Real failure: `protocol version matches uart_task_ids.h's #define: got 11,
want 12`. `firmware/KilnFW/App/drivers/common/uart_task_ids.h`'s
`UART_PROTOCOL_VERSION` was bumped 11 -> 12 by commit `17740e47`
("SaftyFW: volatile config install (KILN_PROFILES_PLAN.md item 15)") --
per that commit's own header comment, purely for enumeration
(`SAFETY_CMD_APPLY_CONFIG_VOLATILE`, which lives entirely on the isolated
ESP<->Pico link) with **no actual PC<->ESP wire change**. The mirrored
Python constant (`tools/PcTools/src/kilnctrl/protocol.py`'s
`UART_PROTOCOL_VERSION`) was left at 11, so the cross-language pin
`selfcheck.py` exists specifically to enforce (see that file's own comment
about the constant sitting stale at "== 5" once before) caught exactly the
drift it is designed to catch. This is unrelated to the "two protocol
versions" note in project memory (UART vs KILNLINK are legitimately
different numbers) -- this is the UART number itself drifting between its
two copies.

Fix: bumped `protocol.py`'s `UART_PROTOCOL_VERSION` to 12 with a matching
changelog comment. **Negative-tested**: set it back to 11, reran
`selfcheck.py`, confirmed the identical `FAILED (1)` reappears, restored to
12, reran to a full green (`exit=0`, no `FAILED` lines).

Also confirms `check_saftyfw_task_stack_budgets.ps1` (reported red earlier)
is currently green against a fresh `SaftyFW.elf` -- rerun directly, exit 0,
9 tasks graded, none over budget. No code change was needed there; whatever
made it red earlier was very likely a stale ELF from before some other
agent's rebuild, and it reads clean now.

## `build_host_tests.ps1`'s `$totalExpected`

Checked directly: `$totalExpected` in
`firmware/KilnFW/App/test/build_host_tests.ps1` is **already 45**, not the
stale 44 described in the task brief -- someone had already corrected it.
Running the full host-test build now reports `Built: 43/45 executables`,
with 2 build failures (`safety_cfg_http`, `safety_link`) that are real and
current, not a stale-count artifact -- see below. No change was needed to
`$totalExpected` itself.

## Other-agent-owned, in-flight -- verified, not fixed

Four of the six remaining `run_all_checks.ps1` failures, plus the two host-test
build failures above, all trace to the same two files explicitly named as
another session's in-flight work in this task's brief:
`kiln_cfg_swap.{c,h}` and `safety_cfg_http.c` (both show as `M` in
`git status --porcelain`, mid-edit, not committed):

- **`check_flash_worker_lint.ps1`** -- flags direct
  `hal_kv_set_blob`/`hal_kv_commit` calls in
  `kiln_cfg_swap.c:119,121` outside the sanctioned flash-worker patterns.
- **`firmware\SaftyFW\tools\check_link_impl_isolation.ps1`** -- flags a CRC
  implementation (`pending_crc()`) in `kiln_cfg_swap.c:47` and one in
  `test_kiln_cfg_swap.c:344` outside `firmware/CommonFW`.
- **`tools\check_c_files_in_cmakelists.ps1`** -- flags
  `kiln_cfg_swap.c` as not referenced by
  `firmware/KilnFW/App/drivers/CMakeLists.txt` at all. This one is worth
  flagging loudly to the owning agent: it means `kiln_cfg_swap.c` is
  **not currently linked into the ESP target build**, which is also why the
  target-build fix above did not have to contend with anything in that file.
- **Host-test build failures `safety_cfg_http`/`safety_link`** -- unresolved
  external `safety_link_send_apply_config_volatile`, called from
  `safety_cfg_http.c`'s `apply_pairs_ex()`. The real function lives in
  `safety_link_commands.c` and a matching-signature test fake already exists
  in `test_safety_cfg_http.c` (added for this same feature, per its own
  2026-09-14 comment) -- the failure is consistent with those files being
  mid-edit right now, not a design gap.

All four are read-only findings for the owning agent's benefit -- per this
task's explicit instructions, `kiln_cfg_swap.{c,h}`/`safety_cfg_http.c` were
not touched. They are very likely to clear on their own once that in-flight
work reaches a checkpoint that (a) wires `kiln_cfg_swap.c` into
`CMakeLists.txt`, (b) routes its NVS writes through the flash worker, and
(c) moves its CRC/byte-stuffing logic into `CommonFW` per `TODO.md` Phase 1.

The remaining two failures trace to a second in-flight area named in this
task's brief, the coredump HTTP reader:

- **`tools\check_mcp_facade_coverage.ps1`** -- a newly-registered `kilnctrl`
  tool, `read_esp_coredump`, has no taxonomy entry in `mcp_facade.py`.
- **`tools\check_mcp_tool_count_doc.ps1`** -- CLAUDE.md's stated tool count
  (154) is now one behind the live registered count (155), for the same
  reason.

Both are the direct, expected consequence of that tool having just landed;
neither was touched, and both should clear once the owning agent adds the
facade taxonomy entry and bumps the CLAUDE.md count for the new tool.

## `check_01_kilnfw_pushed_build.ps1`

Builds `origin/main` (currently `8d685bfe`) fresh in its own worktree. Fails
with the exact same `kiln_cfg_store.c:1478` format-truncation error as
`check_00` did before the fix above -- because the fix is not pushed yet.
Not a second defect; will go green once this commit reaches `origin/main`.

## Full tally

Ran `tools\run_all_checks.ps1` end-to-end after the fixes above:

**88 passed, 0 skipped, 6 failed** (94 discovered, matching the ~89/94 starting
point plus the 3 checks this session fixed: `check_00_kilnfw_target_build.ps1`,
`tools/PcTools/selfcheck.py`, and `check_saftyfw_task_stack_budgets.ps1`,
against 6 remaining):

| Check | Classification | Fixed here? |
|---|---|---|
| `check_00_kilnfw_target_build.ps1` | Environmental (stale worktree) + genuine regression (`c2c9eff2`) | **Yes** |
| `tools/PcTools/selfcheck.py` | Genuine regression (`17740e47`, unclaimed) | **Yes** |
| `check_saftyfw_task_stack_budgets.ps1` | Stale artifact (cleared on its own once ELF was fresh) | No fix needed |
| `check_01_kilnfw_pushed_build.ps1` | Same root cause as `check_00`, will clear on push | No (self-clears) |
| `check_flash_worker_lint.ps1` | Other-agent in-flight (`kiln_cfg_swap.c`) | No (not owned) |
| `check_link_impl_isolation.ps1` | Other-agent in-flight (`kiln_cfg_swap.c`) | No (not owned) |
| `check_c_files_in_cmakelists.ps1` | Other-agent in-flight (`kiln_cfg_swap.c` not in CMakeLists) | No (not owned) |
| `check_mcp_facade_coverage.ps1` | Other-agent in-flight (coredump reader) | No (not owned) |
| `check_mcp_tool_count_doc.ps1` | Other-agent in-flight (coredump reader) | No (not owned) |

No check was weakened, skipped, or made to pass by loosening its assertion.
Both fixes made here were negative-tested (reverted by hand, reconfirmed red,
restored by hand, reconfirmed green) before being counted as done.
