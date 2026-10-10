# Review: fwlow16 review-LOW fixes (2026-10-10)

Commits reviewed on origin/dev: `2695eac4a` (code: live save_as drop, sticky reset
fence + `relay_authority_reset_try_begin()`, floors-unknown header/notice, empty
`id=` 400, link fuzz restore, nested harness test, host-test timeout label) and
`ff7027608` (status marks in `REVIEW_FWB15_2026-10-10.md` and
`REVIEW_SMALL_BATCH_2026-10-10.md`). Reviewer: Opus. Review tree: origin/dev
`96e4d56d9`. No code was changed by this review.

No HIGH or MEDIUM findings.

## Caller questions

- **Can the sticky fence or `try_begin` wedge a later legitimate reset?** No.
  `execute_scope()` (`factory_reset.c:529`) takes the mark with
  `relay_authority_reset_try_begin()`; every path that erases nothing (late
  mode-gate refusal, flash-worker submit failure) calls `_end()`, which clears the
  fence at depth 0. Every path that erases ends in a reboot, and on
  `REBOOT_FAILED` both the HTTP and UART callers run
  `factory_reset_reboot_fallback()`. The mark only outlives a reset that already
  erased, and that boot is ending. No path found that leaves depth > 0 on a board
  that keeps running.
- **Does the drop race with concurrent saves or the save lock?** Yes, narrowly:
  LOW-1.
- **Does `g_stub_try_begin_enforce` hide a real refusal path?** Yes: it is never
  set true, so the new refusal is untested in `test_ota_http.c`. LOW-2. The real
  `try_begin` and sticky setter are covered by
  `test_second_reset_cannot_drop_kiln_nvs_fence` in `test_link_watchdog.c`.
- **Do callers and docs match?** Code callers match (L4: `kiln_configs_page.html`
  never sends an empty `id=`; no PcTools client saves kiln configs). One doc fix
  claimed as done was not made: LOW-3.

## LOW

### LOW-1. `profiles_http_drop_unpersisted()` drops outside the save critical section

`profiles_http.c:1946` (save_ex releases the save lock before returning
`persisted=false`) and `profiles_http.c:1963-1975` (the drop retakes it);
caller `profiles_live_http.c:680`.

Scenario: a live save_as allocates slot X and the file write fails. save_ex
unlocks. Before the drop takes the lock, another task (LCD, UART, the backup
flash worker) saves to explicit id X and persists successfully, or a profile
start on X begins. The drop then clears slot X anyway: a persisted profile is
gone from RAM while its file stays (RAM/storage divergence until reboot), or the
running slot is zeroed. The drop checks neither the slot generation, nor
`s_delete_pending`, nor the executor. The window is small and needs id X to be
targeted before the 500 is returned, so LOW.

Fix: roll back inside save_ex's critical section (an opt-in "drop on persist
failure" flag), or have save_ex return the generation it published and make the
drop a no-op unless the generation is unchanged and the slot is not running.

Test gap: only the fake drop in `test_profiles_live_http.c` is exercised; the
real function has no direct test. Negtest M1 (real drop never clears):
MISSED (test_profiles_http / test_profiles_live_http still pass).

### LOW-2. The "already in progress" refusal and its 409 mapping are untested

`test_ota_http.c:518` declares `g_stub_try_begin_enforce = false`; no test sets
it true, so the fake `try_begin` always succeeds. `factory_reset.c:529-533`
(refuse when `try_begin` fails, `FACTORY_RESET_ERR_MODE_GATE_REFUSED`) and the
HTTP refusal it reaches are never exercised. Negtest M2 (factory reset ignores a
`try_begin` refusal): MISSED (test_ota_http still passes).

Fix: add a test that sets the enforce flag, pre-sets the stub depth to 1, posts
a factory reset, and asserts 409 with "already in progress" and that nothing was
erased; or delete the dead flag.

