# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**kilnCtl** is a KiCad-based electronics design project for a kiln controller. It includes a hierarchical schematic design, PCB layout, component library, and Python tools for programmatic access to project data.

## Where to start

Fresh clone: run `tools/setup.ps1`, then open `kilnCtl.code-workspace` (not the
folder). `docs/SETUP.md` explains what it generates and why those files are
gitignored — **edit `templates/`, never the generated output.**


**ROADMAP.md** at the repo root is the top-level plan spanning both firmwares
(`KilnFW` on the ESP32-S3, `SaftyFW` on the RP2040). Start tasks from there; it
links to the per-area plans that own the detail.

## Tooling: always go through the MCP facade

**Anything involving the boards or the KiCad project starts
with one of these calls.** Do not conclude a capability is missing because you
cannot see a tool for it — each server publishes six or seven tools and keeps
the rest behind a search facade (145 tools for `kilnctrl`, 86 for `kicad`, both
per `kiln_help()`/`kicad_help()` as of 2026-09-06).

```
kiln_help()                      # kilnctrl: main board (ESP32-S3) + RP2040 safety processor
kicad_help()                     # kicad:    the schematic, board and sourcing data
kiln_find(query="read the temperature")
kiln_call(name="thermo_read")
kiln_batch(calls=[{"name":"safety_get_status"},{"name":"safety_get_link_stats"}])
```

`*_batch` is the right form for any sequence of two or more hardware
operations — one round trip, and it stops at the first failure.

Both speak HTTP (`kicad` on 8766, `kilnctrl` on 8767)
and must be running. If a call fails to connect:

```powershell
.\tools\PcTools\scripts\mcp_servers.ps1 status   # or: start | stop | restart
```

The same four actions are status-bar buttons in `kilnCtl.code-workspace`, and
the servers auto-start when the workspace opens.

Both servers are long-running and keep serving whatever code they started
with — restart after editing server source (`kiln_help()`/`kicad_help()` and
`mcp_servers.ps1 status` self-report `fresh`/`stale` off `/health`, which also
carries `changed_files` and `commit`). `flash_firmware()`'s verification fix
(`06ea366`) sat inert for a while this way. Full detail: `docs/MCP_SERVERS.md`
"Stale-server self-announcing".

Firmware builds and host tests are tools too — `build_kilnfw`,
`build_saftyfw_host_tests`, `run_pctools_tests` — so
the toolchain invocations do not have to be rediscovered. Flashing is
`debug_program(peer="pico")` for the Pico. For the ESP32-S3, use `flash_firmware()`
instead — `debug_program(peer="esp")` now **refuses immediately**, before touching
OpenOCD at all, because that generic single-ELF path reliably fails flash-bank
detection/verify on this board (confirmed repeatedly); `flash_firmware()` is the
sanctioned working path, still OpenOCD, never esptool.

`flash_firmware()` accepts an optional `kiln_fw_root` override (absolute path
to a `firmware/KilnFW`-shaped directory whose `build/` already holds the
three binaries) for the sanctioned "build from a clean git worktree at HEAD"
workflow, when the main tree carries another session's foreign WIP that
would otherwise ride along or trip the sensitive-dirty guard below; the
git-provenance record and verification then reflect that worktree, not the
main tree, and `flash_provenance.json` records which path was used.

`flash_firmware()` writes the **`factory`** partition only — it does not touch
`otadata`. If an OTA has ever pointed the boot target at `ota_0`/`ota_1`, the
bootloader keeps booting that image and every later `flash_firmware()` reports
success while the board keeps running the OLD code (a change you added — a log
line, say — looks like it "vanished"). Fix: `ota_rollback_esp()` to restore the
factory boot target.

`ota_rollback_esp()` itself has a hazard, 2026-09-04: rolling back past a
`zones_cfg` schema bump (e.g. v22, `ZONES_CFG_VERSION` in
`firmware/KilnFW/App/drivers/persist/zones_config_json.h`) makes the older firmware
refuse the newer-than-it-knows blob and run that boot on **firmware-default
PID gains**, not the tuned ones — flash is left untouched, so reflashing the
newer firmware restores everything, but a firing started right after the
rollback and before reflashing runs on defaults with no separate warning.
Read back `control_get_zones` (or `GET /api/zones/config`) after any rollback
before heating. Full detail: `firmware/CommonFW/docs/UPDATE_PROTOCOL.md`
("`ota_rollback(processor)`" bullet).

