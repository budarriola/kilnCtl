# Dev firmware review 12 (2026-10-09)

This is a read-only Opus review of three origin/dev commits. No code was changed and the board was not touched.

| Commit | Subject |
|---|---|
| `753f40f6` | webfix MED-2, LOW-1..INFO: run_queue builtin refusal and stray delete, blank guard/xzone field keeps the stored value, `_format_scalar` refuses "", `note=%00` message |
| `462eb838` | review 10 LOW-1/2/3: `kiln_cfg_swap_zone_edits_at_risk()` 409 on POST /api/zones, fetch-heap comment fixes, `relay_authority_reset_in_flight()` pre-checks in ramp_assist_cfg.c and setup_wizard_progress.c |
| `65ac0650` | profiles: retire the legacy `profN` NVS blob once a strictly higher-rev cfg file is adopted |

The reviewed tree is origin/dev `ca6eb058`. Paths are relative to `firmware/KilnFW/App/drivers/` unless they say otherwise.

There are no HIGH or MED findings. All three commits do what they say on the paths they touch, and nine of the eleven C mutations and all three Python mutations are caught. The findings below are about paths the commits do not reach, comments that claim more than the code does, and one partial-failure path.

## Findings

### LOW-1 (462eb838): the zone-edits-at-risk gate covers only POST /api/zones

**Where**
- `http/zones_http_post.c:225`. The gate `kiln_cfg_swap_zone_edits_at_risk()` runs only in the main POST /api/zones handler.
- The other writers of the same zones blob reach `nvs_save()` without it:
  - POST /api/zones/pid (`http/zones_http_pid.c`)
  - `control/adaptive_tune.c`
  - autotune finalize: `zones_config_set_coupling_cell()` from `control/autotune_engine_step_identify.c`, and the ceiling write in `control/autotune_engine_guard.c`
  - the UART bridge setters (`bridge/uart_bridge_ext*.c`)
  - `persist/zones_config_accessors.c:43/78/111` (max ramp, coil power, cal offset)
  - zone normals and CT-map setters in `persist/zones_config_store.c`
  - `backup_import.c`
  - the aux-restore `nvs_save()` in `http/zones_http_post.c:176`

**What goes wrong**
- Take the case where the active_id restore failed, so `s_rollback_id_kept` is set and the journal is kept.
- An adaptive-tune or autotune write, or a PID-page save, still lands in live zones.
- On the next boot, the kept ESP_DONE record no longer matches either side, so the boot latches `ESP_DONE_UNCONFIRMED` (heaters alarmed), or a PICO_* record re-applies R over the edit.
- That is exactly the outcome the new 409 exists to prevent, reached through a different writer.

**Suggested fix**
- Put the check in one shared zones-write gate that every writer passes through: inside `nvs_save()`, or a wrapper that `nvs_save()`'s non-boot callers use.
- Return a distinct error that each HTTP writer maps to the same 409.

### LOW-2 (462eb838): a failed journal clear after a successful rollback does not raise the at-risk flag

**Where**
- `persist/kiln_cfg_swap.c:576` sets `s_rollback_id_kept` only on the id-restore-failure branch.
- The `!clear_pending()` branch at `persist/kiln_cfg_swap.c:593-598` keeps the journal too. Its own LOW-4 comment says edits made after it "can latch ESP_DONE_UNCONFIRMED ... or be re-imported over". It does not set the flag, so POST /api/zones keeps accepting edits in that state.

**Also: the refusal text overstates one path**
- The 409 text says edits made now would be lost or reverted at the next boot.
- On a kept ESP_DONE record, `finish_esp_done_impl()` does not re-import. A post-rollback edit makes live differ from both R and the target, so the boot latches `ESP_DONE_UNCONFIRMED`. That is a fault with the heaters alarmed, not a silent revert.

**Suggested fix**
- Set the flag (or a sibling flag behind the same accessor) on the UNCLEARED path as well.
- Word the refusal as "may be lost or latch a boot fault".

### LOW-3 (753f40f6, pre-existing behavior): an omitted `z%u_xzone` still disables guard 8, while the new comment cites "no guard-disable path"

**Where**
- `http/zones_http_post_parse.c:23-34`. The new blank-tolerant helper's comment says a blank xzone "must never ... disable guard 8 (owner decision: no guard-disable path)".
- `http/zones_http_post_parse.c:704-716`. The comment above the xzone parse still says an OMITTED key means "leave the guard disabled" (z is zero-initialized), and the code still does that.

**What goes wrong**
- A client that leaves the key out entirely turns guard 8 off without any error. Examples are an older PcTools build, a hand-written curl, or a page that does not render the field.
- The blank case is fixed. The omitted case is the same disable through a different spelling.

