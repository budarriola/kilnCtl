# ESP32-S3 brought up to HEAD (2026-09-09)

## Summary

Flashed the ESP32-S3 (KilnFW) from `316967b7` (~16 commits behind) to HEAD
`0dddd435`, via a clean detached worktree at `C:\wt\kilnctl_head` (removed
after the flash). Build ran through the PowerShell tool per the ESP-IDF
gotcha (git-bash's inherited `MSYSTEM=MINGW64` silently no-ops `idf.py`).
`sdkconfig` was copied from the main tree and diffed byte-identical before
building. `flash_firmware(kiln_fw_root=...)` reported flashed-and-verified
(bootloader + partition table + app; RUNNING partition and `fw_build`
matched the just-built binary).

## Pre-flight (board running 316967b7, before flash)

- `get_fw_version`: commit `316967b7`, built `2026-09-09 21:39:34Z`, tree clean.
- Link up, `trip_mask 0x0000`, `state armed`; relays R1-R4 = 0; executor and
  autotune idle.
- Crash report: present, unacknowledged (`IllegalInstruction`,
  `profile_executo`, `PANIC`) — pre-existing, left unacknowledged, did not
  block flashing.
- `/api/readiness` `safety_commissioned`: `not_done`, "6 of 68 applicable
  safety parameters still have no value" — the stale-mirror symptom this
  flash was meant to fix.
- Pico (`safety_get_commissioning`): `commissioned=True`, `config_version
  134`, `config_crc 42374`.
- `cfg` LittleFS: mounted, 6 files (`display_power.dat`, `ramp_assist.dat`,
  `relay_cycles.dat`, `relay_names.dat`, `unit_pref.dat`, `zones.json`),
  77824/524288 B used.
- All 3 main-board thermocouples valid, ~39-40 C; safety-processor TC valid,
  39.46 C.

## Flash

`flash_firmware(kiln_fw_root="C:\\wt\\kilnctl_head\\firmware\\KilnFW",
verify=True)` → `"flashed and verified OK (bootloader + partition table +
app), board reset and running"`, provenance HEAD `0dddd435` tree clean.

## Post-flash: S6a

As expected, the reset tripped S6a while the safety-link handshake came
back up. `safety_get_diag()` showed `trip_reason 7 | trip_mask 0x0040` —
**bit 6 only** (mainFault), nothing else set. Confirmed link up
(`cmd_status_count` climbing, `frames_received` incrementing) before
calling `safety_clear_trip()`; diag then read `state armed | trip_reason 0
| trip_mask 0x0000`.

## Post-flash verification

- `get_fw_version`: commit `0dddd435`, built `2026-09-10 00:09:57Z`, "board
  is running HEAD".
- `debug_check_partition_table`: on-chip table matches `partitions.csv`
  exactly.
- `get_heap_status`: `reset_reason='software (esp_restart)'`, `uptime_s=29`
  right after flash; heap looks normal. The pre-existing unacknowledged
  crash report from the earlier `profile_executor` panic is still reported
  (as it should be — this flash did not touch that record) — **left
  unacknowledged**, per instructions.
- `recovery_mode`: `ok`, not in recovery mode. `boot_guard_reset_counter()`
  wiring did not need to fire any warning.
- `cfg` LittleFS: still `mounted=True status='mounted'`, same 6 files, same
  sizes (77824/524288 B used) — **did not reformat**.
- All 3 main-board thermocouples valid (~43 C); safety thermocouple valid.
- Relays: `io_read` → R1=R2=R3=R4=0. Executor and autotune both idle.

## Readiness mirror reconciliation

- Before: `safety_commissioned` = `not_done`, "6 of 68 applicable safety
  parameters still have no value".
- After: `safety_commissioned` = `not_done`, "**3 of 65** applicable safety
  parameters still have no value".

The applicable-count dropped by 3 (68→65) and the unset count dropped by 3
(6→3) — consistent with `2900db99` correctly gating `ct_channel_map[0..2]`
out of the applicable set now that `ct_topology` says they don't apply, while
correctly still counting `i_normal_a[0..2]` as genuinely unmeasured (no CTs
read live current). This matches the Pico's own `commissioned=True` view —
the two sides now agree in substance; `safety_commissioned` remains
`not_done` on the ESP side only because of the three still-genuinely-missing
current measurements, not because of stale mirroring. Not a full pass/fail
match on the `status` string (still `not_done`), but the *reason* is now the
correct one.

## Pico commissioning / config CRC — flagged for follow-up, not resolved

Per the standing note that a `SET_CONFIG` resend on link down/up can
legitimately bump `config_version`, the version did move (134→135), which
is expected. But **`config_crc` also changed, 42374→63771**, with all
inspected commissioned values (`S1 abs_max_temp_c=80C`, `S8
max_rate_c_per_min=20C/min`, dormant CT channels, `estop_active_level`,
`tc_placement_mode`, `mains_voltage_v`, `tc_type`) reading identical before
and after. `boot_id` on the Pico is unchanged (246), so this was not a Pico
reset — the CRC moved from the ESP's post-flash config resend alone.

This is reported as a finding rather than accepted: a CRC change with no
externally-visible value change is most plausible if the new firmware's
`ct_channel_map`-gating fix (`2900db99`) also changed how the config page is
*encoded* on the wire (e.g. a field now sent as explicitly zero/absent
instead of a prior sentinel), which would move the CRC without moving any
value this tool surfaces. That is a plausible, not confirmed, explanation —
flagging per instructions rather than dismissing it. No corrective action
taken; `safety_get_commissioning` still reports `config CRC ... matches
live, not stale`, and all displayed guard values are unchanged.

## Final board state

Relays off (all 4), nothing running (executor and autotune idle), no trip
latched (`trip_mask 0x0000`, `state armed`), link up, `cfg` filesystem
mounted with its original files intact. Crash report deliberately left
unacknowledged (pre-existing, unrelated to this flash).

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
