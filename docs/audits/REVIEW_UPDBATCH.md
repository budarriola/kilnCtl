# Review UPDBATCH: update-path batch fixing REVIEW_UPDLOW and REVIEW_STAGEVERFX2 (2026-10-13)

Adversarial review of the commits that close `REVIEW_UPDLOW.md` and
`REVIEW_STAGEVERFX2.md`, as landed on origin/dev with a bare `git push` (not
`tools/land.ps1`):

- `c06550dcc` code and tests: dev `PROJECT_VER` composed in `CMakeLists.txt`
  so `-dirty` survives the 31-char `esp_app_desc_t.version`; rate-limit
  classification (`update_status_is_rate_limited`, `access_denied`), reset
  backoff only when `X-RateLimit-Remaining` is 0, backoff cleared by a settings
  POST, wrap-safe backoff expiry, 10-slice stall patience, header loop bounded
  by the job deadline, `wr_late_exited` on the missed-give path
- `457ebc80b` tests (429 with Remaining > 0, reset-window clamp)
- `0f4eeb8ea`, `20236041e` docs (fix status in both review docs)

Also checked: the `fake_support.c` rebase merge against UPDFX4 (`2cf5ace78`)
and the interaction of these fixes with UPDFX4's measured-draw heap thresholds,
DMA abort and settle cancel.

Result: 0 HIGH, 0 MED, 1 LOW, 8 INFO. Every claimed fix is correct. The
missing LOW-1 test is added here (`9d5514ef8`, with a fake fix in `386f897e7`)
and negtested CAUGHT. The `dl >= 6u` boundary was negtested at each site
separately: the declared site is CAUGHT, the undeclared site is an equivalent
mutant (see below).

## Verification of each claimed fix

### REVIEW_UPDLOW MED-1 (rate-limit classification): correct

`update_status_is_rate_limited()` (`update_url.c`) answers true for any 429 and
for a 403 only with `Retry-After` or `X-RateLimit-Remaining: 0`.
`update_rate_backoff_s()` uses `X-RateLimit-Reset` only for a real limit whose
Remaining is absent or 0, so a 5xx never inherits the window and a 429 with
Remaining > 0 falls to the default (pinned by `457ebc80b`). A plain 403 is now
`access_denied` (`update_fetch.c:718`). No web page or PcTools client
enumerates the fetch failure reasons (grep for `rate_limited`/`server_error`
finds only the firmware and unrelated routes), so the new reason string breaks
no consumer. The settings POST clears the backoff; see LOW-1 below.

### REVIEW_UPDLOW LOW-1 (`wr_late_exited` on the missed-give path): correct, test added

On the second-take failure `wr_call` now sets `wr_late_exited` under
`s_wr_mux`. The writer reached arb DONE before the caller's timeout, so the
writer's own abandoned branch never runs; after `writer_done` the writer only
gives `wr_done` and does not read the job's `w`/`body`/`scratch`. `fetch_task`
(`update_fetch.c:1038`) therefore frees the buffers itself instead of parking
them in `leak_*`, which nobody would ever reclaim. The later `WR_ABORT` and
`wr_stop()` return at once because `wr_wedged` is set first, so nothing
overwrites `s_c->wr`. The flag's name says "exited" while the writer task is in
fact still alive and parked on `wr_req`; harmless, since no further op is ever
issued once wedged.

Test (new, `9d5514ef8`): `extra_final_wedge_test()` in
`test_update_http_refusals.c`, run last in the refusals executable because a
wedge is permanent per process. A fake hook (`g_fr_wr_missed_give`) drops the
writer's first `wr_done` give and lets the caller's first wait time out only
after the drop, so `caller_timeout` sees DONE and the second take fails. It
asserts the give was dropped, the writer is wedged, the job fails with
`writer_wedged_reboot_required`, the claim is balanced, and the live heap
returns to the pre-job value (the buffers were freed, not leaked).

The first version of this test was vacuous. The hook dropped the writer
thread's first give of any semaphore, and the first one was the stage lock
inside `WR_WRITE`, not `wr_done`. The op itself then wedged, the ordinary
wedge path ran, and the writer's late return freed the buffers. A mutant that
never sets `wr_late_exited` on the missed-give path passed all 101
executables. Instrumenting the mutant showed `caller_timeout` returning
wedged on op 2 and the second take never reached. `386f897e7` keys the drop to
`wr_done` (the last binary semaphore created before the `update_fetch_wr`
task). The instrumented mutant then reached the second take on op 1 and left
the live heap at 4 against 1 before the job. The negtest rerun below confirms
the mutant is caught.

### REVIEW_UPDLOW LOW-2 (stall patience): correct

`FETCH_STALL_SLICES` is 10 (about 10 s at 1 s slices). The header loop now also
breaks on `deadline_passed(w)`; that case reports `timeout`, the same reason the
body loop and the pre-open check use for a passed deadline.

