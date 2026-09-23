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
display control), an ILI9488 TFT on J2, and an isolated link (an ADuM1201
digital isolator as of 2026-08-25; previously a TCMT1109 optocoupler pair) to
an RP2040 safety processor (`firmware/SaftyFW`).

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
  firmware Kconfig/sdkconfig and `tools/PcTools/src/kilnctrl/protocol.py`. The
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
  opposite directions; see `docs/SAFETY_LINK.md` "Trap 1"), and the isolated
  `Fault` line is an ESP **output**, not an input. (Historical: both isolated
  data directions used to be logically inverted by the now-removed TCMT1109
  optocoupler pair, fixed via `uart_set_line_inverse`/`gpio_set_outover`; as
  of 2026-08-25 that pair was replaced by U6, an ADuM1201 digital isolator,
  which does not invert, and both firmwares' inversion has been removed to
  match — see `docs/SAFETY_LINK.md`.)

## Hardware present on this bench unit (2026-08-20)

- **Local sdkconfig overrides (2026-09-04, doc audit).** `firmware/KilnFW/sdkconfig`
  is gitignored, so a clean clone builds only the Kconfig `default`s below, not
  whatever this bench's own `sdkconfig` currently has set. Two deliberate
  differences on this bench, confirmed against the code that consumes each
  symbol so a future session doesn't "fix" them back to matching the repo
  default:
  - `CONFIG_KILNCTL_ENABLE_GPIO_PROBE=y` here vs. Kconfig default `n`. This is
    the raw GPIO-probe-over-UART debug facility (`gpio_probe.c`,
    `UART_TASK_ID_GPIO_PROBE`) — deny-listed against SPI/I2C/SX1509/display/
    safety-link pins and refused while a profile is running, but still a
    debug-only surface with no role in normal operation. Left on locally for
    active bring-up/debug work on this unit; the repo default stays `n` since
    nothing in the design needs it enabled by default.
  - `CONFIG_KILNCTL_TOUCH_CAL_SWAP_XY=y` here vs. Kconfig default `n`. This
    knob only feeds the legacy NS2009 (resistive) uncalibrated-touch path in
    `lvgl_port.c`/`touch_dev.c`, gated by `!self_calibrating`. This bench's
    panel is the FT6336U capacitive touch controller, which is
    `self_calibrating = true` (see the "touch self-calibrating -- touch_cal_store's
    per-board fit does not apply" boot log line at the end of
    `lvgl_port_start()`, `lvgl_port.c`), so `TOUCH_CAL_SWAP_XY` is never
    read at all on this hardware — it is inert, not a real per-board
    calibration value. The FT6336U's own axis orientation is controlled by
    the separate, panel-conditional `KILNCTL_TOUCH_CAP_SWAP_XY` /
    `_CAP_INVERT_X` / `_CAP_INVERT_Y` family added in `f028e2f`, which already
    defaults correctly for `KILNCTL_DISPLAY_PANEL_ST7796`. No Kconfig change
    needed for `CAL_SWAP_XY`; the local `y` is stale/harmless, most likely a
    leftover from before this board moved to the capacitive panel.

    Consequence for the UI (2026-09-18): because this panel self-calibrates,
    **touch calibration is not offered on this unit at all** — the LCD's
    Configuration hub shows no "Touch Calibration" cell, the forced
    first-boot calibration flow never triggers, and the web Diagnostics page
    reports touch calibration as "not required (self-calibrating
    controller)" rather than as uncalibrated. All of those read one shared
    predicate, `touch_dev_cal_support()` (`touch_dev.h`), which also
    distinguishes a FAILED touch bring-up ("no touch controller detected")
    from this deliberate not-needed case. See `DISPLAY_ST7796_PLAN.md` §7.
- **MAX31856 thermocouple ICs: fitted.** Three channels populated via the J6
  daughterboard; channels 0/1/2 read plausible room temperature with tracking
  cold junctions and no faults; `CR1` reads back the configured value where it
  previously read all-zero before the parts existed.
- **Display: attached and working** — the on-device LVGL UI runs against real
  hardware.
- **Relay/SX1509 expander: confirmed attached (2026-09-04, doc audit).**
  `io_read()` over the live PC-link returns a plausible register state
  (`data 0x78D0 dir 0x3FD0`, DRDY0/1/2 all high) — the part is present and
  answering, not merely wired per schematic.
