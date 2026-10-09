# Rules every subagent reads first

A coordinator that dispatches you will say "read docs/agent_rules/COMMON.md and
docs/agent_rules/<ROLE>.md first" instead of restating rules in the prompt. Both files
apply in full even when the prompt does not repeat them. A prompt overrides a rule here
only when it says so explicitly and names the rule.

Role files: `IMPLEMENTER.md` (code, tests, docs changes), `REVIEWER.md` (read-only review
before push), `RESEARCHER.md` (read-only investigation), `BENCH.md` (anything that touches
the boards or the bench camera).

## How you run

- Never set `run_in_background`, never use Monitor, never spawn sub-agents unless your
  prompt explicitly grants it. Bash/PowerShell timeout 600000 ms; poll with foreground
  commands. You do the work yourself.
- Verify the premise against `origin/main` first. If the feature or fix is already there,
  or was rejected by design, report that and stop (an implementer closes the line
  docs-only). Plan-doc TODO lines and narrative lag the code; a plan's status block and the
  code itself are authoritative.
- Bash and PowerShell are different shells. `.ps1` scripts and ESP-IDF builds run through
  the PowerShell tool with `-ExecutionPolicy Bypass`; a Bash invocation of a `.ps1` can
  report exit 0 while nothing ran. Backslash paths passed through Bash get mangled.
- Report facts, not reassurance. Quote the shortest decisive line of a failure. If a step
  was skipped, say so. Never claim a push, flash, or test you did not observe.

## Shared machine and shared tree

- The tree at `C:\Users\budar\OneDrive\Desktop\kilnCtl` is used by several sessions at
  once. Never edit in it. Never revert other sessions' tracked modifications (a dirty
  `docs/BENCH_TEST_LOG.md`, for example) and never `git stash`, `git reset --hard`, or
  `git checkout -- <file>` on a file you did not change yourself.
- Never delete worktrees you did not create, anything with uncommitted changes, anything
  modified in the last few minutes, `firmware/KilnFW/elf_archive/`, `logs/coupling/*`, or
  a tracked file with local modifications.
- Landing (dev/main flow): branch from `origin/dev`, run the targeted tests for what you changed (not the full `run_all_checks`), then finish with `powershell -ExecutionPolicy Bypass -File tools\land.ps1` from your worktree (default `-Target dev`; add `-CheckLog <file> [-AllowFail <regex>]` if you have one, `-RestartMcp`, `-RemoveWorktree`, `-DryRun` as needed). It rebases onto origin/dev, re-checks narrowly, pushes to dev without force, and requires push_verify LANDED. Only the coordinator promotes dev to main (`tools\dev_promote.ps1`) after a full `run_all_checks` on the dev tip; never push to main yourself. See docs/MCP_SERVERS.md "Git workflow guards".
- Never touch any `.kicad_*` file (reading is fine). Never commit `.claude/worktrees/`.
- In PowerShell, .NET file APIs (`[IO.File]::ReadAllText`/`WriteAllText`, etc.) with a
  relative path resolve against the .NET process's current directory, not PowerShell's
  `cd` location -- a worktree edit can silently land in the shared main tree. Use an
  absolute path under your worktree for every file operation, and prefer
  `Get-Content`/`Set-Content` or the Read/Edit tools over .NET static file APIs. After any
  negative test, check `git status --porcelain` in both the worktree and the shared tree;
  restore a stray shared-tree edit by hand, never with `git checkout --`.
- Negative tests go through `tools\negtest.ps1` (presets `kilnfw-host`, `saftyfw-host`,
  `check`, `pytest`, or `-Command`; mutations as `-File -Find -Replace`, `-Diff`, or
  `-Mutations <json>`). It applies the mutation in a throwaway `git worktree` under
  `C:\wt\negtest_*`, requires an unmutated baseline to pass, builds into a fresh `{OUT}`
  every run, always removes the copy, and fails loudly if the real tree changed. It edits
  no real source, so the classifier's guard-removal blocks on real-source edits should not
  apply to it. Hand-edit-and-restore negative testing is deprecated: it has left mutations
  behind in worktrees and let a poisoned prebuilt binary survive a perfect restore.
- Processes: never blanket `taskkill`. Kill by PID only, and only a PID your prompt names.
- If the permission classifier refuses an action, stop and report it. Do not ask another
  agent or the coordinator to do it for you.