### REVIEW_UPDLOW LOW-3 (backoff tick wrap): correct, residual noted

Start tick plus duration with an unsigned elapsed difference removes the
2^31-tick resurrection. An unpolled backoff can still reappear once after
2^32 ticks (about 49.7 days at 1 kHz) for at most its clamped duration; see
INFO-4.

### REVIEW_UPDLOW LOW-4 (unpinned clamps): correct

As recorded in the doc's own negtest table; not re-run here.

### REVIEW_STAGEVERFX2 MED-1 (dev version loses `-dirty`): correct, configure-time only

`CMakeLists.txt` composes `git describe --always --tags --abbrev=7` plus
`-dirty` (from `git diff-index --quiet HEAD --`) before
`include(project.cmake)` (line 77), falls back to the 7-char short hash when
the result exceeds 31 characters, and fails the configure if even that does not
fit. ESP-IDF picks up a `PROJECT_VER` already set. The 31-char fixture
`v1.0.0-pre.3-123-gabcdef0-dirty` is pinned by `test_update_stage.c`. See
INFO-1..3 for its limits.

### REVIEW_STAGEVERFX2 LOW-1 (`dl >= 6u` boundary): correct

Both sites (`update_stage.c:302` declared, `:321` undeclared) read
`if (dl >= 6u && ci_dirty_suffix(dn + dl - 6u))`. The doc said the boundary
could not be negtested because the two sites share identical text; a multi-line
find including each site's unique preceding lines does it.

- Declared site (`:302`): `>=` to `>` is CAUGHT by
  `test_update_stage.c:979`, "declared, exactly -DIRTY descriptor: refused".
