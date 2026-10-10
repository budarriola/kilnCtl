# Review: r2ace round 2, toolfx4, buildfix (2026-10-10)

Reviewer: Opus, read-only review at origin/dev `d93cf774a`.

Scope:

- r2ace round 2 (host-test gap fill): `51ab1ea36`, `68548bef8`, `a3950b140`,
  `2e515d27e`, `0131bb43a`, `1d5c5d683` (docs).
- toolfx4: `ae7124b01`, `8eba89bd2`.
- buildfix: `d93cf774a` (bx_flash_worker stack ceiling 6240 -> 6320).

The question was whether each test pins real behavior or is vacuous. Mutation
runs (`tools/negtest.ps1`, throwaway worktree, unmutated baseline required to
pass) are listed at the end.

No HIGH or MEDIUM findings. Every new test drives the real code path, not a
re-implementation. The findings are weak or tautological assertions, a few
uncovered branches, one stale comment, and one buffer truncation that a test
now pins as if it were intended.

## Findings

### LOW

- **L1** `firmware/KilnFW/App/drivers/persist/cfg_fs_mount.c:78`,
  `firmware/KilnFW/App/test/test_cfg_fs_mount_state.c:467`.
  `s_format_pending_reason` is 96 bytes. The gate text from
  `cfg_fs_format_gate_describe()` is longer than that, so the operator-facing
  reason in `GET /api/cfgfs/format_pending` and the boot banner is cut
  mid-word ("...(corrupt filesys"). The test asserts `strlen == 95`, which
  pins the truncation as expected behavior. Fix: enlarge the buffer (e.g. 160)
  and assert the full gate text.
- **L2** `firmware/SaftyFW/test/test_link_task_fuzz.c:1089`. Volatile scenario
  4 asserts `(g_tc_reapply == 1) != (pending && value == 6)`, which is an XOR
  of "immediate reapply" and "armed retry". The real volatile handler
  (`link_task.c:2903-2905`) has only one outcome on a tc_type change: it calls
  `thermo_task_request_tc_type_reapply()` under the policy gate, never the
  pending/retry path. So the assertion accepts a regression in which the
  handler arms the commit-path retry instead. Fix: assert
  `g_tc_reapply == 1 && !s_tc_type_reapply_pending`.
- **L3** `firmware/SaftyFW/test/test_link_task_fuzz.c` (commit scenarios). The
  only commit tc_type-change case it covers is the "heat not provably safe" one
  (`g_wex_heat_safe == 0`, line 1019), which takes the deferred/pending branch.
  The heat-safe immediate-reapply branch of the commit handler (`link_task.c`
  ~2750-2755, gated by `link_task_heat_is_safe_for_tc_type_change()`) is never
  driven.
- **L4** `firmware/KilnFW/App/test/test_dashboard_http_relay.c:222,227`. The
  assertions "SAFETY: sources passed through" and "OWNED leaves sources
  untouched" are tautological. `dashboard_set_relay()` (`dashboard_http.c:733`)
  only forwards the `out_safety_sources` pointer, and the test's own fake
  decides whether to write it, so these two checks test the fake. They are
  harmless but add no coverage. The nine-result mapping table and the
  unknown-result default to IO_FAIL are real coverage, and the mutation run
  confirms it.
- **L5** `firmware/KilnFW/App/test/test_kiln_io_owner_sx_dispatch.c`. The
  stubbed gates always return false, so the ERR_UPDATING, ERR_CRASH_UNACK and
  ERR_RUNNING refusal branches of `handle_set_relay`/`handle_set_relay_mask`
  (`kiln_io_owner.c` ~380-441) never run through the real owner_task dispatch.
  The four command kinds it does drive (SET_RELAY, SET_RELAY_MASK,
  SET_RELAY_MASK_AUTHORIZED, SX_RESET) are pinned well.
- **L6 (FIXED in 44ecaf482: tracked-only via git ls-files; real-tree guard message documents that any write counts)** `tools/negtest.ps1:655` (`ae7124b01`). Bare-name resolution for
  `-Preset check` runs a recursive `Get-ChildItem` over the whole real repo
  root, so untracked files count. In the shared main tree,
  `logs/wt_archive_2026-10-09/` and `logs/wt_archive_2026-10-10/` hold copies of
  `check_*.ps1` (e.g. `check_00_kilnfw_host_tests.ps1`,
  `check_embedded_pico_image_fresh.ps1`). Their bare names therefore resolve as
  "ambiguous" and fail with exit 2. This fails closed, so it is safe, but it is
  surprising. Fix: resolve against `git ls-files` (tracked files only).
- **L7** `firmware/KilnFW/App/test/lint_pages.js:191` (`8eba89bd2`). The
  U+00C3 continuation set covers U+0080-U+00BF plus the cp1252-mapped
  characters, but leaves out U+20AC. cp1252 0x80 is "€", so a double-encoded
  "À" ("Ã€") is not flagged. Separately, U+00C2 is only paired with
  U+00A0-U+00BF, so a C2 followed by a cp1252-mapped continuation is also
  missed. Both are narrow, but they are exactly the case the extension meant
  to cover. Fix: add `€` to the C3 class and mirror the cp1252 set for
  C2.

