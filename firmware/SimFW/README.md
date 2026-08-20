# SimFW — Kiln Simulator / Unit-Test Fixture (RP2040)

A second Raspberry Pi Pico that plugs into the main board's connectors and
simulates the rest of the kiln: register-accurate MAX31856 thermocouple IC
emulation (3 main-side + 1 safety-side channels), three programmable 60 Hz CT
waveforms, relay-output sensing, a multi-zone thermal model, and a scheduled
fault-injection engine — all controlled over native USB CDC by a new MCP
server, CLI, and GUI in `tools/PcTools`.

**Status: build/task skeleton only.** The pico-sdk + FreeRTOS-Kernel (SMP)
CMake project and the ten single-owner task shells from
[`docs/PLAN.md`](docs/PLAN.md) section 4.1's task map exist under `src/` and
build clean under the real arm-none-eabi-gcc/pico-sdk toolchain — every task
is created and scheduled, but each is currently just an idling stub (no PIO
SPI emulation, no CT synthesis, no I2C/expander logic, no USB protocol).
Single-image, no A/B bootloader slots — unlike `SaftyFW`, this is a bench
tool, flashed over BOOTSEL or SWD.

Start at [`docs/PLAN.md`](docs/PLAN.md) — it owns the architecture, the
connection diagram, the fault catalog, the standard test library, and the
milestone order.

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

## Planned layout

```
docs/        PLAN.md (this project's owning plan), later PROTOCOL.md, HARDWARE.md
src/         main.c + tasks/ (single-owner FreeRTOS tasks) + sim/ (pure, host-testable)
scenarios/   standard test scenario files (YAML)
test/        host tests (MSVC/CMake, SaftyFW pattern)
tools/       check scripts
```
