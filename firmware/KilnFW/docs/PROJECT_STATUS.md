# Project Status

What is actually done, what is verified vs. merely built, and what is left,
as of this writing. This file is the one to update whenever something here
changes — check dates and specifics against the code before trusting them,
the way you would any status doc.

Settled architecture and design decisions have moved to `docs/ARCHITECTURE.md`
(task/ownership structure) and `docs/ARCHITECTURE_DECISIONS.md` (everything
else); recurring bring-up lessons live in `docs/BRINGUP_HAZARDS.md`. This file
tracks what is current and what remains open, not the history of how it got
built.

## Scope

KilnCtrl retargets a unit-test-fixture firmware/PC-tools pair onto the real
kilnCtl main board: an ESP32-S3 driving three MAX31856 thermocouple channels
(via the daughterboard on J6), an SX1509 I/O expander (relays, digital I/O,
display control), an ILI9488 TFT on J2, and an opto-isolated link to an
RP2040 safety processor (`firmware/SaftyFW`).

## Current build/hardware configuration

- **Build**: `idf.py -C firmware/KilnFW build` from a full clean, ESP-IDF
  v6.0.2, target `esp32s3`, succeeds under this project's
  `-Wall -Wextra -Werror`.
- **Flash/PSRAM (resolved 2026-08-17)**: the bench board is an **N16R8** — 16
  MB flash, 8 MB PSRAM (confirmed against the LonelyBinary product page,
  variant 43784065712285). The BOM (`N8R8`) and the footprint library's 3D
  model (`N8R2`) are both stale and still need fixing at the source —
  separate, unstarted work. `partitions.csv` was reworked (`factory` moved to
  `0x810000`, capacity `0x300000`) as part of the OTA two-slot layout; see
  `ROADMAP.md` M8.
- **PSRAM: on** (`CONFIG_SPIRAM=y`, octal mode at 40 MHz,
  `CONFIG_SPIRAM_USE_MALLOC=y`). In use by LVGL's draw buffers and heap, one
  UI page's canvas, and several task stacks. See `ARCHITECTURE_DECISIONS.md`
  and `BRINGUP_HAZARDS.md` for why it was off, why it was turned back on, and
  the two internal-SRAM crises that followed.
- **PC-link UART baud: 921600** (was 115200), changed consistently across
  firmware Kconfig/sdkconfig and `pc_tools/src/kilnctrl/protocol.py`. The
  DISPLAY blit path is still stop-and-wait (one ACK round-trip per 128-byte
  frame), so per-frame round-trip latency, not bit rate, is likely still the
  dominant cost for a full blit; no RTS/CTS flow control is wired on this
  board.