`flash_firmware()` now checks this **automatically** after every flash (the
`verify` parameter, default `True`): it polls the board's own HTTP API for the
running partition (`/api/partitions`'s RUNNING marker) and for its reported
build timestamp (`fw_build`, compared against the `.bin`'s embedded
`esp_app_desc_t` build time) and FAILS the tool call loud, naming the actual
running partition/build and pointing at `ota_rollback_esp()`, if either
disagrees with what was just flashed. Manually checking the RUNNING marker or
the boot log's `running partition: '<name>'` line is now redundant for a normal
flash — still useful for checking partition state independent of a flash, via
`debug_check_partition_table()`. Pass `verify=False` only for bring-up when the
board's HTTP stack isn't expected to be up yet (e.g. Wi-Fi not provisioned
yet). Verification tries an ordered list of candidate hosts (the address the
board was actually observed answering at moments before the flash, first —
2026-09-04 fixed a bug where it polled only the AP-fallback `192.168.4.1`
immediately post-reset, before Wi-Fi re-associated, while the board was live
on the LAN the whole time) and re-resolves on every poll attempt. A board that
was demonstrably reachable pre-flash and answers at none of the candidates
afterward is a hard verification failure; only a board that was never
observed up (genuine bring-up) gets the soft WARNING instead of a failure,
since that's not the same as confirming the wrong thing landed.

Getting the board's own view of its health is one call: `get_heap_status` now
carries `reset_reason`/`uptime_s` through and also checks
`GET /api/crash_report`, printing a loud `UNACKNOWLEDGED CRASH REPORT` banner
if the board panicked and nobody has reviewed it yet (`2cc90b0` — the previous
version discarded `reset_reason` and never looked at crash_report at all, so a
board that panicked and rebooted clean read as healthy). `capability_preflight`
now refuses to start a run on a board with an unacknowledged crash regardless
of what the run needs.

`flash_firmware()` also records `git status --porcelain`/HEAD at flash time
(reported in the result, persisted to `KilnFW/build/flash_provenance.json`)
and refuses — naming the files — if the dirty set touches config-schema/
migration/safety code (e.g. `zones_config_*`), since this tree is normally
shared across sessions. An ordinary dirty tree is never refused. Override
only after reviewing the named files: `allow_sensitive_dirty=True`.

Also: `debug_reset` does not power-cycle external I2C peripherals. A safety
trip that latches right after an OTA reboot can be the SX1509 I/O expander
failing its post-reset init, not a firmware defect — a second `debug_reset`
clears it.

`tools/PcTools/scripts/capture_lcd.ps1` grabs one frame from the bench webcam
aimed at the board's LCD, cropped to the panel (`-Full` for an uncropped frame
if the camera moved and the crop needs re-measuring) — display rendering can
be checked without a human at the bench. Judge colors by **numeric pixel
sampling**, never by eye or by matching theme source constants:
`ffmpeg -i img.jpg -vf "crop=W:H:X:Y,scale=1:1" -f rawvideo -pix_fmt rgb24 - | od -An -tu1`.
Always sample an off-screen bezel region too, as a neutral reference. If it
fails with ffmpeg exit `-5`, check for another process holding the C920
first — the device still enumerates fine (`ffmpeg -list_devices true -f dshow
-i dummy`), it is just busy.

`tools/PcTools/scripts/sample_lcd_region.ps1` wraps that pipeline: given a
capture from `capture_lcd.ps1` and a region (`-X -Y -W -H`), it prints the
region's mean RGB alongside the same for a bezel reference region, so this
recipe doesn't have to be re-derived by hand each time
(`.\sample_lcd_region.ps1 -Image full.jpg -X 500 -Y 650 -W 8 -H 8`).

**Camera aim (2026-09-06):** the bench webcam has drifted since the crop
defaults (`X=339 Y=487 W=594 H=231`, set as a stopgap in `26a8a98`) were
measured. A fresh `-Full` capture shows the LCD panel's top-left corner
around `(300-339, 460-490)` in the 1280x720 frame, with the panel's bottom
edge running off the bottom of the frame (screen content still fully blue
right at `y=719`, so an unknown slice of the lower UI is not captured at
all) and the right edge cutting close to frame content near mid-frame
(perspective-skewed, not a clean rectangle in-frame). Owner action: re-aim
the camera so the whole panel is inside the frame and roughly centered
(tilt down / pull back enough that the bottom edge clears `y=719` with
margin), then re-run `capture_lcd.ps1 -Full`, remeasure the crop box by
numeric sampling (never by eye), and update `capture_lcd.ps1`'s default
crop to match. Until then, treat the current crop as showing only the
upper portion of the panel.

