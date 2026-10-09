# Dev firmware review 3 (2026-10-09)

Code review only. No board access, no builds, no `run_all_checks.ps1`.
Reviewed at origin/dev `6ea993ce`. Commits in scope:

| Commit | Subject |
|---|---|
| `1a516706` | aux F1/F2: drop aux outputs on any safety fault |
| `2ae8d1e6` | aux_outputs_cfg F3/F4: locking and commit-RAM-only-on-save |
| `b5bcdb73` | update chain: fetch writer task, manifest gate, claim-first re-check |
| `fd196627`, `6ea993ce` | CSRF Origin/Referer check (app and recovery image) |
| `ae4fde7f` | backup export/import of six operator preferences |
| `4c8ecc4f` | profile delete and firing_stats, zones_cfg_lock portMUX, aux switch_count, size comments |

Paths below are relative to `firmware/KilnFW/App/drivers/` unless stated otherwise.

No HIGH findings. In particular, none of the new `zones_cfg_lock()` portMUX
critical sections (`http/zones_http.c:413-428`) contains NVS, flash, logging,
allocation or a mutex take. I checked `http/zones_http_post.c:82-133` and
`654-689`, plus every setter in `persist/zones_config_accessors.c` (for example
`set_pid`, at 537-591). The 409 log and `free(tmp)` both sit outside the lock.

## MED

### MED-1: a profile or autotune start can go RUNNING while a stage upload or fetch writes flash

**Where:**
- The start checks the update claim once, early, at `control/profile_executor_run.c:361`. It publishes the heat claim much later, at `:1326`.
- The same applies to `control/autotune_engine.c:1039` and `:1250`.
- The update side was hardened in `update/update_http.c:185-197`: it takes the claim, then re-checks `mode_gate_refuses()`, which reads `relay_authority_heat_run_active()`.

**Scenario:**
1. A profile start passes `ota_http_heat_blocked_by_update()` at 361.
2. It then spends hundreds of milliseconds in baseline capture and thermocouple SPI reads before reaching 1326.
3. Inside that window, `POST /api/update/stage` (or a GitHub fetch) wins `ota_http_update_try_begin()`.
4. Its re-check of the mode gate sees `heat_run_active == false`, because the heat claim is not published yet, so the upload proceeds.
5. The start then publishes the heat claim and goes RUNNING at about 1453.

Result: stage-partition erase and write traffic runs during a firing. This is
exactly what the claim-first change was meant to rule out. The re-check only
closes the race in one direction.

**Fix:** after `relay_authority_heat_zone_claim_begin()`, re-check
`ota_http_heat_blocked_by_update()` in both starters, releasing the claim on
refusal. Use the same publish-then-re-check pairing already used for
`backup_import_restore_in_flight()` at `profile_executor_run.c:1345` and
`autotune_engine.c:1264`.

### MED-2: a backup cannot be restored if it hides any builtin profile past the eighth

**Where:**
- `http/backup_import.c:3108` declares `bool hidden[8]`.
- The parse refuses any id whose index is 8 or more (`:3191`).
- The commit loop is capped at `i < sizeof(p.hidden)` (`:3282`).
- The catalogue has 28 entries: `persist/profiles_builtin_table.inc:522`, with a 32-bit `s_hidden_mask` at `persist/profiles_builtin.c:63`.

**Scenario:**
1. The operator hides builtin id 140 (index 12).
2. `backup_export_prefs()` (`http/backup_export.c:278-286`) writes `"hidden_builtin_profiles":[140]`.
3. Importing that same file fails pass 1 with "hidden_builtin_profiles holds an id that is not a builtin profile". The whole restore is refused, so a board's own backup cannot be restored.

A second, quieter effect: a backup listing only low ids leaves a live-hidden id
of 136 or above hidden. That contradicts the "applied as a full set" semantics.

**Why the tests miss it:** the host test stubs `g_builtin_profile_count = 3`
(`test/test_backup_import.c:247`), so it can never reach the bound.

**Fix:** size the array from the 32-bit mask (or use a `uint32_t` mask), and
add a test with the real catalogue size or an id of 136 or above.

## LOW

### LOW-1: a writer wedge in the update fetch is reported as failed, but the stage can still complete afterwards

**Where:** `update/update_fetch.c:799-823`, with `wr_task` at `:330-356`.

On a 30 s writer timeout, the job leaks its buffers (correct, because the
writer may still read them) and releases the update claim. The stage object
stays non-IDLE, so a hand upload or a stage clear correctly returns BUSY until
reboot. That part is sound.

The leftover risk:
1. The wedge hits during `WR_FINISH`.
2. The finish later completes, and the image passes the manifest gate.
3. The stage becomes valid and IDLE while `/api/update/fetch` reports FAILED `writer_timeout`.

A wedge in `WR_WRITE` instead leaves the stage UPLOADING until reboot, and no
route explains why stage upload and clear say BUSY.

**Fix:** keep a sticky "writer wedged, reboot required" flag that the status
routes surface.

### LOW-2: aux fault drop repeats every tick while an OFF write keeps failing, and marks the relay off regardless

**Where:** `control/profile_executor_relay_io.c:588-618`, called from the
not-RUNNING branch at `control/profile_executor.c:~760`.

The write is synchronous (`kiln_io_owner.h:228`), so the shadow drops on
success and the function goes quiet. That means no double `relay_cycles_add`
on a successful write.

