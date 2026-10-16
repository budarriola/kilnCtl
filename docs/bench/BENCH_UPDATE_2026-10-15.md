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

## Open item

The unforced pre.3 to pre.4 path is blocked by fetch heap admission on pre.3. A possible workaround not tried here is a reboot to get a fresher heap (not tried: it would not change the 10752 B largest block much), or staging via update_stage_upload from the PC. Both need a decision from the coordinator.