**Suggested fix**
- Treat an omitted `z%u_xzone` like the blank case and keep `current_z->cross_zone_max_delta_c` (omit-preserve).
- Or refuse the POST with 400 when the key is missing for a configured zone.
- Either way, update the stale comment at line 708.

### LOW-4 (65ac0650): a partial retire failure is never retried, and the log says it will be

**Where**
- `http/profiles_http.c:1282-1314`, `retire_legacy_slot_blob()`. It erases `profN`, then read-modify-writes the used bitmap, then commits.
- `http/profiles_http.c:1219` logs "(will retry next boot)" on any non-OK result.

**What goes wrong**
- Suppose the key erase succeeds and `used_bitmap_save()` then fails, for example because NVS is full or a write error occurs.
- `hal_kv_erase_key` is effective on its own, because ESP-IDF commit is a no-op for erases, so the blob is gone while its bitmap bit stays set.
- On the next boot the load loop at `http/profiles_http.c:1135-1141` finds the bit set and the key missing. It logs "load ... failed -- marking unused" and sets `slot_blob_bad[id]`. The retire condition requires `!slot_blob_bad[id]`, so it never runs again.
- Result: no data loss, because the file is adopted through the "NVS unused" path. But the stale bitmap bit and a WARN line remain on every boot, and the earlier "will retry" log was false.

**Suggested fix**
- Clear the bitmap bit first and erase the key second, so a failure part-way leaves a harmless orphan key rather than a dangling bit.
- Or let the retire condition also handle "bit set, key NOT_FOUND" by clearing the bit.
- Make the log line say what actually happened.

### LOW-5 (65ac0650): two parts of the retire logic are not pinned by any test (negtest MISSED)

**Where:** `test/test_profiles_http.c:1020-1082`, the tests `test_pcfg_adopted_file_retires_legacy_nvs_blob` and `test_pcfg_retire_keeps_blob_when_file_not_adopted`.

**Bitmap clear is untested**
- Mutation: delete `profiles_slot_bitmap_clear(&nvs_used, id)` from `retire_legacy_slot_blob()`. The suite still passes.
- The test checks that the blob is gone and the slot is used in RAM. It never reads back the persisted `used` bitmap.
- Without the clear, every later boot logs "load ... failed -- marking unused" for the slot. That is the same dangling-bit state LOW-4 describes, reached on the success path.

**Strict `>` is untested**
- Mutation: change `resolved_rev > nvs_rev[id]` to `>=`. The suite still passes.
- The equal-rev case uses differing bytes, so the resolve adopts NVS, `used_file` is false, and the rev comparison is never reached.
- An equal-rev, identical-bytes case, where the file may be adopted, is not exercised. So whether `>` is actually needed, or only redundant with `used_file`, is unverified.

**Suggested fix**
- Read back the persisted `used` bitmap after retirement and assert bit 0 is clear.
- Add an equal-rev, identical-bytes case that asserts the blob is kept, or confirm the resolve never adopts the file there and drop the redundant clause.

### INFO-1 (462eb838): the reset pre-checks narrow a race but do not close it, and the comments overclaim

**Where**
- `control/ramp_assist_cfg.c:157-161` ("refuse BEFORE touching RAM, so a refused save leaves live == stored").
- `persist/setup_wizard_progress.c:629-634`, same claim.

**What goes wrong**
- `relay_authority_reset_in_flight()` is a lock-free flag. The factory reset takes neither `s_save_lock` nor any lock that these paths hold.
- A reset that starts after the pre-check passes, but before the inner re-check (`ramp_assist_cfg.c:170`, `setup_wizard_progress.c:581`), still produces "RAM changed, save refused".
- `setup_wizard_progress_set_step()` holds no lock at all, so concurrent set_step calls also race on `s_steps`/`s_rev`. That race is pre-existing.
- Impact is negligible, because a factory reset reboots. Only the comments are wrong.

**Suggested fix**
- Reword the comments to "narrows".
- Or restore the previous RAM value when the inner re-check refuses.

### INFO-2 (65ac0650): retiring the NVS copy removes the last fallback when cfg is lost, and rev-0 blobs are never retired

**Where:** `http/profiles_http.c:1206-1213`.

**Fallback lost**
- After retirement, the cfg file is the only copy of the profile.
- On a later boot where cfg is unmounted or formatted (`cfgfs_format`, or the auto-format gate), the slot reads as free and the profile is gone. Before this commit the stale NVS blob would have served as an older fallback.
- Arguably this is better than serving stale content, but it is a new behavior and not documented anywhere.
- A downgrade to firmware that treats NVS as authoritative also loses retired profiles.
- Suggested fix: note both points in `docs/CONFIG_FILESYSTEM.md`.

