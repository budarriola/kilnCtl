# Recovery image fix batch 3 review (2026-10-10)

Scope: commit `e788b153e` on origin/dev. It fixes findings F1-F4, F6 and F7 from
`docs/audits/REVIEW_RECOVERY_FIX2_2026-10-09.md`. F5 was INFO with no action needed.

Files read: `tools/PcTools/src/kilnctrl/mcp_server_recovery.py` and its test,
`firmware/KilnFW/App/drivers/net/wifi_prov_nvs.c` and `test_wifi_prov.c`, the recovery-image files
`recovery_http.c`, `test_recovery_wifi_policy.c`, `check_recovery_wifi_policy.ps1`,
`test_recovery_apply.c` and `check_recovery_apply.ps1`. The ESP-IDF v6.0.2 NVS sources under
`C:\esp\v6.0.2\esp-idf\components\nvs_flash\src` were also read, along with the host KV fake
`firmware/hwAbstraction/host/fake_kv.c`.

This was a read-only review. No code was changed and no board was touched. Line numbers are for
`e788b153e`.

## Summary

| ID | Severity | Area | One line |
|----|----------|------|----------|
| N1 (FIXED 110650ae0) | MED | F2 fix, app Wi-Fi load | The empty-namespace probe reads every key with `hal_kv_get_str`. On real ESP-IDF a string read of a u8 or blob key returns NOT_FOUND, so a namespace holding only u8/blob keys reads as "not found". The probe also runs on the normal `wifi_nvs` boot load, so a board saved in AP mode loses that setting on every boot. The host fake returns INVALID_ARG for a wrong-type read, which is why the host tests pass. |
| N2 (FIXED 110650ae0) | LOW | F6 test vacuity | The F6 "legacy copy not checked" reply is pinned only by text. Removing `*skipped = true;`, or disabling the ternary, passes the check (negtest C1, C2). |
| N3 (FIXED 110650ae0) | NIT | F3 test source | The CRLF strip in `test_recovery_wifi_policy.c` writes a raw CR byte inside a char literal instead of `'\r'`. |
| N4 (FIXED 110650ae0) | NIT | F1 wire contract | The PC tool spots the "not applicable" reply by matching a substring of free text. There is no machine-readable field. |

Per-finding verdicts on `e788b153e`:

| Original | Verdict |
|----------|---------|
| F1 | Fixed. The tool returns `NOT APPLICABLE` for the board's "not applicable" reply. It also returns `UNVERIFIED` when status reports `boot_guard_record: "unreadable"` after a plain-ok reply. The docstring is updated. Both branches are covered by tests (negtest B1, B2 CAUGHT). |
| F2 | Fixed on the host fake only. On target it regresses the main `wifi_nvs` load. See N1. |
| F3 | Fixed. The scans now pin the gate shape, the 500 inside the gate, the R2-I1 ordering in `ota_esp_post`, the `RECOVERY_NVS_FAIL_KILN` bit, single assignment of `legacy_rc`, and erase-then-commit. The fixer's W1-W7 mutants all fail as required, and the check passes with 59 assertions. These are still source-shape scans, which is a reasonable floor for a host-only harness. |
| F4 | Fixed. `yield_writes[]` records the write count at each yield, and the new assertion needs a yield with `0 < writes < final`. Only the copy loop can produce one. The new `copyyield` mutant in `check_recovery_apply.ps1` confirms this, and the check passes with 199 assertions and 22 mutants. |
| F6 | Fixed in behaviour. The reply adds "(legacy copy not checked: default NVS unavailable)" when the erase was skipped. The test is weak; see N2. |
| F7 | Fixed. The comment now says the direct-upload path clears before the write, and that `recovery_apply` clears again just before set_boot. |

## Findings

### N1 (MED): the F2 empty-namespace probe misreads typed keys on target and drops a saved AP mode

`wifi_prov_nvs_load_from()` (`firmware/KilnFW/App/drivers/net/wifi_prov_nvs.c:132-150`) now decides
whether the namespace holds data by reading each key in `WIFI_PROV_NVS_ALL_KEYS_INIT` with
`hal_kv_get_str`. It treats any status other than `HAL_NOT_FOUND` as proof that the key exists. The
comment at line 143 says this includes "wrong type".

