# Recovery image fix batch 2 review (2026-10-09)

Scope: the origin/dev commits that fix the second-round findings in
`docs/audits/RECOVERY_IMAGE_REVIEW_2026-10-09.md`. Those findings are R2-L1, R2-L2, R2-L3, R2-I1, R2-I3
and R2-I4. The commits are `959aee923`, `3b2905d84`, `4f28c5b64` and `e7ff26566`.

The review read `firmware/KilnFW_recovery/main/recovery_http.c`, `recovery_http_policy.h`, the recovery
host tests and checks, the app-side Wi-Fi migration (`firmware/KilnFW/App/drivers/net/wifi_prov*.c`),
and the PC tool `tools/PcTools/src/kilnctrl/mcp_server_recovery.py`.

This was a read-only review. No code was changed and no board was touched. Line numbers are for `e7ff26566`.

## Summary

| ID | Severity | Area | One line |
|----|----------|------|----------|
| F1 | LOW | PC tool, boot_guard reset | `recovery_boot_guard_reset` now reports "ok ... status confirms" when the board answered "not applicable" and cleared nothing. |
| F2 | LOW | app Wi-Fi migration, pre-existing | Recovery's `nvs_erase_all` leaves an empty `wifi_cfg` namespace in the default partition. The app still "finds" it and adopts an empty legacy config, which drops the AP identity override that recovery keeps. |
| F3 | LOW | test vacuity | The new source scans check that names appear, not how they are used. Most behaviour mutants of the R2-L1/L2/I1 fixes pass the checks (see the negtest table). |
| F4 | LOW | test vacuity | The R2-I4 test's `yields >= 1` is also satisfied by the yield inside `hash_region`, so removing the copy-loop yield goes unnoticed. |
| F5 | INFO | `_or_na` scope | No permanent masking. A transient, recovery-only kiln_nvs init failure followed by exit costs one guaranteed bounce back to recovery. |
| F6 | INFO | Wi-Fi reset reply | When the default partition is unavailable, the legacy erase is skipped silently and the success reply does not say so. |
| F7 | INFO | `ota_esp_post` ordering | Pre-clearing before the write is sound because recovery never increments the counter. The comment's "same order as recovery_apply" is loose: apply clears twice. |

Checked and found correct:
- R2-L3: the wrong-version vector in `test_recovery_image_check.c` now carries a correct CRC for its own
  bytes (`zlib.crc32([2,0,0,0,5,0,0,0]) = 0x10d92826`, bytes `26 28 D9 10`). The restore vector
  (`0x9e562fc5`) is also correct, so the vector reaches the version check rather than failing on CRC.
  Mutant I1 confirms it.
- R2-I4: the erase-granularity assertion (`app_erases == 2 && app_erase_max == 65536`, with
  `APP_SIZE = 2 * 65536` and `IMG_LEN = 70000`) pins the 64 KB erase-ahead. Mutant A1 confirms it.
- R2-I3: `docs/OTA_SINGLE_SLOT.md` section 6 and the new "Expected S6b latch" section in
  `docs/RECOVERY_IMAGE_PLAN.md` match the code. Recovery sends only Pico update, status and reboot frames.
  It never arms or heats, and S6b is expected.
- `fn_body()` in `test_recovery_wifi_policy.c` finds the real handler definitions. The handlers have no
  forward prototypes; only the two bool helpers at lines 153-154 do, and those are not scanned by body.
- `3b2905d84`'s CRLF normalisation of the check needles works. The check passes on this checkout.

## Findings

### F1 (LOW): `recovery_boot_guard_reset` reports a "not applicable" reply as a confirmed clear

`boot_guard_reset_post` (`recovery_http.c` around line 739) now calls `boot_guard_clear_or_na()`. When
`RECOVERY_NVS_FAIL_KILN` is set, that function answers 200 "boot_guard not applicable (kiln_nvs
unavailable)" and touches nothing. Before this batch the route returned 500 in that state.

The PC tool (`mcp_server_recovery.py`, `recovery_boot_guard_reset`, line 487) treats
`after["record_present"] is False` as proof of a clear. `read_boot_guard()` reports
`record_present:false` with `boot_guard_record:"unreadable"` when kiln_nvs cannot be opened. So the tool
now prints "ok - board replied 'boot_guard not applicable ...' ... and status confirms: ... after
record_present=False", which is a success claim for an action that did not happen.

The docstring is also stale: "The board answers 200 only after reading BOTH locations back absent" is no
longer true.

