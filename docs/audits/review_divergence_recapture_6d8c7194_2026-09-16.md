# Adversarial review of 6d8c7194 — "Move the divergence recapture poll off the UI; close review items A-H"

Date: 2026-09-16
Reviewed commit: `6d8c7194` (parent `69441590`)
Source audit whose items this commit claims to close:
`docs/audits/review_divergence_wiring_60d6552f_2026-09-15.md`

Method: clean worktrees of both commits under `C:\wt\`, full host-test builds from
scratch into purged build directories, one production poison test, one item-G
reproduction experiment, and `tools/run_all_checks.ps1 -ExecutionPolicy Bypass
-AllowFewerChecks` run in the foreground against both trees. No claim below rests
on `check_00`.

## Verdicts

| Item | Claim | Verdict |
|------|-------|---------|
| A | flash-worker lint fixed by deleting the UI dispatch | **closed** |
| B | bounded dispatch with a 50 ms cap | **partial** |
| C | discarding the return value is correct because the next poll retries | **partial** |
| HIGH1 | poll moved to `safety_poll_task`, display-independent | **closed, with a new risk** |
| HIGH2 | documented, deliberately not fixed | **reasoning is false** |
| LOW7 | all `active_id` writers routed through the drop hook | **closed** |
| E | overflow fixed by bounding content | **not closed** |
| F | `s_latch_evaluated_generation` now under the lock | **closed** |
| G | include removal reverted as a live defect | **closed and independently reproduced** |
| H | pragma gone, explicit bounded copy | **closed, unpinned** |

## Item E — not closed (highest-ranked finding)

The commit's own standard is that a headroom figure is vacuous if the worst-case
mirror omits fields the handler emits. By that standard the new 52-byte figure is
also vacuous: `render_worst_case_status_json()` in
`firmware/KilnFW/App/test/test_dashboard_json.c` still omits five fields that
`dashboard_status_http.c` emits —

- `safety_tc_is_separate_sensor` (`dashboard_status_http.c:368`, unconditional)
- `diag_boot_reason`, `diag_boot_stack_overflow`, `diag_boot_malloc_failed`,
  `diag_boot_assert_failed` (`dashboard_status_http.c:546-552`, inside
  `if (ds->diag_ever_received)` — a condition the mirror itself sets true)

Adding exactly those five to the mirror and rebuilding measures **5353 bytes
against `DASHBOARD_JSON_STATUS_BUF_SIZE` = 5248, i.e. −105 bytes of headroom**,
failing at `test_dashboard_json.c:935` ("headroom has shrunk below this file's own
50-byte minimum margin"), `2 FAILURE(S)`.

So the commit reduced the overflow but did not eliminate it, and the remaining
overflow no longer needs a divergence reason to be present — only
`diag_ever_received`, which is the normal operating state of a linked board. The
`APPEND` truncation path returns HTTP 500 for the **whole** status document, so
the dashboard loses every field, not just the diagnostics.

Per the standing constraint, the remedy is more bounding, never more bytes: drop
or shorten the four `diag_boot_*` fields (they are boot-time diagnostics that do
not belong in a 1 Hz status poll and are already available elsewhere), or move
that quartet behind an explicit `?diag=1` query parameter.

The key-migration sub-claim does hold: repo-wide search finds
`safety_standing_diverged` only in audit prose and `safety_diverged` only in the
handler and the mirror. No web asset, LCD page or tool reads either key.

## HIGH2 — declined for a reason that is false

The stated reason is that `apply_pairs_ex()` stages before committing, so a
refusal writes nothing and creates no divergence. Two paths in
`firmware/KilnFW/App/drivers/http/safety_cfg_http.c` return `ok=false` *after* the
Pico has committed:

1. `confirm_commit_landed()` returns false when the post-commit refetch fails —
   its own text is "the safety processor accepted the commit but this board could
   not read the config back to confirm it -- treating the write as UNCONFIRMED,
   not successful".
2. `estop_verification_clear()` failing on `SAFETY_PARAM_ID_ESTOP_ACTIVE_LEVEL`,
   where the code comment states "the commit DID land".

In both, `if (ok && commit)` skips the recapture, so the ESP's captured Pico half
goes stale against a Pico that genuinely changed — a divergence *created* by a
"refusal". The remedy is not to recapture unconditionally (that would launder a
real fault, and the commit is right about that) but to distinguish
"staged-but-not-committed" from "committed-but-unconfirmed" and raise the standing
divergence latch explicitly in the second case.

Mitigation, and why this is not a blocking finding:
`kiln_cfg_store_capture_expected_pico_fields()` explicitly excludes
`SAFETY_PARAM_ID_ABS_MAX_TEMP_C` and `SAFETY_PARAM_ID_TC_TYPE`, so this staleness
cannot desynchronize `abs_max_temp_c`.

## Finding 3 (bounded acquisition) — CLOSED 2026-09-16, `c616391d`

Fixed same-day by `c616391d` ("Stop the S6b heartbeat stalling behind a flash
write (HIGH 1 of the 60d6552f review)"), an ancestor of `origin/main` as of
this note. `safety_poll_service_pico_half_recapture()`
(`firmware/KilnFW/App/drivers/safety/safety_link_poll.c`) no longer calls
`uart_bridge_ext_run_on_flash_worker_timeout()` (bounded acquire, unbounded
`xSemaphoreTake(s_bx_done, portMAX_DELAY)` await) in front of the GET_STATUS
heartbeat send. It now calls `uart_bridge_ext_post_on_flash_worker()`, which
posts the job and returns immediately without waiting for completion; the
job (`safety_poll_pico_half_recapture_job`) runs later on `bx_flash_worker`'s
own stack. The pending flag stays set until the posted job actually
completes, so a refused/busy post is retried
`SAFETY_POLL_RECAPTURE_INTERVAL_MS` (5 s) later rather than being silently
dropped — the file's own comments name this exact review and finding as the
motivation. `safety_poll_task` can therefore no longer be stalled by the
autosave's NVS write, closing the S6b LINK_DEAD spurious-trip risk this
finding described. No further action needed here.

## Items B, C and HIGH1 — bounded acquisition is not a bounded job (superseded, see above)

`firmware/KilnFW/App/drivers/common/flash_worker.h` states that the `_timeout()`
bound applies **only to acquiring the worker**; once acquired, `fn(arg)` is
dispatched and awaited exactly like the unbounded variant, and the header warns
that "nothing here MECHANICALLY enforces that a `*_timeout()` caller's job stays
short" and that such a caller "MUST dispatch a short job". The new caller
dispatches `kiln_cfg_store_autosave_from_live()` — an NVS store write the source
audit itself characterised as not short. The 50 ms cap is therefore real but
narrow: it bounds the queueing wait, not the stall.

That matters more here than it did on the UI task, because the poll moved onto
`safety_poll_task`, which is the sole sender of the periodic context/GET_STATUS
frames. Blocking the LVGL task froze the LCD; blocking `safety_poll_task` stalls
the link heartbeat, and the Pico trips S6b `SAFETY_TRIP_LINK_DEAD` at
`link_timeout_s` (default 10.0 s, `firmware/SaftyFW/src/safety_guards.c:27`)
against a 500 ms poll period. An autosave that stalls past 10 s trips the kiln.
The direction of that trip is fail-safe (heaters off), so it does not breach
either invariant, but it is an unplanned trip that the previous owner could not
cause. The recapture call sits at `safety_link_poll.c:448`, before the exchange,
so the stall is directly in front of the next frame.

Item C's "the next poll retries" claim is genuinely true for the first time — the
5 s throttle plus the still-set pending flag make the discarded return value
recoverable. The header's separate instruction that a timeout "caller must show
that as a real 'busy, try again' outcome, not silence" is still not met: the site
`(void)`-discards without logging.

## Item G — reproduced, and the hardening is missing

Independently confirmed by removing `#include "safety_cfg_http.h"` from
`safety_ceiling_sync.c` and rebuilding:

