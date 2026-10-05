# Update from a GitHub release -- plan

> Status: plan, opened 2026-10-04 (owner request). Pending work only. No board was touched while writing it.
> (UNVERIFIED) marks facts from memory that need a bench or network test.

## 0. Scope and owner decisions (2026-10-04)

Manual updates stay: a PC or phone can always upload an image by hand. GitHub is an extra source, not a replacement.

Decided by the owner:
- D1: the board downloads the release itself (architecture in section 2).
- Phone/PC upload over the normal Wi-Fi into a stage area: YES. Recovery-AP upload stays as the fallback.
- D3: semver tags `vMAJOR.MINOR.PATCH`; first release `v1.0.0` once the project is stable (section 8).
- D8: downgrade refused by default; ADMIN override with a typed confirm.
- D4 (2026-10-04): unsigned default-repo releases are allowed in v1, shown with an UNSIGNED banner; signing is enforced in M3.
- D5 (2026-10-04): a non-default repo is allowed freely (ADMIN only, no typed-name confirm); its releases are always shown UNSIGNED.
- D6 (2026-10-04): the recovery image is updated by JTAG only.

Everything else marked "default" is a recommendation, not yet owner-confirmed.

## 1. Constraints found in code and docs

1. The application cannot write its own image. One app slot (`app`, ota_0); `POST /api/ota/esp` on the application returns 409
   (`ota_http_esp_target_usable()`, 9faed0e5). Only the recovery image writes `app` (`recovery_enter`, then recovery `POST /api/ota/esp`).
2. The recovery image is SoftAP-only, no STA, no internet, unauthenticated by owner decision 2026-10-02 (`docs/RECOVERY_IMAGE_PLAN.md`).
3. Flash is fully mapped to 0x1000000. `KilnCtrl.bin` is about 2.5 MB; the `app` partition is 8 MB (0x210000..0xA10000).
4. Staging inside the running `app` partition is IMPOSSIBLE: IDF refuses writes and erases in the running partition
   (`esp_partition_main_flash_region_safe`, `partition_target.c`) and `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=y` would panic.
   Never enable `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED`. Staging needs its own partition (section 3).
5. One ESP image covers both processors: it embeds both SaftyFW slot images and updates the Pico at boot (`docs/PICO_AUTO_UPDATE_PLAN.md`).
6. Memory: `MBEDTLS_INTERNAL_MEM_ALLOC=y`, `EXTERNAL_MEM_ALLOC` and `DYNAMIC_BUFFER` off, `SSL_IN_CONTENT_LEN=16384`, full cert bundle
   already built in, `esp_http_client`/`esp-tls` not used anywhere in `App/`. A TLS session in internal RAM is roughly 35-45 KB (estimate,
   UNVERIFIED) against a bench floor of about 17.6 KB and the owner floor `heap_internal min_free >= 8192 B`. TLS is therefore M2 only.
   `.dram0.bss <= 101000 B`; httpd stack buffers must not grow (ceiling 4832 B): stage writes and downloads run in a dedicated task
   (PSRAM stack, registered with `stack_margin`) where possible, never with a bigger httpd stack buffer.
7. URI cap: 163 of 170 used (`check_uri_handler_cap.ps1`). M1 adds about 3 routes, M2 about 3 more; bump the cap in the same change.
8. Tiers: every new application write route is `ROUTE_TIER_ADMIN` (`route_tier_table.h`); non-logged-in users see dashboards only.
9. Gates: updates refused during a firing, autotune or restore (`ota_http_check_interlocks()`, `ota_http_update_try_begin()` single mutex,
   `SYS_ACTION_RECOVERY_BOOT` 409 while heating).
10. Rollback hazard: `ZONES_CFG_VERSION` is 26; an older firmware refuses a newer blob and runs on default PID gains (CLAUDE.md).
11. TLS needs valid wall time (SNTP in `time_sync.c`); refuse with a clear error when unsynced (M2).
12. GitHub (UNVERIFIED on this board): unauthenticated REST limit 60/h per IP; `api.github.com` sends CORS `*`. Release downloads 302 to a signed
    URL on `release-assets.githubusercontent.com` (older `objects.githubusercontent.com`), several hundred bytes long, short-lived.
    The `/releases/assets/<id>` URL with `Accept: application/octet-stream` also 302s.

## 2. Architecture

