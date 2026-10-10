# Recovery image review (2026-10-09)

Scope: `firmware/KilnFW_recovery/` at origin/dev `1e6d375f`. The review also covered the
KilnFW sources it compiles (`App/drivers/update/stage_header.c`, `update_semver.c`, `ota_image_crc.c`),
and the app-side `boot_guard.c` decision logic that the recovery image's boot_guard clear feeds.
Context documents: `docs/RECOVERY_IMAGE_PLAN.md` and `docs/GITHUB_RELEASE_UPDATE_PLAN.md` (WP5 and section 4).

This was a read-only review. No code was changed and no board was touched. The image is deliberately
unauthenticated and reachable only over its own SoftAP (owner decision 2026-10-02), so that is not a finding.

Line numbers are for `1e6d375f`.

## Summary

| ID | Severity | Area | One line |
|----|----------|------|----------|
| M1 | MED | apply / stuck in recovery | The boot_guard clear runs only after `app` has been erased and rewritten. If that clear cannot succeed, every apply destroys `app` and then fails SET_BOOT. |
| M2 | MED | direct upload / stuck in recovery | `/api/ota/esp` sets the boot partition before it clears boot_guard, and reboots even when the clear failed. A count of 3 or more bounces the new app straight back to recovery. |
| L1 | LOW | boot_guard clear | `erase_key_in` opens NVS READWRITE, which creates a missing namespace. On a full `kiln_nvs` this fails with NOT_ENOUGH_SPACE, so exit, apply and boot_guard_reset all fail. |
| L2 | LOW | httpd availability | Body reads on the single httpd task tolerate up to 20 recv timeouts (about 100 s) for a Pico upload. A stalled client blocks every route, including status and abort. |
| L3 | LOW | stale stage | A direct `/api/ota/esp` upload leaves a VERIFIED stage of a different image. That stage stays installable and recovery applies it with no version check. |
| L4 | LOW | Pico relay stack | `RELAY_STACK` is 4096 B for a task with 256 B and 240 B locals plus printf-family and UART driver calls. The measured headroom is only logged. |
| I1 | INFO | power cut mid-copy | Safe when `otadata` is blank or points at factory. When recovery was reached by ota_0 fallback, safety relies on the bootloader's full image validation. |
| I2 | INFO | version policy | Neither apply nor direct upload compares versions (owner decision: no release signing; policy lives app-side). |
| I3 | INFO | httpd stack/latency | `esp_image_verify` of `app` runs on the 8192 B httpd task and blocks it for the full image hash (cached afterwards). |
| I4 | INFO | Pico slot choice | An operator-chosen slot on an old bootloader without a trailer can leave the Pico unbootable until SWD. This is documented in the refusal text. |
| I5 | INFO | apply status window | "done" is visible for only 1.5 s before `esp_restart`. A slow poller sees the AP drop instead. |

Checked and found correct:
- Buffer bounds in every HTTP handler: `send_frag`, `apply_status_get` `body[640]` (length checked before appending `}`), `parse_pico_query` `q[96]`/`v[16]`, Pico JSON escaping (`JSON_CAP` 1536, `json_str`), and every `snprintf`, which is clamped.
- Apply step order: the stage is never modified before success, and `app` is re-hashed and passed through `esp_image_verify` before `set_boot`.
- `set_boot` is verified by read-back (`recovery_boot_verify.c`).
- Pico abort after END is reported as "unknown", never as success.
- Pico busy and reservation handling has no double release.
- Apply-task stack (8192 B, large buffers static).

## Findings

Fix status: M1, M2, L1-L4 and I5 fixed in the commit that adds this note (see git log); I1-I4 are informational and unchanged.

### M1 (MED): an apply whose boot_guard clear cannot succeed erases `app` and leaves the board in recovery -- FIXED (apply_staged_post clears boot_guard (an unusable kiln_nvs counts as nothing to clear) before the first erase of `app`; refuses 500 otherwise)