### LOW-3. A-INFO-1 is marked fixed but `LINK_PROTOCOL.md` was not changed

`ff7027608` marks A-INFO-1 "Fixed in 2695eac4a (fwlow16). Wording corrected" in
`REVIEW_SMALL_BATCH_2026-10-10.md:72`, and the commit message lists "A-INFO-1
LINK_PROTOCOL wording". Neither commit touches
`firmware/CommonFW/docs/LINK_PROTOCOL.md`; line 1018 still says the op-amp
offset floor "reads as current present", and the paragraph still implies the
volatile path works on a disarmed board.

Fix: change to "can read as current present (hardware-dependent: the uncalibrated
fallback margin is 25 counts)" and say the volatile path may be refused on a
disarmed board too, since `link_task_heat_possible_probe()` does not check ARMED;
then keep or re-mark the status line.

## INFO

- `relay_authority.h:304` still says the depth counter exists so "two concurrent
  resets (HTTP and UART) cannot clear each other's mark"; a second reset is now
  refused. `relay_authority.h:323` still says factory_reset.c sets the flag before
  `relay_authority_reset_in_flight_begin()`; it now uses `try_begin`. The flag
  setter has no production caller left, only tests.
- The UART factory-reset path logs its usual "firing/autotune/sweep/restore"
  refusal text for the new "already in progress" reason, which is misleading.
- `profiles_page.html` notice says "Some saved profiles are in a degraded state
  ... cannot be edited or saved" even when only free slots have an unknown floor
  (header-only case). Better: "new saves to free slots are refused until ...".
  There is no LCD equivalent.
- `tools/host_test_exec.ps1:5` header still says a timeout "returns -1"; it now
  returns 124. KilnFW `build_host_tests.ps1` neither resets
  `$global:HostTestTimedOut` nor labels a timeout (it still counts as a failure,
  so nothing is hidden).
- `test_discrete_task_loop.c`'s nested-child run writes `nested_child_stderr.txt`
  into the caller's CWD; the SaftyFW script does not `Push-Location`, so a failed
  read can leave it in the repo root. None was left in this review's runs.
- `test_link_task_fuzz.c`: `after.flags == before.flags` is redundant after the
  full `memcmp`.
- If `cfg_fs_write_atomic()` renamed the file but its read-back verify failed,
  the file can exist after the drop; the profile then reappears after a reboot.
  A retry that lands in the same slot overwrites it.
- The rewritten `used_bitmap_load` comment (ESP-IDF v6.0.2 NOT_FOUND) is
  accurate.

## Tests run (worktree `C:\wt\rvfwlow16_jkxv19`, origin/dev `96e4d56d9`)

- KilnFW `build_host_tests.ps1 -Only 'link_watchdog|ota_http|profiles_http|profiles_live_http|kiln_cfg_http'`:
  11/11 built and passed.
- SaftyFW host tests: all passed (`test_link_task_fuzz` 32329 checks, 0
  failures; the nested-child section ran; no stray stderr file).
- `tools/negtest.ps1` mutations:
  - M1 real `profiles_http_drop_unpersisted()` never clears: MISSED (LOW-1).
  - M2 `execute_scope()` ignores a `try_begin` refusal: MISSED (LOW-2).
  - M3 fence setter not sticky: CAUGHT (`test_link_watchdog`).
  - M4 `try_begin` never refuses: CAUGHT (`test_link_watchdog`).
  - M5 empty `id=` treated as absent: CAUGHT (`test_kiln_cfg_http`).
  - M6 floors-unknown header also set for used slots: CAUGHT.
  - M7 live save_as without the drop call: CAUGHT (`test_profiles_live_http`).
  - S1 task_harness nested guard removed: CAUGHT (SaftyFW host tests).
  The M5-M7 run reported verdict ERROR only because this review's draft file
  appeared in the worktree during the run; its baseline passed and all three
  mutations were CAUGHT.
