# Review: KilnFW GitHub auto-update path (2026-10-17)

Static, read-only review of `origin/dev` (no bench access, no code changed). Scope: release lookup and
version compare, TLS/certificates, download into the stage, sha256 and correct-project checks (no release
signing, owner 2026-10-07), stage/install/boot confirm, mid-download failure, retries, internal RAM during
TLS. Items already in `docs/KNOWN_ISSUES.md` or in the earlier update audits
(GITHUB_UPDATE_CHAIN_REVIEW_2026-10-09, UPDATE_PATH_ROBUSTNESS_AUDIT_2026-10-13, AUDIT_UPDATE_FAULTS,
REVIEW_RAM_OTA_2026_10_17) are not repeated. That includes expired chains being accepted
(`CONFIG_MBEDTLS_HAVE_TIME_DATE` unset), the old stage being lost once `flush_head` erases the header, the
404 to `no_stable_release` mapping, UP-01..UP-03, and the 8704 B largest-block transient.

Counts: HIGH 0, MED 0, LOW 2, INFO 1.

## Findings

### AUP-01 LOW: the partition-table check rests on the manifest only
- Where: `drivers/update/update_policy.c:97-103` (compares `running->partitions_sha256` with
  `candidate->partitions_sha256`), `update_stage.c:432-470` (`update_stage_manifest_gate`).
- Problem: the candidate partition hash comes only from `release.json`. The image's embedded identity
  record (`update_image_id_t`) binds the commit prefix and the zones/kilnlink/UART schema numbers, and the
  gate checks those against the manifest. Nothing in the image bytes carries the partition hash, so the
  gate cannot check it.
- Scenario: a release tool bug, or a hand-edited manifest, publishes the manifest of build A next to the
  image of build B. B has the same commit and schemas but was linked for a different partition table (for
  example a rebuilt branch with a bigger app slot). The sha256 matches because it is B's hash. The policy
  passes on A's partition hash, and the image installs into a layout it was not built for.
- Fix: add the partitions hash (or its first 8 bytes) to a v3 image-id record from `FW_PARTITIONS_SHA256`,
  and have `update_stage_manifest_gate` require it to equal the manifest's value. Until then, the release
  script should take the manifest hash from the same build output as the image.

### AUP-02 LOW: the stable channel trusts /releases/latest, which is ordered by date, not by version
- Where: `drivers/update/update_fetch.c:1036-1053` (`use_list = allow_prerelease`; otherwise
  `update_url_build_latest` + `update_release_parse_api`).
- Problem: GitHub's `/releases/latest` returns the most recently created non-draft, non-prerelease release.
  It is not the highest semver. The list path already picks the highest semver
  (`update_release_pick_from_list`), and it takes an `allow_prerelease` argument, but the fetch always
  passes `true` and uses the list only when pre-releases are allowed.
- Scenario: v1.1.0 is released, then a v1.0.1 fix for the older line is published. A board on 1.0.0 with
  pre-releases off sees v1.0.1 and stays below 1.1.0. A board already on v1.1.0 reports
  `refuse_downgrade`, and the check shows no update until another release is created.
- Fix: read the list endpoint on both channels and pass `allow_prerelease` through to
  `update_release_pick_from_list(..., w->p.allow_prerelease, ...)`. Keep `no_stable_release` for the case
  where the list has no usable stable element. Add a host test with list order [1.0.1, 1.1.0].

### AUP-03 INFO: a declared version passes when the image descriptor version cannot be parsed
- Where: `drivers/update/update_stage.c:283-307` (`flush_head`, `semver_given` branch).
- Problem: the GitHub stager declares the manifest version. The descriptor is compared with it only when
  the descriptor normalises to a valid semver. A blank, `unknown` or otherwise unparseable `app_desc.version`
  is accepted under the manifest's version. Binding then rests on the commit prefix (at least 7 hex
  characters) and the schema numbers from the image-id record. This is acceptable, but weaker than the
  comment ("a declared version may not override a valid one") suggests for a GitHub release.
- Fix (optional): in the gated path (`st->gate != NULL`), refuse a descriptor version that does not parse
  (`UPDATE_STAGE_ERR_BAD_VERSION`). Release builds always set PROJECT_VER, so the only cost is refusing
  malformed images.

## Checked and found clean
- URL rules: https only, lowercase host allow-list (api.github.com, github.com,
  objects/release-assets.githubusercontent.com), a bounded redirect count, Location capture that refuses
  truncation, and asset URLs pinned to `github.com/<repo>/releases/download/<tag>/<name>`.
- TLS: `esp_crt_bundle_attach` with chain, signature and hostname verification. Check and download are
  refused until SNTP has synced. Every hop opens a fresh session behind heap admission.
- Version compare: semver 2.0 parse with leading zeros refused, at most 9 digits per number, prerelease
  ordering (numeric before alphanumeric, fewer identifiers sort lower), and build metadata ignored. The
  policy refuses dirty builds, malformed manifests, a partition mismatch, a prerelease while off, versions
  below `min_updatable_from`, and a downgrade or schema-lower release unless `confirm_downgrade` equals the
  tag exactly.
- Download: the Content-Length must equal the manifest `app_size`, and overrun and short-body checks run
  on both chunked and sized responses. The sha256 of the streamed bytes is compared with the manifest
  before `WR_FINISH`. The stager re-hashes the stage from flash and compares again, and writes the header
  last with a read-back. The project name, ESP32-S3 chip id and image-id commit/schemas are checked before
  any flash write.
- Failure mid-download: cancel, deadline (20 min), stalls (10 s), heat start (checked every 8 chunks and
  at each hop), low heap, and DMA floor all lead to `WR_ABORT`. The OTA claim is released on every path. A
  wedged writer keeps the buffers alive instead of freeing them under it.
- Retries: there is no automatic retry loop. Rate-limit and 5xx answers set a backoff that the POST
  handlers honour with 429 and `retry_after_s`. A plain 403 is reported as `access_denied`.
- Internal RAM: admission needs 9216 + 22512 B free plus the 1 KB scratch. Static asserts tie the
  admission, hop and abort levels to the 8192 B floor. The bench reading (min_free 14115 B after a fetch,
  BENCH_TEST_LOG) is 5.9 KB above the floor, inside the model. TLS records and contexts live in PSRAM
  (`#error` if the option D sdkconfig is missing). The internal draw is the 2 KB rx and 2 KB tx client
  buffers, the handshake transient and the 1 KB scratch.
- Install/boot confirm: these use the recovery apply path, already reviewed (UP-01..03,
  REVIEW_RAM_OTA_2026_10_17). The stale install request is dropped at boot, and rollback is enabled.

Still owed on the bench (already in code comments, not new): production stack high-water marks for
`update_fetch` and `update_fetch_wr` across a JWT-redirected download.
