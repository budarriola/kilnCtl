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

### M1 (MED): an apply whose boot_guard clear cannot succeed erases `app` and leaves the board in recovery

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

### M2 (MED): direct ESP upload selects the new image before clearing boot_guard, and reboots even if the clear failed

Where: `firmware/KilnFW_recovery/main/recovery_http.c:219-237`. `recovery_boot_partition_set_and_verify(target)` runs, then `clear_boot_guard` (line 230). Its result only changes the response text. `restart_soon(500)` runs unconditionally.

Failure scenario:
1. The board reached recovery through boot_guard, so the persisted count is 3 or more (`RECOVERY_MODE_BOOT_THRESHOLD`, `boot_guard.h:68`).
2. An operator pushes a good image. The boot partition is set and the clear fails (L1, a transient NVS error, or a failed read-back).
3. The board reboots into the new app. `next_boot_count()` loads a count of 3 or more, sets recovery mode, and SWITCH_PARTITIONs back to recovery.
4. The operator sees "ok, rebooting into new application image" and then the recovery AP again. This looks like a bad image.

This is the opposite order from `recovery_exit_post` (line 672), which clears first and refuses with 500 on failure.

Suggested fix: clear boot_guard before `recovery_boot_partition_set_and_verify`, as the exit route does. On a failed clear, return 500 and do not reboot. `app` already holds the verified image, so a later `/api/recovery/exit` can select it once the clear works. Applying the M1 pre-check here too covers the case where the upload has already erased `app`.

### L1 (LOW): `erase_key_in` creates the namespace it is erasing from

Where: `firmware/KilnFW_recovery/main/recovery_http.c:592-611`. `nvs_open_from_partition(..., NVS_READWRITE, ...)` creates the namespace when it is absent, so the `ESP_ERR_NVS_NOT_FOUND` branch at line 596 is effectively unreachable for READWRITE. `clear_boot_guard` (line 635) calls it for both the current key (`kiln_cfg`/`bootguard`) and the legacy key (`boot_guard`/`count`), and the legacy namespace is normally absent.

Failure scenario:
- Every clear writes a new namespace entry for the legacy namespace into `kiln_nvs`. This is harmless but needless.
- On a `kiln_nvs` with no free entry, the open fails with `ESP_ERR_NVS_NOT_ENOUGH_SPACE`. `clear_boot_guard` then fails, and with it exit (500), `boot_guard_reset`, apply (M1) and the direct-upload clear (M2). That holds even though the key to erase does not exist.

Suggested fix: open READONLY first. Treat `ESP_ERR_NVS_NOT_FOUND`, or a missing key found with `nvs_find_key`, as success with nothing to erase. Only reopen READWRITE to erase a key that exists.

### L2 (LOW): one stalled client can hold the single httpd task for about 100 s

Where:
- `firmware/KilnFW_recovery/main/recovery_http.c:792-811`. `read_body_exact` allows up to 20 consecutive recv timeouts, each at the default `recv_wait_timeout` of 5 s.
- `recovery_http.c:853-870`. `pico_upload_post` calls it.
- `recovery_upload.c:36-49`. The ESP upload allows `RECV_TIMEOUT_RETRIES`.
- httpd is one task (`recovery_http.c:1015`).

Failure scenario: a client starts a Pico upload over a weak SoftAP link and stops sending. For up to about 100 s no other request is served: not `/api/recovery/status`, not apply status, and not Pico abort. The page and the MCP tools time out and report the board as dead. Nothing is corrupted, but it is misleading during an incident, and abort is not reachable during that window.

Suggested fix: use a wall-clock deadline (for example 15-20 s with no progress) instead of a count of timeouts. Alternatively, lower the Pico retry count to match the ESP path.

### L3 (LOW): the stage stays VERIFIED after a direct upload, and recovery will later apply it with no version check

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

### L4 (LOW): Pico relay task stack is tight and unmeasured

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

### I5 (INFO): "done" visible for 1.5 s

`recovery_apply_esp.c:173-174` waits 1.5 s then restarts. A poller slower than that sees the SoftAP drop and has to infer success from the app coming up. The MCP `recovery_apply_status` tool should treat "connection lost after `verify`/`set_boot` progress" as probable success and confirm through the app.

## Not covered

- No bench run. Every power-cut statement above comes from reading the code. The WP5 bench cases (plan section 12) remain the authority.
- The LCD passphrase and SoftAP code was not reviewed in depth.
