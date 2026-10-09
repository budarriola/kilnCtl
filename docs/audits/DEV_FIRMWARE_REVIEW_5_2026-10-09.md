# Dev firmware review 5 (2026-10-09)

Scope: every commit on `origin/dev` after `728dc8f0` (review 4 doc) through `f9549ae4`:
`7d155f5a`, `adbbec15`, `9155d194`, `80b957da`, `667578a9`, `cb7af514`, `8a4739e6`, `ccabb020`,
`c2c0267a`, `512cb4e3`, `61e579c9`, `8c287553`, `f9549ae4`. Also the commits review 4 did not cover
that this pass was asked to look at: `95070c9a` (Wi-Fi legacy erase / run_state migration),
`d19d3df4` and `11a85801` (update chain, review 3 LOW-1/5/6 and the heat-side update re-check),
`513c07e6` (ota_page error text / test mocks).

Code review only: no builds, no board access.

## HIGH

None.

## MED

### M1. Manifest commit gate compares the manifest with itself (review 3 LOW-6 not actually closed)

- Where: `firmware/KilnFW/App/drivers/update/update_stage.c:361-373` `update_stage_manifest_gate()`;
  `firmware/KilnFW/App/drivers/update/update_fetch.c:631-634` `stage_begin()`.

What happens: the only production caller sets `s_c->wr.want = &w->man.identity` and passes
`w->man.identity.commit` as the stager's declared `commit` to `WR_BEGIN`. The gate then checks
`strncmp(commit, want->commit, ...)`, which is the same string compared with itself. The image's own
embedded commit is never read: `update_image_id_t` (the `id` argument) carries schema versions only,
no commit. Review 3 LOW-6 asked for image-vs-manifest, and that check still does not exist.

Scenario: a release whose `release.json` names commit A but whose `.bin` was built from commit B
(wrong asset uploaded, or a manifest edited after the fact) stages and verifies cleanly. The stage
header records commit A, so every later status read reports the wrong provenance.

The new test in `test_update_stage.c` passes a commit that differs from `want->commit`. Production
cannot produce that input, so the test proves nothing about the real path.

Fix: add the build commit to the embedded identity record (`update_image_id_t`,
`update_policy.h:117-123`; this changes `UPDATE_IMAGE_ID_SIZE`, so it needs a versioned layout). It is
already parsed in `flush_head()`, where the gate runs (`update_stage.c:263`). Compare that commit with
`want->commit`, and make the test feed a mismatching image, not a mismatching argument.

### M2. Recovery-image Wi-Fi reset and "forget last network" resurrect the legacy default-partition credential (reset-one-side)

**Fixed in 7551d44a** (one-shot migration in the app; recovery image left unchanged, its reset needs no legacy erase once migration erases the legacy keys).

- Where: `firmware/KilnFW_recovery/main/recovery_http.c:75` `WIFI_RESET_KEYS` and `:713-740` (wifi_reset);
  `firmware/KilnFW/App/drivers/net/wifi_prov_nvs.c:523` (`adopt`) and `:570-590` `nvs_load_saved_nets()`.
- Related: `95070c9a` added `erase_legacy_default_wifi` only to `factory_reset.c` (scopes `wifi`/`all`).

What happens: the recovery image's Wi-Fi reset erases only the `wifi_nvs` keys. It leaves the default
partition's legacy `wifi_cfg` copy in place. On the next application boot,
`wifi_prov_migrate_from_default_partition()` computes
`adopt = !found_in_wifi_nvs || (default_has_legacy && !wifi_nvs_has_legacy)`, and that is true.
`nvs_load_saved_nets()` then sees `saved_nets.count == 0` with `s_legacy_single.has` set, and rewrites
the old network into the list.

The same path runs whenever the user forgets the last saved network: on the next boot the count is 0
and the legacy credential comes back.

Separately, this build never writes the legacy single keys into `wifi_nvs`, so on every migrated board
`wifi_nvs_has_legacy` is false. As long as the default copy exists, `adopt` is true on every boot. Each
boot then reloads the default copy's mode and AP SSID/password override and writes them back over
whatever the user set since. This part is pre-existing, but `95070c9a` only closes it for a factory
reset.

Scenario:

