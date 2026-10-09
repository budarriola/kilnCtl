# Dev firmware review, 2026-10-09

Read-only review of the firmware commits on `origin/dev` that were not yet on
`origin/main` when the review started (`origin/main..d3e2a6ba`). Scope: commits
that touch `firmware/**/*.c`, `*.h`, `*.html` or `*.js`. Tools and PcTools
commits are covered by a separate review. No board was touched, and
`run_all_checks` was not run. Line numbers are at `origin/dev` `411ed67b`.

Commits reviewed (20): 55155b73, 1a2d2e33, 70659b5d, e9d7ea84, 631920d3,
9fb7bc1b, 02fd73ab, 7716998e, 90b75e27, 9584737f, 8982a37a, f8c641bd,
4c1aebf1, d338466f, 56d1d9c0, ef6ccdd0, 887c5a96, 415bd942, 31bec63b,
ee2d0724.

Not reviewed: firmware commits that landed on dev after `d3e2a6ba`, including
290db19e (relay_cycles backup export/import) and 46616a4e (route tier
changes), and 74c7265a, which edits the same profile rev-floor code as L1.
They need their own pass.

## Summary

One finding is MEDIUM. Everything else is LOW or informational. No safety
regression was found. Aux outputs still go to their fail-safe state on every
non-running state. 56d1d9c0 only removes a field that was always `false`.
No reviewed commit changes how heat is granted relative to the Pico.

## Findings, ranked

### MEDIUM

**M1. A plain `malloc` in the persist layer turns a standing check red on dev.**
`firmware/KilnFW/App/drivers/persist/iter_tune_store.c:129`, commit e9d7ea84.

`note_oversized_nvs_blob()` calls `malloc(real_len)`, with `real_len` up to
`ITER_TUNE_NVS_PROBE_MAX`. That is 4096 B of internal RAM, because
`SPIRAM_MALLOC_ALWAYSINTERNAL` is 8192. Persist scratch must use
`persist_scratch_alloc()`. At the dev tip,
`tools/check_persist_scratch_malloc_caps.ps1` reports:

```
FAIL: firmware/KilnFW/App/drivers/persist/iter_tune_store.c:129: plain malloc(real_len) -- use persist_scratch_alloc()
```

A full-suite run on the dev tip will fail until this is fixed, so dev cannot
be squash-promoted. ee2d0724 removed the allowlist for exactly this class in
the same window. Fix: use `persist_scratch_alloc(real_len)` and include
`persist_scratch.h`.

### LOW

**L1. A failed firing-stats file delete can attach old history to a new profile, and the delete cannot be retried.**
`firmware/KilnFW/App/drivers/persist/firing_stats_cfg_fs.c:205-223`,
`firmware/KilnFW/App/drivers/control/profile_executor_firing_stats.c:886-912` and
`firmware/KilnFW/App/drivers/http/profiles_http.c:1596-1603`. Commits
887c5a96 and f8c641bd; 9584737f made the error propagate.

`firing_stats_erase()` erases the `fs_<id>` blob first. It then calls
`firing_stats_cfg_fs_delete()`, which erases the `fsr_<id>` rev key before the
file. If the file delete then fails, the file is the only copy left. The next
resolve adopts it, because the file is valid and NVS is empty. A profile saved
later at the same id inherits the old run history.

The `nvs_erase_slot()` comment says the error is "propagated so the HTTP delete
reports the incomplete prune". But `profiles_http_delete()` has already cleared
the slot bitmap and RAM at `profiles_http.c:1596-1597`, before
`nvs_erase_slot()` runs. Retrying the delete returns `false` at line 1568
(slot unused), so the prune can never be finished. The same holds for the web
delete in `profiles_edit_http.c:753-759`. That path at least answers with
`cfg_fs_http_persist_failed()`. The benchproto and LCD path
(`profiles_http_delete()`, called from `uart_bridge_ext_control.c:532` and
`ui_page_profile_picker.c:204`) logs the `nvs_erase_slot()` error and still
returns `true` at `profiles_http.c:1603`. That is the "logging unchecked
success" class. f8c641bd's "a retry
finishes the erase" does not hold. Both triggers need a LittleFS delete
failure. Fix options:
- Delete the file before the rev key, inside `firing_stats_cfg_fs_delete()`.
- Have the profile save path call `firing_stats_erase(id)` when it takes an
  empty slot.

**L2. The zones POST lost-update guard is check-then-act, with no lock.**
`firmware/KilnFW/App/drivers/http/zones_http_post.c:652-666`, commit 8982a37a.

The generation re-check closes the long window, which is the blocking Pico
ceiling write. It does not close the short one. Nothing locks the zones config:
there is no mutex in `zones_http*.c` or `zones_config_accessors.c`. A writer in
another task can still bump `s_config_generation` between the compare at line
652 and `s_zones.cfg = tmp` at line 666. Examples are `zones_config_set_pid()`
from adaptive tune or the UART bridge, and `zones_config_set_max_ramp_no_save()`.
That write would be lost. The struct copy is also not atomic against those
writers. This was already the case before the commit, and the window is now a
few instructions. The real fix is a zones-config lock that covers both the
compare and the commit.