The impact is limited. The reply text is quoted in the result, and the app would see count 0 on a
partition it also cannot read (see F5). But this repository treats a confirmed-looking success that was
not verified as a defect class.

Suggested fix: have the tool check `boot_guard_record` in the after-status, or match the reply text. It
should report "not applicable (kiln_nvs unavailable in recovery)" rather than "ok ... confirms". Then
update the docstring.

### F2 (LOW, pre-existing, exposed by R2-L2): an empty legacy `wifi_cfg` namespace is still adopted

`erase_legacy_default_wifi()` (`recovery_http.c` around line 766) opens `wifi_cfg` on the default
partition and runs `nvs_erase_all` plus `nvs_commit`. ESP-IDF does not delete the namespace entry when
its keys are erased: neither `nvs_erase_all` nor erasing every key individually removes it. A later
read-only `nvs_open` therefore still succeeds.

On the next app boot, `wifi_prov_migrate_from_default_partition()` sets
`adopt = !nvs_saved_nets_record_present(WIFI_NVS_PARTITION)`. That is true, because recovery's Wi-Fi reset
erased saved_nets. `wifi_prov_nvs_load_from(NVS_DEFAULT_PART_NAME, &found_in_default)` then opens the
empty namespace and sets `found_in_default = true`. The app adopts an all-default legacy config:
`has_ap_ssid` and `has_ap_pass` are false and mode is HOME.

The credential resurrection that R2-L2 was about is closed, because no SSID or password is left to adopt.
The side effect is that the custom AP SSID and password override, which recovery's Wi-Fi reset
deliberately keeps in `wifi_nvs`, is overridden in RAM by the empty legacy copy on every boot until a
network is saved. The app's own `legacy_default_nvs_erase_wifi()` erases keys individually and has the
same property, so this predates the batch.

This finding comes from reading the code and IDF NVS semantics. It was not reproduced on a board.

Suggested fix (app side): treat a default-partition namespace with no `mode` key and no SSID as "not
found", or have `wifi_prov_nvs_load_from` set `found` only when at least one key was actually read.
`nvs_erase_all` remains the right scope on the recovery side, because it also removes the legacy AP
identity keys.

### F3 (LOW, test vacuity): source scans check names, not behaviour

`test_recovery_wifi_policy.c` unit-tests `rhp_wifi_reset_ok()` with four vectors. It then source-scans:
- that the exit and boot_guard_reset handlers contain `boot_guard_clear_or_na(` and not a bare
  `clear_boot_guard(`;
- that `wifi_reset_post` contains `erase_legacy_default_wifi()` and `rhp_wifi_reset_ok(`;
- that the legacy function contains `nvs_erase_all(` and `nvs_open(WIFI_NVS_NAMESPACE`.

None of these checks how the call's result is used, which bit gates the "not applicable" path, or whether
`ota_esp_post` clears at all. The negtest results below show which behaviour mutants survive.

Source scans of an ESP-IDF handler are a reasonable floor in a host-only harness. The gap is mainly the
R2-I1 ordering, which has no assertion at all, and the gate bit in `boot_guard_clear_or_na`.

Suggested fix: add `ota_esp_post` to the scan (it must contain `boot_guard_clear_or_na(` before
`recovery_upload_stream(`), and pin `RECOVERY_NVS_FAIL_KILN` inside `boot_guard_clear_or_na`'s body.
Alternatively, move the gate decision into `recovery_http_policy.h` as a pure function, the way
`rhp_wifi_reset_ok` was, and unit-test it.

### F4 (LOW, test vacuity): the copy-loop yield assertion is satisfied elsewhere

`test_recovery_apply.c` asserts `yields >= 1`. `recovery_apply.c` also yields inside `hash_region`
(around line 49), which runs on every apply. So the yield in the copy loop (around line 150), the one
R2-I4 was meant to pin, can be removed without failing the test. Mutant A2 tests this.

Suggested fix: count yields between the first and last app write, or assert a count that is only reachable
with one yield per copied chunk.

### F5 (INFO): "not applicable" can cost one bounce, but masks nothing permanently

`boot_guard_clear_or_na()` (`recovery_http.c` around line 694) returns true without touching NVS when
recovery's own `nvs_flash_init_partition("kiln_nvs")` failed at boot.

- For ESP_ERR_NVS_NO_FREE_PAGES and ESP_ERR_NVS_NEW_VERSION_FOUND, the app's `hal_kv_init_partition`
  erases the partition, so the app's boot_guard starts from count 0. "Not applicable" is accurate.