```
safety_ceiling_sync.c(216): warning C4013: 'safety_cfg_http_set_and_confirm_f32' undefined; assuming extern returning int
FAIL test_zones_http.c:9787: target must be the configured max (1200) EXACTLY ... (got 0.0000, want 1200.0000 +/-0.0000)
```

The mechanism and the "compiler only warned" claim are both exact. The include was
correctly kept.

The same implicit-declaration shape is live in a production driver today:
`drivers/persist/zones_config_convert.c:656` calls `snprintf` with no `<stdio.h>`
in scope (`warning C4013: 'snprintf' undefined`). That instance is benign — no
float arguments, and `int` is the correct return type — but it proves the class is
unpoliced.

**It can be made an error.** `firmware/KilnFW/CMakeLists.txt` carries no
`add_compile_options` and `sdkconfig.defaults` sets only
`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`, so no `-Werror` of any kind is in force.
Adding

```cmake
idf_build_set_property(COMPILE_OPTIONS "-Werror=implicit-function-declaration" APPEND)
```

would turn this whole class into a build failure. The `snprintf` site must be
fixed first, or the build breaks on it.

## LOW7 — closed

Six guarded `s_store.active_id` assignment sites (`kiln_cfg_store.c:314, 911, 927,
1200, 1349, 1632`), each preceded by the drop hook. The bulk writes into `s_store`
that bypass the hook — `migrate_store_v2_to_v3(v2, &s_store)` at 413 and 457,
`hal_kv_get_blob(..., &s_store, ...)` at 479, and `s_store = *resolved;` at 600 —
all execute inside `nvs_load_store_with_cfg_fs()`, called from
`kiln_cfg_store_init()` before any hook could matter: `s_pico_half_dirty` and
`s_pico_half_dirty_slot` are file statics (lines 2021-2022), never serialized into
any blob or NVS key, so they are false at that point on every boot. No unhooked
writer exists.