That holds for the host fake. `fake_kv.c:466` returns `HAL_INVALID_ARG` when a string read hits a
non-string key. It does not hold on target:

- `nvs_get_str` calls `nvs_get_str_or_blob`, then `get_item_size(SZ)`, then
  `Storage::getItemDataSize`, then `Storage::findItem` (`nvs_api.cpp:542-575`,
  `nvs_storage.cpp:964-991`).
- `Page::findItem` does return `ESP_ERR_NVS_TYPE_MISMATCH` for a key stored with another type
  (`nvs_page.cpp:1128-1134`).
- But `Storage::findItem` (`nvs_storage.cpp:257-271`) breaks out of its page loop only on `ESP_OK`.
  Any other result moves on to the next page, and the loop ends with `ESP_ERR_NVS_NOT_FOUND`. So
  `hal_kv_get_str` on a u8 or blob key returns `HAL_NOT_FOUND`.

Seven of the sixteen keys are u8: `has_creds`, `mode`, `local_only`, `has_ap_ssid`, `has_ap_pass`
and `ip_mode`. `saved_nets` is a blob. Only the string keys can make the probe succeed: `ssid`,
`pass`, `ap_ssid`, `ap_pass` and the five static-IP/DNS strings. On target, a namespace whose keys are
all u8 or blob reads as "not found", and the function returns `ESP_OK` before it reads `mode`, the
`has_ap_*` flags or `ip_mode`.

The probe is not limited to the legacy default partition. `wifi_prov_start()` loads
`WIFI_NVS_PARTITION` through the same function (`wifi_prov.c:492`). So does the migration's read-back
(`wifi_prov_nvs.c:613`).

Failure scenario: the user saves a home network and then switches the board to AP mode. `wifi_nvs`
now holds `saved_nets` (blob) and `mode=1` (u8). It has no string keys, because `ap_ssid`/`ap_pass`
are written only when an AP override is saved (`nvs_save_ap_ssid`, `nvs_save_ap_password`), and the
static-IP strings only by `nvs_save_ip_config`. On the next boot the probe finds no string key and the
load returns early. `s_wifi.mode` stays at its zeroed default, HOME, and the board joins the saved
home network the user opted out of. The legacy-mode comment in the same function names exactly this
outcome as the one to avoid. It happens on every boot, with no log line.

Second effect: a legacy migration whose adopted config has no AP override writes only `mode` (u8) to
`wifi_nvs`. The read-back then gets `rb_found == false`, logs "migration read-back ... differs", and
never sets `s_legacy_erase_pending`. The migration is retried on every boot. That is harmless but
noisy.

The original F2 case, an empty legacy namespace, also has a target-only variant. A legacy namespace
holding only `mode=1` (AP mode, no credentials, no override) is now skipped on target. The legacy AP
mode is then not migrated.

This analysis comes from the ESP-IDF source and was not reproduced on a board. The host tests pass
only because the fake answers wrong-type reads differently from IDF. Negtest A2 rewrote the probe to
accept only `HAL_OK`/`HAL_INVALID_SIZE`, which is what IDF yields for a wrong-type read. Two existing
host tests then failed: `test_wifi_prov.c:654` ("legacy keys are gone after the verified migration")
and `:679` ("boot2: legacy erased only after the verified copy"). That is the read-back effect above,
showing up under target semantics. So on target the current code behaves the way those two failing
assertions describe. No host test covers the AP-mode loss on the main load.

Suggested fix:
- Probe each key with a getter of its own type (`hal_kv_get_u8` for the u8 keys, a blob size probe
  for `saved_nets`, `hal_kv_get_str` for the rest), or add a HAL "namespace has any entry" call built
  on `nvs_entry_find`.
- Apply the empty-namespace rule only to the default-partition migration read. The main `wifi_nvs`
  load should keep its old behaviour.
- Make `fake_kv.c` return `HAL_NOT_FOUND` for a wrong-type read, as IDF does, so host tests model the
  target. That change touches other users. The comment at `profiles_http.c:797-805` makes the same
  wrong TYPE_MISMATCH assumption about IDF and should be re-checked with it.
- Add a host test where `wifi_nvs` holds only `mode=1` plus `saved_nets`, and require AP mode after
  boot.

