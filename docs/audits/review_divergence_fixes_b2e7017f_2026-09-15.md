# Adversarial review of b2e7017f (divergence-check / autosave fixes)

Date: 2026-09-15. Scope: commit b2e7017f ("Fix six defects in ESP/Pico
safety-config divergence checking and autosave"). 059a896e was inspected and
is unrelated (heat_enable release ordering); no interaction found.

Read-only review. All line numbers are against origin/main as reviewed.

## Verification first (clean worktree at origin/main, C:\wt\rvdiv)

A fresh detached worktree was created at origin/main, submodules initialised
(lvgl), and the board-tuned `sdkconfig` copied in from the main tree (the
target-build check refuses to regenerate it from Kconfig defaults).

| Check | Result |
|---|---|
| KilnFW host tests (`build_host_tests.ps1`, fresh OutDir) | PASS -- 46/46 built and passed; 2 harnesses SKIP on gitignored `logs/coupling/*.jsonl` captures (expected on a clean tree) |
| KilnFW target build (`check_00_kilnfw_target_build.ps1`, PowerShell tool, MSYSTEM cleared) | PASS -- real `KilnCtrl.bin`/`.elf` produced, freshness check satisfied |
| `check_httpd_task_stack_budget.ps1` | OK -- deepest path `cfgfs_status_get_handler` 4304 B; honest free 2088 B (25.5% of 8192 B), classified LOW |
| `check_all_task_stack_budgets.ps1` | OK (28 tasks INDETERMINATE lower-bound only, pre-existing; no LVGL/httpd regression) |

Negative test (performed, then hand-restored and force-rebuilt): the MEDIUM 5
wiring in `firmware/KilnFW/App/drivers/safety/safety_ceiling_sync.c:356` was
poisoned by hand (`row.set && !cache_stale` -> `row.set`) and all host tests
rebuilt into a *fresh* output directory. Result: FAIL at
`firmware/KilnFW/App/test/test_safety_ceiling_sync_divergence.c:581` -- the
new regression test is load-bearing against production code, not a mirror.
The file was then restored by hand (no `git checkout --`/`restore`/`stash`),
`git diff` confirmed empty, and every host test rebuilt again into a second
fresh directory (46/46 PASS) so no poisoned binary survives.

Worktree removed after the run.

## Findings, severity ranked

### HIGH 1 -- the "retry once divergence clears" half of the HIGH1 fix has no driver

`kiln_cfg_store.c:2113` (`kiln_cfg_store_pico_half_recapture_pending()`) has
**zero production callers**. The header at `kiln_cfg_store.h:428-440` names
"the web status handler and/or LCD refresh callback" as the intended
consumers, but neither `dashboard_status_http.c` nor `ui_page_home_refresh.c`
(both touched by this same commit) polls it.

Failure scenario: a standing divergence latches; one zones edit defers the
Pico-half recapture and sets `s_pico_half_dirty`; the divergence later clears
(a refetch lands, or the operator re-commissions). Nothing re-arms. The
deferred recapture happens only if some *unrelated* later edit happens to
call `kiln_cfg_store_autosave_from_live()` again. Until then the active
slot's captured Pico half and `pkg_hash` stay stale -- and a stale expected
half is exactly what the standing check compares against, so the board can
keep reporting a divergence that no longer exists (or, worse, apply a stale
Pico half at the next kiln swap). The commit message's "retries once
divergence clears" is therefore true only opportunistically.

Fix shape: one caller. The cheapest safe one is the flash-worker-dispatched
path that already exists (`zones_config_store.c:413`'s
`uart_bridge_ext_run_on_flash_worker(zones_autosave_job, ...)`) driven from a
periodic, non-PSRAM-stacked context, or a poll in the dashboard handler as
the header already promises.

### HIGH 2 -- an ARMED refusal wedges the divergence indefinitely

`safety_cfg_http.c:1265` gates the recapture on `if (ok && commit)`, and
`ok` is false whenever the Pico refuses the write. `KILNLINK_COMMIT_CONFIG_REJECT_ARMED`
("relay is ARMED -- config writes are refused while ARMED",
`safety_cfg_http.c:833`) is now a common refusal given the tc_type rules.

No other production path pushes the broadened/standing field set:
`safety_ceiling_sync_reconcile_on_link_up()` pushes `abs_max_temp_c` only
(`safety_ceiling_sync.c:131`), and the file's own comment says the broadened
set has "no confirmed push path ... in production today".

Failure scenario: the Pico reboots to defaults on a standing param (or an
operator changes one on the Pico), the ESP latches a standing divergence, and
every attempt to re-push is refused while ARMED. The expected record can then
*never* be corrected without disarming: the LCD shows a permanent "Config
mismatch (Pico)" banner that displaces the ramp-lag notice for the whole
firing, the web JSON reports `safety_standing_diverged:true` permanently, and
the Pico-half recapture stays deferred forever (compounded by HIGH 1). This
is the "chronic nuisance warning gets ignored/switched off" failure mode the
project explicitly warns against. At minimum the refusal class should be
surfaced in the same warning ("diverged; re-push refused because the relay is
ARMED") so the operator sees the action required rather than an unexplained
mismatch.

Note this is a *warning-only* path: heat-off enforcement is unaffected (see
"Ceiling divergence" below).

### MEDIUM 3 -- the ordinary autosave still captures "expected" from a cache, not from a confirmed push

The commit message says the expected Pico record is now "captured only from a
confirmed push (readback)". That is true for
`kiln_cfg_store_recapture_pico_half_confirmed()` (`kiln_cfg_store.c:2118`),
but the ordinary path is unchanged: with `diverged == false`,
`kiln_cfg_store_autosave_from_live()` still calls
`kiln_cfg_store_save_current_ex(..., recapture_pico_half=true, ...)`, which
re-snapshots `safety_cfg_store`'s **live cache** via
`kiln_package_capture_pico_half()`. The only thing standing between a Pico
revert and it being laundered into the expected record is the divergence
latch.

Failure scenario: the Pico reverts a standing param; `safety_poll_task`
refetches the new (reverted) value into the cache; a zones/autotune edit
fires an autosave on the flash worker *before* the poll tick that runs
`enforce_ceiling_divergence()` sets the standing latch. The reverted value is
captured as the new "expected" value, and the divergence is never reported.
Narrow window (one poll period), but this is precisely the laundering the
fix claims to have closed, and it is the "reset one side of a pair" shape:
cache refresh and latch update are separate events with no shared ordering
guarantee.

### MEDIUM 4 -- tc_type (param 0x0105) still counts as standing divergence with no push path

`safety_cfg_store.c:184` lists `0x0105 tc_type` in the mirrored param table,
and `kiln_cfg_store_capture_expected_pico_fields()`
(`kiln_cfg_store.c:2177-2230`) excludes only `abs_max_temp_c` -- so tc_type is
captured into the expected half and compared every tick. With the concurrent
work removing the ESP's tc_type push, any Pico-side tc_type change (including
a post-reboot default) is a standing divergence that the ESP has no sanctioned
way to clear, which feeds directly into HIGH 2. Recommend excluding tc_type
from the expected/compared set for as long as the ESP is not its pusher, or
comparing it only against a value the ESP actually confirmed.

### MEDIUM 5 -- latched reason strings are read cross-task with no lock

`s_standing_warning_active` / `s_standing_warning_reason`
(`safety_ceiling_sync.c:70-71`) are written by `safety_poll_task` inside
`enforce_ceiling_divergence()` and now read concurrently by two more tasks:
the httpd task (`dashboard_status_http.c`, the new
`safety_standing_diverged[_reason]` block) and the LVGL task
(`ui_page_home_refresh.c:187-200`). There is no lock, critical section, or
snapshot: `safety_ceiling_sync_is_standing_diverged()` copies the buffer with
`snprintf` while the producer may be rewriting it, and the flag and the
string are two independent non-atomic accesses. Worst observed consequence is
a garbled or empty reason on screen / in JSON (no overflow -- the destination
buffers are correctly sized `CONFIG_DIVERGENCE_REASON_MAX`), but this is the
same "read a producer's state without a snapshot" pattern the project has had
to fix before. A small copy-under-lock (or a double-buffered snapshot updated
by the producer) would close it.

Related, lower: the dashboard emits the reason with `%.80s` and **no JSON
escaping** while `kiln_cfg_http.c:196` escapes the same class of string.
Today's reason text is built from compile-time param names or
`param_0xNNNN` plus formatted floats, so no quote/backslash can appear --
latent only, but the asymmetry is worth removing.

### MEDIUM 6 -- httpd stack frames grew (the brief forbids enlarging httpd buffers)

`kiln_cfg_http.c` `save_post_handler()` grew `resp[64]` -> `resp[192]` and
added `divergence_reason[96]` + `reason_escaped[96]`: roughly +320 B on the
shared 8 KB httpd stack. `dashboard_status_get_handler()` adds
`standing_reason[160]` to its own frame. No shared JSON buffer
(`DASHBOARD_JSON_STATUS_BUF_SIZE`, the zones JSON buffer) was enlarged --
that part of the brief is satisfied -- and `check_httpd_task_stack_budget`
still passes, but it already reports honest free 2088 B (25.5%), classified
LOW, with the deepest path elsewhere (`cfgfs_status_get_handler`, 4304 B).
These handlers are not the deepest path today, so this is headroom erosion
rather than a violation; it should be spent knowingly. The 96-byte
`divergence_reason` also silently truncates a 160-byte reason, so the web
warning can differ from the LCD/log text for the same event.

### LOW 7 -- `s_pico_half_dirty` is global, not per-slot (reset-one-side)

`kiln_cfg_store.c:1964`. The flag records "a recapture is owed" but not *for
which slot*. If the active slot changes while it is set (kiln swap,
`kiln_cfg_store_set_active_id_raw()` at :1585, import, or the store-reload /
quarantine paths at :302/:896/:910 that reset `active_id` without touching
the flag), the deferred recapture lands on a different slot than the one that
deferred it: the original slot keeps a stale Pico half forever, and the new
slot gets a recapture it never asked for. Store the owing slot id (or clear
the flag wherever `active_id` changes).

### LOW 8 -- the dirty flag is written from two tasks without synchronisation

Set/cleared from the flash worker (`zones_autosave_job` ->
`kiln_cfg_store_autosave_from_live()`) and from the httpd task
(`kiln_cfg_store_recapture_pico_half_confirmed()`), as a plain `bool` with no
lock. Benign on this core in practice, but a lost update silently drops a
pending recapture -- which, given HIGH 1, would never be re-armed.

### LOW 9 -- `s_cache_stale` is only cleared inside `maybe_refetch()`

`safety_cfg_store.c:1582-1607`. A successful direct refetch
(`safety_cfg_store_refetch()`/`_nonblocking()` from
`confirm_commit_landed()`) does not clear the flag; it clears only on the
next `safety_cfg_store_maybe_refetch()` tick that observes matching CRCs. So
immediately after a confirmed commissioning commit the standing check treats
every broadened field as UNKNOWN and raises a transient standing divergence
-- in the very window where the new recapture runs. Transient and fail-safe
(unknown never reads as agreement), but it produces avoidable warning noise
at exactly the wrong moment.

### LOW 10 -- the new accessor's own lifecycle is untested

`test_safety_ceiling_sync_divergence.c` fakes `safety_cfg_store_cache_is_stale()`
(:182), and `test_safety_cfg_http.c` stubs
`kiln_cfg_store_recapture_pico_half_confirmed()`. Both stubs are justified in
the harness comments and the consuming logic *is* genuinely covered (proved by
the negative test above). But nothing tests the production set/clear points of
`s_cache_stale` themselves -- the exact code LOW 9 describes.

## Checks that came back clean

- **Autosave retry loop / NVS spam / PSRAM stack / flash-worker re-entrancy.**
  There is no retry *loop*: the deferral is a single flag consumed by the next
  autosave call, so it cannot spin or spam NVS. The deferred WARN is
  rate-limited to 60 s (`kiln_cfg_store.c:2072-2085`). The autosave path is
  dispatched to the flash worker exactly once from `zones_config_store.c:413`
  and never re-dispatches from on-worker code; the new
  `kiln_cfg_store_recapture_pico_half_confirmed()` is called only from the
  httpd task and performs its NVS write inline, guarded by `nvs_save_store()`'s
  existing PSRAM-stack refusal (`kiln_cfg_store.c:614-620`). No module lock is
  held across a producer or blocking call on any new path.
- **Ceiling divergence still disables heat.** `safety_ceiling_sync.c:389-420`
  is untouched: the heat-off verdict is still computed from the ceiling field
  alone (`n == 1`), still calls both disable hooks, and the Pico's `abs_max`
  comparison is unchanged. Nothing in this commit disarms, widens, or
  conditionalises that path; the new standing-divergence surfaces are
  warning-only.
- **LCD.** Reuses the existing WARNING-styled `s_ui_home_lag_notice` widget
  (`ui_page_home.c:666-675`): `lv_pct(100)` width, `LV_LABEL_LONG_DOT`, no new
  colours, no scrolling, one message per tick on the 480x320 panel. The
  `goto lag_notice_done` still ticks the lag debounce before jumping, so the
  lag counter does not drift; the only behavioural cost is that the ramp-lag
  notice is suppressed for as long as a divergence stands (deliberate, and
  documented in the code) -- which HIGH 2 can make indefinite.
- **Web JSON buffers.** No shared JSON buffer was enlarged (see MEDIUM 6 for
  the stack-frame caveat); the new fields go through the existing `APPEND`
  macro with its `goto truncated` overflow path.

## Verdict

The commit is a real improvement -- the "autosave silently off forever"
defect it targets (HIGH1) is genuinely fixed for the ESP half, and the
MEDIUM 5 stale-cache wiring is proven load-bearing by negative test. But two
of its headline claims are only partly delivered: the deferred recapture has
no retry driver (HIGH 1) and the expected record is still cache-captured on
the ordinary path (MEDIUM 3), and the new always-on warning can be wedged on
indefinitely by an ARMED refusal (HIGH 2), which is the most likely way this
feature ends up distrusted in the field.