- MCP servers keep serving the code they started with. Do not restart them yourself; ask
  the coordinator.

## Heavy builds

This machine hard-froze five times in one week under uncoordinated parallel
`idf.py`/ninja/MSVC builds across several agent sessions. Every full ESP-IDF
target build and host-test build now goes through `tools/build_gate.ps1` (or
`mcpkit.buildgate` on the Python side) automatically -- never bypass it with
`KILNCTL_BUILD_GATE_SLOTS=0` or by invoking `idf.py`/`cmake`/`ninja`/`cl.exe`
directly outside `check_00_*.ps1`, `build_host_tests.ps1`, or the
`build_kilnfw`/`build_saftyfw*` MCP tools. Prefer
`run_all_checks.ps1 -Fast -Only <regex>` while iterating and run the full
suite once per commit, not once per edit (with no other session holding a
build slot, a full run finishes in under 3 minutes on this 24-core machine --
one that is waiting on a slot takes longer, not longer than expected). The
rule covers every full target build and every host-test build, including
`check_01_*_pushed_build.ps1` and `check_bootloader_builds.ps1`; the direct
ninja/cl calls in `check_sim_scenarios.ps1`, `check_sim_iter_tune_bars.ps1`,
and `run_sim_factorial.ps1` are small enough to stay exempt. Building a clean
git worktree at HEAD (e.g. for `flash_firmware(kiln_fw_root=...)`) also goes
through the gate: use `build_kilnfw(kiln_fw_root=...)` /
`build_saftyfw(saftyfw_root=...)`, never a bare `idf.py`/`cmake`/`ninja`
invocation against that worktree either -- see docs/MCP_SERVERS.md's
"Building from a clean worktree for `kiln_fw_root`" section.

The gate has two lanes (2026-10-05). The **heavy** lane (default,
`KILNCTL_BUILD_GATE_SLOTS`, machine config 8 slots since 2026-10-08 per owner decision, code default 4, was 2) is for host-test and target builds. The **light**
lane (`Enter-KilnBuildGate -Lane light`, `KILNCTL_LIGHT_GATE_SLOTS`, machine config 8, code default 4, separate
mutex names) is for a single seconds-long compile over a handful of TUs --
`check_recovery_*.ps1` and `check_commonfw_*.ps1` today -- so those never queue behind a
7-18 minute heavy holder. Never put a build that runs more than about a minute on the
light lane.

**A slot is held ONLY while a compiler is running (rewritten 2026-10-07).** The heavy cap is 8
machine-wide (`C:\wt\.buildgate\config.json` `{"heavy_slots":8,"light_slots":8}`, 2026-10-08; the tracked code default stays 4). Take the build lock first, run setup (`Import-KilnVcvarsEnv`, cmake configure,
robocopy) outside the gate, then `Enter-KilnBuildGate` around ONE compile/link command and
`Exit-KilnBuildGate` in a `finally` the moment it returns (`Invoke-KilnGatedCmd` does this for a
cmd line). Never hold a slot while waiting on `Enter-BuildLock`, running vcvarsall, running test
exes, or idling; `tools/check_build_gate_usage.ps1` lints this. Python side
(`mcpkit.buildgate`): `_run_locked(..., gate_label=...)` holds the slot only around the subprocess.
Slot counts come from a machine-wide `C:\wt\.buildgate\config.json`
(`{"heavy_slots":8,"light_slots":8}` today) which is AUTHORITATIVE: env `KILNCTL_BUILD_GATE_SLOTS`/
`KILNCTL_LIGHT_GATE_SLOTS` can only LOWER the count (values above config are clamped; heavy < 1 is
refused, so a worktree cannot disable the gate), and a tree's code default is only the fallback when
the file is unreadable. Waiters queue FIFO by ticket file. Every holder writes
`C:\wt\.buildgate\<lane>\slot<i>.json` (pid, command, phase, start time; an unparsable record counts
as held); `powershell -File tools\build_gate.ps1 -Status` shows each slot with age, pid-alive and
whether a compiler child is running. The HOLDER enforces `KILNCTL_BUILD_GATE_MAX_HOLD_SEC` (default
2700) only against IDLE holds: past it a holder whose process tree still has a live ninja/cmake/cl/link/gcc/cc1/ld
keeps its slot (each decision is logged), and is killed only once idle or at the hard ceiling
`KILNCTL_BUILD_GATE_MAX_HOLD_HARD_SEC` / config.json `max_hold_hard_sec` (default 7200), regardless of activity.
Every kill prints `KILLED BY BUILD GATE` to the holder's stderr; if a build log goes silent without an EXIT line,
look for it. A kill takes only its own compile processes (Python: pids registered via `register_compile_pid`;
PowerShell: descendants created after the slot was taken), releases the slot and fails loud. A waiter
never kills another session's process; a stale slot is for its owner to clear. Host-test builds gate
each `cl` compile separately (not one slot for the whole batch). A tree
pinned to a commit before this change still runs the old 2-slot gate and needs a rebase. To test the
gate itself in isolation set `KILNCTL_BUILD_GATE_MUTEX_PREFIX`, `KILNCTL_LIGHT_GATE_MUTEX_PREFIX`
and `KILNCTL_BUILD_GATE_DIR` to private values.