Full rationale, token measurements, and how to add a tool: **docs/MCP_SERVERS.md**.

## Project Structure

The tree is split into hardware and software halves. See `docs/REPO_LAYOUT.md`
for the rationale and the move history.

```
hardware/   KiCad projects (mainBoard, ThermocoupleBoard, SaftyThermocoupleBoard,
            UnitTestFixture), shared lib/, datasheets/, simulation/, sourcing/
firmware/   KilnFW (ESP32-S3), SaftyFW (RP2040), CommonFW (shared link code)
tools/      PcTools (GUI + MCP for BOTH processors)
docs/       System-level documents spanning both halves
```

`mykicadMcp/` moved to `tools/mykicadMcp/` 2026-08-28 (its own dedicated pass:
server stopped, submodule remounted at the new `.gitmodules` path, every
`.claude/settings.json` allowlist entry walked one at a time rather than bulk-
edited). `pdfMcp/` moved to `tools/pdfMcp/` the same day — its own running
server process couldn't be moved out from under itself mid-session, so that
directory was copied rather than renamed, and `tools/pdfMcp/.venv`'s shims
still resolved to the root venv's interpreter. The leftover root
`mykicadMcp/` remnant was deleted 2026-09-06. As of 2026-09-06 six live
`pdf-mcp.exe`/`python.exe` processes still held both the root `pdfMcp/`
and `tools/pdfMcp/.venv`, so a self-referential replacement was built
side-by-side at `tools/pdfMcp/.venv_new` instead (`pip install
pdf-mcp==2.0.0`, shebangs confirmed pointing at `.venv_new`). **Pending
swap**: next time no `pdf-mcp.exe` is running, rename `tools/pdfMcp/.venv_new`
to `tools/pdfMcp/.venv` and delete the root `pdfMcp/` copy — `.mcp.json`
already points only at `tools/pdfMcp/.venv/Scripts/pdf-mcp.exe`, no config
change needed.