Where:
- `firmware/KilnFW_recovery/main/recovery_apply_esp.c:131-133`. `cb_set_boot` calls `pre_boot` first and returns -1 if it fails.
- `firmware/KilnFW_recovery/main/recovery_http.c:917-921`. `apply_pre_boot` is `clear_boot_guard`.
- `firmware/KilnFW_recovery/main/recovery_apply.c:124-176`. Step 4 (erase and copy, "`app` stops being bootable at the first erase") runs before step 7 (`set_boot`).
- `firmware/KilnFW_recovery/main/recovery_http.c:934-960`. `apply_staged_post` checks only for a busy apply, a busy Pico, the app partition, and an installable stage header.

Failure scenario:
1. `kiln_nvs` failed to initialise at recovery boot. `recovery_main.c:58-69` marks it failed and continues. Alternatively, the partition is full (see L1), or the read-back keeps failing.
2. The operator presses Apply. The apply erases and rewrites `app`, re-hashes it and verifies it.
3. `set_boot` then fails inside `pre_boot`, and the result is SET_BOOT failed with `app_modified=true`.
4. `otadata` is unchanged. Every retry repeats the full erase and copy and fails the same way.
5. `/api/recovery/exit` (`recovery_http.c:672`) also clears first and returns 500 when the clear fails.
6. The board cannot leave recovery from the page. The only way out is a direct `/api/ota/esp` upload, which has the same clear problem in a different order (M2), or JTAG.

The new image in `app` is good and verified. It is never selected only because of the NVS counter.

Suggested fix (either):
- Prove the clear is possible before step 4. For example, `apply_staged_post` runs `clear_boot_guard` itself and returns 409 or 500 with the message before starting the task. The clear is idempotent, so running it again at step 7 stays as it is.
- Decide explicitly what an unusable `kiln_nvs` means. The app treats a `kiln_nvs` init failure as "not recovery mode" (`boot_guard.c`, count 0). So when the recovery image has marked `kiln_nvs` failed, a persisted count cannot be what sends the app back to recovery. The clear could be reported as "not applicable" rather than failing the apply.

Either way, failing before the first erase is strictly better than failing after it.

### M2 (MED): direct ESP upload selects the new image before clearing boot_guard, and reboots even if the clear failed -- FIXED (ota_esp_post clears boot_guard before set_boot; a failed clear refuses with no set_boot and no reboot)

Where: `firmware/KilnFW_recovery/main/recovery_http.c:219-237`. `recovery_boot_partition_set_and_verify(target)` runs, then `clear_boot_guard` (line 230). Its result only changes the response text. `restart_soon(500)` runs unconditionally.

Failure scenario:
1. The board reached recovery through boot_guard, so the persisted count is 3 or more (`RECOVERY_MODE_BOOT_THRESHOLD`, `boot_guard.h:68`).
2. An operator pushes a good image. The boot partition is set and the clear fails (L1, a transient NVS error, or a failed read-back).
3. The board reboots into the new app. `next_boot_count()` loads a count of 3 or more, sets recovery mode, and SWITCH_PARTITIONs back to recovery.
4. The operator sees "ok, rebooting into new application image" and then the recovery AP again. This looks like a bad image.

This is the opposite order from `recovery_exit_post` (line 672), which clears first and refuses with 500 on failure.

Suggested fix: clear boot_guard before `recovery_boot_partition_set_and_verify`, as the exit route does. On a failed clear, return 500 and do not reboot. `app` already holds the verified image, so a later `/api/recovery/exit` can select it once the clear works. Applying the M1 pre-check here too covers the case where the upload has already erased `app`.

### L1 (LOW): `erase_key_in` creates the namespace it is erasing from -- FIXED (erase_key_in opens READONLY first; a missing namespace/key is success and nothing is created)

Where: `firmware/KilnFW_recovery/main/recovery_http.c:592-611`. `nvs_open_from_partition(..., NVS_READWRITE, ...)` creates the namespace when it is absent, so the `ESP_ERR_NVS_NOT_FOUND` branch at line 596 is effectively unreachable for READWRITE. `clear_boot_guard` (line 635) calls it for both the current key (`kiln_cfg`/`bootguard`) and the legacy key (`boot_guard`/`count`), and the legacy namespace is normally absent.