- **RP2040 safety processor: link proven end to end (2026-08-23).**
  `SaftyFW` now runs and the isolated link has carried real traffic for the
  first time — `safety_get_status()` returns live telemetry (link up, safety
  thermocouple invalid with no sensor fitted, all three currents 0.00 A, a few
  hundred milliseconds old). Getting there needed the UART baud rate dropped
  to 9600 at the time — a ceiling the TCMT1109 optocouplers then fitted could
  not switch past at 115200 — on top of the pin/inversion fixes above; see
  `docs/SAFETY_LINK.md` "Transport" and `firmware/SaftyFW/docs/HARDWARE.md` §1
  for that historical measurement. That optocoupler pair was replaced with a
  digital isolator on 2026-08-25 and the baud sweep is now complete — see
  `CONFIG_KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig` for the
  committed value, not the 9600 figure above.
  **Update (2026-09-04, doc audit): the safety thermocouple IC is now fitted.**
  `safety_get_status()` over the live link reads "safety thermocouple valid |
  29.75 C (CJ 29.84 C)" — plausible room temperature, no fault — so the S5
  paragraph below (guard tripping roughly a minute after boot on an absent
  sensor) is stale and no longer describes this bench unit; it is kept for
  history since S5's absent-sensor behavior itself is unchanged and still
  correct when a sensor genuinely isn't present. Before
  `KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT` defaulted off (see `SAFETY_MODEL.md`
  §3), S6a fired within five seconds and latched, which stopped guard
  evaluation before S5 ever got a chance to run; with S6a gone by default, S5
  becoming the reported trip on this board is the expected outcome.
- **PC↔ESP UART link (command/telemetry): found dead 2026-08-19, working
  again as of 2026-09-04 (doc audit).** A different fault from the Pi↔ESP
  safety link (`ROADMAP.md` M0/M1) — this is the USB-serial link
  `pc_tools`/MCP use. At the time, with the board present, powered, and
  answering normally over JTAG/OpenOCD, every UART command timed out even
  after reconnect and a JTAG reset. Confirmed live now via `link_status()`
  (`connected: True`, port COM14, 921600 baud) plus a round trip
  (`io_read()`, `safety_get_status()`) — this specific link is UART-confirmed
  again, not just build/JTAG-verified. No commit in this repo's history was
  found that root-caused the original dead-link report, so treat any
  "verified" claim dated between 2026-08-19 and whenever this was actually
  fixed with the same caution the paragraph above used to recommend for
  everything after it.

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

**2026-09-22 note:** the items below were written when `SaftyFW` was newly
bring-up-verified; it is now a mature firmware (guards S3–S9, CT sense,
dual-slot OTA) with commissioning in progress under ROADMAP.md M18 — see that
section for the live, current list of what remains open there. The bullets
below are kept as-is (historical) except where a bullet has since been closed
outright, and the "Suggested order" list further down is superseded by M18's
own remaining-work list.

- **Historical.** The RP2040 safety-processor firmware exists and the link now works.
  `firmware/SaftyFW` runs on real hardware and the isolated link has carried
  real telemetry end to end (2026-08-23), after the UART baud rate was
  corrected to 9600 at the time — see `docs/SAFETY_LINK.md`. The barrier was
  reworked from optocouplers to a digital isolator on 2026-08-25 and the baud
  is being re-measured; see `CONFIG_KILNCTL_SAFETY_BAUD_RATE` in
  `KilnFW/App/drivers/Kconfig` for the current value. With no Pico attached the
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

