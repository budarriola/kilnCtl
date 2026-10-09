# Dev firmware review 7 (2026-10-09)

Scope: the review 5 fixes on `origin/dev`, `dcd67f54` (M1 image-embedded commit gate, L1 writer arbiter
`update_wr_arb.h`, L2 status reason, L3 owned abort) and `f19b7c62` (L4 heap scratch for
`rev_repair_junk()`/`profiles_cfg_fs_load_raw()`, L5 guarded CRC write-back in `nvs_save()`), plus the doc
commits up to `96a325ec`. Code review only: no builds, no board.

Verdict: M1, L1 and L5 are correct and their tests are not vacuous. L2 regresses the case it was meant to
keep. L3 still has a narrow race. L4 adds a new failure mode on allocation failure.

## HIGH

None.

## MED

None.

## LOW

### L1. L2 fix hides the wedge in its most likely state ("busy" instead of "reboot required")

- Where: `firmware/KilnFW/App/drivers/update/update_stage.c:474-483` (`update_stage_status_reason()`),
  `:508-516` (busy branch of `update_stage_get_status()`), `update_http.c:454-455`.

A real wedge is a writer op that never returns. While it hangs, the stage phase stays UPLOADING or
VERIFYING, so `update_stage_get_status()` cannot claim the stage and returns `reason = "busy"`. The new
helper replaces only `"blank"`, so for the whole life of a real wedge (until reboot) `GET` stage status
reports `busy`. Before `dcd67f54` it reported `writer_wedged_reboot_required`. The new
`fetch_writer_wedged` field carries the fact, but nothing reads it: `ota_page.html:863` and the PcTools
formatter key only on `reason`. The operator sees a stage that is "busy" forever and gets no hint to reboot.

Fix: also substitute the wedge reason when `info->busy` is set (the stage cannot be busy for any other
reason once the fetch writer is wedged, apart from a hand upload, which the upload route already refuses
with the wedge name at `update_http.c:132`). Alternatively, teach the page and `update_status` to read
`fetch_writer_wedged`. Add a test case for `reason = "busy"` with `busy = true`.

### L2. Owned abort reads `source` unlocked, and begin sets `source` after the claim

- Where: `update_stage.c:462-470` (`update_stage_upload_abort_owned()`), `:142-151`
  (`update_stage_upload_begin()`: `claim()` sets the phase under `lk`, and `st->source = source` follows
  outside it). Caller: `update_fetch.c:375`.

`st->source` keeps the abandoned fetch's `STAGE_SOURCE_GITHUB` until a new begin overwrites it. Scenario:

1. A wedged `WR_WRITE` finally fails, so the stage drops to IDLE.
2. A hand upload's `update_stage_upload_begin()` wins `claim()`, so the phase is UPLOADING.
3. Before that upload stores `source = UPLOAD`, `wr_task` (pinned to the other core) reaches its abandoned
   branch.
4. `abort_owned` sees GITHUB plus UPLOADING. It aborts the hand upload's SHA context and sets IDLE.

The window is a few stores wide, so the race is unlikely, but this is exactly the L3 outcome. The new test
only covers the sequential case.

Fix: set `source` inside `claim()` (under `lk`), and do the source/phase test and the IDLE transition in
one `lk` section in `abort_owned`. Better still, capture a per-upload generation counter at begin and abort
only when it is unchanged.

### L3. `profiles_cfg_fs_load_raw()` allocation failure now reads as "file absent", and resolve then overwrites the file with the stale NVS copy

**Fixed in SHA_PLACEHOLDER.** `profiles_cfg_fs_load_raw_ex()`/`profiles_cfg_fs_resolve_ex()` add an error channel; resolve writes/deletes nothing on error and both boot loaders mark the slot rev-unknown (saves/deletes refused) and return `ESP_ERR_NO_MEM`.

- Where: `firmware/KilnFW/App/drivers/persist/profiles_cfg_fs.c:96-100` (new allocation, returns with
  `*out_valid = false`), consumed by `profiles_cfg_fs_resolve()` at `:194-208`.

The commit says "the caller already fails closed on that". That holds for `rev_repair_junk()` and
`retarget_verify_slot()`, but not for resolve. With `file_valid == false` and `nvs_valid == true`, resolve
adopts the NVS profile and calls `profiles_cfg_fs_save(id, nvs_profile, mig_rev)`. Since the NVS dual-write
close, the NVS slot is a legacy copy that no save updates (`profiles_http.c:2097-2100`). So a transient
allocation failure at boot replaces the current profile file with an older version at an older rev. With
`nvs_valid == false`, the slot reads as unused for that boot, and the next save into the "free" slot
destroys the file. Before `f19b7c62`, `raw[]` was on the stack and this could not fail this way. The
probability is low (a PSRAM-then-internal allocation must fail at boot), but the result is silent loss of
user data.

