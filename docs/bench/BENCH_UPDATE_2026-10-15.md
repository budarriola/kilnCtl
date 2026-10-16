# Bench update test, 2026-10-15

Board: bench unit at 192.168.1.156 (home Wi-Fi). Notes below are bench observations only.

## Earlier steps

A dev build (f75cae102) was first flashed over JTAG with set_otadata_app=True so the board booted the app slot.

v1.0.0-pre.3 was then installed through the on-board update path with force. Dev builds report an unknown version, so both force and confirm_downgrade were required. The install job reported UNKNOWN because pre.3 has no /api/update/status. pre.3 reports "tree: dirty" (commit b9fa1748e, built 2026-10-13). Heap on pre.3: internal free 30007 B, min free 13779 B, largest-block low-water 7680 B (first seen at uptime 3 s, under the 7168 B SK-04 alarm).

## pre.3 to pre.4, normal path without force

Pre-state (uptime about 33800 s): link up, safety thermocouple valid, no firing. update_get_settings: repo budarriola/kilnCtl (default). A settings backup was taken first with backup_export (12073 B, logs/backup_export/kilnctl_backup_20261016T025308Z.json, 3 zones, 2 profiles, no password-shaped fields). Zone PID gains, plant model and coupling matrix were read for later comparison.

Result: the update could not start. Both attempts failed on the board with error=low_heap:

- update_check (twice): state=failed, kind=check, error=low_heap.
- update_stage_release(confirm=True), no force and no downgrade flag: state=failed, kind=download, error=low_heap.

Heap at the time: internal free 30007-30111 B, largest free block 10752 B, min free 13779 B. The board's own fetch admission refuses at this level, so pre.3 cannot reach GitHub or stage pre.4 on this unit while in this heap state. No stage was created, no install was started, and nothing on the board changed. The install, post-install checks and the zones comparison were not run.

## Retry after reset, and PC-side stage

The board was reset with sw_reset_esp (uptime 15 s afterwards, link up, no safety trip latched, so no clear was needed). Heap right after the reset: internal free 29871 B, largest free block 10752 B, min free 13643 B. This is the same largest block as before the reset, so a reboot does not help. update_check and update_stage_release (unforced) again failed with error=low_heap.

Device finding: a pre.3 unit cannot self-update from GitHub. Fetch admission refuses because the largest internal block is about 10.7 KB, and that is the steady state, not a transient.

Fallback: update_stage_upload of KilnCtrl-v1.0.0-pre.4.bin (2975824 B) with version 1.0.0-pre.4. The stage read back as verified, header ok, semver 1.0.0-pre.4, source upload, sha256 3b36b1f0...a17beec, identical to SHA256SUMS. So a PC-side stage works on pre.3.

update_install_start then failed after 1 s: the tool reads GET /api/update/status first, and pre.3 answers HTTP 404 "no such endpoint". No install was performed; the board still runs pre.3 with pre.4 staged and verified. I did not POST /api/update/install by hand because that would bypass the tool. The earlier forced pre.3 install was reported UNKNOWN for the same missing endpoint. Post-install checks were not run.

## Open item

To finish the test, someone must either let update_install_staged proceed when /api/update/status is 404 (a PC-side tooling change) or authorize a direct POST /api/update/install. The stage is in place and can be installed without re-uploading.
