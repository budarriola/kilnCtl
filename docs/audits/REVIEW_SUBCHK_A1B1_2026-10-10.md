# Review: submodule pin check (ccb7b5d2/0122566b) and owner decisions A1/B1 (7cc34326/979d05a6)

Date 2026-10-10. Reviewer: Opus, read-only review of origin/dev at 1088c8a3. No code changed.

Scope:
- `ccb7b5d26` `tools/check_submodule_pins_pushed.ps1`, `tools/test_check_submodule_pins_pushed.ps1`,
  wiring in `tools/land.ps1`, `tools/dev_promote.ps1`, `tools/run_all_checks.ps1`; `0122566b5` gate evidence row.
- `7cc343260` + `979d05a63` A1 (`lcd_touch_cal_saved_exit_target`) and B1 (`debug_program(peer="pico")`
  proceeds with a WARNING when ESP state is unreadable).

Verification done: re-ran `tools\negtest.ps1 -Preset check -PresetArg tools\test_check_submodule_pins_pushed.ps1`
with three mutations (FAIL path `exit 1`->`exit 0`, SKIP path `exit 3`->`exit 0`, fetch fallback disabled):
all three CAUGHT, baseline PASS. `tests/test_pctools_batch_d_2026_10_10.py`: 27 passed. An ad-hoc probe
(exec state idle, autotune `get_status()` raising TimeoutError) confirmed finding B1-1 below.

## Submodule pin check

Answers to the questions:
- **Can it PASS with an unreachable pin?** Not through ls-remote/fetch failure: an unreachable remote is
  SKIP (exit 3), a reachable remote without the sha is FAIL (exit 1). It does not need the submodule
  initialised (uses `git ls-tree <Commit>`), and `fetch --depth 1 <url> <sha>` is a correct existence
  probe. It can still exit 0 without checking, via the paths in S-2 and S-4.
- **Do land/dev_promote refuse on exit 1?** Yes: `land.ps1:342` `Finish 1` before the push,
  `dev_promote.ps1:154` `Fail 'submodule-pins'` before `push origin m:main`.
- **SKIP warn-only?** Acceptable for a real network outage. Not acceptable as written for a wrong URL (S-1).
- **run_all_checks exclusion** (`run_all_checks.ps1:212`): narrow, exact file-name match. The offline
  test is wired explicitly and fails the run if missing. OK.
- **Negtest evidence:** real; reproduced, plus two further mutations, all caught.

### S-1 MED -- a wrong, renamed or private submodule URL is SKIP (warn), not FAIL, and can hang
`check_submodule_pins_pushed.ps1:42-45`. Any `git ls-remote` failure counts as "network". If a `.gitmodules`
URL is mistyped, the repo is renamed or deleted, or it goes private, every land and promote prints a
yellow WARNING and pushes. That is exactly the case where a fresh clone cannot check the submodule out.
Separately, nothing sets `GIT_TERMINAL_PROMPT=0` / `GCM_INTERACTIVE=never` and there is no timeout. On
Windows, an https ls-remote to a missing GitHub repo can raise a Git Credential Manager prompt, and a
stalled connection blocks `land.ps1` with no bound. Suggested fix: first probe `origin`. If origin is
reachable and the submodule URL is not, FAIL. Disable credential prompts and bound both git calls.

### S-2 LOW -- `.gitmodules` is read from the working tree, not from `<Commit>`
`check_submodule_pins_pushed.ps1:21` reads `git -C $RepoPath config --file .gitmodules`, but the pins come
from `ls-tree $Commit`. In `dev_promote.ps1:153` the commit is the synthesized `$m`, while `$RepoPath` is
whatever tree runs the script (often the shared main tree, at an arbitrary checkout). A submodule added or
re-pointed on dev but absent from that working copy's `.gitmodules` is never enumerated, and the check
still prints PASS. The same happens if the working copy has no or a malformed `.gitmodules`: `git config`
exits non-zero and line 22 prints `PASS: no submodules`. Fix: `git config --blob "${Commit}:.gitmodules"`,
and treat any exit other than 1 (no match) as an error, not a PASS.

### S-3 LOW -- land.ps1 checks the repository holding the script, not the tree being landed
`land.ps1:341` passes neither `-RepoPath` nor `-Commit`, so the check defaults to `Split-Path -Parent
$PSScriptRoot` with `HEAD`. land.ps1 itself works on `$top` (cwd's toplevel, line 170). Suppose
land.ps1 is invoked by the main tree's absolute path from inside a `C:\wt\...` worktree (it uses `$top` for
`$ChecksScript`, so this mixed invocation is otherwise supported). The pin check then inspects the main
tree's HEAD and can PASS while the worktree's commit carries an unpushed pin. Fix: pass
`-RepoPath $top -Commit $script:sha` (or the rebased HEAD).

### S-4 LOW -- a SKIP is not carried into land's final verdict
`land.ps1:343` / `dev_promote.ps1:155` print a WARNING mid-log, but the run still ends LANDED / promoted
with no marker in land's final JSON line. A coordinator that reads only the verdict never learns that the
pins were unchecked. Suggest a `submodule_pins: "skipped"` field or a closing reminder line.

