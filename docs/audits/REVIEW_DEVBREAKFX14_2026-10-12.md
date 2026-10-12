# Review: DEVBREAKFX14 (eb10d0caa, 4d2a3eef4, 243f90f00, 859bf2a9a) -- 2026-10-12

This is an adversarial review of the four DEVBREAKFX14 commits on origin/dev, ending at tip `859bf2a9a`:

- `eb10d0caa`: the `check_host_test_verdict.ps1` dot-source regex now allows any whitespace (`\s+`).
- `4d2a3eef4`: raises eight stack ceilings by 16 B. It also changes line 171 of `check_host_test_verdict.ps1`
  from `Join-Path` to `[IO.Path]::Combine`.
- `243f90f00`: rewords the LCDFX13 review so it no longer cites a checkbuild directory name as a hash.
- `859bf2a9a`: does the same for the DOCFX5 review.

I reviewed it in worktree `C:\wt\rvdevbreakfx14_jwok98`, detached at `859bf2a9a`. I built the two target images
for the root-cause check in a separate worktree, `C:\wt\rvfx14stk_n2ap3x`, through the build gate.

**Counts: HIGH 0, MED 0, LOW 3, INFO 4.**

## Root cause of the +16 B (Focus 1)

**Verdict: confirmed.** `44289aa41` (the rx_ms stamp) caused the growth, and nothing else did. The commit message
of `4d2a3eef4` names the wrong frame, though (see LOW-1).

Method:

1. I built `check_00_kilnfw_target_build.ps1` at `01554a779`, which is `44289aa41~1`. The build passed, and
   `KilnCtrl.bin` was 2953552 B.
2. I built incrementally at `44289aa41` in the same worktree. The build passed, and `KilnCtrl.bin` was 2953680 B.
3. I graded both ELFs with tip's `check_all_task_stack_budgets.ps1 -ElfPath`, with each ELF's own sdkconfig
   beside it.

Results:

- The parent ELF measures exactly the old ceilings: 3312, 3152, 3168, 2896, 3888, 7712, 6336 and 3360.
- The `44289aa41` ELF measures exactly the new ceilings: 3328, 3168, 3184, 2912, 3904, 7728, 6352 and 3376. These
  match what the fixer measured at tip.

Only five per-frame sizes differ between the two walks:

| Frame | Parent | 44289aa41 | Task(s) whose deepest path it is on |
|---|---|---|---|
| `safety_link_get_param` | 608 | 624 | safety_uart_bridge |
| `safety_link_send_rollback_ex` | 704 | 720 | ota_pico_rollback |
| `safety_link_get_config_page` | 1024 | 1040 | zone_sweep, safety_poll |
| `thermo_bridge_task` | 592 | 608 | thermo_uart_bridge |
| `touch_bridge_task` | 608 | 624 | touch_uart_bridge |

The http_async_job and bx_flash_worker totals move only through their `extra_roots` callback figures:
`backup_import_job` goes from 5712 to 5728, and `profiles_handle_message` from 5600 to 5616. The checker does not
print those callback frames.

Every grown frame holds a `uart_proto_message_t` on the stack:

- the bridge tasks hold `msg`;
- the `safety_link_commands.c` functions hold `stale`, `reply` and similar locals.

`44289aa41` appends `uint32_t rx_ms` to that struct, after `payload[UART_PROTO_MAX_PAYLOAD]`. The extra 4 B rounds up
to 16 B because Xtensa windowed-ABI frames are 16-byte aligned.

`frame_and_send$constprop$0` (560 B), `uart_protocol_send_broadcast` (320 B) and `hal_uart_send_blocking` (64 B)
are byte-identical between the two walks. danger_mode's ceiling did not need to move, even though its path runs
through `send_broadcast`/`frame_and_send`; that agrees with those frames being unchanged.

The +16 B is justified. It is the real, measured cost of one new 4-byte field that the reboot detector needs. The
eight new ceilings are exactly the measured walks, with no headroom padded on.

## Margin against the allocated stack

Honest free is the declared stack, minus the measured walk, minus the 300 B unmodeled overhead. These figures come
from the `44289aa41` ELF:

