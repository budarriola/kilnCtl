# Adversarial review: the divergence-fix wiring commit (this file's name carries the commit id)

Date: 2026-09-15
Subject: the commit titled "Fix HIGH1/HIGH2/MEDIUM3-6/LOW7-10 from
review_divergence_fixes_b2e7017f_2026-09-15.md", already pushed to origin/main,
15 files.
Reviewer: independent session. Review only; no code changed.

Verification base: a clean worktree of origin/main at that commit, and a second
clean worktree at its parent, both under `C:\wt\`, both built for `esp32s3`
through the PowerShell tool (never Bash -- `idf.py` no-ops under MSYS). Host
tests built with `build_host_tests.ps1`; SaftyFW host tests from a `C:\wt\` path.

## Verdict per finding

| Finding | Verdict |
|---|---|
| HIGH1 (zero-caller recapture-pending) | **Partially closed.** A real consumer now exists, but the consumer is the wrong owner and the dispatch is not what its own comment claims. See "New defects" A, B, C. |
| HIGH2 (`ok && commit` wedge) | **Not closed -- mitigated only.** `safety_cfg_http.c:1308`'s `if (ok && commit)` is unchanged; `kiln_cfg_store_recapture_pico_half_confirmed()` still never runs on a refusal. What landed is an operator *hint*: `safety_cfg_http_recent_armed_refusal()` widens the standing warning text for 5 minutes after a refusal. The original recommendation ("at minimum the refusal class should be surfaced") is met; the wedge itself is not. |
| MEDIUM3 (refetch racing the latch) | **Closed.** `safety_cfg_store_cache_generation()` bumps at the single success choke point in `safety_cfg_store_refetch_locked()`; autosave defers when it disagrees with `safety_ceiling_sync_latch_evaluated_generation()`. Minor: the stamp is written and read outside `s_divergence_state_lock` (New defect F). |
| MEDIUM4 (tc_type wedges the gate) | **Closed.** `kiln_cfg_store_capture_expected_pico_fields()` skips `SAFETY_PARAM_ID_TC_TYPE`, matching the existing `abs_max_temp_c` exclusion. Covered by a test that fails against the parent. |
| MEDIUM5 (lock held across producer calls / stack blob) | **Closed.** Hooks are invoked outside `s_divergence_state_lock`, logging moved out via `should_log`, `standing_reason` moved off the httpd stack. `check_httpd_task_stack_budget` OK, honest free 2088 B (LOW, unchanged class). |
| MEDIUM6 (truncating/unescaped reason) | **Closed, with a new risk.** Full-size static buffers plus `json_escape()` in both `dashboard_status_http.c` and `kiln_cfg_http.c`. New risk: the status reason can now contribute up to 321 escaped bytes where it was capped at 80 (New defect E). |
| LOW7 (pending flag vs. slot change) | **Partially closed.** `pico_half_dirty_drop_if_owned_by()` is hooked into `kiln_cfg_store_set_active_id_raw()` only. Five other writers of `s_store.active_id` bypass it: `kiln_cfg_store.c:302` (`reset_to_defaults`), `:896` and `:910` (both boot-time clears), `:1175` (save path), `:1319` (apply path). The original finding named `:302/:896/:910` explicitly. |
| LOW8 | Closed (dirty flag now slot-qualified, under its own lock). |
| LOW9 | Closed -- stale-flag clear lives at the refetch success choke point, proven by a test that fails against the parent. |
| LOW10 | Closed -- the three previously-defined-but-never-called tests are wired into `run_test_kiln_cfg_store()` and are load-bearing (table below). |

## Is a 1 Hz UI refresh callback the right owner for a divergence poll?

No. Three separate dependencies on display state, none of them stated at the
call site:

1. The timer is created in `ui_page_home.c:1027`, inside the home page's
   `build()`. Pages are built lazily by `kiln_ui_show()`
   (`if (!page->screen) { page->screen = page->build(); ... }`). A boot that
   goes to `touch_cal` instead (`kiln_ui.c:285-288`) never builds home, so the
   timer never exists and the deferred recapture never runs -- for the whole
   boot, silently. That branch is currently unreachable on this hardware
   (no self-calibrating touch device is wired), but it is a live code path
   whose only guard is a board-population accident.
2. The poll is nested inside `if (s_ui_home_lag_notice != NULL)` at
   `ui_page_home_refresh.c:212`. A UI object's existence now gates a config
   correctness action. Nothing in either module says so.
3. Recovery mode and any path that stops or starves the LVGL task stop the
   poll with it.

This is the repo's own consumer-without-producer / one-side-only-wiring shape
turned inside out: a producer that exists, with a consumer reachable only
through the display subsystem. The correct owner is a periodic non-UI task that
already exists for config/safety housekeeping, or the flash worker itself on a
timer. Ownership should move before this is called closed.

## New defects

**A (HIGH, introduced by this commit): `check_flash_worker_lint` fails.**
Measured, both worktrees, same command:

- parent: `flash_worker_lint: clean (213 driver files scanned, ...)`, exit 0
- this commit: exit 1, naming
  `drivers\ui\ui_page_home_refresh.c:259: (void)uart_bridge_ext_run_on_flash_worker(ui_home_pico_half_recapture_job, NULL);`
  -- "no nearby is_on_flash_worker() guard or 'not reachable on-worker'
  justification comment".

So a repo check that passed at the parent fails at this commit, on this
commit's own new line. Whatever else is broken on mainline, this one is this
commit's. (The underlying re-entrancy is in fact safe -- see D -- so the fix is
the missing justification comment or guard, not a code change.)

**B (HIGH): the dispatch blocks `lvgl_task` without bound, and the comment says
the opposite.** The call-site comment claims "fire-and-forget ... the LVGL task
must not block waiting for it". `bx_run_on_internal_stack()`
(`uart_bridge_ext.c`) takes `s_bx_lock`, queues the job, then
`xSemaphoreTake(s_bx_done, portMAX_DELAY)` -- it blocks the caller until the job
returns, behind any other caller's in-flight job. `flash_worker.h` says exactly
this, and says a bounded sibling
(`uart_bridge_ext_run_on_flash_worker_timeout()`) was added the same day
*specifically* so `lvgl_task` would stop using the unbounded entry point, citing
"can freeze the whole LCD for as long as some OTHER caller's job (a profile /
package import, a cfg_fs write) takes, with no bound and no operator feedback".
The header also requires a *short* job; an autosave-to-flash is not short. This
call should use the bounded variant, and the comment is factually wrong as
written.

**C (MEDIUM): backpressure is an LCD freeze, not a drop.** The job queue is
1-deep and mutex-serialised, so nothing piles up and nothing is silently
dropped -- the cost lands entirely on the caller as blocking (see B). The
comment's "a dispatch failure (worker busy/full) just means the next tick tries
again" describes behaviour the code does not have.

**D (clean): flash-worker re-entrancy is genuinely safe.**
`bx_run_on_internal_stack()` compares `xTaskGetCurrentTaskHandle()` against the
worker handle and runs the job inline in that case; `ui_home_refresh_cb()` runs
on `lvgl_task` and is not reachable from the worker. No deadlock. This is the
one part of the highest-risk item that survives scrutiny unaltered.

**E (LOW-MEDIUM): the status endpoint's truncation exposure grew.**
`dashboard_status_http.c` replaced `%.80s` with a full escaped reason
(`CONFIG_DIVERGENCE_REASON_MAX * 2 + 1` = 321 B) inside a 5248 B buffer whose
`APPEND` overflow path is `goto truncated` -- an `ESP_LOGE` and a 500 for the
whole `GET /api/status`, i.e. one long divergence reason can take the entire
status endpoint down rather than truncating one field. `test_dashboard_json.c`
asserts >= 50 B headroom against its worst case; that worst case should be
re-derived with the 321 B reason included.

**F (LOW): `s_latch_evaluated_generation` is written and read outside
`s_divergence_state_lock`** (`safety_ceiling_sync.c:323`, `:561`), while every
other field of that state is now lock-protected. Benign on this MCU for a
`uint32_t`, but it is the one field left outside the invariant the MEDIUM5 lock
was introduced to establish.

**G (LOW): layering.** `safety_ceiling_sync.c` now `#include`s
`safety_cfg_http.h` -- a safety module reaching into an HTTP module for an
operator-interaction timestamp. The timestamp belongs in the safety/config layer
with the HTTP handler as its writer.

