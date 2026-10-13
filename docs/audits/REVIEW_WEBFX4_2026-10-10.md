# Review: webfx4 (web lost-update and guard fixes), 2026-10-10

Scope: origin/dev `062456379` (code) and `ffbb6827c` (doc marks). These commits fix
A1, A2, A3, A5, B1, C3 and D2 from `docs/audits/REVIEW_WEB4_TESTS_2026-10-10.md`.
Reviewed in a clean worktree detached at `ffbb6827c`. No board access.

Verdict: no HIGH findings. One MED, a test gap on the A3 headline path. The other
findings are LOW or INFO.

## Findings

### MED-1: nothing tests the LCD Apply stale-generation refusal (A3)

**FIXED (5c89d3a3b).**

The A3 fix is `ui_edit_firing_apply.c:252`, which calls
`live_profile_save_working_if_gen(candidate, true, expect_gen, ...)`; a STALE
result returns "edited elsewhere -- reopen to reload" (`:253-255`).
`test_ui_edit_firing_apply.c` never makes a web or second-page save land between
opening the page and pressing Apply. So the mutation `true, expect_gen` ->
`false, expect_gen`, which turns the gate off, still passes every
`ui_edit_firing_apply` test (negtest `lcd_check_gen_false`, MISSED, table below).
The fixer's own m02 failures (`test_ui_edit_firing_apply.c:285/287/295`) come from
a mutation that breaks Apply as a whole, not from the stale path.

The line at `:299`, "second Apply from same page is not 'edited elsewhere'", covers
only the positive case. Fix: open ctx, call `live_profile_save_working()` directly
(standing in for a web edit), then Apply, and assert that it is refused with the
"edited elsewhere" text and that the working copy is unchanged.

### LOW-1: the generation is RAM-only, so it can match a stale value again after a reboot (ABA)

**FIXED (webfx6, e36279885 and the following test commit).**

`s_live_profile_generation` (`live_profile.c:88`) is a static atomic that restarts at
0 on every boot. Two clients can hold a generation from before the reboot:

- a PcTools/MCP caller holding a `gen` value;
- a web tab that is dirty, keeps its old `lastGen` (`live_profile_page.html:396-402`)
  and survives the reboot.

After the reboot, the counter reaches that value again after the same number of
saves, and a stale edit is then accepted. The window is narrow: the firing must
survive the reboot as a resumed run with a pending live edit, and the save count
must line up. Fix: seed the counter from `esp_random()` at init. The executor's
`gen - 1` seeding (`profile_executor_run.c:784`) only needs "different from
current", so it keeps working.

### LOW-2: `live_profile_clear()` takes the save lock only for the bump

**FIXED (webfx6, e36279885 and the following test commit).**

In `live_profile_clear()` (`live_profile.c:776`), the NVS erase, file removal and
read-back all run outside `s_live_save_lock`. Only the generation bump at
`:818-820` is locked. An edit that takes the lock between the file removal and the
bump passes its gen check, writes a fresh working file and returns 200. The clear
then bumps the generation, which leaves an orphan working copy with no record.

Reaching this needs a decide (non-active) to overlap an edit (active) around the
end of a run, so it is unlikely. Fix: hold the lock across the whole clear, or
re-check for a working file under the lock before the bump.

### LOW-3: the LCD fork is not atomic with its generation read

**FIXED (1557df89a).**

When there is no pending record, Apply forks (`ui_edit_firing_apply.c:233`) and then
reads `expect_gen = live_profile_generation()` (`:245`) in a separate step. Two cases
let it save over another client's work:

- a web fork or edit that lands between the fork and the read is silently rebased
  over;
- a `live_profile_fork()` that finds an existing pending record for the same
  origin returns without a bump (`live_profile.c:717`), so the read picks up
  whatever generation another client's edits produced, and the LCD save then
  overwrites those edits.

Fix: have `live_profile_fork()` return the generation it produced, or observed,
while holding the live lock. Alternatively, keep `ctx->generation` when the fork
reused an existing record.

### LOW-4: the setup wizard A5 edit-sequence guard is undone on the common path

**FIXED (webfx6, e36279885 and the following test commit).**

`kcEditSeq` (`setup_wizard_page.html:1747-1773`) keeps `kcDirty` set when an edit
lands while a save is in flight. But most call sites do
`postStepState(...).then(loadAll)`. `loadAll` (`:3115`) calls `gGoto(current)`,
which calls `gRenderStepDetail` (`:3052`); that function sets `kcDirty = false` and
re-renders, dropping the newer edit. The test (`test_unsaved_guard_pages.js:170`)
calls `postStepState` alone, so it does not exercise this path. Fix: skip the
re-render and the dirty reset in `loadAll` when `kcEditSeq` moved during the save,
and test the `.then(loadAll)` chain.

### LOW-5: decide is still pre-check-only

