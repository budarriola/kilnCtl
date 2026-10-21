# Review: OF-05 fix (and OF-01..OF-04), 2026-10-20

Scope: a71d481cb (OF-01..OF-04), 25eea695e and d7020a281 (OF-05) on origin/dev. 993f3f14f, named in the
request, is not on any remote branch. 25eea695e is the landed copy and has the same content. Files:
`update_install_record.c`, `update_install_http.c` (`running_image_id`), and `dashboard_http.c`
(`dashboard_running_image_len`). Review only. No code was changed.

Result: HIGH 0, MED 1, LOW 4, INFO 4.

## Findings

### OG-01 MED: when either id is unknown, the match falls back to semver only, and canonicalisation makes semver matches wider
`update_install_res_matches_running` skips the id compare when the running id is NULL (`image_len` unknown,
hash error) or when the record id is all zero. benchmain30 recorded a zero record id (OF-05). In that case
two different builds with the same canonical semver match again, so the OF-01 bug comes back:
`update_install_confirm_result` writes `confirmed` into a record that names another image, and status
derives `confirmed`. `update_semver_from_describe` also makes these matches wider:
- it strips `-dirty`, so a dirty build matches the clean one;
- `X-N-gHASH` with no prerelease drops N, so every dev build after release `1.0.0` canonicalises to `1.0.0`
  and matches the release.

Impact: the record and status only. otadata and rollback are not affected. Fix: when the record id is zero,
or the running id is unknown, report `recorded`/unknown and never write `confirmed`. Or compare only when
both ids are known, and keep semver for display. Also find out why the record id is zero (OF-05 bench
follow-up). Add tests for the dirty case and the no-prerelease case.

### OG-02 LOW: the 3 MB hash runs on the httpd worker and on every boot
`running_image_id` is called on every GET `/api/update/status`, before the handler checks `have`. The first
call hashes about `image_len` bytes (about 3 MB) in 512 B reads, roughly 6000 `esp_partition_read` calls
plus PSA updates, with `vTaskDelay(1)` every 64 KB. Estimate: about 0.5 to 1.5 s, during which the single
httpd worker serves nothing else. `update_install_http_mark_confirmed` also hashes once per boot while it
holds the update claim, even when there is no record or the record is already confirmed. A concurrent upload
attempt then gets a busy 409 for that time. Watchdog: the loop yields every 128 chunks, which is far inside
the 5 s TWDT, so this is not a WDT risk. Fix: compute the id only when the record is OK and unconfirmed, and
prime it from the ota_confirm task before the claim is taken.

### OG-03 LOW: the cache is published without ordering
`static volatile bool have` does not order the plain `cached[]` writes. On the dual-core S3, a second caller
(httpd versus ota_confirm) can see `have == true` before `cached` is complete, and then compare a torn id.
The worst case is a false `not_running` or a false NOT_RUNNING. A false match is not credible. Two callers
that arrive together each hash the full image. Fix: publish under a spinlock or with
`__atomic_store_n(..., __ATOMIC_RELEASE)`, or prime once at a single point.

### OG-04 LOW: the hashed byte range depends on `image_len` matching the uploaded file length
The record id is the stage header sha256 over `image_length` bytes of the uploaded `.bin`. The running hash
covers `esp_image_get_metadata().image_len`, which is header + segments + padding to 16 + checksum + the
appended sha256. For today's unsigned images that equals the esptool `.bin` size, so the two ranges match.
They stop matching if:
- secure boot v2 is enabled (the 4 KB signature sector is in the file but not in `image_len`);
- a release asset is padded.

Both cases fail safe (`not_running`), but silently. `stale_running_matches` uses `req->image_length`, which
is exact. The result record does not store a length. Fix: store `image_length` in the result record and hash
that range, or add a host/target test that `image_len == file size`. Flash encryption is fine:
`esp_partition_read` decrypts.

### OG-05 LOW: describe strings of 32 characters or more are truncated in the record
`UPD_INST_SEMVER_LEN` is 32. `1.0.0-pre.1-2276-ga71d481-dirty` is exactly 31 characters.
`pre.10`, a 5-digit count, or a longer abbrev pushes it past 31, and the truncated string fails
`update_semver_from_describe`. It is then passed through raw and never equals the converted running form, so
the record reads `not_running`. Fails safe. Fix: canonicalise before the record is written, so the record
stores the short form.

### OG-06 INFO: `image_len` is 0 or unknown
`dashboard_running_image_len` returns false when the metadata read failed or the length is 0.
`running_image_id` also rejects `ilen > run->size`. The id is then NULL and the code falls back to semver
only (see OG-01). Flash facts are primed by `dashboard_http_start()` on app_main, before ota_confirm starts.
A late first call would run the metadata read on an internal-DRAM stack, which is safe.

### OG-07 INFO: stack use and cache-disable
The new frame is the 512 B buffer + `psa_hash_operation_t` (about 200-250 B) + 32 B. It runs on httpd (8192 B)
or ota_confirm (5120 B; the OF-04 high-water log before the mark loop now measures it). Both stacks are
ordinary internal `xTaskCreate` stacks, so the cache-disable during `esp_partition_read` is safe. Confirm with
the OF-04 log line on the next bench boot.

### OG-08 INFO: semver canonicalisation is correct for the release, dev and recovery strings
- Release `v1.0.0-pre.3` becomes `1.0.0-pre.3`.
- Dev `1.0.0-pre.1-2276-ga71d481` becomes `1.0.0-pre.1.2276`, which equals the running form (tested). The
  d7020a281 removal of the `v`-strip is correct: `update_semver_parse_n` accepts a leading `v` (the test
  covers it).
- A string that cannot be parsed (for example a recovery name) passes through unchanged and is compared
  exactly.
- Confirm is ESP-app-only (OC-09). In recovery, the status id is the hash of the recovery image, so it
  correctly reads `not_running`.

The exceptions are the widening in OG-01 and the truncation in OG-05.

### OG-09 INFO: OF-01..OF-04 (a71d481cb)
- OF-01's original `esp_partition_get_sha256` returned the digest appended inside the image, so a non-zero
  record id could never match. OF-05 replaced it correctly.
- OF-02 `not_running` applies only to OK, unconfirmed records. Correct.
- The OF-03 message is fine.
- The OF-04 early high-water log is fine.

Test gaps:
- `running_image_id` has no test (target-only code).
- There is no test for `-dirty` or for a no-prerelease describe string.