**Rev-0 blobs**
- The `nvs_rev[id] != 0` exclusion leaves legacy rev-0 blobs in place for good, with the DIVERGED warning on every boot.
- Retiring at rev 0 looks safe: the rev array is untouched, and "rev 0 means never deleted" keeps the file live.
- Suggested fix: decide this explicitly and either retire rev-0 blobs or document why they stay.

### INFO-3 (753f40f6): the `save_profile_segments` result check can escape as AttributeError

**Where:** `tools/PcTools/src/kilnctrl/run_queue.py`, `save_profile_segments`.

**What goes wrong:** `result.get("ok")` raises `AttributeError` rather than `RunQueueError` when the board returns valid JSON that is not an object.

**Suggested fix:** check `isinstance(result, dict)` first.

### INFO-4 (462eb838): a stale arithmetic line remains in update_fetch_heap.h

**Where:** `update/update_fetch_heap.h:45-46`.

**What goes wrong:** the line still reads "29647 - 16500 = 13147 B, 4955 B above the 8192 B floor". The comment above it was updated to the 17524 B worst draw and the 29556 B precheck.

**Suggested fix:** recompute the line from the new figures or delete it.

### INFO-5 (462eb838): the rollback-pending zones test does not assert the 409 or that nothing was written

**Where:** `test/test_zones_http.c`, `test_zones_post_refused_while_rollback_pending`.

**What goes wrong:** the test checks the body text and that `ok` was not reported. It does not check the HTTP status or that no NVS or cfg write happened. A regression that sent 200 with the same text, or that saved before refusing, would still pass.

**Suggested fix:** assert the status code and an unchanged blob or write counter.

## Negative tests (tools/negtest.ps1)

The C mutations ran against the tree at `ca6eb058` with the host tests selected by `-Only 'test_zones_http|test_profiles_http|test_kiln_cfg_swap|test_ramp_assist_cfg|test_setup_wizard_progress'`. The baseline passed. negtest ended with "REAL TREE CHANGED", but the only change was this review document being written in the same worktree during the run.

| Mutation | Verdict |
|---|---|
| xzone blank keeps stored value: removed | CAUGHT |
| wrong-dir-window blank keeps stored value: removed | CAUGHT |
| POST /api/zones at-risk gate: removed | CAUGHT |
| `s_rollback_id_kept` never set | CAUGHT |
| `s_rollback_id_kept` never cleared | CAUGHT |
| ramp_assist reset pre-check: removed | CAUGHT |
| setup_wizard reset pre-check: removed | CAUGHT |
| retire call: removed | CAUGHT |
| retire also erases the prof_rev array | CAUGHT |
| retire on equal rev (`>` to `>=`) | **MISSED** (LOW-5) |
| retire leaves the used bitmap bit set | **MISSED** (LOW-5) |

The Python mutations used `-Preset pytest` on `tests/test_run_queue_inplace_rewrite.py tests/test_zones_http_client.py`. The baseline passed and all 3 mutations were CAUGHT: builtin refusal removed, stray delete removed, blank-scalar refusal removed.

## Checked and fine

- The `753f40f6` run_queue builtin refusal (ids >= 128) runs before any POST, and the stray-delete target `/api/profile/delete` exists (`http/profiles_http.c:2155`).
- The blank-field handling in `zones_http_post_parse.c` copies `current_z` values, not firmware defaults, for every guard field and xzone.
- The `65ac0650` retire condition requires `used_file`, which implies a mounted cfg and a validated file, and the rev array (floor) is left untouched. An equal rev adopts NVS and does not retire. The unmounted case is tested.
- `nvs_load_all_from()` has a single caller (`profiles_boot_load_body`, boot only), so the retire erase never runs concurrently with a save.

## Fix status

- LOW-1, LOW-2, LOW-4, INFO-4, INFO-5: fixed in `422d7d35`. `nvs_save()` is the shared gate (refuses while the rollback journal is kept), the journal-clear-failed path sets the at-risk flag, the 409 text is shared, and `retire_legacy_slot_blob` clears the used bit first and erases the key second.
- LOW-5: fixed in `df990cb3` (persisted used-bitmap and equal-rev identical-bytes tests; both former MISSED mutations are now CAUGHT).
- LOW-3: fixed upstream in `fa4a62ef`.
- Follow-up (thermo writers): the THERMO UART writers (config_channel, set_thresholds, set_cj_offset, clear_faults, write_reg) are refused with "refused: run active" while a profile or autotune is active (`f1567116`, `uart_bridge_thermo_gate.h`). Reads stay allowed.
