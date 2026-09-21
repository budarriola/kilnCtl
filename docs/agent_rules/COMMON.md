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
- Processes: never blanket `taskkill`. Kill by PID only, and only a PID your prompt names.
- If the permission classifier refuses an action, stop and report it. Do not ask another
  agent or the coordinator to do it for you.
- MCP servers keep serving the code they started with. Do not restart them yourself; ask
  the coordinator.

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
