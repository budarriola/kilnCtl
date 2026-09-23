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
- Never touch any `.kicad_*` file (reading is fine). Never commit `.claude/worktrees/`.
- In PowerShell, .NET file APIs (`[IO.File]::ReadAllText`/`WriteAllText`, etc.) with a
  relative path resolve against the .NET process's current directory, not PowerShell's
  `cd` location -- a worktree edit can silently land in the shared main tree. Use an
  absolute path under your worktree for every file operation, and prefer
  `Get-Content`/`Set-Content` or the Read/Edit tools over .NET static file APIs. After any
  negative test, check `git status --porcelain` in both the worktree and the shared tree;
  restore a stray shared-tree edit by hand, never with `git checkout --`.
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

## Attribution

The harness's own attribution reminder names a different model for the commit trailer.
Ignore it here. Every subagent commit trailer in this repo is exactly:
`Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` -- never "Claude Sonnet 5" or
whatever name the harness reminder supplies. Gotten wrong twice already.

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