Failure scenario:
- Every clear writes a new namespace entry for the legacy namespace into `kiln_nvs`. This is harmless but needless.
- On a `kiln_nvs` with no free entry, the open fails with `ESP_ERR_NVS_NOT_ENOUGH_SPACE`. `clear_boot_guard` then fails, and with it exit (500), `boot_guard_reset`, apply (M1) and the direct-upload clear (M2). That holds even though the key to erase does not exist.

Suggested fix: open READONLY first. Treat `ESP_ERR_NVS_NOT_FOUND`, or a missing key found with `nvs_find_key`, as success with nothing to erase. Only reopen READWRITE to erase a key that exists.

### L2 (LOW): one stalled client can hold the single httpd task for about 100 s -- FIXED (read_body_exact uses a 15 s no-progress wall-clock deadline)

Where:
- `firmware/KilnFW_recovery/main/recovery_http.c:792-811`. `read_body_exact` allows up to 20 consecutive recv timeouts, each at the default `recv_wait_timeout` of 5 s.
- `recovery_http.c:853-870`. `pico_upload_post` calls it.
- `recovery_upload.c:36-49`. The ESP upload allows `RECV_TIMEOUT_RETRIES`.
- httpd is one task (`recovery_http.c:1015`).

Failure scenario: a client starts a Pico upload over a weak SoftAP link and stops sending. For up to about 100 s no other request is served: not `/api/recovery/status`, not apply status, and not Pico abort. The page and the MCP tools time out and report the board as dead. Nothing is corrupted, but it is misleading during an incident, and abort is not reachable during that window.

Suggested fix: use a wall-clock deadline (for example 15-20 s with no progress) instead of a count of timeouts. Alternatively, lower the Pico retry count to match the ESP path.

### L3 (LOW): the stage stays VERIFIED after a direct upload, and recovery will later apply it with no version check -- FIXED (ota_esp_post erases the stage header after a successful upload; a failure is logged and reported, not fatal)

Where:
- `firmware/KilnFW_recovery/main/recovery_http.c:183-237`. `ota_esp_post` never touches the stage.
- `recovery_apply.c`. Apply has no version comparison, which is by design (WP5: "policy is WP6's").
- The app's stale-stage auto-clear (`update_http_stale_stage_check()`, `GITHUB_RELEASE_UPDATE_PLAN.md` section 4) clears only a stage whose `app_elf_sha256` equals the running image.

Failure scenario:
1. Version A is staged (VERIFIED). The operator instead pushes version B directly from recovery and boots it.
2. The app's auto-clear does not match A, so A stays staged and installable.
3. Later the `/ota` Install button, or a recovery Apply, installs A over B. If A is older than B, this is a downgrade the app's upload gate (section 6) would have refused, because that gate runs only at stage time. If A is older than a `zones_cfg` schema bump, the rollback hazard in CLAUDE.md follows: the older firmware runs on default PID gains.

Suggested fix (any of):
- Clear the stage header in `ota_esp_post` after a successful direct upload.
- Have the app's Install path re-run `update_policy_decide` against the running version before `recovery_boot`.
- Show the staged version next to the running version on the recovery page's Apply box, with a warning when it is older.

### L4 (LOW): Pico relay task stack is tight and unmeasured -- FIXED (RELAY_STACK raised to 6144)

Where: `firmware/KilnFW_recovery/main/recovery_pico.c:48` (`RELAY_STACK 4096`). The task uses `char t[256]` in `handle_frame` (line 305) and `char m[240]` in `run_transfer`. It also makes vsnprintf-family calls (about 1 KB+ on ESP-IDF newlib) and UART driver calls.

Failure scenario: a deep path, such as a status-format error message during a transfer, overflows into adjacent heap. This is the same class as the 2026-08/09 stack incidents. The high-water mark is only logged, and `check_stack_margin_registration.ps1` coverage of recovery tasks was added recently, but no bench measurement is recorded.

Suggested fix: measure the high-water mark on the bench during a full Pico transfer that includes an abort. Alternatively, raise the stack to 6144 now (stack bumps are pre-authorised). `RELAY_INTERNAL_NEED` (line 54) follows automatically.

### I1 (INFO): power cut during the copy