1. The board was migrated from old firmware and still has `wifi_cfg` in the default partition.
2. The owner uses the recovery image's "reset Wi-Fi" (or forgets the only saved network in the app).
3. The board reboots into the application.
4. The board rejoins the old network, and any mode or AP-name change made after migration reverts.

Fix: make the migration one-shot. After a verified copy (or on the first boot that finds `wifi_nvs`
populated), erase the default `wifi_cfg` namespace through `legacy_default_nvs_erase_wifi()`, or write
a "migrated" marker key in `wifi_nvs` that suppresses `adopt`. Either one also covers the recovery
image without teaching it about the default partition. At minimum, add the legacy erase to the
recovery wifi_reset and to the app's forget/clear path.

## LOW

### L1. Wedge clean-up can miss an op that finishes exactly at the timeout

- Where: `firmware/KilnFW/App/drivers/update/update_fetch.c:357-364` (`wr_task` post-op check) and
  `:418-424` (`wr_call` timeout).

What happens: `wr_task` reads `wr_wedged` once, right after the op returns and before it gives
`wr_done`. If the op completes in the same tick that `fetch_task`'s `xSemaphoreTake` times out,
`wr_task` reads `false`, skips the clear/abort, and gives a semaphore nobody waits on. Only then does
`fetch_task` set `wr_wedged = true`.

Result: a late `WR_FINISH` that succeeded leaves a valid, verified stage behind a job that reported
FAILED. This is the residual case of review 3 LOW-1. `volatile` gives visibility but not ordering
between the two sides.

Fix: hand the decision to one side. For example, `wr_task` sets an `op_done` flag under a small
critical section (or with an atomic exchange) before giving the semaphore. `wr_call` sets `wedged` in
the same critical section only if `op_done` is still clear. If `wr_call` loses that race, it treats the
op as completed.

### L2. Stage status reason is overwritten for the rest of the boot after any wedge

- Where: `firmware/KilnFW/App/drivers/update/update_http.c:454-456`.

What happens: once `wr_wedged` is set, every `GET` of the stage status replaces `info.reason` with
`writer_wedged_reboot_required`. It does this even when the stage's real reason is `sha_mismatch`,
`blank` or `read_error`, and even when the stage holds a verified image.

A verified image can get there two ways: the late op completed and was not undone (L1), or an operator
then staged a good image by hand upload (that path does not use the fetch writer). The ota_page and the
MCP `update_status` formatter then show "Staged: no (writer_wedged...)" next to a valid header.

Fix: report the wedge as its own boolean field (`fetch_writer_wedged`). Override `reason` only when the
stage is not valid.

### L3. `update_stage_upload_abort()` does not check which writer owns the upload

- Where: `firmware/KilnFW/App/drivers/update/update_stage.c:437-446`; called by the wedge clean-up at
  `update_fetch.c:362`.

What happens: abort resets any UPLOADING/VERIFYING stage to IDLE regardless of source.

Scenario (narrow):

1. The fetch writer wedges inside `WR_WRITE`.
2. The update claim is released.
3. The late write fails, so the stage drops to IDLE.
4. An HTTP hand upload begins.
5. The abandoned `wr_task` then reaches its wedged branch and calls `update_stage_upload_abort()`,
   killing the hand upload.

Fix: compare the stage's recorded source (`STAGE_SOURCE_GITHUB`), or a per-upload token captured at
begin, before aborting.

### L4. `rev_repair_junk()` adds an unmeasured boot-path stack frame

- Where: `firmware/KilnFW/App/drivers/http/profiles_http.c:844-895` (512cb4e3).

What happens: the repair now holds `has_file[100]` and `back[100]` (about 500 B), plus a `profile_t`
per loop iteration. It calls `profiles_cfg_fs_load_raw()`, which puts `raw[4 + PROFILE_BLOB_MAX_SIZE]`
on the stack, plus LittleFS's own depth. It runs inside `nvs_load_all_from()`, which already holds
several 100-entry arrays, and as a single-call `static` it is likely inlined there. No stack-budget
check covers this boot path, and it runs only on the rare junk-blob boot, so an overflow would show up
first in the field.

Also, the loop computes `frev` and discards it. `maxrev` comes only from `s_profile_rev[]`.