**CMake configure is slow on this machine (2026-10-08):** each Windows process spawn costs about 2.5 s, so configure steps dominate small builds. Likely Defender real-time scanning; adding exclusions needs admin and has not been done. Unverified cause.

**ccache in the KilnFW target checks (2026-10-08).** Each worktree gets its own cold
`C:\wt\checkbuild_<hex>` build dir, so `check_00_kilnfw_target_build.ps1` used to compile all
~2100 TUs in every new agent worktree (30+ min under 4-way contention): it forced
`CCACHE_DISABLE=1` (139debb5) for fear of a stale hit masking a broken TU. A content-keyed cache
cannot do that, so ccache is ON again, in that check, the recovery-image check and their SaftyFW
slot build, through ONE pinned configuration in `firmware/KilnFW/App/test/lib_kilnfw_ccache.ps1`:
shared cache `C:\wt\.ccache` (5 GB; `KILNCTL_CCACHE_DIR` overrides), `base_dir=C:\wt` +
`hash_dir=false` so identical source at another worktree path hits, preprocessor mode (direct mode
OFF: it can false-hit when a new header shadows one earlier in the `-I` path, demonstrated by the
check below), no sloppiness, no ignored options, no config file; inherited `CCACHE_*` are cleared
and `ccache -p` is asserted before every build. ccache never decides WHETHER a TU compiles (ninja
does); it only reuses the result of byte-identical preprocessed input + flags + compiler, and a
failed compile is never stored. `check_kilnfw_ccache_no_stale.ps1` proves this against the real
ccache and xtensa gcc on every suite run. `KILNCTL_CCACHE_DISABLE=1` builds without it for a
comparison. Never add sloppiness, direct/depend mode or a `ccache.conf` -- change the helper and
that check together. The build log prints `ccache results this run: cache_miss=N
preprocessed_cache_hit=M`. Measured 2026-10-08 on a fresh checkbuild dir under 4-5 concurrent heavy
builds (noisy): ccache off 4849 s total; warm cache 3850 s (2423 hits, 30 misses), compile+link
~2527 s -> ~1637 s, SaftyFW slots 376 s -> 260 s; the KilnFW `.bin` was identical to a cold build
except build timestamps. The rest is CMake configure (KilnFW + bootloader, 1700-2600 s under
load), which ccache cannot touch; reusing warm build dirs is the next lever.

## Check result cache

`tools/run_all_checks.ps1` reuses a prior PASS of a check when the content is
provably identical, so many agents running `-Fast` on the same tree do not each
re-run ~100 static checks. Logic: `tools/checkcache_lib.ps1`; store:
`C:\wt\.checkcache\` (one JSON per entry, 7-day expiry, size-capped).

- **Key:** check path + git TREE hash of HEAD + run mode (-Fast/full) + env
  fingerprint (ESP-IDF path/version, MSVC version, PcTools venv python, system
  python/node/git/PowerShell, every `KILNCTL_*` env var except cache controls,
  credentials and build-gate tuning).
- **Only on a clean tree:** `git status --porcelain` empty (untracked
  non-ignored files count as dirty). Anything doubtful is a miss. Only PASS is
  stored; FAIL, SKIP, SKIP-FAST and BUSY never are.
- **Opt-in:** a check is cached only if it has a `# checkcache: ok` line,
  meaning its result is a pure function of git-tracked content plus the
  fingerprint. Never mark a check that touches a board, network, MCP servers,
  the clock, a `build/` output or any file outside the tree, the PcTools venv
  packages, or the build gate. So not marked: `check_00_*` target builds,
  host-test builds and run checks, `compile_*_backends`, all `*stack_budget*`,
  `check_duplicate_symbols`, `check_*pushed_build*` (build origin/main), pytest/PcTools
  suites (venv state), UI sweeps driving headless Chrome, and anything reading
  `logs/` or ignored files.