The board downloads or receives the image and stages it; the recovery image installs it. Recovery has no network, so TLS never enters it.

Flow: an application task writes the image into the new `stage` partition (from a phone/PC upload in M1, from GitHub in M2), verifies sha256,
and writes a stage header (magic, size, sha256, version, commit, source). The user confirms; the application does `recovery_enter`.
New recovery route `POST /api/recovery/apply_staged` validates the header, re-hashes from flash, copies `stage` to `app` in 4 KB blocks,
runs `esp_image_verify`, and only then sets the boot partition to `app` and reboots.
Rejected: browser-only download (the blob cannot cross the origin change to the recovery AP, `crypto.subtle` is missing on plain http,
asset CORS unproven) and a hybrid (TLS cost without the benefit).

## 3. Partition change (one-time, JTAG)

Current `app` is 0x210000 size 0x800000, `recovery` at 0xA10000. New layout; every other offset is unchanged so `otadata` stays valid:

| name | type | offset | size |
|------|------|--------|------|
| app | app/ota_0 | 0x210000 (unchanged) | 0x400000 (was 0x800000) |
| stage | data | 0x610000 | 0x400000 |
| recovery | app/factory | 0xA10000 (unchanged) | 0x1E0000 |

- Size gate: `KilnCtrl.bin <= 4 MB` (4,194,304 B; today about 2.5 MB). Add it to the build checks and to `ota_http_esp.c`'s partition-size refusal.
- Changing the table has no OTA path: a one-time JTAG `flash_firmware()` is required on every board (it already writes `partition-table.bin`,
  offset from `CONFIG_PARTITION_TABLE_OFFSET`). A board still on the old table reports `stage` absent and the feature stays disabled with a clear message.
- Verify the recovery image's partition handling: `firmware/KilnFW_recovery/partitions.csv` must match, its `app_size`/`max_upload` fields
  (`GET /api/recovery/status` reports the `app` PARTITION size) become 4 MB, and anything hardcoding 0x800000 is found by grep
  (`tools/`, `firmware/*/tools/`, `tools/PcTools/tests/`) before the split lands.
- Update `boot_partition_verify` and `debug_check_partition_table()` expectations.

## 4. Power-cut safety and rollback

- `apply_staged` sets the boot partition to `app` only AFTER `esp_image_verify` succeeds. Until then `otadata` keeps pointing at recovery, so a
  power cut during the copy leaves the board in recovery with the stage intact (re-run apply, or push from a PC). Same exposure as today's push.
