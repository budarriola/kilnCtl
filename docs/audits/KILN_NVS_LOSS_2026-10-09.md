# kiln_nvs loss on the bench board, 2026-10-09

Status: root cause found. A host sent a factory reset on purpose, and nobody recorded or undid it.
It was not a silent firmware erase.

Follow-up (owner decision 2026-10-09): backup format version 7 carries `thermo_count`/`relay_count`. `backup_import` onto a board with `thermo_count` 0 sets that topology first (validated, unwritten until the batched commit), then the zones; a configured board whose topology differs is refused with nothing written. A version 6 or older backup onto an empty board is still refused as before.

## Symptoms (seen on dev 8fcd3237, 2026-10-10)

1. The zones config was gone. Zones loaded with `thermo_count=0, relay_count=0`.
2. `kiln_cfg/estop_verif` was missing from `kiln_nvs`, so readiness showed `estop_verified` as not_done.
   It had read ok in every BENCH_TEST_LOG entry up to and including the 10-09 aux suite on 1e722ee0.
3. `kiln_cfg/crash_rpt` was missing. As a result an old coredump (the tls_spike panic) was captured
   again as a new, unacknowledged crash record, stamped with the running build.

## Cause

A host sent `SYSTEM_CMD_FACTORY_RESET` scope 1 (kiln) over UART on 2026-10-09 at
**22:54:19Z** (15:54:19 PDT). The board was running 129586d4 and had been up about 64 minutes.
Evidence is in `tools/PcTools/logs/session_20261009_145021.log`, lines 1989-1994:

```
W (3861274) factory_reset: factory_reset: scope 'kiln' requested -- erasing
W (3861614) factory_reset: erased NVS partition 'kiln_nvs'
W (3862004) factory_reset: kiln factory reset: 14 cfg file(s) deleted
W (3862004) uart_bridge: system: FACTORY_RESET scope 1 requested by host -- erasing and rebooting
W (3862504) factory_reset: rebooting now ...
kilnctrl.serial_link: no reply for msg 47565 to dev0/task6, retry 1/10
```

Scope kiln erases the whole `kiln_nvs` partition (`factory_reset.c` `kKilnOnly`, then
`hal_kv_erase_partition`) and deletes the kiln-scope cfg LittleFS files. That covers all three symptoms:

- `zones.json` (cfg) was deleted. Since the cfg dual-write close (af6e12eb), the NVS copy is no
  longer a fallback.
- `estop_verif` and `crash_rpt` are keys in `kiln_cfg` in `kiln_nvs`, and the erase took them.
- Scope kiln leaves the `coredump` partition alone. With `crash_rpt` gone, `crash_report.c`
  captures the dump that is still stored as a new record on the next boot. That boot's log shows
  `dualwrite_window: boot_check: clean=0 reason=3 crash_pending=1`.

The boot after the reset, `session_20261009_155428.log`:

- 15:54:29 PDT: `zones config did NOT load cleanly`, then `zones API up (thermo_count=0, relay_count=0)`.
- 15:56:51: admin web login from 192.168.1.87.
- 15:56:52: `boot_guard_reset ... recorded as a deliberate flash`.
- Nothing else until SNTP at 16:54. No preset was applied and no restore was run.

The board was idle for the hour before the reset; the log shows only heartbeats.

## What sent it

The board does not record who sent the command; it logs only "requested by host" on the UART task
`SYSTEM` (task 6). The kilnctrl MCP server's log carries no per-call timestamps. These are the only
code paths that send `devices.system_factory_reset()`:

| Path | File |
|---|---|
| MCP `run_action("System: Factory Reset", scope, confirm=True)` | `tools/PcTools/src/kilnctrl/actions.py:785-805` |
| MCP `factory_default_then_load_preset(name, scope=1)` (`load_config_preset` family) | `tools/PcTools/src/kilnctrl/mcp_server_ui_test.py:141-208` |
| PcTools GUI Danger Zone | `tools/PcTools/src/kilnctrl/gui_danger_zone.py:232` |

No bench_test case sends a factory reset (`cases_web_safety.py:21` says so explicitly, and a grep of
`bench_test/` finds no caller). The default scope of `factory_default_then_load_preset` is exactly
kiln. That tool returns `factory reset ok, but preset apply failed` and leaves the board wiped when
the preset step fails afterwards. That fits a board that was left with no zones. The
`boot_guard_reset` from 192.168.1.87 two minutes later points to an agent session. The likely
candidate is the one that built and flashed 129586d4 from `C:\wt\benchweb_g0hcrw` (ELF archived
21:49:57Z, web judge run 21:50-21:53Z). This attribution is inferred; it is not proven.