A related point: when the 409 fires after the ceiling RAISE succeeded, the
Pico ceiling stays raised above the unchanged zone maxima. That still satisfies
"Pico ceiling same or looser", so it is not a safety issue. It is not reported
in the 409 body.

**L3. `aux` `switch_count` counts transitions whose relay write failed.**
`firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c:506-514`,
commit 70659b5d.

When `kiln_io` returns an error (line 499, "relay state is unknown"),
`commanded_on` still flips and `switch_count++` still runs. `relay_cycles_add()`
has the same pre-existing behavior. The new `switch_count` and `on_time_s`
fields on the dashboard can therefore report a switch that never happened.
Informational.

**L4. The iter_tune newer-version probe only covers one of the cases it is meant to report.**
`firmware/KilnFW/App/drivers/persist/iter_tune_store.c:114-153`, commit
e9d7ea84.

The probe runs only when the fixed-size read fails with `HAL_INVALID_SIZE`,
that is, when the stored blob is larger than the struct. A newer blob that is
the same size or smaller, or a newer copy in the cfg_fs file, is still
rejected without reporting its version.

**L5. Stale size comments after the `DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE` / `profile_exec_status_t` growth.**
`firmware/KilnFW/App/drivers/http/dashboard_json.h:81-87`,
`gpio_probe.c:61`, `adaptive_tune.c:1171`, `zones_current_sweep_task.c:2276` and
`profiles_http.c:1579`. Commit 70659b5d.

The comments still say "960 -> 1344" and "1384-byte". The new note sits after
the `#define`. Cosmetic.

The memory impact checked out. The buffer is heap-allocated in PSRAM
(`heap_caps_malloc(..., MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)`,
`dashboard_exec_http.c:257`), not on a stack, so the 1344 -> 1920 growth costs
no task stack. `profile_exec_status_t` grew by 32 B. It is heap-allocated at
every call site except one static (`telemetry_log.c:144`, +32 B .bss). No stack
local of that type exists.

**L6. A backup import precheck validates against the live thermocouple count.**
`backup_import_profiles_precheck()`, commit ef6ccdd0.

The candidate state uses the live `zone_count`, but the candidate relay masks,
zone types and aux entries. A backup whose zones change `thermo_count` has its
profiles checked against the old count. Profiles are committed last, after
zones, so a mismatch shows up as a profile-commit failure rather than a
corrupt state. The name `backup_import_apply_locked` suggests a lock that does
not exist. No lock is taken.

**L6 resolved (commit titled "backup import L6"):** a backup import cannot change `thermo_count` (it restores per-zone tuning entries only, rejected when index >= live count), so the live count is right; comment added. Function renamed `backup_import_apply_two_pass` (it takes no lock).

## Checked and found clean

- **4c1aebf1**: the header erase and `sha_start` moved into `flush_head()`
  after the identity, version and policy gates. Body erases happen in `put()`,
  only after `flush_head()`. A refused upload therefore leaves the old stage
  untouched. `fail()` calls `sha_abort()` before `sha_start()` ran, which is
  safe because both IO implementations check `sha_active`.
- **31bec63b and 415bd942** (zones config OOM handling):
  - OOM during resolve or decode is now "could not decide", not "absent" or
    "corrupt".
  - The rev floor is kept (`s_zones_cfg_rev = nvs_rev`), the load fails with
    `ESP_ERR_NO_MEM`, and `zones_http_start()` skips the default-partition
    migration on an error.
  - The `resolved` leak that 31bec63b introduced is freed in 415bd942.
  - The static asserts cover every historical layout from v1 to v25.
- **ee2d0724 and 7716998e**: mechanical `malloc` to `persist_scratch_alloc`
  conversions. No `realloc` follows any of the converted buffers.
- **ef6ccdd0**: the shared `profiles_validate.c` matches the code it removed
  when `state == NULL`. The new commit order puts profiles last. The phase-1
  aux revert is skipped once zones have landed. A truncated export fragment now
  fails loudly.
- **9584737f**: `nvs_load_files_only()` passes floor 0, so the degraded boot
  path never deletes. `s_profile_rev_unknown[]` only blocks saves into slots
  that have no file.
- **56d1d9c0**: `failsafe_on_pause` was always `false`, so behavior is
  unchanged. PAUSE holds the last commanded state. IDLE, DONE, FAULTED and halt
  still select `failsafe_state_on`. The guard and trip precedences above level 3
  are untouched.
- **02fd73ab and 90b75e27**: no HTML or JS in `firmware/KilnFW/App/drivers`
  still references the removed ids (`espFile`, `espPicker`, `espRollbackBtn`,
  `espUpdateBtn`, `updateOrderHint`).
- **d338466f**: `bootPage()` runs on `DOMContentLoaded`, which fires after the
  deferred `app.js` has executed. Inline scripts always see `readyState`
  `'loading'`, so the `else bootPage()` branch is never taken at parse time.
- **631920d3 and 9fb7bc1b**: the fixtures are frozen byte arrays, and size
  asserts tie them to the v1 and v2 structs. Each one can fail. The kiln_cfg
  negative test only corrupts the version byte, so it tests version dispatch,
  not body corruption.
- **55155b73 and 1a2d2e33**: string and comment changes only.