- App rollback is enabled, so the new image boots pending-verify. Bench check: it marks itself valid and `boot_guard_reset` runs (a deliberate
  update must not walk the boot counter toward recovery mode; same logic as `flash_firmware()`'s verify step).
- The stage header is erased after a successful apply so a stale stage is never re-applied. On a stage hash mismatch (STAGE_HASH) the header is
  deliberately KEPT, not erased: the failed re-hash proves nothing about `app`, which has not been touched yet, so erasing would only destroy
  the evidence and force a re-upload; a corrupt stage can never install because every apply re-hashes it first, and the app-side stager
  (`stage_clear`) or a fresh upload replaces it.
- End-to-end operator flow (recovery apply): app `/ota` stage upload (sha256 computed and verified, state VERIFIED) -> `recovery_enter` (app route
  `POST /api/ota/esp/recovery_boot`) -> join the recovery SoftAP (random passphrase shown only on the LCD) -> open the recovery page, check the
  staged semver/commit/sha256 in the "Apply staged update" box, press Apply and confirm -> the board copies, verifies and restarts into the new
  image (pending-verify; it marks itself valid and `boot_guard_reset` runs). Follow-up, NOT in WP5: the app must clear a stage whose sha or commit
  matches the running image (OT-G06) so an already-installed stage is not offered again.
- MCP tools `recovery_apply_staged` / `recovery_apply_status` are pending (not added in WP5; tool counts are being edited elsewhere).
- Bench case owed: power cut during the first pending-verify boot of an applied image (expect the bootloader rolls back or falls to recovery and
  the board stays recoverable; boot counter and `otadata` state recorded).

## 5. Integrity, signing and threat model (stated plainly)

- sha256 per image from `release.json` (M2) or computed on upload (M1) catches truncation and corruption. It does NOT authenticate the publisher:
  manifest and image come from the same repo.
- **The recovery `apply_staged` route is unauthenticated (recovery image decision 2026-10-02): anyone on the recovery AP can install whatever is
  currently staged.** The AP passphrase shown only on the LCD is the sole barrier. Staging itself is ADMIN-only on the application. A staged
  image is therefore one step from installation: clear `stage` on cancel and on a timeout, and keep `recovery_enter` as the only way in.
- A changeable repo (M2) lets any ADMIN point the board at any repo. Admin can already flash any image by hand, so this adds convenience, not
  privilege; the UI says so. Mitigations: ADMIN plus confirm; show repo, tag, commit and sha256 prefix (D5: no typed-name confirm for a non-default
  repo); M3 adds an optional Ed25519 signature over `release.json` with a compiled-in key, enforced for the default
  repo only, other repos shown as UNSIGNED. The manifest carries a `signature` slot from the start so M3 is not a format change.
- Refuse an image whose `esp_app_desc` project name or commit differs from the manifest, or whose `partitions_sha256` differs.

## 6. Versioning and downgrade (D8)

Compare the tag and commit of the running image (`build_info.h`, `esp_app_desc`) with the candidate, and `zones_cfg_version` (26 today) plus the other
schema versions in `compat` with what the board reports. Newer with schema >= current: allowed with confirm. Same commit: "up to date", reinstall
needs `force`. Older, or schema lower: refused (409 naming the rollback hazard); override only with `allow_downgrade` and a typed confirm, and the
result tells the user to read back `control_get_zones` before heating. The page offers a backup export first. Dirty builds are never released.

## 7. Release assets and manifest (WP1 produces, M2 reads)

Assets per release: `KilnCtrl-<tag>.bin` (includes both Pico slots), optional `KilnRecovery-<tag>.bin` (JTAG-only, informational), `release.json`
(fixed name), `SHA256SUMS`, `CHANGELOG.md`, a zipped ELF for crash symbolization, later `release.json.sig`.
```json
{ "schema": 1, "tag": "v1.0.0", "channel": "stable", "published": "...", "repo": "budarriola/kilnCtl",
  "commit": "<40 hex>", "dirty": false, "build_date": "...", "min_updatable_from": "v0.9.0",
  "compat": { "zones_cfg_version": 26, "kilnlink_version": 16, "uart_version": 13, "partitions_sha256": "..." },
  "images": [ { "name": "app", "file": "KilnCtrl-v1.0.0.bin", "size": 0, "sha256": "...", "includes": ["pico_slotA","pico_slotB"] },
              { "name": "recovery", "file": "KilnRecovery-v1.0.0.bin", "size": 0, "sha256": "...", "apply": "jtag_only" } ],
  "signature": null }
```
Metadata: `GET api.github.com/repos/<o>/<r>/releases/latest` (or `/releases?per_page=5` when prereleases are on), then pick the `release.json` asset.
Check only on a button press and cache 10 min in RAM; a 403/429 reports the reset time (60/h unauthenticated is not a concern).

`tools/make_release.ps1` (WP1): refuses unless run in a clean worktree at an `origin/main` commit (`tools/worktree_mint.ps1`) and the tag is new;
requires a recorded gate result; builds KilnFW (SaftyFW slots first) and recovery from that worktree; writes `release.json` and `SHA256SUMS`;
dry-run by default, `-Publish` acts. Publishing is by REST (`Invoke-RestMethod`; `gh` is not installed): token from env var `KILNCTL_GITHUB_TOKEN`
(fine-grained PAT, this repo only, contents:write; never echoed), draft release, upload to `uploads.github.com`, re-download and compare sha256,
then undraft. Archive the ELF via `elf_archive`.

## 8. "Stable enough to release" gates (record in `logs/release/<tag>_gates.json`)

1. Full `tools/run_all_checks.ps1 -ExecutionPolicy Bypass` (not `-Fast`, not `-Only`) green at the release commit, no unexplained SKIP.
2. Release commit equals `origin/main` HEAD; artifacts built in a clean worktree, never the shared tree.
3. ROADMAP.md has no open release-blocking row (open on 2026-10-04: the backup round-trip bug and the unexplained ESP restart); open items of
   `docs/RELEASE_HARDENING_PLAN.md` section 2 are closed or waived by the owner in that file.
4. Latest bench pass (suites `ota`, `lcd`, safety link) at most 7 days old on the exact image released (`fw_build` matches); no crash report;
   readiness ok except hardware-gated items named in the notes.
5. 24 h soak on the release image: `heap_internal min_free >= 8192 B`, all `$requiredNames` tasks alive.
6. `ota_matrix_run` passes, including the new update cases (section 10).
7. `KilnCtrl.bin <= 4 MB` and `.dram0.bss <= 101000 B`.
8. Release notes list schema versions and any rollback hazard against the previous release.

## 9. The repo setting (M2)

NVS key `ota_repo` (<= 15 chars, `NVS_KEY_LEN_CHECK`), value `owner/repo`; `ota_chan` (`stable` or `pre`, default `stable`). Default
`budarriola/kilnCtl` compiled in; missing means default. Mirrored to the `cfg` dual-write like other prefs (NVS authoritative). Validated
server-side, never trusting the page: `^[A-Za-z0-9._-]{1,39}/[A-Za-z0-9._-]{1,100}$`, no `..`, owner not starting or ending with `-`. Only owner/repo
is stored (not a URL), so no SSRF to arbitrary hosts; redirects are accepted only to https `*.githubusercontent.com` or `github.com`. Included in
backup export/import (`backup_export.c`, `backup_import.c`, with a host test; import re-validates). Set by POST body only, refused mid-run; a
non-default value needs no extra confirm (D5). Route `GET/POST /api/update/settings`, ADMIN.

## 10. Routes, UI, MCP

M1 application routes (ADMIN): `POST /api/update/stage_upload` (streamed body into `stage`, same drain and WDT rules as `ota_http_refusal_drain()`),
`GET /api/update/status`, `POST /api/update/stage_clear`. Recovery: `POST /api/recovery/apply_staged` (recovery has its own URI budget; check it).
M2 adds `GET /api/update/check`, `POST /api/update/stage` (download, async 202), `GET/POST /api/update/settings`.
Bump `max_uri_handlers` 170 to 175 in the change that needs it.
UI: a section on the existing `/ota` page (`ota_page.html`, ADMIN): choose file (phone or PC), progress, sha256, then "Install (reboots into
recovery)"; M2 adds the repo field, "Check for updates" and the result card. After apply the page says to rejoin the normal Wi-Fi.
MCP (CLAUDE.md convention; bump the tool count and `docs/MCP_SERVERS.md`): M1 `update_stage_upload`, `update_status`, `update_apply(confirm)`,
`update_stage_clear`; M2 `update_check`, `update_stage_release(confirm)`, `update_get_settings`, `update_set_settings(confirm)`.
New OTA cases OT-G01..G06: bad sha256, truncated upload, downgrade refused, interlock refused, wrong repo, stale stage not re-applied.

