# Review: kilnlink link-down/heap-scratch and profile slot seqlock (2026-10-09)

Read-only Opus review of origin/dev `b475e7d7..dd46bc6d`:

- `cb4ffda7`, `cd90a125`, `a69ff8fb`, `259d8771`: KilnFW kilnlink link-down
  path, reannounce on a gated boot-clear, heap scratch for large httpd locals.
- `6f154560`, `dd46bc6d`: per-slot seqlock (`s_slot_gen`) for profile RAM
  assigns, executor capture/recheck.

No code was changed by this review. Line numbers are against `dd46bc6d`.

## Findings

### MED-1: `check_aux_relay_conflict_sites.ps1` now fails on dev (a69ff8fb)

- `tools/check_aux_relay_conflict_sites.ps1:57` pins the aux-conflict call to
  `Func = 'zones_post_apply'`. `a69ff8fb` turned `zones_post_apply`
  (`firmware/KilnFW/App/drivers/http/zones_http_post.c`, about line 814) into a
  thin wrapper that allocates the `zones_cfg_t` scratch and calls
  `zones_post_apply_body()`. The `zones_config_json_aux_conflict_mask(` call
  now lives in `zones_post_apply_body()` (about line 598).
- Verified: running the check directly in a dev worktree exits 1.
- Failure scenario: the standing suite is red on dev and blocks promotion.
  Worse, the obvious quick fixes (loosen the regex to file scope, or add a
  dummy call in the wrapper) would leave the guard no longer covering the
  function that actually commits `s_zones.cfg = *tmp`.
- Fix: change the entry to `Func = 'zones_post_apply_body'` and update the
  comment to name the `s_zones.cfg = *tmp` commit in the body function.
  Negative-test with `tools\negtest.ps1 -Preset check` by deleting the call.

### LOW-1: boot load/migration RAM writes are not gen-bracketed

- `profiles_http.c:1492` (migration assign) and `:1503` (migration failure
  memset) in `migrate_from_default_partition()`, called from
  `profiles_boot_load()` at `:2015`; also `nvs_load_all_from()` at `:2010` and
  the fallback memset / `nvs_load_files_only()` at `:2024-2026`. None call
  `profiles_slot_gen_begin/end`.
- Reachability: `/api/profile_exec/start` is registered by
  `dashboard_http_start()` at `main_network_http.c:305`, before
  `profiles_http_start()` (boot load) at `:423`, and httpd is already live
  (started by `wifi_prov_start()` in `main_boot_early.c`). The login route is
  registered later (`:690`), so this is only reachable with web auth off, in
  a short boot window. LVGL (`ui_page_home_actions.c:127`) and bridges
  (`uart_bridge_ext_control.c:563`) start later and cannot race it.
- Failure scenario: a start captures gen 0 for slot N, copies a half-written
  slot during the migration assign, or copies a migrated profile that the
  failure path then memsets away; the recheck under `s_exec.lock` sees gen
  still 0 and accepts. The executor runs a torn or no-longer-stored profile.
  The content is a valid legacy profile in the common case, so impact is low.
- Fix (any one): bracket the migration assign and memset (and the bulk load
  loops, per slot) with `profiles_slot_gen_begin/end`; or register the
  profile-exec start route after `profiles_http_start()`; or refuse starts
  (503) until a `profiles_loaded` flag set at the end of `profiles_boot_load()`.

### LOW-2: `s_slot_rev_pub` is dead

- Declared `profiles_http.c:82`, stored at `:1284`, `:1365`, `:1419`, never
  loaded since `profiles_http_slot_rev()` (`:1877`) now returns `s_slot_gen`.
  The comment at `:79-82` and `profiles_store.h:66-81` still describe the
  "save revision" semantics the executor no longer uses.
- Failure scenario: a later change reads `s_slot_rev_pub` believing it
  still guards starts, or a reviewer trusts the header doc and misses that
  the gate is now the seqlock.
- Fix: remove the array and its stores, or keep it explicitly as a
  diagnostic; either way rewrite the `profiles_store.h` doc to describe the
  gen seqlock (odd = in flight, any change = refuse).

### LOW-3: link-down clear dropped the fixed stale threshold

