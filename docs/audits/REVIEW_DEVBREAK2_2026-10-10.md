# Review: devbreak2 dev-check fixes (2026-10-10)

Range reviewed: `d3187e59d..f65950541` on origin/dev (six commits):

| SHA | Subject |
|---|---|
| 93693b8da (pre-rebase 3c0c365cb) | Fix dev checks: build-gate test exemption, source-path drift, link-impl allowlist, reset fence predicate rename, checked relay-off result |
| 10ddf02f4 | Doc citations: refresh blob hashes to landed blobs |
| 1152994ab | Gate negative-test table: rows for touch_cal exit-target caller and submodule-pins test |
| 9334c65cc | selfcheck: zones POST-only control field expected_generation declared in the PC client |
| a695ff6df | Stack ceilings: re-measured ... |
| f65950541 | Doc citations: relay_io blob hash at current dev |

Method: worktree `C:\wt\rvdevbreak2_y9hpmk` at f65950541 (`-NoSubmodules`).
Every touched check was run there; suspected gaps were negative-tested with
`tools\negtest.ps1 -Preset check -Mutations <json>` (every baseline PASS).
`selfcheck` was run with `uv` in the worktree. No code was changed.

Summary: no HIGH. One MED (stack ceilings measured on a pre-rebase ELF, not
the landed tip). Five LOW. Several INFO.

## Checks run in the worktree

| Check | Result |
|---|---|
| check_build_gate_usage.ps1 | PASS |
| check_build_gate_reentrant.ps1 | PASS |
| check_link_impl_isolation.ps1 | PASS |
| check_reset_fence_hooks.ps1 | PASS |
| check_safety_call_results_checked.ps1 | PASS (49 sites, 1 allowlist) |
| check_gate_negative_test_table.ps1 | PASS (198 checks, 199 rows) |
| check_doc_citations.ps1 | PASS |
| check_source_path_drift.ps1 | PASS |
| check_zones_per_zone_field_drift.ps1 | PASS |
| check_doc_hash_citations.ps1 | exit 3: SKIP for 2 submodule citations only (worktree minted `-NoSubmodules`); every graded citation resolves |
| `uv run python selfcheck.py` (tools/PcTools) | "all checks passed"; zones top-level GET and POST both PASS |
| `pytest tests/test_selfcheck_zones_fields.py` | 10 passed |

## Negative tests

| Check | Mutation | Result |
|---|---|---|
| touch_cal_exit_target_caller | second `lcd_touch_cal_saved_exit_target` call in ui_page_touch_cal.c | CAUGHT |
| touch_cal_exit_target_caller | `if ($allowedCalls -ne 1)` -> `if ($false)` plus second call | MISSED |
| touch_cal_exit_target_caller | `if ($allowedCalls -ne 1)` -> `if ($false)` plus zero calls | MISSED |
| build_gate_usage | drop the `$deliberateMisuse` exemption | CAUGHT |
| build_gate_usage | misuse in a new `firmware/KilnFW/tools/check_build_gate_reentrant.ps1` | MISSED |
| build_gate_usage | control: same misuse in `firmware/KilnFW/tools/gate_probe_misuse.ps1` | CAUGHT |
| link_impl_isolation | drop the test_cfg_fs_mount_state.c allowlist entry | CAUGHT |
| link_impl_isolation | add a CRC16-CCITT routine to the allowlisted test file | MISSED |
| reset_fence_hooks | main.c predicate calls `relay_authority_reset_in_flight()` instead | CAUGHT |
| reset_fence_hooks | main.c predicate `... refuses_kiln_nvs_writer() && 0` | MISSED |
| reset_fence_hooks | relay_authority.c predicate body `bool refuse = 0 && ...` | MISSED |
| safety_call_results_checked | revert off_err capture to the inline `== ESP_OK` | CAUGHT |
| safety_call_results_checked | `(void)off_err; if (1) {` | MISSED |

## MED-1: stack ceilings were measured on a pre-rebase ELF, never on the landed tip

