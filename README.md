# kilnCtl

> **WARNING: read the [Liability waiver and safety disclaimer](#liability-waiver-and-safety-disclaimer)
> before you build, flash, wire, or power anything from this repository.**
> This is an unfinished, experimental controller for an appliance that runs above
> 1000 °C on mains power. Even where it is believed to be stable, assume it **will**
> start a fire that burns your house down with everyone inside it.

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

## Running the regression suite

```
python tools/regression_suite.py
```

One command, every automated gate this repo has, one pass/fail summary and a
non-zero exit on any failure (usable in CI or a pre-commit hook). It runs, in
order: `tools/run_all_checks.ps1` (the mechanical `check_*.ps1` guards, plus
the JS/browser harnesses and the headless-Chrome responsive sweep, which are
themselves wired into that script's discovery); then, in parallel, the KilnFW
and SaftyFW off-target host-test suites and the `tools/PcTools` pytest suite
(its live-bench tests self-skip with no board attached); then,
in parallel, the two target firmware *compiles* (KilnFW over ESP-IDF, SaftyFW
over pico-sdk -- no flashing, ever). One gate failing does not stop the
others; the final summary lists every gate's pass/fail and points at the full
log for anything that failed. See the script's own header comment for why
each stage is grouped and parallelized the way it is.

## Opening this repository

On a fresh clone, run **[`tools/setup.ps1`](tools/setup.ps1)** first — it
discovers where ESP-IDF, clangd and OpenOCD live on this machine and generates
the config files that cannot be committed. Full detail:
[docs/SETUP.md](docs/SETUP.md).


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
firmware/   KilnFW (ESP32-S3), SaftyFW (RP2040), CommonFW (shared)
tools/      PcTools - GUI and MCP server for both processors
docs/       System-level documents that span hardware and firmware
```

`mykicadMcp/` and `pdfMcp/` both moved under `tools/` 2026-08-28.
The rationale for the split, and what broke during it, is
[docs/REPO_LAYOUT.md](docs/REPO_LAYOUT.md).

## The three boards

| Board | What it is |
|---|---|
| `hardware/mainBoard` | ESP32-S3 controller, SSR drivers, current sense, isolated link, safety RP2040 |
| `hardware/ThermocoupleBoard` | 3-channel MAX31856 daughterboard |
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

## Liability waiver and safety disclaimer

**This project is a work in progress. It is not finished, not certified, and not
safe.** Everything in this repository (schematics, PCB layouts, firmware for both
processors, PC tools, documentation, release images, and any advice in issues or
discussions) is an experimental, hobbyist development effort that is still
changing daily.

**Even where a part of it is believed to be stable, it is not.** Code that has
passed every test in this repository, every bench run, and every review can still
fail in the field. Treat every release, including those marked "stable", as a
pre-release prototype.

**Assume it will burn your house down with everyone inside.** A kiln is a
resistive heater that reaches well above 1000 °C, draws tens of amps from mains
power, and is often left unattended for many hours. A controller fault, a
firmware bug, a stuck or welded relay, a failed thermocouple, a wiring mistake, a
bad update, a power glitch, or a single misread value can leave the elements
energized with nothing to stop them. The likely result is a fire that destroys the
building and kills or seriously injures the people and animals inside it.
Electrocution, burns, toxic fumes, and damage to the kiln, its contents and
nearby property are also likely. Plan as if these outcomes will happen, not as if
they might.

**The safety processor does not make this safe.** The RP2040 safety processor, its
trip guards, the E-stop path, the pilot relay, and every other protective feature
described here are part of the same unfinished, unverified design. They have not
been independently evaluated and must not be relied on to prevent fire, injury or
death. This design does not replace a certified, independent over-temperature
cutoff, a correctly rated contactor, proper overcurrent protection, a
non-combustible installation, working smoke and fire detection, and a person
physically present and watching the kiln.

**No certification.** Nothing here has been tested, listed or approved by UL, CSA,
CE, TÜV, or any other safety or standards body, and nothing here complies with
any electrical, building or fire code. Installing it may break local law, void
your insurance, and void the kiln manufacturer's warranty. Mains wiring must be
done by a licensed electrician where your jurisdiction requires it.

**No warranty.** This project is provided "AS IS" and "AS AVAILABLE", without
warranty of any kind, express or implied. That includes, without limitation, any
warranty of merchantability, fitness for a particular purpose, safety, accuracy,
reliability, non-infringement, or that it will work at all.

**No liability.** To the fullest extent permitted by law, the authors,
contributors, copyright holders, and anyone who distributes this project are not
liable for any claim, damage, injury, death, loss of property, loss of data, loss
of income, or other loss of any kind, whether direct, indirect, incidental,
special, consequential, or punitive, and whether based on contract, tort
(including negligence), strict liability, or any other theory. This holds even if
they were told such loss was possible, and it applies to any use of this project,
any inability to use it, and any failure of it.

**You accept all risk.** By building, flashing, installing, operating, modifying,
or distributing anything from this repository, you agree that you do so entirely
at your own risk, that you are solely responsible for the safety of your
installation and of everyone and everything near it, and that you will indemnify
and hold harmless the authors and contributors against any claim arising from
your use. If you do not accept these terms, do not use this project.

**Not advice.** Nothing in this repository is professional, electrical,
engineering, or legal advice. If any part of this waiver cannot be enforced where
you live, the rest of it still applies.
