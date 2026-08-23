# boot_probe -- dead-board control experiment

Diagnostic tool, not part of SimFW. Built after SimFW was flashed twice to a
bare Raspberry Pi Pico (original, not a Pico W) and came up completely dead
both times: LED dark, no USB enumeration at all.

**Question this answers:** is the bare board / toolchain / UF2 pipeline
good, independent of anything SimFW-specific (FreeRTOS, TinyUSB, the rest of
SimFW's own init)?

Two standalone pico-sdk programs, no FreeRTOS, no TinyUSB, no SimFW code:

- `blink_gpio/` -- toggles GPIO25 (onboard LED on a bare Pico) at ~1 Hz.
  Nothing else: no stdio, no USB, no interrupts beyond what `pico_stdlib`
  sets up itself.
- `blink_uart/` -- the same blink, plus a printed line over UART0
  (GP16 TX / GP17 RX, 115200 8N1 -- same pins/baud as SimFW's own console)
  each toggle, via `pico_enable_stdio_uart`. Validates the UART wiring and
  console path independently of SimFW.

Both are built with the exact same toolchain, `PICO_BOARD=pico`, and
`../elf2uf2.py` UF2-conversion path SimFW's own build uses, so a working
result on either .uf2 simultaneously validates the board, the toolchain,
and the UF2 generator.

## Reading the result

- Neither blinks, no USB, when SimFW also failed to come up at all: points
  away from SimFW's own code and toward the board, the flash procedure, or
  something environmental (power, crystal loading, a flash-write problem
  common to both images, etc).
- `blink_gpio` blinks (LED alone proves boot2 CRC/flash-read/reset-vector/
  clock init all work) but SimFW doesn't: narrows the problem to something
  SimFW adds on top -- FreeRTOS scheduler start, TinyUSB init, a driver, or
  its second (RAM-resident) LOAD segment copy.
- `blink_gpio` blinks but `blink_uart` doesn't print (LED still blinks):
  narrows to the UART wiring/pins/level or the stdio_uart init path, not
  the core boot/clock path.
- Both blink and print cleanly: board, toolchain, crystal, flash write
  path, and UART wiring are all good -- the bug is somewhere in SimFW's own
  source or the FreeRTOS/TinyUSB glue around it.

## Building

Same invocation shape as `../../CMakeLists.txt` (SimFW itself); this
machine has no `cmake`/`arm-none-eabi-gcc` on `PATH`, so call them by full
path:

```powershell
$env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"
& "C:\Program Files\CMake\bin\cmake.exe" -G Ninja -B build -DPICO_BOARD=pico `
  "-DPICO_TOOLCHAIN_PATH=C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin"
& "C:\Program Files\CMake\bin\cmake.exe" --build build
```

Then convert each `.elf` to a `.uf2` with `../elf2uf2.py` -- the SAME
converter SimFW's own build uses (no picotool involved either place, see
that script's own header comment for why):

```powershell
$objcopy = "C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-objcopy.exe"
python ../elf2uf2.py build build/blink_gpio.elf --objcopy $objcopy --verify
python ../elf2uf2.py build build/blink_uart.elf --objcopy $objcopy --verify
```

(`elf2uf2.py`'s `build` subcommand takes the `.elf` path and writes a
sibling `.bin`/`.uf2` next to it by default; `--verify` re-decodes the
produced `.uf2` and re-checks the boot2 CRC before reporting success.)

## Flashing

**This tool does not flash anything.** Hand the produced `.uf2` files to
whoever owns the hardware for this bench (BOOTSEL drag-and-drop, same as
any other RP2040 UF2).