Fix: move `has_file`/`back` to `persist_scratch_alloc()` (the pattern `zones_config_store.c` uses),
mark the function `noinline`, and either use `frev` in `maxrev` or drop the out-parameter.

### L5. `nvs_save()` mirrors the CRC into RAM outside the snapshot critical section

- Where: `firmware/KilnFW/App/drivers/persist/zones_config_store.c:780-790` (667578a9).

What happens: the snapshot is taken under `zones_cfg_lock()`, the lock is dropped, the CRC is
computed, and then `s_zones.cfg.crc32` is written in a second critical section. A setter that edits
`s_zones.cfg` between the two leaves RAM holding new fields with the old snapshot's CRC.

Today nothing outside the boot migration read-back (`:487`, single-threaded) checks the RAM CRC, so
this is latent. Two concurrent `nvs_save()` callers can also both derive `new_zones_rev = rev + 1`.
That race is pre-existing; the commit did not add it.

Fix: drop the RAM mirror, and have the migration read-back compare against a stamped snapshot, as
`zones_config_persisted_equals_ram()` now does. Serialize `nvs_save()` with a save mutex so the rev
derivation is single-writer.

### L6. Review 4 M3 (web judge write gates fail-open on suite) is still open

- Where: `tools/PcTools/src/kilnctrl/bench_test/cases_web_prof.py:61`, `cases_web_diag.py:50-62`,
  `cases_web_safety.py:101-103`.

No commit in range touches these gates. `adbbec15` removed ZONE-02's write entirely but left
`_mutating_gate()` without a suite check for the other writers. Carried forward unchanged.

## Checked, no defect found

- `11a85801`: heat-side `ota_http_heat_blocked_by_update()` re-check after
  `relay_authority_heat_zone_claim_begin()` in both autotune and run start. It correctly pairs with
  `update_http.c` `claim_refuses()` (Dekker order on both sides), and every refusal path ends both
  claims and gives the lock.
- `d19d3df4` LOW-5: heap precheck now 30836 B (8192 + 18548 + 4096 slack). This refuses the observed
  29647 B idle minimum and admits the common 31123 B sample.
- `95070c9a` run_state migration: probes the destination read-only, copies, verifies the read-back by
  `memcmp`, and erases the legacy copy only after verification. `legacy_default_nvs.c`'s probe-before-
  erase and the factory_reset `wifi`/`all` scope wiring are correct.
- `7d155f5a`: `rev_repair_junk()` PSRAM-stack guard fails closed.
- `512cb4e3`/`61e579c9`/`f9549ae4` (review 4 M2): the repair is deferred without cfg mounted (latch
  stays set), every file-less slot is raised to the max, a longer newer-firmware `prof_rev` tail is
  preserved, and both new tests exercise the real load path.
- `cb7af514` (LOW-7): NVS slot erase now precedes the RAM clear, so a failed delete stays retryable;
  the test checks the slot survives.
- `9155d194` (review 4 L2): the oversized builtin catalogue is refused in pass 1, before any pref write.
- `80b957da` (LOW-3): aux store I/O now runs into locals and `s_lock` is held only to publish. No
  shared state is read in the unlocked section (boot-only, before any `set()`).
- `667578a9`/`ccabb020` (LOW-4): the persisted blob is a locked snapshot; the copy-only critical
  section has no I/O, allocation or logging inside it. See L5 for the residual.
- `8a4739e6` (LOW-8): the shared origin glue keeps the overlong-Origin/Host refusal. A truncated
  Referer is accepted only when its authority ended inside the kept bytes, and is refused otherwise.
  The prehandler now keys on `req->method`. The recovery image uses the same helper.
- `8c287553` (review 4 M1, L1): fault-drop candidates are now every non-zone relay plus claims.
  Disabling an enabled aux drives the relay OFF, and an I/O failure is reported as 500. A failed aux
  write no longer counts as a transition or a relay cycle. Logging is rate-limited per relay.
- `adbbec15` (review 4 H1, H2, L3): KCFG-02 no longer calls `save` and never deletes the source
  config (`active_id` is `null` when none, so the fallback works). ZONE-02 is read-only. LOG-02/X-02
  log in before any password write and report INCONCLUSIVE, not FAIL, when that login fails.
- `513c07e6`/`c2c0267a`: JS mock bodies and the `writer_wedged_reboot_required` page text.