The decide handler (`profiles_live_http.c`, around 600-740) checks the generation
under `s_decide_lock`, but `profiles_http_save_ex()` and `live_profile_clear()` then
run without a generation re-check under the live save lock. This is already
documented as accepted. Decide only runs once the firing is no longer active, and
edit and LCD Apply only run while it is active, so the two paths cannot both act on
the same record at the same time. Listed for completeness.

### LOW-6: tooling race in `build_gate.ps1`

**FIXED (webfx6, e36279885 and the following test commit).**

The fixer's baseline log shows a non-fatal `Get-Content` PathNotFound at
`tools/build_gate.ps1:292`, when a slot json disappeared between enumeration and
read. It is harmless, but noisy under a contended gate.

### INFO-1: the C3 premise was already met before this fix

`security_http_core.c:230-231` already destroyed both session roles and
force-locked the LCD after a successful `clear_all_credentials`. The new backend
call (`security_backend_web_auth.c:404-407`) now runs the same invalidation a second
time (four LCD force-locks instead of two). This is idempotent and harmless. Both
paths go through `web_auth_table_destroy_role(http_session_table())`, the single
session store. The caller's own session is destroyed either way, and the response
is already being written when that happens, so the caller is logged out on its
next request. The backend test (`test_security_backend_web_auth.c:357-358`) proves
only the backend-level call.

### INFO-2: lock ordering is sound

`s_live_save_lock` is a `cfg_save_lock_t`. Its take order is: registry, then flash
worker reservation (`s_bx_lock`, recursive, `portMAX_DELAY`), then the mutex. While
it is held, nothing takes the profile save mutex or `s_exec.lock`, so no ordering
cycle exists with the profile store or the executor.

On the LCD side, the LVGL lock -> `s_bx_lock` ordering already existed through the
commit dispatch, which was already `portMAX_DELAY`. The flash write and read-back
now run under the live lock. That only serializes live saves against each other, so
an LCD Apply that contends with a web edit waits about one extra save. Scratch
memory comes from `persist_scratch_alloc` (heap), not the LVGL task stack. The
registry has about 11 of 32 slots used. Factory reset is still refused through
`pref_cfg_fs_save()` (`pref_cfg_fs.c:366`), even though `live_profile` does not
call `cfg_save_lock_reset_refused()` itself.

### INFO-3: response generation equals the saved generation

`profiles_live_http.c:517` returns the `saved_gen` that its own save produced,
which is correct. The mutation that re-reads `live_profile_generation()` instead
survives (MISSED), because single-threaded host tests cannot tell the two apart.
This mutation is equivalent in single-threaded tests, not a test gap that should be
filled.

## Negative tests (run in this review)

All runs use `tools/negtest.ps1` with
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`. Each
run reports `real_tree_unchanged: true`.

| Mutation | Command | Verdict | Failing tests |
|---|---|---|---|
| `if_gen_check_ignored` (`live_profile.c`: `if (false && check_gen && ...)`) | `-Preset kilnfw-host` (full suite, baseline 1242 s) | CAUGHT | `test_live_profile.c:641/642/645`, `test_profiles_live_http.c:651-679` |
| `lcd_check_gen_false` (`ui_edit_firing_apply.c`: `true, expect_gen` -> `false`) | `-Only live_profile\|profiles_live_http\|ui_edit_firing_apply` | **MISSED** | none (MED-1) |
| `http_resp_gen_reread` (response prints `live_profile_generation()`) | same | MISSED | none (equivalent in single-threaded tests, INFO-3) |
| `http_gen_present_dropped` (`gen_present` -> `false`) | same | CAUGHT | `test_profiles_live_http.c:651/652/655/659/661/666/667/669/679` |

## The fixer's negtest ERROR (exit 2)

Run log `%TEMP%\negtest_logs\20261010_102104_94o8`. The baseline passed 7/7. All five
mutations ended in `RUN FAILURES` with real `FAIL file:line` lines:

- m01: `test_live_profile.c:641/642/645`
- m02: `test_ui_edit_firing_apply.c:285/287/295`
- m03: `test_zone_aux_convert_http.c:239`
- m04: `test_security_backend_web_auth.c:357/358`
- m05: `test_ui_profile_builder_segment_logic.c:39/40`

negtest gives an overall ERROR while every mutation is CAUGHT only when its
real-tree guard trips. The most likely cause is that the doc-mark commit landed in
the fixer's own tree while the run was in progress. This review's rvweb4 run hit an
ERROR the same way. The run directory holds no summary JSON to confirm it. The
per-mutation evidence holds, but m02 does not cover the stale path (MED-1). My own
re-runs above report `real_tree_unchanged: true` and give a clean verdict.

## Host and JS tests at the dev tip (`ffbb6827c`)

- `build_host_tests.ps1 -Only "main|live_profile|profiles_live_http|ui_edit_firing_apply|zone_aux_convert_http|security_backend_web_auth"`:
  all 7 executables built and passed.
- `test_live_profile_action_errors.js` 25 PASS, `test_unsaved_guard_pages.js` 38 PASS,
  `test_live_profile_page.js` 27 PASS, `test_live_profile_guard.js` 17 PASS.