- For other persistent failures, the app also fails to open kiln_nvs and its boot_guard reads count 0.
  Also accurate.
- The two sdkconfigs show no NVS encryption difference that would make the partition readable only to
  the app.

The remaining case is a transient failure seen only by recovery, such as ENOMEM at init. The app then
reads the real count, which is usually still at threshold because that is why the board is in recovery.
Exit therefore bounces straight back to recovery. The cost is one bounce, and both the "not applicable"
reply and status's `nvs_unavailable` field make it visible. No action needed beyond F1.

### F6 (INFO): skipped legacy Wi-Fi erase is not reported

When `RECOVERY_NVS_FAIL_DEFAULT` is set, `erase_legacy_default_wifi()` returns 0 and the Wi-Fi reset reply
reads as a full success. The reasoning holds: if recovery cannot init the default partition, the app's
migration usually cannot read it either. The exception is the transient case from F5. One clause in the
reply ("legacy copy not checked: default NVS unavailable") would make this visible. It is optional.

### F7 (INFO): `ota_esp_post` pre-clear ordering

`ota_esp_post` (`recovery_http.c` around line 210) now clears boot_guard after the busy checks and the
`app` partition lookup, and before `recovery_upload_stream`. On failure it answers 500 with
`Connection: close` and leaves the body unread.

One clear is enough, because recovery never increments the counter. A rejected or failed upload leaves
the counter cleared, which is harmless: the board stays in recovery until a successful write sets the
boot partition. The comment's "same order as recovery_apply" is slightly loose. Apply probes in the
handler and clears again in `apply_pre_boot` just before set_boot. The direct-upload path does not need
the second clear.

## Negative tests

Each mutation was applied with `tools\negtest.ps1 -Preset check` in a throwaway worktree. Every baseline
passed.

| Check | Mutation | Result |
|-------|----------|--------|
| `check_recovery_wifi_policy.ps1` | W1: `!rhp_wifi_reset_ok(...)` negated to `rhp_wifi_reset_ok(...)` in `wifi_reset_post` | MISSED (F3) |
| `check_recovery_wifi_policy.ps1` | W2: `legacy_rc` overwritten with 0 after `erase_legacy_default_wifi()` | MISSED (F3) |
| `check_recovery_wifi_policy.ps1` | W3: `nvs_commit` after the legacy `nvs_erase_all` removed | MISSED (F3) |
| `check_recovery_wifi_policy.ps1` | W4: `boot_guard_clear_or_na` always returns true, never clears | MISSED (F3) |
| `check_recovery_wifi_policy.ps1` | W5: "not applicable" gated on `RECOVERY_NVS_FAIL_DEFAULT` instead of `_KILN` | MISSED (F3) |
| `check_recovery_wifi_policy.ps1` | W6: `ota_esp_post` pre-clear removed (R2-I1 reverted) | MISSED (F3) |
| `check_recovery_wifi_policy.ps1` | W7: `boot_guard_reset_post` ignores the clear result and always answers 200 | MISSED (F3) |
| `check_recovery_apply.ps1` | A1: 64 KB erase-ahead replaced by one whole-remaining erase | CAUGHT (R2-I4 fixed) |
| `check_recovery_apply.ps1` | A2: copy-loop `io->yield` call (line 150) removed | MISSED (F4) |
| `check_recovery_image_check.ps1` | I1: boot_guard version check removed from the decoder | CAUGHT (R2-L3 fixed) |

The wifi-policy and image-check runs ended with negtest's "REAL TREE CHANGED" error. The cause was this
audit doc being written into the reviewing worktree while those runs were in progress. The doc was the
only change (`git status` showed just this untracked file). Every baseline passed, and every per-mutation
verdict above is from a completed run.

## Check results

- `firmware/KilnFW_recovery/main/check_recovery_passphrase.ps1`: PASS (1073 assertions; its built-in
  negative-test mutants failed as required).
- `tools/check_recovery_image_size.ps1`: PASS. `recovery.bin` is 783344 B (0xbf3f0) against the
  1966080 B (0x1e0000) `recovery` partition, leaving 1182736 B (60%) free. The image was built first with
  `run_all_checks.ps1 -Only check_00_kilnfw_recovery_target_build -NoCache`, which passed.
- Also run directly: `check_recovery_wifi_policy.ps1` PASS (42 assertions), `check_recovery_image_check.ps1`
  PASS (39), `check_recovery_apply.ps1` PASS (198 assertions, 21 built-in mutants).