- **Python**: `python -c "import kilnctrl"` succeeds. `selfcheck.py` had 16
  pre-existing failing checks as of 2026-08-16 (confirmed unrelated to that
  session's repo reorganisation — they failed identically before it); recheck
  before trusting the current pass count, `tools/PcTools/**` is out of scope
  for this pass.
- **Board wiring traced from the schematics, not assumed** — `docs/HARDWARE.md`.
  Real discrepancies found and fixed: the safety link's pin assignment was
  swapped in `KilnFW`'s Kconfig defaults relative to the board
  (`DataToSafty`/GPIO5 is the ESP's TX, `DataFromSafty`/GPIO4 is its RX —
  measured on the bench 2026-08-23 after two schematic traces got it wrong in
  opposite directions; see `docs/SAFETY_LINK.md` "Trap 1"), both
  isolated data directions are logically inverted by the optocouplers (fixed
  via `uart_set_line_inverse`; the RP2040 side needs no inversion of its own —
  see `docs/SAFETY_LINK.md`), and the isolated `Fault` line is an ESP
  **output**, not an input.

## Hardware present on this bench unit (2026-08-20)

- **MAX31856 thermocouple ICs: fitted.** Three channels populated via the J6
  daughterboard; channels 0/1/2 read plausible room temperature with tracking
  cold junctions and no faults; `CR1` reads back the configured value where it
  previously read all-zero before the parts existed.
- **Display: attached and working** — the on-device LVGL UI runs against real
  hardware.
- **Relay/SX1509 expander: not confirmed attached** as of the last session
  that checked.
- **RP2040 safety processor: link proven end to end (2026-08-23).**
  `SaftyFW` now runs and the isolated link has carried real traffic for the
  first time — `safety_get_status()` returns live telemetry (link up, safety
  thermocouple invalid with no sensor fitted, all three currents 0.00 A, a few
  hundred milliseconds old). Getting there needed the UART baud rate dropped
  to 9600 — the TCMT1109 optocouplers cannot switch fast enough for 115200 —
  on top of the pin/inversion fixes above; see `docs/SAFETY_LINK.md`
  "Transport" and `firmware/SaftyFW/docs/HARDWARE.md` §1 for the measurement.
- **PC↔ESP UART link (command/telemetry): found dead 2026-08-19.** A
  different fault from the Pi↔ESP safety link (`ROADMAP.md` M0/M1) — this is
  the USB-serial link `pc_tools`/MCP use. With the board present, powered, and
  answering normally over JTAG/OpenOCD, every UART command timed out even
  after reconnect and a JTAG reset. **Read every "flashed"/"verified" claim
  anywhere in this repo's history as build-verified plus, at most, a
  JTAG-observed liveness check, not a UART-confirmed round-trip, until this is
  fixed** — this is the single fact most likely to make an older status claim
  read as more verified than it actually was.

## What has run on real silicon vs. what has not

- **Verified live on the board**: Wi-Fi provisioning (AP fallback, station
  join, mode persistence across reflash), the dashboard's hardware-absent stub
  responses on all five page routes plus zones/rules/profiles JSON APIs,
  thermocouple channel reads (now that the ICs are fitted), a PID/thermal-guard
  slice (guard 6 tripping on sensor-invalid, latching the executor, clearing
  correctly), and the single-writer ownership tasks' boot behavior (see
  `ARCHITECTURE.md` §4 for the full per-task verification table — it is the
  authoritative source for what's build- vs. boot-smoke- vs.
  hardware-verified, current as of 2026-08-19).
- **Never run on hardware**: the RP2040 safety processor's own guard trips
  (guards evaluate, but a live bench trip test has not been run); the safety
  processor influencing main-board relays (isolated link is ESP→Pico only
  today — see below); live thermocouple-fault gating outside of an active
  profile run (general
  case, only the profile-running case above closed); any
  on-target automated test suite (only host-side `App/test/` and PC-side
  `selfcheck.py` exist); PID autotune of either method (aborts on guard 6 with
  no thermocouples attached at the time it was last tried, before the ICs
  were fitted — worth re-attempting); relay-feedback identification's cross-
  gain matrix against a real coupled kiln (guard 8's threshold and the RGA
  display both wait on this); concurrent multi-zone execution against real
  relays; the reboot breadcrumb triggering any actual recovery action (it is
  recorded and displayed, nothing reads it back into a run).

## Explicitly NOT done — do not assume otherwise

- **The RP2040 safety-processor firmware exists and the link now works.**
  `firmware/SaftyFW` runs on real hardware and the isolated link has carried
  real telemetry end to end (2026-08-23), after the UART baud rate was
  corrected to 9600 — see `docs/SAFETY_LINK.md`. With no Pico attached the
  link still degrades to "no peer" correctly (fail-safe fault assertion, not a
  hang).
- **Live thermocouple faults gate relays only while a profile is actively
  running that zone.** `thermal_guard.c` guard 6 trips on
  `spi_failed`/`NaN`/`THERMO_FAULT_OPEN`/`OVUV`/`TCRANGE` after 3 consecutive
  bad reads in that case only. A channel faulting with no profile running
  that zone — including a direct UART/MCP read or the dashboard view outside
  a running profile — is still only reported, never acted on. See
  `docs/SAFETY_MODEL.md` for the precise scope.
- **The safety processor cannot influence the main board's relays.** An
  E-stop or fault reported back from the Pico firmware over the (now-working)
  telemetry link is visible in `SAFETY_CMD_GET_STATUS` and drops nothing on
  the main board directly. Isolated-fault-line traffic (GPIO6) is
  one-directional, ESP → Pico only; the Pico has no hardware path back.
