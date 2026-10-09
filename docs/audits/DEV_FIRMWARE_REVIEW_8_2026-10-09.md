# Dev firmware review 8 (2026-10-09)

Scope: the dev commits that answer reviews 6 and 7.

- `ba714c93`: Wi-Fi legacy migration adopt, now keyed on a verified saved_nets record.
- `6afbcb6f`: profiles cfg_fs `load_raw_ex`/`resolve_ex` OOM path, and the zones unlock hook ifdef.
- `2841144c`: update_stage busy wedge reason, atomic `claim_upload` source, `abort_owned` under the lock, v1+v2 identity records, `UPDATE_STAGE_HEAD_LEN` 344.
- `9e2ac9ef`: WEB-ZONE-10 write gate.

Review base: origin/dev at `2c85ccc0`.

High: none. Medium: none.

## Low

**L1 [Fixed in 523ba6dc] (2841144c). Dev boards on the v2-only gate cannot take the new image over GitHub.**
Code: `firmware/KilnFW/App/drivers/update/update_fetch.c:296` places v2 at offset 308. `update_stage.h:47` sets the head length to 344.
Builds from `dcd67f54` up to `2841144c^` have a 328-byte head and scan only for the v2 magic, at offsets up to 292. They never look for v1. Such a board finds no record in any image from `2841144c` on. Every GitHub fetch is refused as POLICY, and a hand upload needs force plus the typed confirm. No single layout serves both old gates: v1 must start by 300 and v2 by 292, so the two records would overlap. This is the same class as review 7 L4, now on dev-only boards. Fix: document that the first update from those builds is a forced hand upload.

**L2 [Fixed in 523ba6dc] (2841144c). A v1-only image skips the commit binding.**
Code: `update_stage.c:388` skips the commit-prefix check when `id->magic == UPDATE_IMAGE_ID_MAGIC_V1`. `update_policy.c:270` provides the v1 fallback.
Scenario: a release whose manifest names commit X but whose asset is an older build. That build is any image from `735875b6` up to `dcd67f54^`, which carries only v1. If its schema versions match, the gate accepts it with no commit check. This reopens review 5 M1 for that image class. Fix: refuse a v1 record when the manifest commit is non-empty. Otherwise, accept the gap and document it.

**L3 [Fixed in 523ba6dc] (6afbcb6f). The OOM fallback at boot erases the rev-unknown marks.**
Code: `firmware/KilnFW/App/drivers/http/profiles_http.c:1815`, with the memset at `:933`.
When `nvs_load_all_from` returns NO_MEM, `profiles_boot_load` falls back to `nvs_load_files_only`. That function first clears `s_profile_rev_unknown`, so the marks pass 1 set are lost. If the allocation succeeds on the retry, a slot whose only copy is the legacy NVS profile (no file) reads as free and saveable. A save there writes rev floor+1, the new file wins on every later boot, and the NVS profile is gone. The new OOM test calls `nvs_load_all_from` directly and does not cover this path. Fix: on NO_MEM, return the error without the files-only fallback, or keep the marks across it.

**L4 [Fixed in 523ba6dc] (6afbcb6f). Review 7 L3 is only partly fixed: junk repair still uses the error-blind load.**
Code: `profiles_http.c:875`, where `rev_repair_junk` calls `profiles_cfg_fs_load_raw` and not `_ex`.
Scenario: the rev array is junk, and the scratch allocation fails inside this load. A live file then reads as fileless. Its floor is raised to maxrev and persisted. On the next boot `resolve` sees `nvs_valid` false and deletes the live file as stale (`persist/profiles_cfg_fs.c:241-262`). Or, if a differing legacy NVS copy is valid, NVS wins and overwrites the file. Either way the profile is lost silently. Fix: use `load_raw_ex` here and return false, failing closed, on error.

## Earlier findings, status

- Review 6 M1 (Wi-Fi adopt) and L1 (comments): fixed.
- Review 7 L1, L2 (`claim_upload` source race), L4 and L5: fixed.
- Review 7 L3: partly fixed (L4 above).

## Checked, no defect

- The v1 record (magic, versions, check word) at offset 288 matches what the old 320-byte gate expects.
- The `image_id_pair_t` static assert holds with no padding.
- The symbol sits in `.rodata_custom_desc` and is kept by the linker.
- `abort_owned`: the source/phase check, `sha_abort` and the IDLE move all run under one non-recursive lock. `sha_abort` takes no lock, so there is no deadlock.
- The busy-with-GitHub status now reports the wedge reason and the source.
- The zones unlock hook ifdef matches the host-test build line that compiles its only user.
- The rev-unknown re-apply after a successful junk repair is correctly ordered in both loaders.
- Wi-Fi with a corrupt, wrong-size or wrong-version saved_nets record: adopt and re-migrate, which is safe.
- An interrupted Wi-Fi migration is retried on the next boot.
- One Wi-Fi edge case: a board that only ever ran the `022d464e` split-era build, before saved_nets existed, would adopt the frozen legacy copy over newer wifi_nvs data. That population is negligible.
- `9e2ac9ef`: WEB-ZONE-10 now gates on `write_refusal()` before any read or POST. The suite-gate test now lists it as a writer.