- `safety_link.c:301-316` (`safety_reset_stale_peer_info_if_link_down`) now
  clears `peer_version_known`, `pico_boot_id_known`, `peer_build_known` and
  `pico_uptime_baseline_known` on `!safety_link_up_locked()` only
  (age > `poll_period_ms * SAFETY_LINK_UP_PERIODS`). The removed site in
  `safety_update_health()` (`safety_link_poll.c:173-210`) also ORed
  `safety_link_is_stale(age, SAFETY_LINK_STALE_MS /* 1500 */)`.
- Identical at the default poll period (500 ms x 3 = 1500 ms). With a larger
  configured period the baseline and boot_id survive longer outages, so a
  Pico reboot inside that window is detected only by boot_id/uptime regression
  (still detected; regression check at `safety_link_frames.c:1031-1038`). The
  only miss is an outage spanning a 49.7-day uptime wrap.
- Fix: OR in `safety_link_is_stale(age, SAFETY_LINK_STALE_MS)` inside the
  reset helper to keep the old bound.

### LOW-4 (informational): seqlock reader has no explicit acquire fence

- `profiles_http.c:83-104` uses `atomic_fetch_add` (seq_cst) for
  `gen_begin/gen_end`; the reader (`profile_executor_run.c:289` capture,
  `:460` `profiles_http_slot_runnable_rev()` under `s_exec.lock`,
  `profiles_http.c:1885-1895`) loads gen, copies via `profiles_http_get()`,
  then rechecks. On the dual-core ESP32-S3 this is safe today because the
  copy is an opaque cross-TU call and `xSemaphoreTake` acts as a full barrier
  before the recheck.
- Failure scenario: LTO or inlining of `profiles_http_get()` plus removal of
  the semaphore between copy and recheck would let the data loads sink past
  the recheck load.
- Fix: `atomic_thread_fence(memory_order_acquire)` immediately before the
  recheck load, and a short ordering note at `gen_begin/end` (seq_cst RMW on
  both sides of the plain stores is the writer's release).

### LOW-5 (informational): reannounce from the gated boot-clear branch

- New else-if in `safety_link_frames.c` (around `:1110`) sets
  `reannounce_pending` each time a v>=17 peer's 30-byte DIAG blocks the
  boot-clear. DIAG arrives every 2000 ms; the burst is 4 x 250 ms
  (`:118-119`). Bounded by `s_boot_clean` (30 s window) and
  `SAFETY_LINK_BOOT_CLEAR_MAX_ATTEMPTS` (3): at most about 15 bursts of
  about 750 ms. No storm, no unbounded loop.
- Optional: rate-limit to one burst per retry gap.

## Checked, no issue

- Every early return frees the heap scratch: `status_get_handler()`
  (`wifi_provision_http.c:359-399`, freed after `httpd_resp_send`, 500 with
  nothing to free on alloc failure) and `zones_post_apply()` (frees `body` and
  returns 500 on alloc failure; the body function owns `body` on every path;
  the wrapper frees `tmp` unconditionally).
- No other code or check expects the old stack layout, apart from MED-1.
- `safety_link_note_link_down` fully removed; no dangling references.
  The link-down clear now resets the uptime baseline together with
  boot_id/version (both sides of the pair), and the reset runs after the
  `period == 0` continue, same as `safety_update_health()`.
- Every user-visible RAM assign is bracketed on every path:
  `profiles_http_save` (`:1800-1804`), `profiles_delete_slot` (`:1959-1965`,
  around the erase plus clear/memset), retarget revert (`:2366-2369`) and
  commit (`:2435-2439`), `profile_post_handler`
  (`profiles_edit_http.c:697-702`); live overwrite and backup import go
  through `profiles_http_save`. No early return between begin and end, so gen
  is never left odd.
- A failed save still bumps gen (end runs after the NVS error), so a start
  that copied pre-save content is refused, as the test covers.
- Executor capture is before the unlocked copy and the recheck is under
  `s_exec.lock`; an odd capture is refused. Correct.
- `test_profiles_slot_gen_seqlock` (`test_profiles_http.c:2063-2112`) covers
  odd-during-write, success after capture, failed save, and in-flight capture.
  `cd90a125`'s test-isolation fix is correct.