All main-board KiCad project files live under **hardware/mainBoard/** (paths below are relative to that
directory unless noted). A second, independent board — the 5-channel thermocouple daughterboard —
lives under **hardware/ThermocoupleBoard/** with its own `.kicad_pro`/`.kicad_pcb`/`.kicad_sch`; several of
its sub-sheets (e.g. `Thermocouple.kicad_sch`) are copies of the same circuit used in the main
board, so a fix found in one project's copy often applies to the other's too.

### Schematics (hardware/mainBoard/)
- **hardware/mainBoard/kiln.kicad_sch** — Main schematic file; top-level hierarchy
- **hardware/mainBoard/MainControler.kicad_sch** — ESP32-S3-DevKitC main processor
- **hardware/mainBoard/Thermocouple.kicad_sch** — MAX31856 thermocouple interface (5 channels)
- **hardware/mainBoard/Regulators.kicad_sch** — Input power conditioning/protection and 5V/3.3V LDO regulators
- **hardware/mainBoard/5V_Regulator.kicad_sch** — Dedicated 5V regulation block
- **hardware/mainBoard/CurrentSense.kicad_sch** — Current monitoring circuitry
- **hardware/mainBoard/SSD.kicad_sch** — Seven-segment display interface
- **hardware/mainBoard/SaftyProcessor.kicad_sch** — Safety monitoring subsystem

### Board & Layout
- **hardware/mainBoard/kiln.kicad_pcb** — PCB layout and routing
- **hardware/mainBoard/kiln.kicad_prl** — KiCad project settings and layers configuration

### Component Data
- **hardware/mainBoard/parts/SamacSys_Parts.pretty/** — Component footprints
- **hardware/mainBoard/parts/SamacSys_Parts.3dshapes/** — 3D models for visualization and export
- **hardware/mainBoard/fp-lib-table** — Footprint library table

### Python Tools
`tools/mykicadMcp/` is a separate git submodule (github.com/budarriola/mykicadMcp) holding the MCP server and its supporting tools:
- **tools/mykicadMcp/kicad_pcb_tool.py** — Lightweight parser for PCB and netlist files; does not require KiCad runtime
- **tools/mykicadMcp/kicad_mouser_tool.py** — Mouser Search API sourcing/stock/pricing lookups
- **tools/mykicadMcp/kicad_ipc_tool.py** — Live-KiCad tools via the IPC API (`kicad-python`); requires a running KiCad session
- **tools/mykicadMcp/kicad_mcp_server.py** — MCP server for the KiCad tools; HTTP on 8766 by default,
  `--transport stdio` still available
- **tools/mykicadMcp/kicad_facade.py** — search taxonomy (groups, keywords, synonyms) for the facade
- **tools/mykicadMcp/mcpkit_registry.py** — vendored copy of `tools/PcTools/src/mcpkit/registry.py`;
  edit the original and re-vendor, never this copy
- **tools/mykicadMcp/requirements-mcp.txt** — Python dependencies (requires `mcp>=1.0.0`)
- **tools/mykicadMcp/README.md** — Full setup guide and tool reference for the MCP server

### MCP Server Tools
The KiCad MCP server holds 86 tools in 12 groups: inspection/netlist, schematic data, Mouser
sourcing, audits, hierarchical sheet groups, layout/placement, PCB groups, net classes & buses,
nets, route templates, live IPC tools, and server control. Like the hardware servers it publishes
a search facade, not the whole set — start with `kicad_help()` or `kicad_find(query=...)` and
reach the rest through `kicad_call` / `kicad_batch`:

```
kicad_help()
kicad_find(query="is this capacitor rated high enough")
kicad_call(name="inspect_kicad_project", args={"project_path":"hardware/mainBoard/kiln.kicad_pro"})
```

`inspect_kicad_project` and `get_kicad_ipc_status` stay directly published. Everything else —
`list_kicad_components`, `get_kicad_component`, `get_kicad_component_connections`,
`list_kicad_nets`, `get_kicad_net` and the rest — is one `kicad_call` away. See
**tools/mykicadMcp/README.md** and `tools/mykicadMcp/docs/mcp-tools/` for the full reference, and
**tools/mykicadMcp/NETCLASS_PLAN.md** for the net-class design doc.

The net classes & buses group supports bus detection, net-class proposal/creation, trace-cost
scoring (with live deviation measurement), bus corridor-area measurement, capacitor voltage
auditing, critical-net classification, connector detection, and `pcb_settings.json` management.

There is **no autorouter**: it was removed (`mykicadMcp` commit "remove autorouter engine, keep
reference-copy/template tools"). What remains is the reference-copy family —
`copy_kicad_component_routing`, `apply_kicad_route_template`, `diff_kicad_route_template` — which
replicates routing already drawn on one instance of a repeated block onto its siblings.

## Development Setup

### Python MCP Server
1. Ensure Python 3 is installed and in `PATH`
2. Activate the virtual environment:
   ```powershell
   tools\mykicadMcp\.venv\Scripts\Activate.ps1
   ```
3. Install dependencies (if needed):
   ```powershell
   pip install -r tools\mykicadMcp\requirements-mcp.txt
   ```
4. Test the MCP server:
   ```powershell
   python tools\mykicadMcp\kicad_mcp_server.py
   ```

### Using MCP with Claude Code
- Configure MCP in your editor using the server path: `python tools\mykicadMcp\kicad_mcp_server.py`
- Example tools: "List the components on the PCB", "Show me component R1 and its connections", "Provide details for net /MainControler/CLK"

## Firmware gotchas

Symbolize a crash against the ELF that matches the RUNNING image, not
`build/KilnCtrl.elf` — that path is whatever was built most recently and
produces confident, wrong line numbers once the board is running an older
flash. Use `build/elf_archive/KilnCtrl-<hash>.elf`, matched by embedded build
timestamp against the board's `fw_build`.

`KILNCTL_TOUCH_CAL_SWAP_XY` is inert on this board's FT6336U capacitive
panel — it only feeds the legacy resistive NS2009 path; the live knob is the
`KILNCTL_TOUCH_CAP_*` family. See `firmware/KilnFW/docs/PROJECT_STATUS.md`
"Hardware present on this bench unit" for the full explanation.

Run `tools/run_all_checks.ps1` with `-ExecutionPolicy Bypass`. Without it the
script fails to load, and the Bash tool still reports exit 0 for the wrapper
— an unbypassed run looks like a pass when nothing ran. Separately, SaftyFW
host-test builds need a short worktree path (e.g. `C:\wt\...`); the default
`.claude/worktrees/...` path overflows the MSVC command line.

A 2026-09-04 panic (`safety_poll`, `IllegalInstruction`, `exc_addr 0x0`) ran
five hours unnoticed before `get_heap_status` was fixed to surface it (see
the flash/OTA section above). `exc_addr 0x0` was a red herring: the real
cause was a deliberate `abort()` from FreeRTOS's `configASSERT(pxQueue->uxItemSize
== 0)` in `queue.c`, reached via `thermo_owner.c:286`'s `xSemaphoreTake` —
the semaphore handle was non-null but its `StaticSemaphore_t` storage had
been corrupted. Root cause: `s_lvgl_task_stack` (`lvgl_port.c:988`, 8192 B)
sits close behind `thermo_owner.c`'s `s_slots[]` in `.bss`, and the
pre-`51e1ef5` reentrant `lv_obj_invalidate()`-inside-flush-callback path
(see that commit) could run the LVGL task stack deep enough to corrupt it.
Fixed by `51e1ef5` and flashed.

After splitting an oversized firmware file: grep `tools/`, `firmware/*/tools/`
and `tools/PcTools/tests/` for the old filename *and* any renamed identifiers,
then re-run every `check_*.ps1` and lint script — not just the build and host
tests. Nine of twelve 2026-09-04 splits broke a check/test silently this way
(one hardcoded-path guard was `if not path.is_file(): skipTest(...)`, so it
reported green with zero coverage), and separately, prefix-rename every symbol
widened from `static` even when the grep is clean — a same-named global can
collide silently with an unrelated `static` elsewhere. See `b9a5112` for a
worked example of both.

KilnFW's `boot_guard.h` RECOVERY MODE deliberately skips starting subsystems
(`profile_executor`, `autotune_engine`), so a task started unconditionally in
`main_boot_early.c` must gate on `boot_guard_is_recovery_mode()` before calling
into one. Today's accessors are prestart-hardened (they check their lock for
NULL), so the gate is defence in depth rather than the only thing standing
between you and a hang — but the board has been bricked into a permanent
recovery loop twice from this area (`e7b8efc`, and 2026-08-22).

Both times the real fault was a task stack, not the missing gate: a 700-byte
overflow corrupted the heap, and the pool walk then looped inside a critical
section until the interrupt watchdog fired. **Register every new task for
stack-margin reporting and measure it** — `check_stack_margin_registration.ps1`
enforces this. Equally, never hold a module lock across the producer calls a
policy tick makes (`dashboard_get_status()` alone does five MAX31856 SPI reads,
a 200 ms-capable queue wait and four interrupts-disabled heap walks): cache a
snapshot outside the lock instead (`7a8594d`).

**"Reset one side of a pair" bug class — no mechanical check, review by hand.**
Four confirmed instances so far, all silent, all cost real debugging time:
the PC-side `msg_index` restarting per benchproto connection while firmware's
dedup ring/ACK cache lives for the MCU's boot lifetime; SimFW's
`apply_reset()` zeroing `s_ring_next_seq` while `telemetry.c`'s consumer
cursor was only ever initialised at boot (stranded cursor, `drain()` returns
0 forever); `fault_sched.c` reseeding `s_seed` on `SET_SEED` while
`fault_engine_t.rng_state` stayed hardcoded to `0` from
`fault_engine_init(&s_engine, 0)` (every scenario's `seed:` was cosmetic);
and `safety_link_frames.c`'s Pico-reboot handling, where `trip_seq` restarts
at 0 on the Pico but the ESP's dedup `trip_last_seq` did not — fixed in the
2026-08-27 audit (see the comment at `safety_link_frames.c` around the
`boot_id_changed` block, which now explicitly clears `trip_last_seq` and
names this class).

What the four share structurally: two pieces of state, in different modules
(sometimes different processes/processors entirely), joined by a semantic
contract — equality, a monotonic derivation, a shared seed — that is never
expressed as a shared type or a single owning function. A reset/reinit event
is naturally written against only ONE side (the side whose lifecycle event it
is: a new connection, a sim reset, a reseed command, a reboot), and nothing
forces the other side's dependent state to be revisited. Both sides stay
internally consistent afterward — no crash, no assertion — so only the
*relationship* is broken, and it fails silently: health counters on the
stalled side often read perfectly clean (`evt_seq_gap_count: 0`) precisely
*because* nothing downstream of the break ever ran again. (One example fix,
`reset_client_evt_cursors()`, lived in the SimFW/`kilnsim` reference model
before that tree was deleted 2026-08-28 — no replacement reference model
exists today, so this class currently has to be spotted by inspection, not
by diffing against a maintained comparison implementation.)

This was evaluated for a mechanical `check_*` and rejected: the four
instances have no unifying syntactic shape (a per-connection PC/firmware
pair; a struct field skipped by one initialiser but not its sibling; an RNG
seed shadowed by an unrelated hardcoded one; a reboot-driven dedup counter)
and no naming convention connects the two sides of any of them — a regex or
AST rule general enough to catch all four would also flag the large majority
of ordinary, correctly one-sided resets in this codebase (most reset
functions have no paired counterpart at all — see `heater_output_reset()`
zeroing its window state alone, which is correct: nothing outside it derives
from that window). A *narrow* check pinned to one specific pair (e.g. a
mirror-drift check like `approach_rate_cap_mirror_drift_check.py` uses) is
honest only once a concrete pair is nailed down and stable; the current live
candidate for one (`thermal_guard.c`'s window state vs. the PWM-chopping
behaviour under investigation as of 2026-09-04) is mid-edit in another
session's pass, so writing a check against it now would either duplicate
that fix or break under it. **Standing practice instead:** whenever code
resets a counter, window, timestamp, or seed, ask explicitly "who else holds
a copy or a derived expectation of this?" before committing — the four
instances above are the checklist.

## Key Architecture Notes

### Hierarchical Schematic Design
The project uses a hierarchical schematic structure where sub-sheets (ADC, Thermocouple, etc.) are instantiated in the main schematic. This allows modular design and easier debugging of subsystems.

### Multi-Channel Thermocouple Interface
Five MAX31856 converters (U10–U14) provide independent thermocouple monitoring with built-in cold-junction compensation. Each is on a separate schematic page for clarity.

### Power Distribution
Three voltage rails:
- **12V input** (from external supply)
- **5V** (main logic and relay coils)
- **3.3V** (microcontroller I/O and sensors)

Input protection uses TVS diodes (SMAJ24CA) and current-limiting resistors. See hardware/mainBoard/Regulators.kicad_sch.

### Safety Processor
A separate safety processor monitors critical parameters and can disable the main controller if needed. Isolated communication via relay feedback circuits.

### Data Access Pattern
Use Python tools (kicad_pcb_tool.py) when you need to read PCB data, netlist connectivity, or component properties without opening the GUI. The parser reads `.kicad_pcb` and auto-generated netlist files directly.

## Common Tasks

### View the PCB or Schematic
Open with KiCad:
```powershell
kicad hardware\mainBoard\kiln.kicad_pcb
kicad hardware\mainBoard\kiln.kicad_sch
```

### Replicate Routing Across Repeated Blocks
There is no autorouter in this repo any more -- it was removed from `mykicadMcp`, and
`kicad_router_tool.py` no longer exists. Route by hand in KiCad, then replicate that work onto the
sibling instances of a repeated block (the five identical thermocouple channels, for example):

```
kicad_call(name="copy_kicad_component_routing", args={
  "project_path": "hardware/mainBoard/kiln.kicad_pro",
  "template_reference": "U10", "target_reference": "U11"})