- **Raw expander debug commands bypass the relay-on gate for direction
  changes.** `IO_CMD_SX_SET_DIR` unconditionally refuses flipping a relay pin
  to an input; `SX_WRITE_REG` is gated per-pin against
  `relay_authority_on_blocked()`, but the other 12 expander pins stay
  reachable for debug by design. See `docs/SAFETY_MODEL.md`.
- **Display connector pin identity is unresolved.** `docs/HARDWARE.md` and
  `docs/ILI9488.md` document a real disagreement between this board's
  schematic net names and every found BIGTREETECH TFT35 SPI pinout over which
  J2 pin is D/C vs. touch IRQ, and whether SDA/SCL are swapped. Handled with a
  menuconfig switch (`KILNCTL_DISPLAY_SWAP_DC_RESET`) defaulting to the
  module's reading, not resolved. The module's touch controller is more
  likely an NS2009 (I2C) than the commonly-claimed XPT2046 (SPI) — moot for
  this firmware, which drives the panel only and does not support touch.
- **`SX_SET_INT_MASK`'s sense field cannot express per-pin edge selection.**
  The frozen wire contract specifies 16 bits (2 per pin *pair*); the part
  wants 32 (2 per pin). `SX_WRITE_REG` against 0x14–0x17 is the workaround;
  fixing it properly needs a wire-protocol v3 field, not a firmware change.
- **No refusal signal on the wire.** A safety-refused relay command produces
  no reply, success or failure — see `docs/UART_PROTOCOL.md`. The GUI's relay
  state is read back from live auto-report so a refusal is visible within one
  report period, but there is no explicit "refused, here's why" message.
- **No on-target automated test suite.** A host-side one exists
  (`App/test/`, covering the pure control modules against a coupled
  multi-zone sim with injected faults) plus `selfcheck.py` on the PC side and
  the ESP-IDF build itself. Nothing runs the test suite on the ESP32-S3
  itself; a guard passing in the sim is shown *logically* right, not that it
  fires against real silicon.
- **Phase 6 (system-mode command gate) is design-only, not built.** See
  `ARCHITECTURE.md` §3 for the sketch and open questions.
- **`ui_page_network.c`'s three ad-hoc Wi-Fi worker tasks are not migrated**
  onto `wifi_prov_owner`'s queue — see `ARCHITECTURE.md` §3.

## Suggested order for what's next

1. Fix the PC-link UART before trusting any further "verified" claim that
   depends on it — almost everything downstream of it is currently
   unobservable.