`recovery_apply.c` erases ahead in 64 KB blocks and writes 4 KB chunks, header block first. When recovery was entered through `recovery_enter` or the boot_guard switch, `otadata` points at factory, so a cut during the copy reboots into recovery with the stage intact and re-applying is correct (plan section 4).

If recovery was reached by the bootloader's fallback (ota_0 invalid while `otadata` still selects ota_0), a cut mid-copy leaves a partially written ota_0 with a valid first block. The board then relies on the bootloader's power-on validation (full checksum and SHA, which is not skipped: no `SKIP_VALIDATE_ON_POWER_ON` in either sdkconfig.defaults) to reject it and fall back to factory. This is expected to hold.

Suggestion: name this case in the WP5 bench list, or have apply select factory explicitly (or erase `otadata`) before the first erase. That would make it independent of bootloader validation.

### I2 (INFO): no version or downgrade check in recovery

Apply and direct upload accept any image that passes structural checks, chip id, `project_name` and `esp_image_verify`. This is consistent with the owner decision of no release signing and with WP5's "policy is WP6's", but see L3 for the path that bypasses WP6.

### I3 (INFO): `esp_image_verify` on the httpd task

`app_image_verified` (`recovery_http.c:129`) runs from status (line 465), exit (line 682) and sw_reset (line 776) on the 8192 B httpd stack. It hashes up to the full `app` image, blocking every route for that time. The result is cached and invalidated around uploads and apply, so this is a one-off per change. The cache (`s_verify_known`, line 120) is unlocked and relies on httpd being one task. That holds today and should be noted if a second server task is ever added.

### I4 (INFO): operator-chosen Pico slot

When an old Pico bootloader reports no trailer, the operator picks the slot. A wrong pick leaves the Pico unbootable until SWD. The refusal text says so. No change suggested beyond keeping that text.

### I5 (INFO): "done" visible for 1.5 s -- FIXED (recovery_apply_staged reports PROBABLE-OK when contact is lost after the finalizing phase)

`recovery_apply_esp.c:173-174` waits 1.5 s then restarts. A poller slower than that sees the SoftAP drop and has to infer success from the app coming up. The MCP `recovery_apply_status` tool should treat "connection lost after `verify`/`set_boot` progress" as probable success and confirm through the app.

## Not covered

- No bench run. Every power-cut statement above comes from reading the code. The WP5 bench cases (plan section 12) remain the authority.
- The LCD passphrase and SoftAP code was not reviewed in depth.

## Second pass (2026-10-09, origin/dev db1f4df5)

This pass was a read-only Opus review. It covered the parts the first pass left out: the passphrase and SoftAP, the relay hold, NVS failure handling in recovery, Wi-Fi reset, and test vacuity. It also re-checked the app-side entry and exit paths against current origin/dev. No board was accessed. Owner decisions are taken as given and are not findings: the image is unauthenticated, the passphrase is random per boot and shown only on the LCD, and there is no USB-serial recovery.

### R2-L1 (LOW): recovery exit and boot_guard reset fail outright when kiln_nvs failed to init in recovery

**Status: FIXED in 138f001ca: exit and boot_guard_reset now use `boot_guard_clear_or_na()`; a real write/verify failure is still a failure. Host-pinned by `check_recovery_wifi_policy.ps1` source scan + mutant.**

`boot_guard_reset_post` and `recovery_exit_post` in `firmware/KilnFW_recovery/main/recovery_http.c` call plain `clear_boot_guard()`. They do not call `boot_guard_clear_or_na()`, which treats the `RECOVERY_NVS_FAIL_KILN` init-failure bit as "not applicable".

If kiln_nvs failed to init, `nvs_open_from_partition()` returns `ESP_ERR_NVS_PART_NOT_FOUND`, not `ESP_ERR_NVS_NOT_FOUND`. As a result exit always answers 500 and never selects `app`. The same state does not block apply and direct upload, because both use the `_or_na` variant. After a first-boot power cut, exit is the documented operator action. The only way out of this state is to re-upload or re-apply an app image.

The `_or_na` assumption is sound (see R2-I2), so exit could use it too.