Fix: give `load_raw` a tri-state result (valid / absent-or-bad / error) and have resolve keep RAM empty and
skip the migration write on error, or allocate the scratch once in the caller and pass it in.

### L4. Builds between `735875b6` and `dcd67f54` cannot take any later release over GitHub

- Where: `update_policy.h:115` (magic changed from `0x4B494449` to `0x32444B49`), and the old firmware's
  `update_stage_manifest_gate()`, which returns POLICY on `id == NULL` (unchanged at `update_stage.c:367`).

A board running a v1-record build (on `origin/main` today, not in any tag; `v1.0.0-pre.1` predates the
record) scans a new image for the v1 magic, finds none, and refuses every release fetch. A hand upload
works only with force plus the typed confirm, because the image reads as "no schema identity record". No
released firmware is affected, but the bench board probably is. Either note in the release notes that the
first post-change update must be a hand upload, or emit a v1 record as well (the 20-byte v1 record at
offset 288 followed by the v2 record; this needs `UPDATE_STAGE_HEAD_LEN` at 344 or more).

### L5. The zones unlock test hook ships in production firmware

**Fixed in SHA_PLACEHOLDER.** Hook declaration, definition and call are behind `KILNCTL_ZONES_UNLOCK_TEST_HOOK`, defined only in the zones cfg_fs host-test build line.

- Where: `firmware/KilnFW/App/drivers/http/zones_http.c:423-429`, `persist/zones_http_internal.h:216`.

`s_zones_cfg_unlock_test_hook` is a writable global function pointer that every production
`zones_cfg_unlock()` loads and calls through when it is non-NULL. Unlike `persist_scratch_alloc()`'s seam,
it is not behind an `#ifdef`. Nothing sets it on target today, but any stray write to it turns every
zones unlock into a call through a corrupted pointer. Gate it the same way, for example
`#ifdef KILNCTL_ZONES_UNLOCK_TEST_HOOK`, and define that only in the zones host-test build.

## Checked, no defect found

- M1 gate (`update_stage.c:365-387`): the comparison is now between the image's embedded commit and the
  40-hex manifest commit (`update_release.c:912` requires it). Prefix compare, case-folded, 7 to 15 hex
  characters. `"unknown"`, `""` and non-hex all fail closed. A 16-character `git rev-parse --short` would
  initialize `commit[16]` without a NUL, but `update_image_id_find()` truncates it to 15 and that is still
  a prefix. `make_release.ps1` builds from HEAD, which is the commit the manifest names, so production
  releases match. The test now feeds mismatching images, not a mismatching argument.
- `UPDATE_STAGE_HEAD_LEN` 320 to 328: the record at offset 288 (32 + 256) ends at 324, so 4 bytes of slack
  remain. The stage header format is unchanged, so an already-staged image stays installable. The recovery
  image never parses the identity record (no reference under `firmware/KilnFW_recovery`), so it is
  unaffected. `put()` has no alignment requirement on the 328-byte head write. There is still no
  built-artifact check that `.rodata_custom_desc` lands at offset 288; that gap predates this change.
- L1 arbiter: `issue`/`writer_done`/`caller_timeout` are each serialized by `s_wr_mux`. Either the writer
  sees WEDGED and undoes its own result, or the caller sees DONE and collects it; both cannot be skipped.
  The writer's reads of `c->cmd`/`c->res` in the cleanup are its own. `wr_wedged` is set after the arbiter
  decides and is read only by later calls. The leftover 1000 ms re-take is reached only when the writer
  already marked DONE and is about to give.
- L5 (`zones_config_store.c:787-800`): memcmp with the CRC field swapped in is correct. Both copies come
  from `memcpy`, so padding matches. Only a copy and compare happen inside the critical section. The test
  is not vacuous: `fill_valid_cfg()` leaves `crc32 = 0`, so the pre-fix code would have stamped a nonzero
  CRC.
- L4 `rev_repair_junk()`: the scratch is freed on every path after allocation (the open-failure path and
  the normal exit). Allocation failure returns false with the rev still unknown. The OOM test checks the
  allocation is reached (`seen == 1`) and that the blob is not rewritten.
