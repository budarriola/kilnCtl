# SimFW — Kiln Simulator / Unit-Test Fixture (RP2040)

A second Raspberry Pi Pico that plugs into the main board's connectors and
simulates the rest of the kiln: register-accurate MAX31856 thermocouple IC
emulation (3 main-side + 1 safety-side channels), three programmable 60 Hz CT
waveforms, relay-output sensing, a multi-zone thermal model, and a scheduled
fault-injection engine — all controlled over native USB CDC by a new MCP
server, CLI, and GUI in `tools/PcTools`.

**Status: software complete, hardware-gated.** The pico-sdk + FreeRTOS-Kernel
(SMP) CMake project and every single-owner task from
[`docs/PLAN.md`](docs/PLAN.md) section 4.1's task map are fully implemented
under `src/` — PIO MAX31856 SPI slave emulation, CT waveform synthesis, I2C/
expander/relay/E-stop/DUT-power drivers, and the sim-engine/fault-scheduler
orchestration are all real code, not stubs. It builds clean under the real
arm-none-eabi-gcc/pico-sdk toolchain (`-Wall -Wextra -Werror`), and the pure
`src/sim/` modules (thermal model, MAX31856 register machine, sine synth,
fault engine) pass their MSVC/CMake host-test suite. The 17-scenario standard
test library exists as real YAML in `scenarios/`, and a fresh `kilnsim` PC
toolset (CLI/GUI/MCP server) lives in `tools/PcTools/src/kilnsim/`, built
against `benchproto` — a new hardened protocol library extracted into
`firmware/CommonFW` from `UnitTestFw`'s prototype (see
[`docs/PLAN.md`](docs/PLAN.md) section 12).

**What that status does *not* mean: nothing here has been hardware-verified.**
No fixture hardware has ever been built or connected to a bench ESP32/Pico.
The single biggest unproven risk is still the PIO SPI slave's real-world
timing — no Saleae capture exists proving 5 MHz mode-1 transactions against a
real master (`docs/PLAN.md` section 10, milestone M-A). A real SPI-mode bug
*was* found and fixed during implementation: the PIO engine originally
sampled MOSI on the wrong clock edge (mode 0 behavior under a mode-1 label);
it is now corrected to match the MAX31856 datasheet and both real masters'
drivers — see `docs/HARDWARE.md` and the PLAN.md status header for detail.
CT amplitude calibration is also still an identity placeholder pending real
hardware to calibrate against. None of the 19 scenarios (grown from 17) has
ever run against a real `KilnFW`+`SaftyFW` pair.

**A fourth, software-only capability exists now and it already found a real
bug — in `SaftyFW`, not in `SimFW`.** `tools/virtual_simfw/` compiles this
project's own `src/sim/*.c` unmodified and serves the real `benchproto` wire
protocol over TCP, so a complete scenario runs end-to-end with zero RP2040
attached (see `tools/virtual_simfw/README.md`). `tools/virtual_dut/` goes
further, compiling `SaftyFW`'s real `safety_guards.c`/`relay_grace.c`
unmodified and ticking them against `virtual_simfw`'s live data, so guard
verdicts genuinely PASS/FAIL instead of skipping (see
`tools/virtual_dut/README.md`). **Neither is hardware verification** — see
each tool's own README — but running real guard code against a simulated
kiln established that in today's shipping `SaftyFW`, only 4 of 13 guards
(S5, S6b, S7, S12) can structurally fire; the rest are blocked on inputs
`safety_core.c` never populates yet (Phase 6/7 work). See `docs/PLAN.md`'s
status header and `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`'s new
reachability section for the full detail.

Single-image, no A/B bootloader slots — unlike `SaftyFW`, this is a bench
tool, flashed over BOOTSEL or SWD.

Start at [`docs/PLAN.md`](docs/PLAN.md) — it owns the architecture, the
connection diagram, the fault catalog, the standard test library, and the
honest milestone-by-milestone status (section 10). See also
[`docs/HARDWARE.md`](docs/HARDWARE.md) for the reconciled pin map and
bring-up checklist, and [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the full
USB command reference.

## Build

Same toolchain, pico-sdk and FreeRTOS-Kernel checkout `SaftyFW` uses (see
`CMakeLists.txt`'s header comment for the full requirements). PowerShell,
toolchain via winget, SDK/kernel cloned to `C:\pico-tools`:

```powershell
$env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"
$env:FREERTOS_KERNEL_PATH = "C:\pico-tools\FreeRTOS-Kernel"
cmake -G Ninja -B build -DPICO_BOARD=pico `
  "-DPICO_TOOLCHAIN_PATH=C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin"
cmake --build build
```

Produces `build/SimFW.elf`, clean under `-Wall -Wextra -Werror`. No `.uf2`
yet (picotool is not set up in this environment, same gap `SaftyFW`'s
CMakeLists.txt documents) — flash the `.elf` over SWD/OpenOCD until that is
resolved.

Not to be confused with:

- `firmware/UnitTestFw` — the ESP32-S3 instrument bench (DAC / AD9833 / OLED /
  PCF8575). A first attempt, **slated for deletion** once its wire protocol
  is lifted into `CommonFW` and SimFW's link is proven — see
  [`docs/PLAN.md`](docs/PLAN.md) section 12.
- `firmware/SaftyFW` — the RP2040 **on** the main board (A1, safety
  processor). SimFW is a different Pico that lives on the bench and talks *to*
  the main board from outside.

## Repo layout

```
docs/        PLAN.md (this project's owning plan), HARDWARE.md (pin map,
             connector mating, bring-up checklist — all provisional, see its
             own header), PROTOCOL.md (USB command reference)
src/         main.c + tasks/ (single-owner FreeRTOS tasks, all implemented) +
             sim/ (pure, host-testable thermal model / MAX31856 regs / sine
             synth / fault engine) + drivers/ (PIO SPI slave, CT PWM, MCP23017)
scenarios/   19 standard test scenario files (YAML) — grown from 17; see
             GUARD_TEST_MATRIX.md §5 for the guard cross-reference
test/        host tests (MSVC/CMake, SaftyFW pattern)
tools/       spi_test_master/ (standalone SPI reference-master bench firmware
             + host soak runner, built to make M-A's proof achievable);
             virtual_simfw/ (host build of this project's own sim code,
             speaking real benchproto over TCP — no RP2040 needed);
             virtual_dut/ (host build of SaftyFW's real guard code, ticked
             against virtual_simfw — a software-only DUT cross-check, not
             hardware verification); check_single_owner.ps1/
             check_sim_purity.ps1/check_scenarios.py + run_checks.ps1 (CI
             checks, not yet wired into any pipeline)
```