### R2-L2 (LOW): recovery Wi-Fi reset can be undone by the app's legacy adoption

**Status: FIXED in 138f001ca: `wifi_reset_post` also erases the default-partition `wifi_cfg` namespace (`nvs_erase_all`) and answers success only through `rhp_wifi_reset_ok()`; an unreachable default partition counts as not applicable. Pinned by the policy check + mutants.**

`wifi_reset_post` erases the `WIFI_RESET_KEYS` in `wifi_nvs`/`wifi_cfg` only. On the next boot, the app's `wifi_prov_migrate_from_default_partition()` (`firmware/KilnFW/App/drivers/net/wifi_prov_nvs.c`) sets `adopt = !nvs_saved_nets_record_present(...)`. The reset just erased `saved_nets`, so `adopt` is true. Any legacy `wifi_cfg` copy still in the default `nvs` partition is then adopted, and the forgotten network comes back.

A legacy copy survives only if the app's earlier best-effort stale erase failed. The comment on that erase names this exact "recovery wifi reset cannot resurrect it" case as its purpose, so in that narrow case the guarantee is not met. The route reports "cleared" anyway.

Suggested fixes, either of which closes it:

- have the recovery reset also erase the default-partition legacy keys;
- have it leave a tombstone that the migration honours.

### R2-L3 (LOW, test vacuity): `ric_boot_guard_decode` version check is untested

**Status: FIXED in 138f001ca: the wrong-version vector now carries a valid CRC; negtest of the version comparison is CAUGHT.**

In `test_recovery_image_check.c`, the wrong-version vector sets `rec[0]=2` without recomputing the CRC. The CRC check therefore rejects the vector before the version check is reached. A negtest that deleted the version comparison was MISSED (table below).

The impact is limited to the boot_guard count shown on the status page and LCD. The recovery image never acts on that count. The fix is to recompute the CRC in that vector.

### R2-I1 (INFO): direct ESP upload has no boot_guard pre-clear probe

**Status: FIXED in 138f001ca: `ota_esp_post` clears the boot counter before streaming into `app`; failure refuses with `app` untouched and the body unread (same existing pattern as the no-app-partition refusal), no new failure mode. A rejected upload leaves the counter cleared, which is harmless.**

`ota_esp_post` streams and overwrites `app` first, and only then calls `boot_guard_clear_or_na()`. Apply probes before it writes; this is the M1 ordering it was fixed to follow. If the clear fails here, the new image sits in `app` unselected, and with R2-L1 exit fails too.

This is not a brick: factory stays bootable and the upload can be retried. Probing before the write would make the path match apply.

### R2-I2 (INFO): the "not applicable" assumption holds

The app's `boot_guard_init()` reads count 0 in three cases:

- kiln_nvs init fails;
- the record is missing or corrupt;
- `hal_kv_init_partition` erased on NO_FREE_PAGES or NEW_VERSION_FOUND.

So when recovery cannot open kiln_nvs, skipping the clear does not walk the app straight back into recovery. A failure that happens only in recovery and is transient costs at most one extra bounce back into recovery. The recovery image never erases NVS itself.

### R2-I3 (INFO): stale docs on Pico contact and S6b

**Status: FIXED in 138f001ca: `docs/OTA_SINGLE_SLOT.md` section 6 corrected (update/status/reboot frames, never arm/heat); expected S6b latch documented there and in `docs/RECOVERY_IMAGE_PLAN.md`.**

`docs/OTA_SINGLE_SLOT.md:126` says the recovery image "contains no code that can talk to the Pico at all". That is no longer true: `recovery_pico.c` sends kilnlink frames. It sends only `UPDATE_*`, `ANNOUNCE_VERSION` (0x0F), `GET_STATUS` and `REBOOT` (0x29), and no arm or heat command, so the safety conclusion still holds.

Neither `docs/RECOVERY_IMAGE_PLAN.md` nor `docs/GITHUB_RELEASE_UPDATE_PLAN.md` says that an S6b (link-dead) latch is the expected result of any recovery dwell. Nor do they say that it needs a clear after exit; the owner decision is to clear it without asking when the cause is the recovery image. Both points are worth a sentence in the plan.