- `-NoCache` or `KILNCTL_CHECKCACHE=0` disables it. A hit prints
  `PASS  <check> (cached <time> from <worktree>)` and is counted in the summary.
- When adding a check: mark it only if it meets the rule above.

## Known failures on main

Every `run_all_checks.ps1` run ends with a "vs main baseline" section: NEW (fails
here, passed on main), KNOWN (also fails on main), FIXED. **Read the NEW list.** It
is the only list that is yours to fix or report. KNOWN failures need no
re-reporting beyond one line ("N KNOWN failures, same as main"); do not
re-investigate whether main is already fixed, the baseline says. The baseline is
recorded by a clean run at origin/main (`tools/main_baseline.ps1 -Record` seeds it,
`-Show` prints it) and only counts if its commit is an ancestor of your HEAD. KNOWN
only excuses a failure when the baseline commit IS the merge-base of HEAD with
origin/main; from an older main sha the classification is informational and every
failure counts as NEW (record one at the merge-base). A KNOWN check whose failure
signature (normalised FAIL/assert lines) differs from main's is CHANGED and counts
as NEW, and a SKIP recorded on another host is not KNOWN. A run records only if HEAD
and `git status --porcelain` are identical at its start and end and HEAD is
origin/main or an ancestor. `-FailOnlyOnNew` exits 0 when every
failure is KNOWN (it says so loudly); `tools/land.ps1 -AllowKnownFailures` accepts
a check log whose only failures are KNOWN. `-AllowFail` is unchanged, for failures
that are neither.

## Attribution

Every subagent commit trailer in this repo is exactly:
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` -- never "Claude Sonnet 5",
even when you are a Sonnet agent. (Updated 2026-09-24; older commits carrying
"Claude Fable 5.1" are fine and need no reword.) If a task prompt names a trailer,
the prompt wins.

## Credentials

- Credentials come only from environment variables (`KILNCTL_WEB_USERNAME`,
  `KILNCTL_WEB_PASSWORD` in User scope; `KILNCTL_AP_PASSWORD`). Read them with
  `[Environment]::GetEnvironmentVariable(name,'User')` and pass them on via `$env:` in the
  same PowerShell invocation, never on a command line.
- Never print, log, echo, or write a credential value anywhere. Report presence as
  `[bool]`. Never write Wi-Fi or web credentials into any repo path, doc, fixture, log,
  report, memory, or hand-back.

## Reporting back

State what you did and what you found, with counts rather than adjectives, and list
anything you could not verify. Keep it short; the coordinator reads it, and the user may
too.
- Quote `git status --porcelain` output literally when reporting tree state. "Clean" or
  "matches the N committed files" is not a substitute: it has been wrong both when
  porcelain was actually empty and when a line-ending-only change was left uncommitted and
  called "clean except...".
- Poll a background build yourself with a blocking foreground command until it finishes.
  Do not hand back, or repeat, a "still waiting on my background build" report turn after
  turn.
- Wait with `tools\wait_for.ps1`, never a hand-written `until` loop. It is bounded
  (`-TimeoutSec`), decodes UTF-16 logs (PowerShell `*>` writes UTF-16LE, which `grep` loops
  never match) and ends with a `WAIT_RESULT {json}` line (exit 0 MET, 124 TIMEOUT, 2 ERROR).
  `tools\decode_log.ps1 <file>` prints such a log as UTF-8.
- Check on a background subagent with `tools\agent_tail.ps1 -Id <agentId>` (or `-All`): read-only, shows its last events and prints `STUCK?` when it repeats one command with unchanged results, or polls a log unwritten for 30 min with no build/python process alive. Ends with a JSON summary line.