**H (LOW): `#pragma GCC diagnostic ignored "-Wformat-truncation"`** around the
ARMED suffix append (`safety_ceiling_sync.c:538-542`) suppresses the diagnostic
for any future edit to that `snprintf`, not just this one.

## Claim audit (commit message vs. measurement)

- **"Also fixes two unrelated origin/main build breaks ... `-Werror=format-truncation` ... implicit declaration of `hal_time_now_us()`": false.**
  The parent worktree builds clean for `esp32s3` (exit 0, full link). There is
  no such break on mainline; the reported break is a target-selection artifact
  (a fresh worktree with no `CONFIG_IDF_TARGET` defaults to `esp32` and fails in
  `hal_sysinfo_esp.c`, unrelated to either file). Both "fixes" are in fact
  *required by this commit's own new code*: the truncating `snprintf` is the
  ARMED suffix this commit adds, and `hal_time_now_us()` is called by the
  refusal-window code this commit adds. Presenting them as pre-existing
  mainline breakage misattributes this commit's own cost.
- **"46/47 host-test executables build (missing one is an unrelated in-flight
  file from another session)": false as stated -- the gate break is this
  commit's.** This commit's `build_host_tests.ps1` adds the
  `heat_owner_active_decide` recipe and bumps `$totalExpected` 46 -> 47, while
  `test_heat_owner_active_decide.c` exists in no git revision (`git cat-file -e`
  fails at this commit, its parent, and origin/main). Measured: parent
  `Built: 46/46`; this commit `Built: 46/47`, `BUILD FAILURES (1)
  heat_owner_active_decide`, script exit 1. The dangling recipe was committed
  here, so the red gate on clean origin/main is attributable to this commit
  (since resolved by another agent's later commit).
- **"lvgl total 4544 B vs 4880 B ceiling": confirmed.** Measured from this
  commit's own clean-worktree ELF:
  `+ 3792 B deepest known dispatch-target callback (ui_home_refresh_cb)`,
  `total 4544 B; ceiling 4880 B; honest free 3348 B (40.9% of 8192 B)`.
  The 7104 B figure is not reproducible from any committed state (it was the
  uncommitted inline variant), so it is taken on report. The checker marks lvgl
  INDETERMINATE (unresolved indirect call) -- a lower bound, not a pass, which
  the commit message does not say.
- **"check_httpd_task_stack_budget.ps1: OK ... honest free 2088 B": confirmed**
  (classified LOW).
- **Negative test (poison the tc_type exclusion, full rebuild, hand-restore,
  empty diff, second full rebuild): sound in form and target.** The named
  failure site, `test_kiln_cfg_store.c:2475`, is exactly the assertion that
  fails when the exclusion is absent -- independently reproduced here by
  building that test file against the parent (table below), which is a stronger
  demonstration than poisoning. The forced second rebuild is the right
  procedure for this repo's prior poisoned-binary incident.
- **"main host-test binary 7889/7889 checks pass": confirmed** at this commit.
- SaftyFW host tests at this commit: `259/259 checks passed`.
- `run_all_checks.ps1` in a clean worktree: 90 passed, 3 failed --
  `check_flash_worker_lint` (defect A, this commit's),
  `check_all_task_stack_budgets` (`gpio_probe` root symbol, a known checker
  defect on any clean worktree, owned elsewhere), and
  `check_zones_per_zone_field_drift` (`ModuleNotFoundError: kilnctrl`, a clean
  worktree provisioning gap).

## Per-test table: the three newly-wired tests, against the parent

Method: clean parent worktree; overlaid **only** `test_kiln_cfg_store.c` from
this commit. That alone does not link (`unresolved external symbol
test_safety_cfg_store_stage_page_for_kiln_cfg_store_test`,
`unresolved external symbol safety_cfg_store_cache_generation`), so
`test_safety_cfg_store.c` was overlaid too and a review-only, never-committed
shim `safety_cfg_store_cache_generation() { return 0; }` was appended to that
test file -- constant 0 being exactly parent behaviour (the parent has no
generation counter). No production file was modified in the parent worktree.

| Test | vs. parent | vs. this commit |
|---|---|---|
| `test_autosave_defers_when_cache_generation_moved_since_latch_evaluated` | **FAIL** -- `:2418` "the real refetch actually bumped the generation"; `:2427` "the generation mismatch alone defers the Pico-half recapture"; `:2428` "reason explains the deferral" | PASS |
| `test_capture_expected_pico_fields_excludes_abs_max_and_tc_type` | **FAIL** -- `:2475` "exactly one field survives -- abs_max_temp_c and tc_type are both excluded" | PASS |
| `test_set_active_id_raw_drops_pending_flag_owned_by_old_slot` | **FAIL** -- `:2505` "the pending flag was dropped -- it was owed to slot A, not B, and A is no longer active" | PASS |

Carried along by the second overlay, also load-bearing (LOW9/LOW10 evidence):

| Test | vs. parent | vs. this commit |
|---|---|---|
| `test_cache_stale_cleared_by_direct_refetch_not_only_maybe_refetch` | **FAIL** -- `test_safety_cfg_store.c:1214`, `:1217` | PASS |

Parent run totals with the overlay: `Built: 46/46`, `7 FAILURE(S)` -- the five
assertion failures above plus the two lines' shared test bodies; no other test
regressed. Every failure is a behavioural assertion, not a missing-symbol or
API-does-not-exist artifact (the two link-level gaps were resolved before
measuring, as described). All three tests are therefore genuinely load-bearing.

## What should happen next (not done here)

1. Move the divergence poll off `ui_home_refresh_cb()` to a non-UI owner.
2. Switch the dispatch to `uart_bridge_ext_run_on_flash_worker_timeout()` and
   correct the call-site comment; add the guard or the
   "not reachable on-worker" justification the lint requires.
3. Close HIGH2 properly: recapture (or explicitly latch a fault) on a refused
   commissioning write, rather than only widening the warning text.
4. Route the remaining five `s_store.active_id` writers through
   `pico_half_dirty_drop_if_owned_by()`.
5. Re-derive `test_dashboard_json.c`'s worst case with a full-length escaped
   divergence reason.

Nothing found here suggests loosening the policy that a config divergence is a
fault: every defect above is about *when and by whom* the fault is noticed, not
about reconciling it silently. `abs_max_temp_c` and `tc_type` remain excluded
from the broadened field set only because the ESP has no push path for them;
the ceiling latch that keeps the Pico armed is untouched by this commit.