| Task | Declared | Walk | Honest free |
|---|---|---|---|
| safety_uart_bridge | 4096 | 3328 | 468 B (11.4%) |
| touch_uart_bridge | 4096 | 3184 | 612 B (14.9%) |
| thermo_uart_bridge | 4096 | 3168 | 628 B (15.3%) |
| zone_sweep | 5120 | 3904 | 916 B (17.9%) |
| ota_pico_rollback | 4096 | 2912 | 884 B (21.6%) |
| http_async_job | 10240 | 7728 | 2212 B (21.6%) |
| bx_flash_worker | 10240 | 6352 | 3588 B (35.0%) |
| safety_poll | 8192 | 3376 | 4516 B (55.1%) |

All eight tasks keep at least 10%. The tightest is safety_uart_bridge, at 11.4%. Every figure is INDETERMINATE (see
INFO-1), so each is a lower bound on use. LOW-3 covers whether any allocated stack should grow.

## Findings

### LOW-1: the `4d2a3eef4` commit message names the wrong frame

The message says "a shared uart send-path frame grew" and "every failing path runs through
frame_and_send/uart_protocol_send_broadcast, +16 B each". That is not what happened.

- The send-path frames did not change at all.
- The growth is in each *caller* frame that holds a `uart_proto_message_t` local (table above).
- The paths do pass through `frame_and_send`, but that is coincidence, not cause.

The attribution to `44289aa41` is right; the mechanism is wrong. This has no code effect. The risk is that a future
reader chasing stack in the send path looks in the wrong place. Commit messages cannot be amended on dev, so this
review is the correction of record.

### LOW-2: stale measurement comments left next to two raised ceilings

- **`bx_flash_worker` (6352):** the comment still says "re-measured 6336 B ... honest free 3604 B (35.2%)".
  Measured now: 6352 B, honest free 3588 B (35.0%).
- **`http_async_job` (7728):** the comment still says "honest free 2228 B (21.8%)" and "chain 5632 -> 5712 B".
  Measured now: 7728 B, `backup_import_job` 5728 B, honest free 2212 B (21.6%).

The other six raised rows carry no measurement comment of their own, so they did not go stale. Fix: update the two
comments to the 2026-10-12 figures and name `44289aa41` (`uart_proto_message_t.rx_ms`) as the cause.

### LOW-3: safety_uart_bridge is the one task worth a bigger allocated stack

At 468 B (11.4%) honest free, safety_uart_bridge has the thinnest margin of the 35 graded tasks. It clears the 10%
bar by about 58 B. That is less than four more 16 B frame bumps of the kind this commit absorbed, and its
`safety_link_get_param` frame already carries a ~600 B message struct. The walk is also only a lower bound
(INFO-1).

Recommendation (owner standing permission: "stack bumps allowed without asking"): raise its declared stack from
4096 to 4608 B. Do this in the same change as the next edit that touches the bridge, or on its own, and re-measure
then. touch_uart_bridge and thermo_uart_bridge (about 15%) can wait. The other five raised tasks have at least 17.9%
and need nothing. Raising only the ceiling was the correct move for this commit, since the measured growth is
understood.

### INFO-1: every task walk is INDETERMINATE

All 35 tasks report an unresolved `callx` in their call graph. So "11.4%" and the other figures are an upper bound on
free stack, not a measurement. The 300 B overhead term is the only cushion against an unfollowed indirect call.
This is pre-existing and not introduced by these commits. A hardware high-water-mark read (`get_stack_margin`) is
the real check, and it is outside this review (no board access).

### INFO-2: the `\s+` loosening in `eb10d0caa` is harmless

The regex still anchors with `^...$`, still names the exact relative path, and still requires a single
dot-source per file. Only spacing became flexible. Negtests V1-V6 below show every meaningful mutation of the call
site is still caught.

### INFO-3: `[IO.Path]::Combine` in `4d2a3eef4` dodges the source-path drift check, safely

`check_source_path_drift.ps1` flags `Join-Path $PSScriptRoot "<literal>.ps1"` patterns. Line 171 builds
`"$ln.ps1"` from a loop variable, which the drift check cannot resolve, so the `Combine` form avoids a false
positive. `$PSScriptRoot` is absolute, so `Combine` cannot silently drop the base the way it would with a rooted
second argument. Negtest D2 confirms the old form was really flagged, so the change was needed and not cosmetic.

