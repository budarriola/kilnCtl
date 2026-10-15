# CLAUDE.md

kilnCtl: KiCad kiln controller + two firmwares (`KilnFW` ESP32-S3, `SaftyFW` RP2040) + Python tools.
History/rationale: `docs/CLAUDE_HISTORY.md` (read only when needed). Shared rules: `docs/agent_rules/COMMON.md`.

## Where to start
Fresh clone: `tools/setup.ps1`, then open `kilnCtl.code-workspace` (not the folder); `docs/SETUP.md`:
**edit `templates/`, never generated output.** Start tasks from **ROADMAP.md** (links per-area plans).

## MCP facade (always start here for boards or KiCad)
Servers publish 6-7 tools; the rest (235 tools for `kilnctrl`, 86 for `kicad`) sit behind a search facade.
Never conclude a capability is missing because you cannot see a tool.
```
kiln_help()   kicad_help()   kiln_find(query="read the temperature")
kiln_call(name="thermo_read")   kiln_batch(calls=[{"name":"safety_get_status"},{"name":"safety_get_link_stats"}])
```
Use `*_batch` for 2+ hardware ops. Connect failure: `.\tools\PcTools\scripts\mcp_servers.ps1 status|start|stop|restart`
(kicad :8766, kilnctrl :8767). Restart after editing server source (`fresh`/`stale`). Detail: `docs/MCP_SERVERS.md`.
Builds/tests are tools too: `build_kilnfw`, `build_saftyfw_host_tests`, `run_pctools_tests`.

## Hard rules
- Flash ESP only with `flash_firmware(confirm=True)` (OpenOCD, never esptool); Pico `debug_program(peer="pico")`.
- Never edit `.kicad_*` files directly. Never delete `firmware/KilnFW/elf_archive/`, `logs/coupling/*` or captured run data, or tracked files with local modifications.
- Git: no stash, no reset --hard, no checkout --, no force push. A green build in a dirty tree proves nothing: verify in a clean worktree.
- Negative-test every new check/test with `tools/negtest.ps1`, never by hand.
- PowerShell `*>` logs are UTF-16: wait with `tools/wait_for.ps1`, read with `tools/decode_log.ps1`.
- Run scripts with `powershell -ExecutionPolicy Bypass` (else Bash reports exit 0 with nothing run).
- Edit `tools/PcTools/pyproject.toml` => commit `uv.lock` too.

## Landing rule (owner 2026-10-14, batch checks)
Agents do NOT run `run_all_checks` and do not use `land.ps1`. Run targeted checks only, `git fetch`, rebase
onto origin/dev, `git push origin HEAD:dev` (ff only), then `tools\push_verify.ps1 -Branch dev -Commit <sha>`.
The coordinator batch-runs the full `-Fast` suite on the dev tip; `dev_promote` is gated on green.
Worktrees: `tools/worktree_mint.ps1` (PowerShell, under `C:\wt\`), `tools/commit_guard.ps1`, `tools/wt_status.ps1`.

## Topic index (read on demand)
- `docs/agent_rules/FLASH_OTA.md`: before flashing, OTA/recovery, boot_guard, or clearing S6a/S6b trips (`trip_mask = 1 << (reason-1)`, S6a `0x0020`).
- `docs/agent_rules/CHECKS.md`: run_all_checks (-Fast, SKIP-FAST, cache, baseline), negtest, log waiting, safe-to-delete cleanup.
- `docs/agent_rules/FIRMWARE_GOTCHAS.md`: editing firmware: crash ELF, task stacks, locks, URI cap, reset-one-side, file splits.
- `docs/agent_rules/BENCH_CAMERA.md`: capturing or judging the LCD with the bench webcam.
- `docs/agent_rules/KICAD.md`: schematic/PCB/hardware layout, mykicadMcp usage, git guards, KiCad setup.
- `docs/agent_rules/COMMON.md`: shared agent rules (heavy builds, check cache marking).