**2026-09-22: superseded.** All seven items below were written before
`SaftyFW` existed and are historical; every step (UART fix, autotune,
on-target guard walk, thermocouple-fault gating, the safety-processor
firmware itself, and the J2 pin identity — resolved via
`KILNCTL_DISPLAY_SWAP_DC_RESET`) has since landed. **`ROADMAP.md`'s M18
section ("Full commissioning of the dev board") is the live plan** for what
remains; as of this writing that is: finishing the Class C backend rows still
needing owner authorization (C1/C6/C9/C16 and the C7/C8/C21-C24/C26 ruling
conflict), unwired web-UI commissioning rows (W8/W9/W10 profile
create/delete/favorite, W45/W51 destructive import/Wi-Fi-forget, and the
owner-gated heat/E-stop/OTA/auth/Pico-reset rows), and the LCD-class sweep.
Do not restart any of the numbered items below; read M18 instead.

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
   `App/drivers/safety/safety_link.h` (`docs/SAFETY_LINK.md`, no inversion needed on
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

### 2026-09-05 — Pending owner live test: fixture reconnect swapped a zone/TC pairing, plus three unexplained bench events

Owner disconnected and reconnected the fixture 2026-09-05. Zone 0 (relay 1)
now heats thermocouple channel 2, and zone 1 (relay 2) heats channel 1
(correct); zone 2 untested. Bench rotation in code is `cs_pins = {THERMO_CS2_IO,
THERMO_CS0_IO, THERMO_CS1_IO}` (`MAX31856.c` ~line 687). A `cpl_z0` run tripped
guard 1 HEATING_FAILED at 61 s because of this mismatch. Either swap the zone
0/zone 2 thermocouple plugs or update the rotation, then verify before any
firing.

Thermocouple polarity confirmed rising-with-heat on channels 1 and 2 and on
the safety thermocouple; channel 0 unverified.

Three unexplained events from the owner's own firing attempt, parked:
- Web UI reported the safety relay did not close; the board had a POWERON
  reset at that time, cause unknown. No trip latched (`trip_event` count 0).
- A `debug_reset` took over 4 minutes to come back and needed a third reset;
  not reproduced.
- Pico (running `d179132`) refused `UPDATE_BEGIN` with a trip-pending
  precondition during an OTA attempt, while no trip was latched.

Heating runs are on hold until the owner's live test (owner instruction
2026-09-05).

### 2026-09-05 — crash_report.c dump_id was hashing uninitialized stack padding

`crash_report_init()`'s `dump_id` CRC covered the raw (unzeroed) stack-allocated
`esp_core_dump_summary_t`, so padding bytes made an operator's ack fail to
survive a reboot; fixed to zero the struct and hash only the identifying
fields. Re-verify on hardware: ack a crash record, then `debug_reset()` twice
and confirm it stays acknowledged.

### 2026-09-04 — Display-power hardware verification, then a task-watchdog reset mid-firing; root-caused and fixed

An agent hardware-verified `docs/UI_PLAN.md`'s Display power feature on a
LIVE firing (`fuzzy_ab_20260904d`, arm B1, profile 7, segment 2/3): `POST
/api/settings/display_power` with `timeout=1min` + `keep_on_while_firing=1`
kept the screen ON through the timeout (>100 s, confirmed via
`touch_get_state()`/pixel sampling), and `keep_on_while_firing=0` correctly
BLANKED it 15 s later with the executor still `RUNNING`. **Both results are
real, standing hardware verification** — not affected by what follows.