### R2-I4 (INFO, test vacuity): apply copy erase granularity is not pinned

**Status: FIXED in 138f001ca: `test_recovery_apply.c` asserts exactly two 64 KiB app erases plus yields; negtest of the erase-ahead clamp is CAUGHT.**

`recovery_apply.c` erases `app` in 64 KB blocks just ahead of the copy and yields between blocks. A negtest that replaced this with a single erase of the whole remaining partition was MISSED: the host test still passed (194/0). Under that mutation a long blocking erase would starve the httpd/watchdog. Nothing would catch it before a bench run.

### Negative tests

Each mutation ran in a throwaway worktree via `tools\negtest.ps1 -Preset check`, base db1f4df5. The real tree was unchanged afterwards and every copy was removed.

| Check | Mutation | Result |
|---|---|---|
| `check_recovery_image_check.ps1` | chip id comparison removed | CAUGHT |
| `check_recovery_image_check.ps1` | image magic check removed | CAUGHT |
| `check_recovery_image_check.ps1` | segment 0 body bound removed | CAUGHT |
| `check_recovery_image_check.ps1` | app desc magic check removed | CAUGHT |
| `check_recovery_image_check.ps1` | length > content check removed | CAUGHT |
| `check_recovery_image_check.ps1` | boot_guard CRC check removed | CAUGHT |
| `check_recovery_image_check.ps1` | boot_guard version check removed | MISSED (R2-L3) |
| `check_recovery_apply.ps1` | first-chunk read error ignored | CAUGHT |
| `check_recovery_apply.ps1` | copy read error ignored | CAUGHT |
| `check_recovery_apply.ps1` | 64 KB erase-ahead replaced by one whole-remaining erase | MISSED (R2-I4) |

Two stale copies from other sessions, `C:\wt\negtest_9gk3kq` and `C:\wt\negtest_fdjlv9`, could not be removed by negtest. They are not from this review and were left alone.

### Checked and found correct

- **Passphrase.**
  - 12 characters drawn with `rnd[i] & 31` from a 32-character alphabet: unbiased, 60 bits.
  - `esp_fill_random` is called with `bootloader_random_enable()` active before Wi-Fi init.
  - The value lives only in `s_net_pass` and is used only by the LCD draw. No log or HTTP response carries it, and `test_recovery_lcd_policy` scans for log leaks.
- **Relay hold.**
  - The SX1509 gets both a hard and a soft reset, and data is written before direction.
  - IO0-IO3 and IO5 are held low, verified by read-back with 3 attempts.
  - A hold task re-asserts every 1 s and latches a fault.
  - The relay bits in `s_data` are never set, and LCD pin writes share `s_io_lock`.
  - With no safety-link task, the Pico sees silence, latches S6b and drops K4. ARMED needs the ESP, so heat cannot be granted.
- **Pico relay bounds.** `recovery_pico_reserve` rejects a length of 0 and anything over the 832 KB slot. It requires PSRAM plus an internal-heap floor, and keeps its busy state under `s_lock`. The single httpd worker serialises the cross-module busy checks.
- **Apply.** It pre-clears via `_or_na` and invalidates the verify cache before applying. It then re-hashes `app`, runs `esp_image_verify`, and selects `app` with a verified set_boot before erasing the stage.
- **App side.**
  - The `recovery_enter` handler checks, in order: ADMIN tier, system-mode gate, OTA interlocks, update mutex, an authoritative relay-off read, then verify-and-select factory. Only after that does the reboot task send ANNOUNCE_REBOOT.
  - The threshold switch verifies the recovery image before `boot_partition_set_and_verify(factory)`, and restores the running partition on SET_FAILED.
  - The recovery `ric_boot_guard_decode` layout (version, reserved[3], count u32, crc32 of the first 8 bytes) matches the app's 12-byte record.
- **Request bounds.** The `apply_status` body and the other parse bounds, re-read from the first pass, are bounded.

### Not covered (second pass)

- No bench run.
- `recovery_http.c` has no host test. The ordering and `_or_na` selection above come from reading the code only.
- LCD drawing beyond passphrase handling, and the Pico bootloader side of the update relay.