a695ff6df raises httpd (autotune_start_post_handler) 4448 -> 4528,
link_watchdog 1168 -> 1200, kiln_io_owner 2160 -> 2208, bx_flash_worker
6320 -> 6336, system bridge 3152 -> 3216. The commit text says "re-measured",
and the numbers do match `--dump-ceilings` on
`C:\wt\checkbuild_ea33868cb6`'s ELF, but that ELF was built from the
pre-rebase e631901ba (its kiln_io.c and kilnlink_diag.c differ from the tip).
The rebase pulled in firmware the measurement never saw: kilnlink 18
(c2a866b23: TEST_TRIP, DIAG V3, CLEAR_TRIP V3), plus kiln_io.c,
profile_executor_run.c, zones_config_store.c, live_profile.c and
kiln_cfg_swap.c changes. Two later checkbuild ELFs
(`checkbuild_633d47beed`, `checkbuild_e77db94fc2`) dump the same
link_watchdog/bx_flash_worker/kiln_io_owner values, which is reassuring for
those three, but neither is confirmed to be built from the tip, and the dump
over all three ELFs timed out before the httpd and system bridge rows
printed, so growth was not attributed per commit.

Headroom (stack - ceiling - 300 B unmodeled overhead):

| Task | Stack | Ceiling | Honest free |
|---|---|---|---|
| system bridge | 4096 | 3216 | 580 (floor 256; was 644 earlier the same day) |
| link_watchdog | 3072 | 1200 | 1572 |
| kiln_io_owner | 4096 | 2208 | 1588 |
| bx_flash_worker | 10240 | 6336 | 3604 |

The system bridge is the one to watch: it lost 64 B in one day and is now
324 B above the 256 B floor. kilnlink 18 adds link-path code that can run on
that task.

Scenario: a ceiling that is stale low for the tip makes the standing budget
check pass while the real worst-case path is deeper; the next growth on the
system bridge then lands with less headroom than the table says.

Fix: next time a tip target build exists (the standing
`check_00_kilnfw_target_build.ps1` run), run
`check_all_task_stack_budgets.py --elf <tip ELF> --dump-ceilings` and commit
the tip numbers with the ELF's source commit in the comment. If the system
bridge ceiling grows again, bump its stack (stack bumps are pre-authorized)
rather than its ceiling alone.

## LOW-1: touch_cal "equivalent mutant" claim in the gate table is false

1152994ab's row for check_touch_cal_exit_target_caller says the
`if ($allowedCalls -ne 1)` -> `if ($false)` mutant is equivalent. It is not.
That branch is the only thing that catches a second, or a missing, call
inside ui_page_touch_cal.c itself (the one legal call is at line 196).
Negtest: with the guard disabled, both a duplicate call and zero calls are
MISSED; with the guard intact, the duplicate call is CAUGHT.

Scenario: someone later "simplifies" the check by deleting the branch on the
strength of the table row, and a second exit-target call in the touch-cal
page (bypassing the role choice) lands green.

Fix: correct the row to record the duplicate-call mutation as CAUGHT and
remove the equivalent-mutant wording.

## LOW-2: build-gate deliberate-misuse exemption matches by file name, whole file

93693b8da adds `$deliberateMisuse = @('check_build_gate_reentrant.ps1')` and
filters `$_.Name -notin $deliberateMisuse`. That skips all four rules, for
the whole file, for any file with that name anywhere in the tree. Negtest: a
misuse in a new `firmware/KilnFW/tools/check_build_gate_reentrant.ps1` is
MISSED; the same content under another name is CAUGHT; dropping the exemption
is CAUGHT (so the exemption is needed).

Scenario: a copy or a second check with the same name in another directory
misuses the gate (no try/finally Exit) and leaks a slot; the check stays
green.

Fix: compare the repo-relative path (`tools\check_build_gate_reentrant.ps1`),
not the leaf name. Better, use a per-line marker on the deliberate misuse
lines so the rest of that file is still checked.

## LOW-3: check_reset_fence_hooks pins only the predicate name