Restoring settings and injecting a wake touch immediately reset the board:
`reset_reason: task watchdog`, executor `RUNNING` → `IDLE`, firing lost.
Reproduced twice more on an **idle, non-firing** board (ruling out the live
firing and concurrent HTTP polling as the cause): `ili9488_flush_cb()`
(`lvgl_port.c`) was calling `lv_obj_invalidate(lv_screen_active())` on the
wake edge **from inside LVGL's own active-refresh flush callback** —
unsupported reentry into LVGL's invalid-area walk, starving the idle task
until `CONFIG_ESP_TASK_WDT_TIMEOUT_S` (5 s) fired on the `lvgl` task. Third
distinct bug in this file the same day (after `e7b8efc`'s stack overflow and
`7a8594d`'s SPI-under-lock fix), on the least-tested path of the three.
Fixed by moving the wake-edge detection/invalidate into a new
`lvgl_port_service_idle_wake()`, called from `lvgl_port_task()`'s loop before
`lv_timer_handler()` runs — outside any active refresh, same footing the
existing on→off blank handler already had. Verified clean on hardware
(rebuilt, flashed, two more blank/wake cycles, zero resets); regression test
`test_display_power_wiring.c` section 6 added, negative-tested by hand. Full
writeup: `docs/UI_PLAN.md`'s Display power section.

Separate, unrelated: the campaign runner's restore-on-exit did not run when
it died with the board, so the board's live zone config may not currently
match `fuzzy_ab_baseline_20260903` — flagged for whoever is handling that
recovery.

### 2026-09-04 — Backlight PWM enabled; DRAM floor now breached at boot

`be02d34` flipped `KILNCTL_BACKLIGHT_PWM_ENABLE` to `y` (the flying wire from
`CONFIG_KILNCTL_BACKLIGHT_GPIO` to the panel backlight input was fitted but
reading flat, because with the flag off `backlight_pwm_init()`/`_start()` are
no-ops that never touch the GPIO). Turning it on actually allocates the
`backlight_pwm` task's 3072 B stack, which had never been counted against the
`dram_margin.h` floor before. Measured live: `dram_free` at `app_main_done`
drops from 22123 to ~18323 B (below the owner's `KILN_DRAM_FREE_FLOOR_BYTES`
= 20480), and largest free block from 13824 to 10240.

The task itself is not oversized — it uses 2168 of its 3072 B stack (29.4%
headroom) — so shrinking it is not the fix, and per `dram_margin.h`'s own
policy the floor is not to be moved just to silence the alarm. The one lever
identified so far is `KILNCTL_ENABLE_GPIO_PROBE` (6144 B stack, debug-only,
currently `y` only in the gitignored bench sdkconfig per the mismatch this
file already records above) — freeing it would more than cover the breach,
but that is an owner decision, not yet taken. Left open.

### 2026-08-24 — Stack high-water-mark reporting added (the section 13 blocker's measurement, not its fix)

Follow-up to the entry immediately below. That investigation named six
internal-only task stacks (~20.5KB) as resize/PSRAM candidates but explicitly
forbade acting on them "from the numbers in this entry alone" — this repo
has already shipped a real stack overflow caused by sizing a stack from a
comment rather than a measurement. This pass removes that blocker by making
`uxTaskGetStackHighWaterMark()` readable from the PC side; it changes no
stack size and was not run against real hardware (bench in use elsewhere).

**Added**: `App/drivers/common/stack_margin_calc.h` (pure: FreeRTOS's word-
granularity high-water mark → bytes, plus a 15%/30% headroom triage
classification against each task's own configured stack size — a first-pass
heuristic, not a measured threshold) and `App/drivers/stack_margin.{h,c}`
(the registry: `uxTaskGetStackHighWaterMark()` wrapper, reading through the
caller's own `TaskHandle_t*` fresh on every call rather than a snapshot, so
a torn-down task reports "not alive" instead of a stale reading).

Registered all six stacks named in the entry below: `uart_owner_task`/
`uart_owner_evt_task`/`uart_proto_rx` (`main.c`, right after
`uart_owner_init()`/`uart_protocol_init()` succeed — **the PC-link instance
only**; `safety_link.c` runs the identical code for the isolated Pico link
under the same task names and is deliberately NOT registered here, to avoid
conflating the two links' readings under one name), `rules_task`/
(historical: `uart_owner_task`/`uart_owner_evt_task` were since deleted along
with the uart_owner request-queue path; the surviving PC-link tasks are
`uart_owner_event_task` and `uart_protocol_rx_task`.)
`rules_watchdog` (`rules_task.c`), `system_uart_bridge` (`uart_bridge.c`,
which also needed its task handle actually captured — the existing call
site passed `NULL` for it).

**Exposed on the wire** as `INFO_CMD_GET_STACK_MARGIN` (0x04,
`uart_task_ids.h`) — purely additive, so `UART_PROTOCOL_VERSION` (frozen at
7 since the same day's earlier entry) was NOT bumped; the bump policy in
that constant's own doc comment exists precisely to distinguish this case
("a new subcommand an old peer has never heard of") from a wire-incompatible
change. `KILNLINK_PROTOCOL_VERSION` (the separate ESP↔Pico link's version)
is untouched — `tools/check_uart_version_independence.ps1` confirms the two
never got re-coupled. `tools/check_bridge_reject_reason.ps1` also re-run
clean (79 `bridge_reply_reject()` calls, all with a reason) — this change
added none.

**PC side** (`tools/PcTools/src/kilnctrl`): `devices.parse_stack_margin_
response()` / `StackMarginEntry` / `StackMarginLevel`, `devices.info_get_
stack_margin()`, wired into `info.InfoClient.get_stack_margin()` and into
`parse_info_response()`'s structural dispatch (INFO replies carry no
subcommand byte — an empty `GET_STACK_MARGIN` reply is byte-identical to an
empty `GET_PIN_CONFIG` reply, both just `0x00`; only the caller's `prefer`
hint breaks that tie, same mechanism the existing GET_WIFI_STATUS/GET_PIN_
CONFIG ambiguity already relied on).

**Host-tested both sides, each with a demonstrated negative-test failure**:
- `App/test/test_stack_margin.c` (13 checks) — word→byte conversion pinned
  against absolute values (not the constant being tested), classification
  boundaries. Broke `STACK_MARGIN_WORD_BYTES` (4→1): 3 checks failed
  (954/957). Broke the CRITICAL/LOW boundary (`<` → `<=`): 1 check failed
  (956/957). Both reverted, 957/957 green.
- `tools/PcTools/tests/test_stack_margin_info.py` (17 checks) — wire decode,
  truncation/overrun handling, the empty-reply disambiguation above. Broke
  the entry-length constant (9→8 bytes): 6 of 17 failed with a `struct.error`
  from the resulting misaligned unpack. Reverted, 17/17 green
  (735 passed + 91 subtests across the full `pytest` run, no other
  regressions).

**Verification run this pass**: on-target `ninja -j 24` clean (89/89 steps,
no new warnings); KilnFW host suite 957/957 (baseline 944 + this pass's 13
new, no failures). `pytest`: 735 passed + 91 subtests, no failures — this
pass added 17 (`test_stack_margin_info.py`); the stated baseline was 707, so
the other 11 are from concurrent work elsewhere in this tree (this repo has
other live sessions committing in parallel — see project memory), not
attributable to this change. Both `tools/check_uart_version_independence.
ps1`/`check_bridge_reject_reason.ps1` guards clean.

**Still true, unchanged by this pass**: no real high-water-mark figure has
been read on this board — everything above only makes that reading
possible. Next bench step: flash this build, exercise each of the six
tasks' worst-case code path at least once (a UART burst, a rule evaluation,
a SYSTEM command), then call `InfoClient.get_stack_margin()` and read the
actual numbers before resizing or PSRAM-stacking anything. See TODO.md
section 13's own follow-up note for the same detail.

### 2026-08-24 — Internal-DRAM boot trough investigated; low-water alarm added

Bench (commit 750dc33) showed `heap stage uart_bridges_1 largest= 7680
delta= +0 dram_free= 12483` — a ~35KB drop in `dram_free` since the
`executor+autotune` stage. Itemized as far as static analysis honestly goes:
~20.5KB is attributable to internal-only (plain `xTaskCreatePinnedToCore`,
not `*WithCaps(MALLOC_CAP_SPIRAM)`) task stacks created in that window —
`uart_owner_task`+`uart_owner_evt_task` (4096B each, since deleted — see the
2026-09 uart_owner collapse; surviving PC-link tasks are
`uart_owner_event_task` and `uart_protocol_rx_task`), `uart_proto_rx`
(4096B), `rules_task`+`rules_watchdog` (3072+2048B), `system_uart_bridge`
(3072B). `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` means every one of
these lands internal regardless of PSRAM being present, since none of them
requests `MALLOC_CAP_SPIRAM` explicitly. The remaining ~14KB is NOT
attributed to any single line found by inspection — candidates (per-task
TCB/newlib-reent overhead across ~10 tasks created in the window, NVS handle
opens in `kiln_cfg_store_init()`/`safety_cfg_store_init()`) were checked and
none is individually large enough to close the gap; this is reported as a
genuine gap, not filled with a guess.

The collapse of `largest` (31744 → 7680) is mostly consumption (a
monotonically shrinking heap during a boot sequence that creates ~10 tasks
back to back), but real fragmentation is also present at that instant: free
(12483) is well above largest (7680), meaning ~4.8KB is free but scattered
into pieces smaller than the largest block — a single allocation between
7680 and 12483 bytes fails there even though "12.4KB free" sounds fine.

Good news found while tracing this: the historical cause of the identical
7680-byte figure (LVGL's 8192-byte task stack racing other internal-SRAM
consumers) is already fixed — `lvgl_port.c` now uses a static `.bss` stack
array, decided at link time, which no longer competes for a dynamic
contiguous block at all. `uart_bridge_ext_start_flash_worker()`'s 8192-byte
internal stack is also already started early (before this window), per its
own file-header comment. Both of those fixes predate this investigation and
remain correct; nothing here was found to have regressed them.

Risk verdict: the 12483/7680 boot-time trough is in the same neighborhood as
the ONE documented real failure with numbers attached — `free=11903,
largest=8704` at 8000s of *steady-state* uptime (not boot), which broke
`/app.js` delivery (`ERR_CONNECTION_RESET`, truncated JS, "Loading..."
forever) — see the session's DRAM-exhaustion memory note. These are not the
same measurement point (a boot-time trough vs. a multi-hour steady-state
floor) so treat "~500 bytes of margin" as suggestive, not literal; but they
are the same heap cap (`MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT`) and the same
order of magnitude, and DRAM fragmentation only gets worse with uptime, not
better — a board starting this close to the known failure zone at boot has
no demonstrated slack before it degrades into the same failure over a long
firing. Not verified on this bench past `uart_bridges_1`; the later stages
(`lvgl_start`, `uart_bridges_2`, `app_main_done`) were not captured in the
log this investigation had, so whether this recovers by end of boot (as an
earlier, differently-configured pass's own recorded figures suggest it
might) is unconfirmed, not assumed.

**Implemented**: `App/drivers/common/dram_margin.h` (`dram_margin_check()`, pure
and host-tested) wired into `main.c`'s `heap_stage()` — logs `ESP_LOGE` at
any stage where `largest < 8704` or `dram_free < 11903` (the documented
failure's own figures, not an invented margin). Host-tested in
`App/test/test_dram_margin.c`, including a negative test using this
session's actual bench figures (7680/12483) that must trip the alarm;
verified failing when the threshold was temporarily weakened, then restored
green (921/921). Not yet exercised on real hardware — will fire the next
time this board boots.

**Not implemented, needs a bench measurement before it's safe to do**:
shrinking `uart_owner`/`uart_protocol`/`system_uart_bridge`'s
stacks (`rules_task` was removed 2026-08-27 with the rule engine, see
`docs/PROFILES.md`; see `feedback_negative_test_every_check`/prior `SimFW stack sizing`
species note — must be backed by `uxTaskGetStackHighWaterMark()`, not a
guess) or moving any of them to `MALLOC_CAP_SPIRAM` (each such task must be
individually checked against the flash-cache-disabled hazard
`uart_bridge_ext.c` documents before being PSRAM-stacked). Left for a
future pass with real hardware access.

### 2026-08-24 — HTTP `max_uri_handlers` cap fell behind for the 4th time; now machine-guarded

Bench boot log (commit `750dc33`):
`httpd_register_uri_handler(/api/safety/commissioning/bench_preset) failed:
ESP_ERR_HTTPD_HANDLERS_FULL` — that route silently 404s. `max_uri_handlers`
(84) was one short. Recounted by machine, not by hand: every
`.uri = "..."` `httpd_uri_t` literal under `App/drivers/*.c`, comments
stripped, gives **85 routes in a normal build, 87 with
`CONFIG_KILNCTL_SIM_PLANT`** (sim_backend.c's 2 `/api/sim` routes are the
only conditionally-compiled ones). Raised to **95** (worst case + 8
headroom, same order as the three prior bumps: 59→72→80→84). RAM cost:
`esp_http_server` allocates `hd_calls` as an array of 4-byte pointers, not
structs, so the bump costs 11 × 4 = **44 bytes** — noise against the
12483-byte `dram_free` this same boot log measured.

The fix that matters more than the number: `tools/check_uri_handler_cap.ps1`,
a standalone guard (comments stripped, `throw`-based non-zero exit, refuses
to run blind on an implausible route count) that recounts every
`.uri = "..."` under `App/drivers/` and fails when `max_uri_handlers` is
below that count. This is the fourth time this cap has drifted behind the
real count with only a prose comment guarding it — the comment has a 0%
save rate on this bug across three prior misses, which is why this pass
adds a script instead of another paragraph. Verified failing three
different ways (unmodified tree before the fix, cap manually set one below
the true count, and 9 dummy routes added elsewhere in `drivers/` with the
cap left untouched) and passing again after each was reverted — see
`firmware/KilnFW/TODO.md` section 12 for the full readout. Not yet wired
into any build/CI step; running it is still a manual step.

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