The `reset_to_defaults()` claim is structurally correct for the same reason: the
flag lives outside `s_store`, so `memset(&s_store, 0, sizeof(s_store))` at line 312
cannot clear it, and the unconditional drop at 311 is the right shape.

## Stack budgets

The implementer's "all numbers are INDETERMINATE lower bounds" framing is correct
and confirmed: `check_all_task_stack_budgets.ps1` reports "0 of 28 tasks fully
measured", every walk hitting an unresolved `callx4/8/12`.

The concern that moving the poll onto `safety_poll_task` adds the autosave chain to
that task's depth does **not** materialize: `safety_poll`'s walk contains no
`kiln_cfg_store` / autosave / recapture symbols at all, because the work is
dispatched to `bx_flash_worker` rather than run inline. Deepest measured chain is
`safety_poll_task -> safety_build_and_send_context -> uart_protocol_send_broadcast
-> frame_and_send -> hal_uart_send_blocking -> uart_write_bytes -> uart_tx_all ->
uart_enable_tx_write_fifo`, 2848 B against a 3136 B ceiling, honest free 5044 B.
`lvgl` 4544 B / 4880 B ceiling / 3348 B honest free and `httpd_worker` 2088 B
honest free (LOW, check OK) both match the claims.

The real gap is elsewhere: `bx_flash_worker` measures **48 B total, 7844 B honest
free** — an absurd lower bound, because every dispatched job body is reached
through a function pointer and so is invisible to the walk. Nothing in the checker
covers the worker's actual depth, and the autosave chain that historically measured
7104 B inline on the LVGL task now runs there against an 8192 B stack. That is not
a defect introduced by this commit, but it is where the remaining risk sits, and
the commit moved more traffic onto it.

## Counts

All builds run in the foreground, captured whole to a log file and extracted from
the saved log — never piped through a truncating filter.

KilnFW host tests (`build_host_tests.ps1`), both trees:

- `6d8c7194`: `Built: 47/47 executables`, `all 47 host test executables built and
  passed`, exit 0, 2 SKIPs.
- `69441590`: `Built: 47/47`, all passed, exit 0, 2 SKIPs.
- Both SKIPs are `sim_credibility_gate` and `firing_score_from_capture`, each
  missing the gitignored `logs/coupling/noise_floor_p7_run1.jsonl`. Expected on a
  fresh clone.