### S-5 INFO
- The test enables `uploadpack.allowAnySHA1InWant`. GitHub behaves like `allowReachableSHA1InWant` and
  also serves shas from the fork network. So a pin that exists only in a fork, or an unreachable commit
  that has not yet been garbage-collected, can PASS on GitHub. `git submodule update` would fetch it the
  same way, so this is not a false PASS today, but it is not a guarantee either.
- `git ls-remote $url` runs in the caller's cwd, so a relative `.gitmodules` URL (`../x.git`) would
  resolve wrongly. None are relative today.
- A transient fetch failure after a successful ls-remote is a FAIL (a false refusal, not a false pass).
  Acceptable.

## A1 -- touch_test role exception

Clean. `lcd_touch_cal_saved_exit_target()` has exactly one caller, `ui_page_touch_cal.c:196`, in
`finish_calibration()`. It runs only after a non-degenerate fit, and only past the L11 gate
(`ui_page_touch_cal.c:180`: re-saving an existing calibration needs ADMIN). `touch_cal` itself is
reachable without a role only on a never-calibrated board: the boot path at `kiln_ui.c:349`, and the
uncalibrated branch of `ui_page_config.c` touch_cal_nav_cb, on Config, a non-dashboard page and so PIN-gated under the 2026-09-28 owner decision (not re-verified here).
`lcd_touch_cal_exit_target()` no longer exempts any page name (tested: no role + "touch_test" -> "home").
No other `kiln_ui_show("touch_test")` exists. `ui_topbar.c` nav_cb, UART bridge and HTTP never name it.
touch_test's only exits are Clear and Done->home.

- **A1-1 LOW:** the "only ui_page_touch_cal.c may call this" rule (`lcd_auth_state.h:262-266`) is a comment
  only. No check pins the caller set, so a future caller anywhere gets role-free touch_test. A grep-style
  check (one call site, in ui_page_touch_cal.c) would make it mechanical.
- **A1-2 INFO:** the exception also fires when `touch_cal_store_save()` fails (`ui_page_touch_cal.c:186-190`
  logs and falls through). The header's "after a successful calibration save" is slightly overstated. This
  is harmless, since touch_test shows no data. The `has_user_role` parameter is unused (`(void)`) and
  could be dropped to avoid suggesting it gates anything.

## B1 -- debug_program(peer="pico") with unreadable ESP state

The confirmed-running cases still refuse. Profile state not in {0,3,4} refuses, including an unknown state
(it is treated as running, not as unreadable). Autotune state not in {0,5,6} refuses. allow_running=True
overrides only by explicit opt-in. Every other caller (`reset`, `halt`, `step`, `leave halted`, `write
memory`) passes no list, so those callers still fail closed (tested).

### B1-1 MED -- an autotune read failure on a demonstrably reachable ESP proceeds
`mcp_server_debug.py:366-371`. The WARNING path is justified as "recovery, bricked or link down". But the
autotune branch runs only after `get_exec_status()` has already answered over the same UART link with an
idle state. Autotune runs with the profile executor idle, so a single timeout or exception on
`_autotune.get_status()` reflashes and resets the Pico mid-autotune with only a WARNING line. Reproduced
with a mock: exec state 0, autotune raises TimeoutError, and `program` is called and returns "programmed
pico OK". In this case the ESP is readable, so "unreadable" is reached from a possibly-running state. The
owner decision covers an unreadable ESP, not one that answered a moment earlier. Suggested fix: apply
the warning only when the exec read itself failed. If exec answered, an autotune read failure refuses,
as before. Add a test for that case. No test today covers "exec idle + autotune running" under the
`unreadable_warnings` list.

### B1-2 LOW -- an exec-status timeout during a live firing is treated as "unreadable" (by owner decision)
`mcp_server_debug.py:345-356`. A 2 s `get_exec_status` timeout (ESP busy, or the serial port held by another
client -- see the stale-pytest serial-hub note) proceeds while a firing may be running. This is within
owner decision B1, and a Pico reset drops the safety link, so the ESP de-energises. The cost is an
interrupted firing, not unsafe heat. No second source is tried before declaring the state unreadable
(for example a retry or `GET /api/profile_exec`). Recorded as residual risk only.

### B1-3 INFO
`7cc343260` committed an unterminated string literal (a raw newline inside `"..."`) in
`mcp_server_debug.py`, so the module failed to import until `979d05a63` two minutes later. Any bisect
landing on 7cc34326 sees the whole kilnctrl MCP server broken. Nothing to fix now.

## Fix status

Fixed in fd1905824: B1-1 (autotune read failure refuses when the exec read answered; test + negtest CAUGHT), S-1 (prompts disabled, bounded git, only DNS/connect/timeout is SKIP, bad URL/not found/auth is FAIL), S-2 (.gitmodules read from the commit; gitlinks without .gitmodules FAIL), S-3 (land.ps1 passes -RepoPath/-Commit; dev_promote already did), S-4 (land final JSON carries submodule_pins), A1-1 (tools/check_touch_cal_exit_target_caller.ps1, negtested). B1-2, A1-2, S-5 unchanged (residual/info).