### N2 (LOW): the F6 skip note is pinned only by its text

`test_recovery_wifi_policy.c` checks that `wifi_reset_post` contains the string "legacy copy not
checked" and the text `httpd_resp_sendstr(req, legacy_skipped`. Nothing checks that
`erase_legacy_default_wifi()` sets `*skipped` on the `RECOVERY_NVS_FAIL_DEFAULT` path
(`recovery_http.c:771`), or that the ternary actually selects the note.

Negtest (`-Preset check`, `check_recovery_wifi_policy.ps1`):
- C1: `*skipped = true;` replaced with `(void)0;` was MISSED.
- C2: `httpd_resp_sendstr(req, legacy_skipped` replaced with `httpd_resp_sendstr(req, legacy_skipped && 0`
  was MISSED.

Failure scenario: a later edit drops the assignment, and the reply goes back to silent success when
the default partition is unavailable. This is the F6 INFO case returning. The impact is small.

Suggested fix: in `erase_legacy_default_wifi`'s body, require `*skipped = true;` inside the
`RECOVERY_NVS_FAIL_DEFAULT` branch, before its `return 0;`. Alternatively, move the reply choice into
`recovery_http_policy.h` and unit-test it.

### N3 (NIT): raw CR byte in a char literal

The new CRLF strip in `slurp()` (`test_recovery_wifi_policy.c:52`) compares against `'<CR>'`, with a
literal 0x0D byte in the source file, instead of `'\r'`. MSVC accepts it, and the check passes. But
the file now has mixed line terminators (`file` reports "with CR, LF line terminators"). An editor that
normalizes line ends would turn the literal into a line break, and the build would fail with "newline
in constant". The failure would be loud, not silent. Use `'\r'`.

### N4 (NIT): "not applicable" is detected by free-text match

`recovery_boot_guard_reset` (`mcp_server_recovery.py:517`) tests
`"not applicable" in reply["text"].lower()`. The board's reply has no machine-readable field, and
"not applicable" appears in no other boot_guard message (`recovery_http.c:676-697`), so this works
today. If the firmware string is reworded, the tool falls through to the status read. The
`boot_guard_record == "unreadable"` guard at line 526 then catches it and reports UNVERIFIED rather
than ok, so a reword fails safe. No action is needed beyond keeping the two strings in step.

## Negative tests

Each mutation was applied with `tools\negtest.ps1` in a throwaway worktree at `e788b153e`. Every
baseline passed. These target fixes the fixer did not mutate. The A run ended with negtest's "REAL TREE CHANGED"
error because this doc was written into the reviewing worktree while it ran. The doc was the only change. Both
per-mutation verdicts come from completed runs. The fixer's own W1-W7 and `copyyield`
mutants are built into the checks and were confirmed by running the checks.

| Command | Mutation | Result |
|---------|----------|--------|
| `build_host_tests.ps1 -Only wifi_prov` | A1: `if (!any_key) {` changed to `if (0) {` (F2 reverted) | CAUGHT (`test_wifi_prov.c` empty-namespace test) |
| `build_host_tests.ps1 -Only wifi_prov` | A2: probe accepts only `HAL_OK`/`HAL_INVALID_SIZE`, which is IDF's behaviour for a wrong-type read | CAUGHT, but by `test_wifi_prov.c:654/679` (migration read-back), so it is evidence for N1, not a regression net for it |
| pytest `test_mcp_server_recovery.py` | B1: "not applicable" branch disabled | CAUGHT |
| pytest `test_mcp_server_recovery.py` | B2: `boot_guard_record == "unreadable"` guard disabled | CAUGHT |
| `check_recovery_wifi_policy.ps1` | C1: `*skipped = true;` removed | MISSED (N2) |
| `check_recovery_wifi_policy.ps1` | C2: skip-note ternary forced false | MISSED (N2) |

## Check results

- `check_recovery_wifi_policy.ps1`: PASS (59 assertions, built-in mutants failed as required).
- `check_recovery_apply.ps1`: PASS (199 assertions, 22 mutants).
- `build_host_tests.ps1 -Only wifi_prov`: PASS.
- `pytest tools/PcTools/tests/test_mcp_server_recovery.py`: 125 passed.
