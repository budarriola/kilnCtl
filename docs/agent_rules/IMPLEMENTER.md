# Implementer rules

Read `COMMON.md` first. You change code, tests, or docs and hand a reviewed-ready commit
back to the coordinator. You do not push.

## Worktree and commits

- Mint a private worktree with the PowerShell tool:
  `powershell -ExecutionPolicy Bypass -File tools\worktree_mint.ps1 -Label <name>`
  (prints a path under `C:\wt\`). Work only there -- never edit a file in the shared main
  tree first "to check" and then `git checkout --` it to undo; that tree is shared with
  other sessions and the undo is exactly the destructive command COMMON.md forbids. If you
  ever find you edited outside your worktree, stop and report it rather than reverting it
  yourself. Fresh worktrees need `-AllowFewerChecks` on `run_all_checks.ps1`.
- Every Read/Edit/Write path must be ABSOLUTE and start with your `C:\wt\<name>\` path.
  The session's working directory is the shared tree, so a relative path or a path
  copied from a grep of the shared tree silently edits the wrong copy -- this happened
  three times on 2026-09-25. Run `git -C <worktree> status --porcelain` after your first
  edit to confirm it landed in the worktree.
- `git fetch` first, then commit with `git commit -o <every changed path, explicitly>`.
  Never `git add -A`, never `--amend`, never force-push. Normal-prose message; end it with
  exactly this trailer line:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Do not push, rebase, or remove the worktree unless the prompt says to.
- Preserve each file's existing line endings (`ROADMAP.md`, firmware sources, TODO files and
  most docs are CRLF in the working copy). Never author doc prose through a PowerShell
  single-quoted string -- an embedded apostrophe doubles (`''`) and ships silently; write
  prose with a heredoc, the Edit tool, or a small Python script instead. One file shipped
  39 doubled-apostrophe artifacts this way.

## Checks

- Run `powershell -ExecutionPolicy Bypass -File tools\run_all_checks.ps1` in the foreground
  before handing back. `-Only <regex>` / `-Skip <regex>` filter by path while iterating;
  `-Fast` skips the three target builds only when you ran them yourself. A SKIP fails the
  run by default; under `-Fast`, `check_recovery_image_size.ps1` and
  `check_embedded_pico_image_fresh.ps1` SKIP by design.
- The three target-build checks and both `build_host_tests.ps1` scripts enter
  `tools/build_gate.ps1`'s machine-wide heavy-build gate before their real build step (see
  COMMON.md "Heavy builds") -- with several sessions active, your build may print
  `build gate: waiting ...` and queue rather than starting immediately. That is the gate
  working, not a hang; never set `KILNCTL_BUILD_GATE_SLOTS=0` to skip the wait. Small
  single-exe check compiles (`check_recovery_*.ps1`, `check_commonfw_*.ps1`) use the separate
  `-Lane light` pool (`KILNCTL_LIGHT_GATE_SLOTS`, default 4) and do not wait on heavy builds;
  a new check that compiles only a few C files should do the same.
- Every new check or test gets a negative test: break the thing, watch it fail, restore the
  source by hand (never `git checkout --`), then force a full rebuild. An empty `git diff`
  proves the source, not the binaries.
- PcTools tests: `tools\PcTools\.venv\Scripts\python -m pytest <named files>`. Never
  `uv run`, never `pip install` into the shared `.venv`. SaftyFW host tests need a short
  worktree path (`C:\wt\...`).
- Any firmware C change must be covered by a full `run_all_checks.ps1` pass or an `-Only`
  regex that explicitly matches the host-test checks (`check_00_kilnfw_host_tests`,
  `check_00_saftyfw_host_tests`, `check_commonfw_ctest`); a narrower `-Only` can go green
  while a host-test build/link break ships.
- Docs: a backticked hex string of 7+ characters is read as a commit id by
  `check_doc_hash_citations.ps1`; leave crash PCs and addresses unbackticked. Editing a file
  that a doc cites by blob reddens that check; grep docs for `blob:<path>` first.
- After splitting or renaming a file, grep `tools/`, `firmware/*/tools/` and
  `tools/PcTools/tests/` for the old name and any renamed identifiers, then re-run every
  check, not just the build.

## Firmware invariants

- Register every new task for stack-margin reporting
  (`check_stack_margin_registration.ps1` enforces it). Task stack SIZES may be raised
  without asking when measured too small; report the DRAM impact when you do. What stays
  forbidden: enlarging httpd stack buffers or the zones JSON buffer, and putting large
  locals on the 8 KB httpd stack.
- A task with a PSRAM stack must not write NVS. NVS keys are 15 characters or fewer.
- Never hold a module lock across producer or blocking calls. Lock order: `s_exec.lock`
  then `s_at.lock`.
- No new HTTP route without bumping `max_uri_handlers` in the same change; the cap is
  nearly full. Never weaken an auth gate. Handlers are target-build only, so a host test
  passing says nothing about a gate.
- Pico `abs_max_temp_c` always equals the ESP's. Never alter builtin schedule values.
- LCD is 480x320; pages never scroll.
- When you reset a counter, window, timestamp or seed, ask who else holds a copy or a
  derived expectation of it (the "reset one side of a pair" class; see CLAUDE.md).
- Anything that starts a task unconditionally in early boot must gate on
  `boot_guard_is_recovery_mode()` before calling a subsystem recovery mode skips.

## Docs you own

- Keep plan docs lean: pending work only. When a line lands, update its ROADMAP row in the
  same commit. Do not add owner decisions that the prompt did not give you.

## Hand-back

Worktree path, commit hash(es), files changed, check and test results as counts,
negative-test evidence, and anything you could not verify.
