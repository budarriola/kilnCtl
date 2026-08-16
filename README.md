# kilnCtl

A kiln controller: three KiCad boards, two independent firmwares, and the PC
tooling that drives both.

Heating is commanded by an ESP32-S3 through solid-state relays. A separate
RP2040 watches the same kiln and can cut power through a mechanical pilot relay
that is wired in series with the contactor coil. The two processors talk over an
opto-isolated UART, and the main processor is not allowed to heat unless the
safety processor is answering.

## Start here

**[ROADMAP.md](ROADMAP.md)** is the top-level plan across both processors. It
holds the milestone order and the cross-processor dependencies, and links to the
plan that owns each area. If you do not already know which document covers what
you are about to do, start there.

## Opening this repository

In VS Code, open **`kilnCtl.code-workspace`** rather than the `kilnCtl` folder.
The ESP-IDF extension treats a workspace folder as one project and needs a
`CMakeLists.txt` in it; the repository root does not have one. The workspace file
lists each firmware, `tools/PcTools` and `hardware` as folders in their own right,
and carries the shared tasks.

Shell commands in this repository are written to run from the root, so they carry
a path argument: `idf.py -C firmware/KilnFW build`,
`uv run --project tools/PcTools kilnctrl-gui`.

## Layout

```
hardware/   KiCad projects, shared library, datasheets, LTspice, sourcing
firmware/   KilnFW (ESP32-S3), SaftyFW (RP2040), CommonFW (shared), UnitTestFw
tools/      PcTools - GUI and MCP server for both processors
docs/       System-level documents that span hardware and firmware
```

`mykicadMcp/` and `pdfMcp/` are still at the root pending a move under `tools/`.
The rationale for the split, and what broke during it, is
[docs/REPO_LAYOUT.md](docs/REPO_LAYOUT.md).

## The three boards

| Board | What it is |
|---|---|
| `hardware/mainBoard` | ESP32-S3 controller, SSR drivers, current sense, isolated link, safety RP2040 |
| `hardware/ThermocoupleBoard` | 5-channel MAX31856 daughterboard |
| `hardware/SaftyThermocoupleBoard` | The safety processor's own thermocouple front end |

`Thermocouple.kicad_sch` appears in more than one project as a copy of the same
circuit, so a fix found in one usually applies to the others.

## The two firmwares

- **`firmware/KilnFW`** — ESP-IDF. Thermocouples, PID, profile execution, Wi-Fi
  provisioning, web UI. Partly built, partly verified on hardware; the honest
  ledger is [firmware/KilnFW/docs/PROJECT_STATUS.md](firmware/KilnFW/docs/PROJECT_STATUS.md).
- **`firmware/SaftyFW`** — pico-sdk and FreeRTOS SMP. Independent overheat and
  fault detection, owns the pilot relay. Planned in detail, **not yet built**.
- **`firmware/CommonFW`** — the link contract and its codecs, implemented once
  and linked by both. Also not yet built.

The safety processor's job is to catch faults, not to be sensitive: every trip
has to clear both a magnitude that correct operation cannot reach and a duration
a transient cannot sustain. That reasoning is
[firmware/SaftyFW/docs/SAFETY_MODEL.md](firmware/SaftyFW/docs/SAFETY_MODEL.md).

## Tooling

`tools/PcTools` is one GUI and one MCP server for both processors — serial to
the ESP, the isolated UART or SWD to the Pico, per-processor consoles and log
files. See [tools/PcTools/README.md](tools/PcTools/README.md) for what exists and
[tools/PcTools/TODO.md](tools/PcTools/TODO.md) for what is planned.

`mykicadMcp` is a separate submodule exposing KiCad inspection, sourcing and
autorouting over MCP.

## Bench setup

Programming, debug and console for the RP2040 all go through a Raspberry Pi
Debug Probe: SWD to the DEBUG pads, and the probe's UART bridge on GP16/GP17.
The Pico's own USB is deliberately not used. Wiring is
[firmware/SaftyFW/docs/HARDWARE.md](firmware/SaftyFW/docs/HARDWARE.md) §7b.