- Item E's measurement at `6d8c7194`: `/api/status worst-case render: 5196 bytes
  (strlen), against DASHBOARD_JSON_STATUS_BUF_SIZE=5248 -- measured headroom = 52
  bytes`.

SaftyFW host tests at `6d8c7194`: `2568/2568`, `56/56`, `270/270 checks passed`.
The commit message's "259/259" does not match any figure this suite prints.

`tools/run_all_checks.ps1 -ExecutionPolicy Bypass -AllowFewerChecks` (93 checks in
a clean worktree; `-AllowFewerChecks` passed symmetrically to both trees, since a
fresh worktree lacks `tools/PcTools/selfcheck.py`'s venv and the run otherwise
exits 2 before running anything):

- `6d8c7194`: 87 passed / 1 skipped / 5 failed.
- `69441590`: 86 passed / 1 skipped / 6 failed.
- Commit failures: `check_main_task_stack_budget`, `check_mcp_facade_coverage`,
  `check_mykicad_golden_suite_runs`, `check_zones_per_zone_field_drift`,
  `check_doc_hash_citations`. Parent failures are those same five **plus**
  `check_flash_worker_lint`.
- The strict-subset claim holds. The absolute numbers do not match the claimed
  89/0/4 vs 85/1/7.
- `check_main_task_stack_budget` fails in **both** trees ("could not read
  CONFIG_ESP_MAIN_TASK_STACK_SIZE from sdkconfig") — a clean-worktree provisioning
  artifact, not on the known-not-theirs list. The single SKIP in both trees is
  `check_01_kilnfw_pushed_build`.
- Item A verified directly rather than through the aggregate:
  `check_flash_worker_lint.ps1` exits 1 at the parent, naming
  `drivers\ui\ui_page_home_refresh.c:259`, and exits 0 at the commit ("clean, 214
  driver files scanned").

## Poison test

Mutation: removed the LOW7 drop call at
`firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c:1630`
(`pico_half_dirty_drop_if_owned_by(s_store.active_id);` in
`kiln_cfg_store_set_active_id_raw()`), then rebuilt every host-test executable from
a purged build directory.

Result — the assertion is load-bearing:

```
FAIL C:\wt\divrev_k7q2m9\firmware\KilnFW\App\test\test_kiln_cfg_store.c:2505:
  the pending flag was dropped -- it was owed to slot A, not B, and A is no longer active
7923/7924 checks passed
1 FAILURE(S)
RUN FAILURES (1): main
```

Restored by hand (no `git checkout --`, `git restore` or `git stash`), confirmed
`git hash-object` = 76630ccadd80a24556b2e0cfbf9f7d86bedcf4ef (a blob hash, not a
commit), identical to
`git rev-parse 6d8c7194:firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c`, with
an empty `git diff`, then forced a full rebuild into a purged directory before any
further measurement: `Built: 47/47`, all passed, headroom back to 52 bytes. The
same hash-and-rebuild discipline was applied to the item-E and item-G experiments
(`test_dashboard_json.c` = `0b96d170...`, `safety_ceiling_sync.c` = `99601327...`,
both matching the committed blobs).

Item H has no regression pin at all — no test exercises the bounded-copy path with
a reason long enough to reach the truncation boundary. The code is correct by
inspection, but a future edit to `kArmedSuffix` or to
`sizeof(s_standing_warning_reason)` would not be caught.

## Invariants

- **Nothing may make it possible for the Pico to end up not armed.** Not breached.
  No path in this commit disarms the Pico or removes an arming step. The one new
  failure mode (a long autosave stalling `safety_poll_task` past `link_timeout_s`)
  drives the Pico to *trip*, which de-energizes — the fail-safe direction.
- **The Pico's `abs_max_temp_c` must always equal the ESP's.** Not breached.
  `kiln_cfg_store_capture_expected_pico_fields()` explicitly excludes
  `SAFETY_PARAM_ID_ABS_MAX_TEMP_C` from the captured Pico half, so neither the
  HIGH2 staleness nor the recapture deferral can move it. The only code that could
  have broken it — item G's implicit declaration writing the ceiling as 0 — was
  correctly reverted, and is now confirmed to fail `test_zones_http.c:9787` if it
  ever returns.

## Ranked findings

1. **Item E is not closed.** The status document still overflows its buffer with
   only `diag_ever_received` true; five handler-emitted fields are missing from the
   mirror. Fix by bounding content, never by enlarging the buffer.
2. **HIGH2's stated justification is false.** Two post-commit failure paths create
   exactly the divergence the comment says cannot occur. Not `abs_max_temp_c`, so
   not blocking.
3. **A bounded acquisition is not a bounded job.** The autosave now sits in front
   of the safety link's own heartbeat; past `link_timeout_s` (10 s) it trips S6b.
4. **`-Werror=implicit-function-declaration` is absent and addable**, with one
   production site (`zones_config_convert.c:656`) to fix first.
5. **`bx_flash_worker`'s stack is unmeasured** (48 B lower bound) while this commit
   routes more work through it.
6. Item H has no regression pin.
7. `check_main_task_stack_budget` fails in a clean worktree in both trees.

## Is this safe to stop iterating on?

No. Finding 1 needs another pass: the status endpoint returns HTTP 500 for the
whole document under ordinary conditions, and the check that was supposed to prove
otherwise still measures a mirror that is not self-contained. Findings 2 and 3 are
real but bounded and can be scheduled. Neither invariant is breached, so nothing
here blocks flashing.