The regex now requires `relay_authority_reset_refuses_kiln_nvs_writer()`
somewhere inside `kiln_nvs_reset_refuses_write`'s body (`[^}]*`). It accepts
`... refuses_kiln_nvs_writer() && 0` (MISSED), and it never looks at the
predicate's own body in relay_authority.c (`bool refuse = 0 && ...` MISSED).
The semantics are covered by test_link_watchdog.c (lines 223-266), not by
this check.

The rename itself is complete: main.c line 219 returns
`partition != NULL && strcmp(partition, "kiln_nvs") == 0 && relay_authority_reset_refuses_kiln_nvs_writer()`,
hooks are installed at lines 239-242, and the predicate keeps ea86f4d43's
semantics (refuse when depth != 0, the reset scope erases kiln_nvs, and the
caller is not the reset job's task). By design a wifi or profiles scope reset
does not fence kiln_nvs writes, since that partition is not erased. Writes to
legacy keys in the default partition are not fenced; that is pre-existing.

main.c line 214's comment still names `relay_authority_reset_refuses_writer()`.

Scenario: a short-circuit or a body edit disables the fence; this check stays
green and only the host test (if run) notices.

Fix: name test_link_watchdog.c as the semantic guard in the gate table row,
and make the check reject any `&&`/`||` term after the call (or match the
whole return expression). Fix the stale comment at main.c line 214.

## LOW-4: stale line citation for heat_enable_release

The doc text refreshed in 10ddf02f4/f65950541 says "line 505 still holds
heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE)" for
profile_executor_relay_io.c. At the tip that call is at line 1106. The blob
id (`60c6b410`) is right; the line number is not.

Scenario: a reader checks line 505, finds something else, and either distrusts
the citation or "fixes" the wrong code.

Fix: cite the function name instead of a line number, or update to 1106.

## LOW-5: blob-pinned citations churn

Two refreshes of the same relay_io blob citation landed about 24 minutes apart
(10ddf02f4, f65950541). Every edit to a cited source file forces a doc commit,
and in a busy dev branch that races.

Scenario: the citation check fails on dev after every unrelated edit to the
cited file, producing repeat fix-up commits like f65950541.

Fix: cite a symbol or a commit SHA for history claims, and keep blob pins
only where byte identity is the claim.

## INFO

- Item 4 link-impl allowlist: the entry for test_cfg_fs_mount_state.c
  (`ref_crc()`, littlefs CRC32) is necessary (dropping it is CAUGHT). It is a
  whole-file exemption, so a link CRC16 added to that test file is MISSED;
  that matches every existing entry in the list. This negtest is the item 4
  evidence that was missing.
- Item 8 off_err: the capture satisfies check_safety_call_results_checked
  (reverting it is CAUGHT), and behaviour is identical to the old inline
  compare: on ESP_OK the tracker notes the write, otherwise the zone is added
  to `zone_off_pending_mask`. The check only requires a capture, so
  `(void)off_err; if (1)` passes (MISSED); pre-existing heuristic.
- Item 9 selfcheck: `client_post_keys = set(_TOP_FIELD_FORM_KEY) | set(_TOP_POST_ONLY_FORM_KEYS)`
  keeps the comparison bidirectional, and firmware reads
  `expected_generation` at zones_http_post.c:362. Two nits:
  `build_post_body` hardcodes `fields["expected_generation"]` (line 1224)
  instead of using the constant, so the two can drift; and the PASS label
  still says "_TOP_FIELD_FORM_KEY matches firmware". Running
  `selfcheck_zones_fields.py` directly does nothing (no main); it runs
  through selfcheck.py line 280.
- Item 3: `$work` -> `$tmpWork` is correct; it is a scratch directory, and
  source_path_drift exempts `$tmp*` names by design.
- Pre-existing: check_build_gate_usage's directory exclusion regex
  `[\/](\.git|node_modules|\.venv|build[^\/]*)[\/]` uses forward slashes only
  and never matches Windows backslash paths
  (`'C:\a\build\x.ps1' -match ...` is False), so build output directories
  are scanned too.