### INFO-4: the citation rewordings keep the meaning, with one wording nit

- **DOCFX5:86** now reads "cites a checkbuild directory name that looks like a hash". The meaning is intact.
- **LCDFX13:108** now reads "A survey of the checkbuild worktree above found 110 alias groups". The table above
  lists two checkbuild directories (`C:\wt\checkbuild_30301fbe48` and `C:\wt\checkbuild_280d180149`), and the original meant the first. Writing the
  full path in backticks (`C:\wt\checkbuild_<hex>`) would have kept the exact referent: the checker only flags
  tokens that are entirely hex. This is a nit, not worth a separate commit.

`check_doc_hash_citations` still catches a real bad hash (negtests H1-H3).

## Negtests

All runs used `tools\negtest.ps1`. Every baseline passed. No run printed "REAL TREE CHANGED", and every copy was
removed. Each CAUGHT below comes from a mutant that ran to completion and hit the named failing assertion. None is
a parse or launch failure.

| ID | Check | Mutation | Verdict | Failing line in log |
|---|---|---|---|---|
| V1 | check_host_test_verdict (-Preset check) | dot-source path depth `../../../` | CAUGHT | `FAIL: build_host_tests.ps1 dot-sources host_test_verdict.ps1 exactly once` |
| V2 | same | dot-source line commented out | CAUGHT | same |
| V3 | same | dot-source line duplicated | CAUGHT | same |
| V4 | same | `.` replaced by `&` call operator | CAUGHT | same |
| V5 | same | target renamed `host_test_verdict_old.ps1` | CAUGHT | same |
| V6 | same | trailing `; $null = 1` | CAUGHT | same |
| H1 | check_doc_hash_citations (-Command, see below) | restore the original checkbuild-dir citation in LCDFX13:108 | CAUGHT | `FAIL: 1 cited hash(es) do not resolve to a commit` (names hash and line) |
| H2 | same | restore the original citation in DOCFX5:86 | CAUGHT | same |
| H3 | same | nonexistent hash (tip SHA with last digit changed) | CAUGHT | same |
| D1 | check_source_path_drift (-Preset check) | revert line 31 regex to single spaces | CAUGHT | `SOURCE PATH DRIFT CHECK: FAILED` at check_host_test_verdict.ps1:31 |
| D2 | same | revert line 171 to `Join-Path $PSScriptRoot "$ln.ps1"` | CAUGHT | `SOURCE PATH DRIFT CHECK: FAILED` at :171 |

These runs answer Focus 2. The mutated call site sits in `firmware/KilnFW/App/test/build_host_tests.ps1:46`, and
every mutation of it fails `check_host_test_verdict`.

**Deviation for the doc-hash runs:** `-Preset check` against `tools/check_doc_hash_citations.ps1` fails its
baseline in a negtest copy with exit 3. The copy has no lvgl or mykicadMcp submodule history, so
`sub:lvgl`/`sub:mykicadMcp` citations are ungraded. H1-H3 therefore ran with
`-Command 'powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_doc_hash_citations.ps1 -AllowUngradedSubmodules'`.
The baseline then passed, and each mutant failed on the planted hash alone.

## Build evidence

| Build | Commit | Result | KilnCtrl.bin |
|---|---|---|---|
| parent | `01554a779` (= `44289aa41~1`) | PASS | 2953552 B |
| fix | `44289aa41` | PASS (incremental, same worktree) | 2953680 B |

Both builds ran through `check_00_kilnfw_target_build.ps1` (heavy build-gate slot) in `C:\wt\rvfx14stk_n2ap3x`.
The task explicitly authorized these two builds. No other target build or `run_all_checks.ps1` run was made.

## Fix status

- LOW-1: commit-message error only; history not rewritten, correction recorded above.
- LOW-2: fixed in a28c7154d (comments now 6352 B / 35.0% and 7728 B / 21.6%).
- LOW-3: fixed in a28c7154d (safety_uart_bridge 4096 -> 4608 B; check_all_task_stack_budgets reads declared 4608 from source and is OK on a fresh full target build). No negtest: a stack-size bump has no meaningful mutation.
- INFO-4: fixed in a28c7154d (LCDFX13 names the first-row checkbuild directory).