kicad_call(name="diff_kicad_route_template", args={...})   # preview before applying
kicad_call(name="apply_kicad_route_template", args={...})
```

Always diff before applying. The same template pattern exists for placement
(`*_layout_template`), reference-designator positions (`*_property_position_template`) and
footprint flips (`*_flip_template`) -- `kicad_find(query="template")` lists them all.

### Query Component or Net Information
Use the MCP server tools or call Python directly:
```powershell
python tools\mykicadMcp\kicad_pcb_tool.py
```

### Update the BOM
No BOM CSV is checked into the repo currently. Regenerate one via KiCad's built-in BOM generator
(part numbers, values, footprints, Mouser links) or query components live through
`kicad_call(name="list_kicad_components", ...)`.

### Check Design Rule Violations
In KiCad: **Tools → Design Rule Checker** or press `Shift+I`. Refer to hardware/mainBoard/JLCPCB.kicad_dru for manufacturing rules if fabricating at JLCPCB.

## Notes for AI Assistants

- The schematic files (`.kicad_sch`) are large text-based files; use Python tools to query data rather than reading raw files.
- Component designators (R1, U1, etc.) in a regenerated BOM match those on the schematic and PCB.
- Footprints are organized in `hardware/mainBoard/parts/SamacSys_Parts.pretty/`; do not modify these directly unless sourcing new parts.
- The MCP server is useful for scripting or integration with other tools; most editing should happen in KiCad GUI.