BENCH_TEST_LOG.md has no entry for the 129586d4 flash or for the factory reset. Its 10-09 "dev
8fcd3237 flash" section records the config wipe as being of unknown origin.

`docs/audits/BENCH_PROF1_DIVERGE_ZONES_WIPE_2026-10-09.md` left the zones wipe UNKNOWN. That doc's
path 5 (factory reset) is the answer, with scope kiln, not zones/all.

## Secondary event: kiln_config_apply on 8fcd3237

At 20:58:59 PDT (03:58:59Z on 10-10), `session_20261009_205126.log` shows
`estop_verification: E-stop interlock verification invalidated`. After it,
`kiln_cfg_swap` step 13 `commit_config` was rejected by the Pico because the relay was ARMED.
`kiln_config_apply` always invalidates the E-stop verification on purpose. So even if `estop_verif`
had survived the reset, this apply would have cleared it again. This is expected behavior, but the
bench log does not record it. The crash record and coredump were cleared at 21:09:40 PDT.

## Ruled out

- **A partition moved.** `kiln_nvs` stays at 0x18D000, size 0x10000, across every partitions.csv
  change: 9359c5a2 (2026-09-19), b78e8701/12d193aa (2026-10-04, 4 MiB app plus stage) and
  72555993. A partition-table flash cannot have moved it.
- **The NVS init erase path.** `hal_kv_init_partition` (`hal_kv_esp.c:337-371`) erases only on
  NO_FREE_PAGES or NEW_VERSION_FOUND, and it logs
  `NVS partition '%s' needs erase ... erasing THAT PARTITION ONLY`. No session log has that line.
- **backup_import or estop param writes.** Neither appears in the logs for the window, and neither
  can delete `crash_rpt`.
- **The firmware erasing on its own.** The erase was logged as a host request from the UART bridge.

## Verdict

This was not a firmware defect. A deliberate tooling or operator action erased the data, and that
is expected behavior for a scope-kiln factory reset. The defect is in process and tooling:

1. The reset was not recorded in BENCH_TEST_LOG, and no backup was taken before it.
2. No preset or backup was restored after it, and the result of the composite tool's failed
   preset step was not acted on.
3. Neither the MCP server nor the firmware attributes the call, so the sender cannot be named.

## Recommendations

1. Bench practice: log every UART or HTTP factory reset in BENCH_TEST_LOG with its scope and
   reason. Run `backup_export` first. **DONE bc334d45**: rule added to the `docs/BENCH_TEST_LOG.md` header.
2. Tooling: make `run_action("System: Factory Reset")` and `factory_default_then_load_preset` take
   a `backup_export` automatically before sending, and put the backup path in the result.
   **DONE bc334d45**: `factory_reset_guard.backup_before_reset()` runs first in the action, `factory_default_then_load_preset` and the GUI Danger Zone; refuses on export failure unless `skip_backup=True`; a failed preset apply now says `BOARD WIPED` and names the backup. MCP servers need a restart to pick it up.
3. Tooling: log every MCP tool call with a timestamp (tool name, caller, time) so that a
   destructive call can be traced to its sender.
   **DONE bc334d45**: `mcpkit/call_log.py` appends `<UTC time> <tool> <arg keys>` (never values) to gitignored `logs/mcp_calls/<port>_<date>.log`, hooked on every `kiln_call`/`kiln_batch` target and kept direct tool. Caller identity is not available to the server.
4. Firmware: a crash record that is missing after a deliberate reset should not bring back an
   already-reviewed dump as a fresh one. Either have factory reset scope kiln/all also erase the
   coredump, or have crash_report cross-check the dump's `app_elf_sha256` and not stamp it with the
   running `fw_build`. (Another agent is already working on the re-stamp.)
   **DONE (coredump half) 994c14d1**: `factory_reset.c` scope kiln and all call `crash_report_clear()` (record ack + coredump erase) on the flash worker; wifi/profiles do not; host-tested in `test_ota_http.c`. The re-stamp cross-check half is the other agent's.
5. Board recovery (owner or bench agent with write access):
   - The newest backup taken before the wipe is
     `logs/backup_export/kilnctl_backup_20261008T220350Z.json` (2026-10-08 22:03Z).
     `kilnctl_backup_20261010T041440Z.json` was taken at 04:14Z on 10-10, which is after both the
     wipe and the kiln_config_apply. Use it only after checking that its zones section holds the
     bench values and not post-wipe defaults.
   - A human must re-run the E-stop procedure in `firmware/SaftyFW/README.md` and then call
     `estop_verify`. That record can never be inferred or restored from a backup.
