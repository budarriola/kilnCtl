# Checks, negative tests, logs, cleanup

Split out of CLAUDE.md 2026-10-14; history in docs/CLAUDE_HISTORY.md.

**Check suite (coordinator/batch use):** run `tools/run_all_checks.ps1` with `-ExecutionPolicy Bypass` (without it the
script fails to load and Bash still reports exit 0). Three phases: the three target builds
(KilnFW, SaftyFW, recovery) concurrently; everything else throttled parallel (`-MaxParallel`,
default 8); `check_ui_responsive_sweep.ps1` alone. `-Only <regex>`/`-Skip <regex>` filter by
path for iterating (but do a full `-Fast` run before landing, filtered runs let regressions
land). `-Fast` skips the phase-1 builds and `check_00_kilnfw_host_tests.ps1`; checks that then
lack a build artifact print `SKIP-FAST:` (own summary bucket, never fatal); any other SKIP still
fails the run (`-AllowSkips` only for a machine permanently lacking a prerequisite). A green
`-Fast` run in a fresh worktree says nothing about stack/DRAM headroom (those need a `build/`
ELF). Re-verify the check count with `-ListOnly -AllowFewerChecks` (~160).
SaftyFW host-test builds need a short worktree path (`C:\wt\...`, MSVC command-line limit).
KilnFW target builds share a pinned ccache (`C:\wt\.ccache`); see "Heavy builds" in
`docs/agent_rules/COMMON.md` for build-gate slots.

**Check result cache:** a PASS of a check marked `# checkcache: ok` is reused when the tree is
clean and HEAD tree hash, mode and fingerprint match (`tools/checkcache_lib.ps1`,
`C:\wt\.checkcache\`); `-NoCache`/`KILNCTL_CHECKCACHE=0` disables. Marking rules:
`docs/agent_rules/COMMON.md` "Check result cache".

**Main baseline:** each run prints NEW / KNOWN / FIXED failures vs the main baseline
(`tools/main_baseline.ps1 -Record|-Show`; `-FailOnlyOnNew`; `land.ps1 -AllowKnownFailures`).
Landers read the NEW list; KNOWN needs one line, no re-reporting.

**Negative-test with `tools/negtest.ps1`, never by hand** (throwaway worktree, unmutated baseline
must pass, fresh `{OUT}`, CAUGHT/MISSED per mutation, presets `kilnfw-host`, `saftyfw-host`,
`check -PresetArg <check>`, `pytest`). Every new check/test gets a negative test. An empty
`git diff` proves the source is restored, not the build artifacts: never measure from a prebuilt
binary of unestablished provenance; rebuild first. Example:
`powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset kilnfw-host -File <file> -Find "<old>" -Replace "<new>" -ExpectPattern "FAIL .*<test>"`.

**Logs:** PowerShell `*>` writes UTF-16LE, so `grep`/`until` never match. Wait with
`tools/wait_for.ps1` (bounded, encoding-aware, `WAIT_RESULT {json}`; from Bash:
`powershell -ExecutionPolicy Bypass -File tools/wait_for.ps1 -TimeoutSec 600 -LogFile x.log -Pattern DONE`);
read with `tools/decode_log.ps1 x.log`.

**Cleanup.** Safe to delete on sight: untracked build byproducts in the main tree (loose `.obj`,
host-test scratch dirs `cfg_fs_test_*/`, `log_store_test_*/`, `build_agent/`, `build_sim_*_obj/`)
and mangled-path files from Bash-vs-PowerShell backslash accidents (add a `.gitignore` pattern
if a class recurs). Under `C:\wt\` remove only worktrees positively confirmed unregistered or
with no pending work AND stale (nothing modified in the last few minutes). Never delete
`firmware/KilnFW/elf_archive/`, `logs/coupling/*` or other captured run data, any `.kicad_*`
file, or a tracked file with local modifications (other sessions' WIP; do not revert to
investigate). Use `tools/wt_status.ps1` (`-Prune`, preview with `-WhatIf`), not hand cleanup.

## Git guards (detail in docs/MCP_SERVERS.md "Git workflow guards")
`tools/worktree_mint.ps1` (mint/remove short worktree under `C:\wt\`; run from PowerShell, not Bash), `tools/push_verify.ps1` (verify a commit landed on origin), `tools/commit_guard.ps1` (refuse a commit whose working copy differs from origin until each difference is confirmed yours), `tools/land.ps1` (coordinator only: check-log gate, rebase, narrow re-check, push, push_verify).