2. Now that the MAX31856 ICs are fitted, re-attempt PID autotune (both step
   and relay-feedback methods) and the on-target guard walk
   (`docs/GUARD_TEST_MATRIX.md`'s "reasoned about" rows) — both previously
   blocked purely on missing sensor hardware.
3. Flash the board and validate the boot sequence, SPI bus sharing, and the
   relay-on gate end to end (command a relay on, unplug USB, watch it drop
   and the fault line assert) before trusting any of this near an actual
   kiln.
4. Decide the open design questions in `docs/SAFETY_MODEL.md` for the
   general-case live thermocouple-fault gating, then implement it — the gap
   most worth closing before unattended operation.
5. Write the RP2040 safety-processor firmware against the contract in
   `App/drivers/safety_link.h` (`docs/SAFETY_LINK.md`, no inversion needed on
   that side).
6. Decide whether a safety-processor-reported fault/E-stop should drop the
   main board's relays, and if so, add the isolated-link path for it.
7. Confirm the J2 pin identity against the physical connector and drop the
   now-unneeded `KILNCTL_DISPLAY_SWAP_DC_RESET` ambiguity from the docs.

## Session log

Older dated entries below record what changed and what was verified in each
pass; keep adding new dated entries here rather than folding them into the
sections above, but move anything with lasting design value out to
`ARCHITECTURE.md`/`ARCHITECTURE_DECISIONS.md`/`BRINGUP_HAZARDS.md` once it
lands, rather than letting it grow indefinitely.

### 2026-08-20 — MAX31856 thermocouple ICs fitted

Supersedes every earlier note in this repo saying the ICs are physically not
connected. Three MAX31856 ICs and their thermocouples are now populated on
this bench board (via the J6 daughterboard). Verified live: channels 0/1/2
read ~31-32 °C, cold junctions tracking ~0.3 °C below, no faults; `CR1` reads
back the configured value where it previously read `0x00`. `thermo_owner.c`'s
bench verification is unblocked by this. Unchanged: the safety processor
(RP2040) still has no ICs fitted — see `ROADMAP.md` M3 and
`../SaftyFW/docs/THERMOCOUPLE.md`.

Also this session: the mDNS host was renamed `kiln.local` → `kilnctl.local`,
and a CMSIS-DAP probe was confirmed wired to the safety processor's SWD,
giving an agent-usable debug/program path there too — see
`../SaftyFW/README.md`.

### 2026-08-19 — single-writer ownership tasks landed; PC-link UART found dead

Four phases of the ownership/single-writer architecture landed
(`firmware/KilnFW/TODO.md` section 10.14): `kiln_io_owner` (relay/SX1509),
`thermo_owner` (MAX31856), `wifi_prov_owner` (Wi-Fi state, including the
driver's own event callbacks), and the `kilnlink_announce` codec migration for
`safety_link.c`'s `ANNOUNCE_VERSION`. Phase 3 (`profile_executor`) was
reviewed and deliberately skipped — already correctly mutex-guarded, no
lost-update bug to fix. Full design, task table, and per-phase
hardware-verification status: `docs/ARCHITECTURE.md`.

**New bench fact discovered this session, changing how to read every earlier
"verified" claim in this file: the PC↔ESP command UART link was found dead.**
With the board present, powered, and answering fine over JTAG/OpenOCD, every
UART command timed out even after reconnect and a JTAG reset. This blocked
confirming any of the four ownership phases beyond build-verified plus a
boot-liveness check (Phase 4/Wi-Fi got the furthest: flashed, boots, stays
running, but no actual Wi-Fi behavior was observed). See the current-state
section above.

### 2026-08-16 and earlier — PID/thermal-guard/autotune build-out

The bulk of the PID/thermal-guard/autotune control stack (`pid.c`,
`thermal_guard.c`, `heater_output.c`, `profile_executor.c`,
`autotune_engine.c`, the coupled multi-zone simulator, guard 8, RGA,
feedforward, relay-feedback autotune, config-reload-while-running, relay
contact-cycle accounting, the reboot breadcrumb, and per-zone relay timing)
was built and host-tested across several sessions between 2026-08-11 and
2026-08-16. The settled design of each is now in
`docs/ARCHITECTURE_DECISIONS.md` (PID / thermal guard / autotune section);
real bugs found while building it (the guard 3 rate miscalculation, the
`fault_guard` numbering mismatch, several NVS/heap/task issues) are in
`docs/BRINGUP_HAZARDS.md`. **None of it has been exercised against real
thermocouple/relay hardware** — every on-target run during this period
aborted on guard 6 (no sensors attached), which changed 2026-08-20 (see
above); autotune specifically has not yet been re-attempted since.

Also from this period, still true and not duplicated elsewhere: a
bootloader-wipe incident from a manual `flash erase_sector` recovered only
via a full USB power cycle (a soft/JTAG reset does not clear a stuck
SPI-flash write-protect state) — this is why `flash_firmware()` and
`kill_openocd_sessions()` exist as MCP tools and why a raw `erase_sector`
sequence must never be improvised again.

### 2026-08-13 — Wi-Fi mode-switch bug and NVS version-check ordering

`esp_wifi_set_config(AP)` failed with `ESP_ERR_WIFI_MODE` on every boot — see
`docs/BRINGUP_HAZARDS.md`. Also found and fixed: `zones_http.c` checked blob
size before version, making its migration path dead code for a struct that
grows (fixed there; the same latent bug is still unfixed and unflagged-as-
fixed in `rules_http.c`/`relay_cycles.c`/`profiles_http.c` — nothing in that
session's diff grew those three structs, so it wasn't forced, but it remains
open in those three files).
