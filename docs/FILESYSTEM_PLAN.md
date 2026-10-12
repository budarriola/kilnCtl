# Filesystem plan (pending work only)

History, the `cfg` partition rollout, runbooks and audits live in
`docs/FILESYSTEM.md`. What shipped for user data: `docs/CONFIG_FILESYSTEM.md`
and `docs/FILESYSTEM_USER_DATA.md`. Rationale for the `logs` track below:
`docs/FILESYSTEM.md` section "The one viable track" and
`docs/LITTLEFS_ASSESSMENT.md`.

## Parked: `logs` SPIFFS to LittleFS

Scope: swap the filesystem behind the existing `logs` partition only. No
partition-table offset or size change.

**Trigger to start (neither has fired):** `logs` retention requirement raised
past what 256 KiB/kind covers (~1.9 h of firing telemetry, `LOG_STORE_MAX_SEGMENTS`
= 8), OR `joltwallet/littlefs` fetch at build time stops being a concern.

Done (see FILESYSTEM.md): step 1 managed component pinned (`ca5d90c5`), step 2
`CONFIG_KILNCTL_LOGS_LITTLEFS` flag guarding the mount swap (`b039d5ee`;
default keeps SPIFFS).

Pending:

- **Step 3, bench flash with the flag on (hardware-gated).** One board,
  `flash_firmware(verify=True)`, accepting the one-time loss of existing `logs`
  content. Confirm log writes land and rotate at the 8-segment cap across a
  reboot, and record the `get_heap_status` internal-heap delta in the commit
  (8 KB internal floor applies).
- **Step 4, flip the default and delete the SPIFFS path.** Gate: step 3 has
  run through at least one full firing with no regression. The flag flip and
  the `partitions.csv` `logs` subtype change must land in the same commit
  (`check_partition_labels_vs_firmware.ps1` derives the expected subtype from
  `CONFIG_KILNCTL_LOGS_LITTLEFS`). Last and hardest to undo; do not batch it
  with anything else.