- Undeclared site (`:321`): `>=` to `>` is MISSED, and the mutant is
  equivalent.
  - With `dl == 6`, a name ending in a case-variant `-dirty` is exactly that
    6-character suffix, for example `-DIRTY`.
  - That is never a parseable semver. `dry_run_header()` (via
    `stage_header_encode`'s semver check) refuses it with the same
    `BAD_VERSION`, before the gate.
  - The refused list at `test_update_stage.c:945` already includes `-DIRTY`
    undeclared and still passes under the mutant.
  - No test can tell the two operators apart at this site. The `dl >= 6u`
    guard there only protects the `dn + dl - 6u` pointer arithmetic.

### REVIEW_STAGEVERFX2 INFO items: partly closed

INFO-3 (stale `update_policy.h` comment), INFO-4 (labels, now
`STAGEVERFX LOW-1/LOW-2`) and INFO-5 (pre-rebase SHA) are fixed. INFO-1
(CMakeLists regex vs gen_build_info), INFO-2 (reconfigure message names only
the sdkconfig hash) and INFO-6 (build_kilnfw MCP tool has no release-version
reconfigure) are not mentioned in the Fix status table; they remain open. See
INFO-8.

## fake_support.c rebase merge and UPDFX4 interaction

The merge in `stubs_update_fetch/fake_support.c` keeps both sides: UPDFX4's
`g_fr_dma_seq_armed` DMA sequence block and the stall-per-chunk block from
`c06550dcc` are both present and both reset in `fr_reset()`. Both update_fetch
executables build and pass (472 and 630 checks with the new test).

UPDFX4 (`2cf5ace78`) changed the start/hop heap need, the consecutive-sample
DMA abort and the settle-wait cancel. None of it touches the writer arbitration
or the backoff. The new header-loop deadline check and the body loop's
deadline check sit next to UPDFX4's heap/DMA checks with the same "first
reason wins" order (cancel, deadline, heap, DMA), so a deadline is never
reported as a heap abort or the reverse. The new missed-give fake hook is
active only while `g_fr_wr_missed_give` is set, so it does not disturb
UPDFX4's settle-wait tests.

## Findings

### LOW-1: any successful settings POST clears the rate-limit backoff, not only a repo change

`update_settings_http.c:109` calls `update_fetch_backoff_clear()` after every
successful `update_settings_set()`, including one that re-saves the same repo.
An admin (or a script) that re-POSTs unchanged settings between check
attempts walks past the anti-hammer backoff the 429 answers enforce. The test
comment (`test_update_http_refusals.c:168`) says "cleared by a successful repo
change only", but the test changes the repo, so it does not pin the same-repo
case. Fix: compare the old and new repo (case-insensitive, as GitHub treats
them) and clear only on a real change; add a same-repo test that still gets
429.

### INFO-1: dirty is decided at configure time only

`git diff-index` runs when CMake configures. An edit after configure, followed
by `idf.py build` without a reconfigure, produces an image still labelled
clean. `diff-index` without `update-index --refresh` can also report a
touched-but-unchanged file as dirty; that fails closed (refused as dirty), so
only the first case matters. A release build is unaffected
(`KILNCTL_RELEASE_VERSION` path).

### INFO-2: the short-hash fallback is refused as `bad_version`, not by the dirty rule

When the describe form is too long, `PROJECT_VER` becomes `<shorthash>-dirty` (or
`<shorthash>`). Neither is semver, so an upload is refused as `bad_version` before
the dirty rule sees it. Fails closed; the operator message names the wrong
reason.

### INFO-3: silent ESP-IDF fallback when git is missing or describe fails

If `find_package(Git)` fails or `git describe` returns non-zero (for example a
shallow clone without tags still succeeds via `--always`, but a non-git source
tree fails), `PROJECT_VER` is left unset and ESP-IDF's own logic applies, which
is the truncating path MED-1 fixed. A configure-time `message(STATUS ...)`
naming the fallback would make it visible.

### INFO-4: backoff can reappear once after 2^32 ticks unpolled

`backoff_remaining_s()` clears `s_backoff_valid` only when polled. A backoff
noted and never polled for 2^32 ticks makes the unsigned elapsed wrap small,
and the next POST sees up to the clamped duration again. Bounded and one-shot;
not worth a fix.

### INFO-5: non-atomic start/ticks pair

`backoff_note()` writes `s_backoff_start`, then `s_backoff_ticks`, then
`s_backoff_valid`, without a lock; a POST reading between the writes can see a
new start with the old duration. The result is a slightly wrong remaining time
for one answer. Benign.

### INFO-6: a non-rate 403 still gets a backoff

`access_denied` is no longer labelled a rate limit, but `backoff_note()` still
runs for it (`update_rate_backoff_s` gives the server default). A token or
repo-permission problem therefore blocks retries for that default. Defensible
(it stops hammering a 403), but the doc says only that the label changed.

### INFO-7: missed-give wedge after WR_BEGIN or WR_WRITE leaves the stage UPLOADING

Unchanged from before the fix: on the missed-give path the writer's abandoned
branch never runs (it reached DONE first), and the job's own `WR_ABORT` returns
at once because the writer is wedged. The stage stays UPLOADING until a reboot,
which the `writer_wedged_reboot_required` error already demands. The log line
says so for the non-finish case.

### INFO-8: REVIEW_STAGEVERFX2 Fix status omits INFO-1, INFO-2 and INFO-6

The table claims "INFO (stale comment, labels, form feed, pre-rebase SHA)"
fixed. INFO-1, INFO-2 and INFO-6 are neither fixed nor listed as open.
Also, `backup_import` sets the repo through `update_settings_set()` without
clearing the backoff (its link stub is empty); that fails safe (the backoff
stays).

## Negtest

`tools/negtest.ps1 -Preset kilnfw-host`, no `-RequireAssertion`,
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
Each mutant's log was checked: it compiled and failed on an assertion, never
on a build error. Baseline passed and "real tree unchanged" on both runs.

| Mutation | Site | Verdict | Evidence |
|---|---|---|---|
| `low1_drop_late_exited`: `s_c->wr_late_exited = false;` inserted before the "completion give missing" `ESP_LOGE` | `update_fetch.c` wr_call missed-give path | CAUGHT (run 2, base `386f897e7`) | `FAIL test_update_http_refusals.c:387: missed give: fetch_task freed the job buffers itself (wr_late_exited)` |
| same | same | MISSED (run 1, base `9d5514ef8`) | The fake dropped the stage-lock give instead of `wr_done`, so the test never reached the missed-give path. Fixed by `386f897e7`. |
| `dirty_ge6_site1_declared`: declared `dl >= 6u` changed to `dl > 6u` | `update_stage.c:302` | CAUGHT | `FAIL test_update_stage.c:979` |
| `dirty_ge6_site2_undeclared`: undeclared `dl >= 6u` changed to `dl > 6u` | `update_stage.c:321` | MISSED (equivalent mutant) | A 6-char "-dirty" name with dl == 6 is not valid semver, so `dry_run_header` refuses it as `bad_version` with or without the suffix check. No observable difference. |

Logs: `%TEMP%\negtest_logs\20261013_105528_2d86` (run 1, all three) and
`%TEMP%\negtest_logs\20261013_111823_w8de` (run 2, LOW-1 only).

## Checks

`tools\run_all_checks.ps1 -Fast` (with `-ExecutionPolicy Bypass`) in the review
worktree at `386f897e7` (origin/dev `20236041e` plus the two test commits):
184 passed, 0 failed, 0 skipped, 17 SKIP-FAST (the expected missing-build-output
class). Versus the stored baseline: NEW 0, KNOWN 0. Five checks reported as FIXED
pass here because the baseline is from an older origin/dev, not because of
these commits.

## Fix status (UPDLOWFX)

- LOW-1: fixed: `settings_post_handler` copies the repo in force into the (dead) form-body buffer before `update_settings_set()` and clears the backoff only when the repo in force afterwards differs case-insensitively (or the old one was unreadable). Cost: 0 B internal static/heap, 0 B extra stack (the body buffer is reused). Test: same-repo and case-only saves keep the 429; a real change clears it.
- INFO-1..8: unchanged.