## 11. Memory plan for M2 (TLS)

- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` (and `DYNAMIC_BUFFER` with the free-config/free-CA options) moves contexts to PSRAM; `esp_http_client`
  buffer 2048 for the long redirect Location header. This is a global sdkconfig change (D9, default, not yet owner-confirmed) with TWO gates:
  (a) before/after `heap_internal min_free >= 8192 B` and `.dram0.bss`, with a recorded negative test;
  (b) before/after login KDF latency (the KDF once starved the idle watchdog), measured through the web login path with no TASK_WDT in the log. **Bench 2026-10-05: gate (b) FAILED in the spike configuration (see section 14, Bench results); must be re-run on the bench with the production fetch task before WP8 is done.**
- Download only when idle, one update claim, free-internal precheck (proposed 40 KB) and an in-flight abort below 12 KB; 4 KB PSRAM chunk buffer;
  `vTaskDelay(1)` yields between flash writes.
- `check_recovery_image_size.ps1` must cover the new recovery route (recovery.bin is about 771 KB of 1,966,080 B) and be negative-tested; any new task
  is added to `check_stack_margin_registration.ps1`'s `$requiredNames` as `liveness: on-demand`.

## 12. Milestones and work packages

Shared files (`App/drivers/CMakeLists.txt`, `tools/build_host_tests.ps1`, `route_tier_table.h`, the URI cap in `wifi_provision_http.c`) have ONE owner
(WP0); other WPs hand it their entries or serialize behind it so parallel packages do not collide.

**Independent, starts now**
- WP1 release script: `tools/make_release.ps1`, `tools/release_manifest.py`, `tools/check_release_manifest.ps1`, `docs/RELEASING.md`. DONE 2026-10-04 (`c2cd5dac`); the live GitHub hop (real release upload and asset fetch) is still unverified.

**M1: stage partition + upload over normal Wi-Fi + recovery apply, no TLS (one-click phone/PC update)**
- WP0 shared-file owner: `App/drivers/CMakeLists.txt`, `tools/build_host_tests.ps1`, `route_tier_table.h`, URI cap bump.
- WP2 partition split: both `partitions.csv`, size gate in build checks, grep sweep for 0x800000, `docs/OTA_SINGLE_SLOT_PLAN.md`; then the one-time JTAG flash. **Code and checks landed 2026-10-04; the one-time JTAG flash was done and bench-verified the same day (see OTA_SINGLE_SLOT_PLAN.md section 10).**
- WP3 pure logic and host tests: `update_policy`, `stage_header`, version compare (`App/drivers/update/*.[ch]`, `App/test/test_update_*.c`). DONE 2026-10-04 (`update_semver`, `stage_header`, `update_policy`; registered in the drivers CMakeLists and `build_host_tests.ps1`).
- WP4 stager: `stage_upload` handler and task, sha256 stream, interlock and mutex, `stage_clear`, status (`update_stage.c`, `update_http.c`). **Done 2026-10-04 (`e4ebc0aa`, review fixes `d2cdeb69`).** Routes `POST /api/update/stage`, `POST /api/update/stage/clear`, `GET /api/update/stage`, all ADMIN; refusal order is `system_mode_gate` (`SYS_ACTION_STAGE_WRITE`), OTA interlock, then the single update claim. Staged means a valid header (CRC) AND a matching sha256; the header is erased at begin and written last. Image capacity is the partition minus the 4096 B header sector (0x3FF000 on the 4 MiB stage). Operational notes: upload egin erases the header first, so a failed or interrupted upload wipes a previously good stage (never leaves a half-valid one); an upload holds one httpd task for its whole duration (httpd is a single task, so every other request waits). The 4 KB buffer is ota_http_esp.c's static internal chunk buffer (no new RAM), and a verified or mismatching sha is cached (negative results too) so repeat GETs read only the header. Bench-untested: host tests only.
- WP5 recovery apply. **Code and host tests done 2026-10-05; bench-untested (no board access).** Files: `firmware/KilnFW_recovery/main/recovery_apply.[ch]` (pure core, injected flash/SHA/verify/set-boot), `recovery_apply_esp.[ch]` (ESP glue and the `rec_apply` task), `test_recovery_apply.c` + `check_recovery_apply.ps1` (host test with a power cut at every mutating step, 19 negative-test mutants plus the real build), `recovery_http.c` (`POST /api/recovery/apply_staged`, `GET /api/recovery/apply_status`; 14 routes of the 16 cap), `main/CMakeLists.txt` (links `stage_header.c`, `update_semver.c`, `ota_image_crc.c` from the application tree so the header format has one implementation; adds `mbedtls` for PSA SHA-256), and `check_00_kilnfw_recovery_target_build.ps1` (mirrors those shared files into its build directory). Recovery image 777,632 B of the 1,966,080 B partition. Nothing writes the `recovery` partition (D6) and the new routes are unauthenticated like the rest of the recovery image.
  Design decisions (the plan text left these open; safest option taken):
  1. **The stage header is never rewritten to APPLYING.** Flash can only clear bits without an erase, so changing the state byte means erasing the header sector, which destroys the stage's installability before the copy is verified. The stage therefore stays VERIFIED and untouched through the whole copy; a cut or failure is retried by applying again. The header sector is erased only after `set_boot` succeeded (a cut between the two leaves a booting app and a harmless still-valid stage).
  2. **Order:** decode header (must be VERIFIED), re-hash the stage from flash against the header sha256, validate the image identity and size with the same gate the upload route uses (`recovery_image_check.c`), erase `app` ahead in 64 KB blocks and copy 4 KB at a time, re-read `app` and compare sha256, `esp_image_verify` of the whole copy, then `set_boot`, then erase the header. `otadata` is untouched until step `set_boot`; an invalid `ota_0` makes the bootloader fall back to `recovery`, so a cut anywhere during the copy leaves a bootable board.
  3. **boot_guard is cleared before `set_boot`** (same order as `/api/recovery/exit`): an unverified clear fails the apply with the boot partition unchanged rather than booting the new app into a counter that walks straight back into recovery.
  4. **Asynchronous:** the POST returns 202 and a task does the work (the single httpd task must keep answering status polls); every other mutating route (OTA upload, exit, wifi reset, sw reset, Pico upload, boot_guard reset) answers 409 while an apply runs, and the status route does not read `app` meanwhile. All buffers are static (4 KB scratch), no allocation after the apply starts. A failed apply clears the busy flag and stays in recovery for a retry; success waits 1.5 s so a poll can read "done", then restarts.
  5. **No version comparison in recovery.** Downgrade and semver policy belong to the application (`update_policy`, WP6); recovery installs whatever verified stage the application handed it, and re-checks integrity only.
  Bench cases still owed: real power cut during copy (expect recovery boots, stage still installable), power cut between `set_boot` and header erase, first boot of an applied image is pending-verify (rollback behaviour per OTA_SINGLE_SLOT_PLAN), apply with an oversized/corrupted stage refused, and the 409 guards during an apply.
- WP6 UI and MCP (IN PROGRESS 2026-10-05): `ota_page.html` section, `update_http_client.py`, `mcp_server_update.py`, `cases_ota.py` OT-G*, `docs/MCP_SERVERS.md`, CLAUDE.md count.

**M2: GitHub download + repo setting**
- WP7 network spike FIRST (no flash write): TLS handshake and redirect from the board, heap and KDF trace, browser CORS note in `docs/BENCH_TEST_LOG.md`.
- WP8 TLS fetch (M2; production fetch task on core 1 or unpinned, host allowlist, at most 3 redirect hops, PSRAM buffers; gate (b) must be re-run first-class before done): sdkconfig change, `update_fetch.c`, host allowlist, heap gate, stack_margin entry.
- WP9 setting and backup: `update_settings.[ch]`, `backup_export.c`, `backup_import.c`, repo validator tests.
- WP10 check and stage-release routes, UI card, MCP tools `update_check`, `update_stage_release`, settings.

**M3: signing**
- WP11 Ed25519 verify, key list, `tools/sign_release.py`.

## 13. Owner decisions

Decided 2026-10-04 (also listed in section 0): D4 (unsigned default-repo releases allowed in v1 with an UNSIGNED banner; signing enforced in M3), D5 (non-default repo allowed freely, ADMIN only, no typed-name confirm), D6 (recovery image updated by JTAG only).

Still open, all at their defaults, not yet owner-confirmed:

- D2 stage partition split (section 3): default yes; no alternative without relocating data.
- D7 prerelease channel: default off.
- D9 mbedTLS to PSRAM globally: default yes, behind the two gates in section 11.
- D10 publish token: default fine-grained PAT in `KILNCTL_GITHUB_TOKEN`.
- D11 release trigger: default owner only, dry-run first.

## 14. WP7 network spike findings (desk work and build measurement; nothing flashed)

No runtime handshake number exists yet: the spike was built, never flashed. Everything under "Runtime" is an expectation from IDF docs/source and the
board's current heap, to be replaced by the bench run below.

**Spike code.** `firmware/KilnFW/components/tls_spike/` (commit 9f4623f0), `CONFIG_KILNCTL_TLS_SPIKE` default n. One-shot task after a boot delay
(default 90 s): GET chain with manual redirects, bundle-verified TLS, heap sampler, writes no flash. No WP0 shared file touched.

**sdkconfig today (A).** ESP-IDF v6.0.2, mbedTLS 4.1.0. Octal PSRAM 8 MB, `SPIRAM_USE_MALLOC`, `SPIRAM_MALLOC_ALWAYSINTERNAL=8192`,
`SPIRAM_MALLOC_RESERVE_INTERNAL=32768`, `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`. mbedTLS: `INTERNAL_MEM_ALLOC=y`, `ASYMMETRIC_CONTENT_LEN` IN 16384 / OUT 4096,
dynamic buffer off, full cert bundle already built in, TLS 1.3 not enabled. Spike variants: B = flag on, sdkconfig otherwise unchanged;
D = flag on plus `MBEDTLS_EXTERNAL_MEM_ALLOC=y`, `DYNAMIC_BUFFER=y`, `DYNAMIC_FREE_CONFIG_DATA=y`, `DYNAMIC_FREE_CA_CERT=y` (the section 11 / D9 set).

**Measured sizes (bytes, `xtensa-esp32s3-elf-size -A`).**

| Section | A baseline | flag off, clean commit | B flag on | D flag on + PSRAM/dynamic |
|---|---|---|---|---|
| .dram0.bss (cap 101000) | 99240 (origin/main after WP-2: 99304) | 99240 | 100344 | 100344 |
| .iram0.text | 89207 | 89207 | 89207 | 89207 |
| .flash.text | 1490248 | 1490248 | 1562596 | 1565476 |
| .flash.rodata | 982904 | 982904 | 1067632 | 1068008 |
| .ext_ram.bss | 119972 | 119972 | 119972 | 119972 |
| KilnCtrl.bin (4 MiB slot) | 2587232 | 2587232 | 2744304 | 2747552 |

- Flag off at the clean commit is section-for-section identical to baseline A and the same .bin size (the bytes differ, as every build's timestamps do).
- Flag on costs +1104 B bss (mostly the spike's 1 KB static URL buffer), about +157 to +160 KB image, no IRAM. Image stays about 1.45 MB under the 4 MiB slot.
- **bss headroom is the constraint:** baseline leaves 1760 B under the 101000 cap (about 1696 B on origin/main after WP-2, 99304 measured there); the spike leaves 656 B on the old baseline (about 592 B if the +1104 B delta carries over; not re-measured). Production fetch code must keep URL, header and
  chunk buffers in PSRAM or heap, not static internal bss. The PSRAM/dynamic options themselves cost no bss.

**Runtime (expectation, unmeasured).**
- Board facts read earlier: idle `min_free` about 17.6 KB, largest free internal block 9728 B.
- IDF mbedtls docs memory table: about 42 KB for a default TLS connection (16 KB IN + 16 KB OUT buffers plus contexts); asymmetric 16384/4096 saves about 12 KB, so
  roughly 30 KB (my inference) in internal RAM. That exceeds the floor, and a 16 KB record buffer cannot even be allocated from a 9728 B largest block.
  **Option B (internal) does not fit. Do not ship it.**
- Option D: with `EXTERNAL_MEM_ALLOC` mbedTLS allocations route through `esp_mem.c` to PSRAM; dynamic buffer plus free-config/free-CA shrink the
  PSRAM footprint (docs give about 22 KB total). Internal use should drop to small lwip/socket/esp_http_client mallocs plus DMA bounce buffers for
  hardware AES/SHA on PSRAM data. That residual is the number the bench must produce. Not measured.
- Keep IN = 16384: GitHub/Fastly servers will probably not honour max-fragment-length negotiation, so a smaller IN could fail on 16 KB records (inference, unverified).
- TLS 1.3 not measured; GitHub serves TLS 1.2, which is enough.
- External mode shifts crypto cost and may lengthen the login KDF: D9 gate (b) applies unchanged.

**Redirects.** `api.github.com/.../releases/latest` returns JSON; the asset URL 302s to the `objects.githubusercontent.com` / release-assets host. Use
`disable_auto_redirect` and follow Location manually: https only, host allowlist (`api.github.com`, `github.com`, `*.githubusercontent.com`), max 3 hops
(the client default of 10 is too many). `buffer_size` 2048 for the long signed Location; the spike logs the real Location length so the bench can confirm 2048.
Each hop opens a fresh TLS session, so peak heap is per hop, not cumulative.

**CORS.** `api.github.com` is expected to send `Access-Control-Allow-Origin: *`; the asset redirect host is unproven (not checked from this session). That is why the board
downloads and the browser only triggers it.

**Recommendation.** Adopt D9 as written: option D set, IN 16384 / OUT 4096, `esp_http_client` `buffer_size` 2048, manual allowlisted redirects, dedicated
PSRAM-stack task, 4 KB PSRAM chunk. Revisit the proposed 40 KB free-internal precheck: the board idles near 30 KB free, so 40 KB would never pass. The precheck compares CURRENT free internal heap (`heap_caps_get_free_size`), not the low-water `min_free`; set the threshold to the 8192 B floor plus the measured residual, never the residual alone. The in-flight abort should watch the same current-free number. Keep the 12 KB in-flight abort until measured. Both D9 gates still apply, with a negative test.

**Production requirements (the spike deliberately does not meet these).**
- Redirect allowlist is enforced on a parsed host with an exact or suffix match (`api.github.com`, `github.com`, `*.githubusercontent.com`), never a substring or prefix match on the URL string. Max 3 hops; the spike allows 4 and any https host, acceptable for a spike only.
- Production fetch code lives under `App/drivers` (or registers by hand): `check_stack_margin_registration.ps1` does not scan `components/`, and the spike tasks are unregistered (flag-on builds only). Any production task is added to `$requiredNames` as `liveness: on-demand`.

**Bench procedure (later, board free, owner-authorised flash).**
1. Build the worktree with `CONFIG_KILNCTL_TLS_SPIKE=y` plus the D deltas (and B for comparison); `build_kilnfw_start` with `kiln_fw_root` pointing at it.
2. Confirm the board is running `app`, not recovery (`GET /api/partitions` RUNNING marker; otadata gap), then `flash_firmware(kiln_fw_root=...)`; no other agent on the bench; firing idle. The flag-on build is never left on the board, and `CONFIG_KILNCTL_TLS_SPIKE=y` is never committed (sdkconfig stays gitignored).
3. Capture `get_heap_status` before; the `TLS_SPIKE` log lines (HEAP per stage, `int_free`, `int_largest`, `int_min_global`, `sampled_min_free`, Location length, per-hop status);
   `get_heap_status` after. Pass if `sampled_min_free >= 8192` with margin.
4. Repeat with the asset URL (covers the redirect hop). Optionally raise `_REPEAT` to catch fragmentation.
5. KDF trace: do a web login during the fetch; require no TASK_WDT in the log and record latency against a flag-off boot (gate b). The spike heap sampler runs at priority 10 and perturbs this latency; note it, or lower its rate during this step.
6. Reflash the normal build and confirm `fw_build`.

**Not done / unverified.** No board access, no flash. No runtime heap numbers. Server max-fragment-length behaviour, CORS on the asset host and TLS 1.3 untested.
No network requests to GitHub from the host (a curl attempt was denied by the permission classifier and not retried). Full check suite not run (flag-off build
identical to baseline; no check-relevant files changed).

**Bench results (2026-10-05, option D build of 2d7bfb0d, bench board, one fetch of `releases/latest`).**

| Measurement | Value |
|---|---|
| Flag-off idle, before | int free 30679 B, largest 9728 B, min_free 17687 B |
| Spike boot (uptime 9 s) | int free 31403 B, largest 10240 B, min_free 18159 B |
| pre_init / pre_open | int free 31683 B / 27247 B |
| post_open_handshake | int free 23775 B, sampled_min 20639 B |
| post_headers / post_body | 23635 B / 23775 B |
| post_cleanup | 31291 B |
| RESULT | sampled_min_free_internal 20639 B, sampled_min_largest_block 10240 B, int_min_global 18159 B |
| Floor 8192 B | PASS (sampled minimum 20639 B, margin 12447 B) |
| Spike task stack high-water | 7952 B of 12 KB |
| Handshake | success, bundle-verified, about 2.7 to 3.9 s; TLS version and cipher not logged |
| HTTP result | hop 0 status 404, content_length 130, url_len 63 |
| Image size | flag-on KilnCtrl.bin 2753344 B; clean origin/main 2593136 B |
| Login latency | about 480 ms flag-off; during the fetch the login POST timed out |

- **Gate (b) FAILED in the spike configuration.** A web login during the handshake starved IDLE0 on core 0 (spike task, priority 3, pinned to core 0, inside mbedTLS
  ECDH) and the board reset with `TASK_WDT` (crash report dump_id 2919415307, left unacknowledged). Production fetch needs a different placement (core 1, or
  yielding between crypto steps) and a re-run of step 5.
- Heap floor is met with wide margin, so the heap question is closed for one hop. Not done: asset-URL hop (the URL returned 404, so there was no redirect),
  `_REPEAT` above 1, TLS version/cipher capture, a clean KDF latency under fetch.


**Gate (b) failure record and production requirements (2026-10-05).** Gate (b) FAILED on the bench: a web login during a fetch produced `TASK_WDT`, IDLE0 starved
while `tls_spike` (priority 3, pinned to core 0) was inside mbedTLS ECDH. Coredump archived as `coredump-9ba85c503229.bin`, dump_id 2919415307.
- The production fetch task must not starve IDLE0: run it on core 1 (or unpinned) at a priority that leaves the KDF and httpd tasks schedulable.
- Re-run gate (b) on the bench with the production task before WP8 can be called done.
- Hop 0 returned 404, so the asset/redirect hop is still untested and must be covered by the same re-run.