### INFO

- **I1** Author's open question: "`cfg_fs_mount_device()` returns the original
  mount error after a deferred format is started". This is **not a defect**.
  `maybe_auto_format_and_remount()` (`cfg_fs_mount.c` ~354-450) documents that
  it does this on purpose: cfg_fs really is unavailable when it returns, and
  progress is observed through `GET /api/cfgfs` "format". Callers that treat
  non-OK as "cfg_fs not mounted" are correct.
- **I2** `cfg_fs_mount.c:482-484`. The comment "Auto-formatted and freshly
  registered -- fall through" is stale, and that branch is dead.
  `maybe_auto_format_and_remount()` never returns ESP_OK now, so
  `finish_mount_after_register()` is reached only after a first-try register
  succeeds. Remove the comment and branch, or reword them.
- **I3** `main_boot_early.c:447-455` logs `cfg_fs_mount_device(): ESP_ERR_x`
  even when a deferred format has been scheduled. Someone reading the boot log
  may take a self-healing format for a hard failure. A one-line "deferred
  format scheduled" log when `cfg_fs_mount_format_in_progress()` (or the
  equivalent) is true would remove the ambiguity.
- **I4** `firmware/KilnFW/App/test/build_host_tests.ps1:3526`. The count
  comment "93 -> 94: added test_kiln_io_owner_sx_dispatch.c" names the wrong
  file. The 94th entry added in `2e515d27e` is `test_dashboard_http_relay.c`.
  The commit message's "exe 87" also does not match. Cosmetic.
- **I5** `tools/run_all_checks.ps1:200` uses the same recursive glob, so a run
  from the shared main tree would also discover the archived `check_*.ps1`
  copies under `logs/`. Checks normally run from a minted worktree, which has
  no `logs/wt_archive_*`, so this is latent.
- **I6** `firmware/KilnFW/App/test/check_all_task_stack_budgets.py:1034`
  (`d93cf774a`). Raising the bx_flash_worker ceiling to 6320 B against a
  measured 6272 B on a 10240 B stack (35.8% free) is safe. Two caveats:
  - The new ceiling adds 48 B of headroom beyond the measurement, while earlier
    entries' comments follow a "ceiling == measurement" convention.
  - The commit does not name which frame grew by 32 B.

  The static walk for this task is INDETERMINATE (unresolved indirect calls),
  so a bench high-water mark is still the real confirmation.
- **I7** `check_all_task_stack_budgets.py:1095`. The profile_exec_wdt ceiling
  is 2752 B, exactly the measured value. This is intended as a tripwire: the
  declared stack is 6144 B (`profile_executor_start.c:206/217`), so about
  3392 B is really free. Any future growth of 16 B or more trips the check even
  though the stack has plenty of room. Expect a ceiling bump when that
  happens; it does not signal real stack pressure.
- `68548bef8` (SaftyFW image identity record test) has no findings. It asserts
  exact field values and byte offsets, a scanner round trip, corruption
  rejection and record-version-3 rejection, and it is registered through
  `Add-HostBuild`.
- `ae7124b01` makes `check_lint_pages.ps1` run the mojibake fixture even when
  lint passes, and adds C2/C3 positive cases and a negative case that uses
  legitimate characters. It also adds negtest name-resolution fixtures to
  `check_negtest.ps1`. No findings beyond L6.

## Mutation runs

Both runs used `tools/negtest.ps1` at `d93cf774a` with a passing unmutated
baseline. The KilnFW run used `build_host_tests.ps1 -Only` for the three new
executables and `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
The SaftyFW run used preset `saftyfw-host`.

| Mutation | File | Verdict | Assertion that fired |
|---|---|---|---|
| drop `relay_off_tracker_note_write(0xFF,0)` in SX_RESET | kiln_io_owner.c:566 | CAUGHT | test_kiln_io_owner_sx_dispatch.c:182, :198 |
| return ERR_SAFETY for owner ERR_UPDATING | dashboard_http.c | CAUGHT | test_dashboard_http_relay.c lines 201, 210 |
| drop pending flag on unsafe gate verdict | cfg_fs_mount.c:432 | CAUGHT | test_cfg_fs_mount_state.c:449, :461, :476 |
| drop pending flag on scan read error | cfg_fs_mount.c:419 | CAUGHT | test_cfg_fs_mount_state.c:424, :435 |
| drop `link_staging_reset` after a commit | link_task.c:2731 | CAUGHT | link_task_fuzz_tests |
| drop the volatile tc_type reapply | link_task.c:2903 | CAUGHT | test_link_task_fuzz.c line 1090 (the XOR) |
| invert `.dirty` normalisation | saftyfw_image_identity_record.c:25 | CAUGHT | saftyfw_image_identity_record_tests |

7 of 7 were caught. The XOR in L2 caught the "no reapply at all" mutation
because both of its sides were false. It would still pass a mutation that arms
the pending retry instead of reapplying, which is the gap L2 describes.

Both runs reported `real_tree_unchanged: false`. The cause was this audit
document, which was created as an untracked file in the reviewer worktree
while the runs were going. No source file changed. negtest reported no change in any mutated file's hash,
in either tree.