On a persistent I2C or expander failure, though:
- It logs `ESP_LOGW` "dropping aux mask" on every executor tick for as long as the fault lasts.
- It sets `actuated_on = false` after a failed write. The dashboard then shows the aux OFF while the relay may still be energised.
- The manual path never clears `commanded_on`.

The retry itself is desirable. The log should be rate-limited, and the
`actuated_on`/`commanded_on` state should be left unknown or ON when the write fails.

**Test gap:** the host tests call `profile_executor_aux_fault_drop()`
directly, so nothing proves the tick wiring or the pre-lock Pico-trip read.

### LOW-3: `aux_outputs_cfg_start()` holds `s_lock` across NVS and cfg-fs I/O

**Where:** `persist/aux_outputs_cfg.c:145-224`.

This contradicts the module's own comment at `:52-56` ("never across flash
I/O"). Startup is boot-only (`http/aux_outputs_http.c:181`). But
`aux_outputs_cfg_enabled_mask()` is read by the executor under `s_exec.lock`
(`profile_executor_relay_io.c:596`) and by the zones aux-enabled provider. Any
tick that is already running when start happens therefore blocks behind a
flash read for its duration.

**Fix:** load into locals, then publish under the lock.

### LOW-4: the zones L2 lock does not cover the persist snapshot

**Where:** `persist/zones_config_store.c:757-775`.

`nvs_save()` stamps `version` and `crc32` in place on the live
`s_zones.cfg` without `zones_cfg_lock()`. `zones_config_cfg_fs_save()` then
memcpy's the live struct and re-stamps the CRC on its copy.

**Scenario:** a setter on another task (for example a PID write from autotune
while an HTTP save runs) can land mid-copy. That yields a mixed snapshot that
is still CRC-valid on disk. In addition:
- `zones_config_persisted_equals_ram()` memcmp's against unlocked live RAM.
- `s_zones_cfg_rev` races between concurrent savers.

**Fix:** copy under the lock, then stamp and write the copy.

### LOW-5: fetch heap slack shrinks to 1932 B

**Where:** `update/update_fetch_heap.h:57-59`.

The 2048 B internal scratch was moved out of the slack term (now
`3980 - 2048`), and the precheck sum is unchanged at 28672. The margin against
a concurrent login KDF or a second httpd request during a TLS fetch is now
under 2 KB, against the owner's 8192 B `min_free` floor.

**Fix:** re-measure the worst concurrent draw on hardware before relying on it.

### LOW-6: the manifest gate does not compare the commit

**Where:** `update/update_stage.c` (`update_stage_manifest_gate`, about line 361).

The gate compares the zones_cfg, kilnlink and uart versions only. Semver is
checked in flush_head. A release whose `release.json` commit disagrees with the
image's embedded commit is staged without complaint.

### LOW-7: profile delete reports failure after the slot is already gone live

**Where:** `http/profiles_http.c:1601-1614`.

When `nvs_erase_slot()` fails, the function returns false, but the slot was
already cleared and memset, and its firing stats erased. The web caller shows a
persist failure. A retry finds an unused slot, which looks like a success or a
404. After reboot the profile reappears, with its firing stats gone.

**Fix:** erase NVS before clearing RAM, or report "deleted, will reappear" distinctly.

### LOW-8: CSRF check edge cases

**Where:**
- `http/http_auth_http.c:285-315` and `:332`.
- `firmware/KilnFW_recovery/main/recovery_http.c:244-282`.
- `http/http_origin_check.h`.

What works:
- PcTools (urllib sends neither Origin nor Referer), curl, MCP tools and the LCD (no HTTP) are unaffected.
- The recovery page at `192.168.4.1` matches its own Host.
- Recovery handlers do not use `user_ctx`, so wrapping the handler pointer in it is safe.

Edge cases:
- **Long Referer:** a Referer of 96 characters or more, with no Origin, is refused as "overlong". Modern browsers send Origin on every POST, so only old browsers or privacy extensions that strip Origin hit this, but a long page URL with a query string then gets a confusing 403.
- **Proxy:** a reverse proxy that rewrites Host to the board IP while the browser sends the public Origin is refused. The scheme is ignored and the port defaults to 80, so https behind a Host-preserving proxy happens to pass.
- **Registered method:** the app check keys on the registered method (`ctx->method`), not `req->method`. That is fine today, since no `HTTP_ANY` route exists.
- **Commit message:** it names a `test_http_origin_check.c` that does not exist. The cases live in `test/test_http_auth_enforce.c` (`test_origin_check`). Only the pure function is tested; the header-extraction glue (Referer fallback, overlong detection, method skip) is not.

## Checked and found sound

- `2ae8d1e6` F3: `aux_outputs_cfg_set()` commits RAM only after a successful file save. The test asserts that RAM is unchanged after a failed save.
- `4c8ecc4f`:
  - aux `switch_count` now increments only on a successful ON write.
  - `relay_cycles_add` still counts any commanded transition, consistent with the zone relays.
- `ae4fde7f`, apart from MED-2:
  - Every new key is optional, and an invalid value refuses the whole import in pass 1.
  - `backup_json_obj_find()` matches top-level keys only, so a nested `"unit"` or `"tz"` cannot be mistaken for a preference.
  - The parse struct and escape buffers are a few hundred bytes in `NOINLINE` frames.
  - Relay names go through the persisting `zones_config_set_relay_name()` and `zones_config_set_relay_device_type()` setters.
  - A commit failure is reported as a partial write, not as success.
- `b5bcdb73` `claim_refuses()` (`update/update_http.c:185-197`) releases the claim on refusal.
