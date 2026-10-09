# PcTools — one GUI and MCP server for both processors

> **Status:** planning · **Last reviewed:** 2026-09-02
> **Keep this file current.** If a tool, page or transport changes, update it in
> the same commit. If it disagrees with the code, **the code wins.** Finished
> items are removed from this file rather than left as a completed marker —
> see this repo's plan-doc convention. Checklist at the bottom.

The code lives at `tools/PcTools/` (`docs/REPO_LAYOUT.md`) and serves both
processors — `safety.py`/`probe.py`/`pico_gpio_probe.py`/`debug_probe.py` talk
to the RP2040 side, everything else to the ESP. This document tracks the
remaining "serve both processors evenly" work and the capabilities that make
the tool usable by an agent rather than only by a human at a Tk window.

The Python package keeps the name `kilnctrl`: it is the system's name, not the
main board's.

## Three transports, one interface

| Path | Reaches | Notes |
|---|---|---|
| **USB serial → ESP32** | ESP directly, **and the Pico through it** | The primary path. The `SAFETY` task (task 7) already relays; no second cable |
| **USB-TTL adapter → isolated UART** | Pico directly | For bring-up before the ESP side works, or when the ESP is the thing under suspicion. **Must invert** — see `firmware/SaftyFW/docs/HARDWARE.md` §1. **2026-09-05: this row is documentation only** — `link_hub.py` has no code path for a direct USB-TTL-to-Pico connection; every `RemoteUartLink`/hub client goes through the single ESP-attached serial port. Using this path today means a separate ad hoc script/terminal outside `kilnctrl`, not this tool. |
| **SWD/RTT → Pico** | Pico directly | Development and flashing. Also the only path when the Pico will not talk |


## Capabilities to add, in priority order

**Hardware-gated, bench status — DONE 2026-08-23.** The coordinated two-board
GPIO test (`firmware/SaftyFW/docs/HARDWARE.md` §1 Steps A/B/C) passed in full,
both data directions and the fault line, with a negative control and
register-level corroboration over JTAG/SWD. Result: **ESP GPIO5 = TX,
ESP GPIO4 = RX** — the opposite of what the schematic traces had concluded.

Two things had been blamed for the earlier failures, and neither was the cause:

- *"`12v_Safty` not powered"* — it was powered; Pico GP5 idles high off R9
  from `3.3v_Safty`.
- *"`pico_gpio_probe.py`'s `write()` sets FUNCSEL but does not move the pad"* —
  `write()` works. Both real causes were on the ESP side: (1) `KilnFW`'s
  UART1 still owned GPIO4 as an output under the old pin config, so the ESP was
  itself holding the net the Pico was trying to drive, and (2) `gpio_probe`'s
  task stack was 3072 bytes and overflowed on the first command it ever
  handled, rebooting the board — which surfaced only as "ACKed but no reply".
  Both fixed; stack raised to 6144 with a coredump-backed comment.

ESP GPIO6 (fault) is still hard-denied by `gpio_probe.c` by design. Step C was
run instead by observing the firmware's own fault assertion (Pico GP10 low,
ESP `GPIO_OUT_REG` bit 6 set) against the ESP held in reset (GP10 high) — which
also documents that **this line fails de-asserted**.

### 2. Saleae capture — decode landed 2026-09-20, unvalidated against real hardware

`kilnctrl.kilnlink_capture` decodes a Saleae Async Serial "Export Table Data"
CSV (or a raw binary capture) of the isolated ESP<->Pico kilnlink UART into a
frame timeline — framing, msg type, device/task, sequence number, CRC
verdict, and per-cmd payload fields; malformed/partial frames are reported
at their byte offset and decoding resyncs afterward rather than stopping.
Exposed as `saleae_decode_kilnlink(path, csv=True)`. Built and pytest-covered
(`tests/test_kilnlink_capture.py`) purely against synthetic fixtures encoded
with the protocol's own rules (`tests/fixtures/kilnlink/`) — still **not
run against a single real capture**, since no board + Logic analyzer have
been on the bench together this session either. First real capture should
be treated as this decoder's actual validation, not a formality.

### 3. Everything reachable headlessly — DONE 2026-09-05

Page-by-page audit of every `gui_*.py` popup against `mcp_server.py` +
`actions.py`'s `press_button`/`list_buttons` registry. Every control had a
headless equivalent except one: the Danger Zone popup's "Factory Reset..."
button (`gui_danger_zone.py`'s `danger_zone_confirm`, wire command
`SYSTEM_CMD_FACTORY_RESET`/`devices.system_factory_reset`) had no bare
headless path — only `load_config_preset()`'s factory-reset-then-apply-a-
preset flow (`mcp_server_ui_test.py`) touched it, always applying a preset
afterward. Closed by adding a `"System: Factory Reset"` action
(`actions.py`, scope 0-3 = wifi/kiln/profiles/all) alongside the existing
watchdog-panic get/set actions, reachable via `press_button`/`list_buttons`
the same way every other GUI-only control already was. Covered by
`selfcheck_actions.py`'s live virtual-link exercise.

### 4. One-call board snapshot — DONE

`get_board_state()` (`mcp_server_codec.py`) reports both processors: it
already carries `safety_status` and `safety_link_stats` alongside the ESP
sections. Stale entry — this was written while the isolated link was still
capped at 9600 baud (see the Pico-through-the-ESP item above); once that was
fixed the Pico sections came along with the rest of the snapshot for free,
nothing else had to be added here.

### 5. Software peer stub

Done both directions (`fake_peer.py`), including fault injection tied to
specific `LINK_PROTOCOL.md` rules.

## Debug and programming — OpenOCD wrapper

`kilnctl.debug_probe` wraps OpenOCD for both chips (ESP32-S3 JTAG, RP2040 SWD
via CMSIS-DAP), exposed as MCP `debug_*` tools taking a `peer` argument. This
is done and covers: program/reset/halt/resume/step/read/write memory/read
registers, `openocd.exe` path resolution, halt refused on the ESP mid-profile,
flash writes requiring explicit confirm, every halt/reset/write logged.


⚠️ Standing hazard, not yet mitigated by anything in code: **any PC debug
connection into the safety domain bonds `GND_Safty` to PC ground**, and if the
ESP is on the same PC, bypasses the isolation barrier for the duration.
Bench only, never with load wiring connected. Also: debugging the ESP over
JTAG works, but halting it stops telemetry, which the Pico will correctly read
as a dead main controller (S6) — expect a trip, and expect it correct.

## Logging and consoles

**Decided: the bench setup is the Raspberry Pi Debug Probe — SWD plus its
UART bridge on GP16/GP17. The Pico's own USB is not used** (see
`firmware/SaftyFW/docs/ARCHITECTURE.md` §1 for the full reasoning: TinyUSB
cost with nothing replaced, blocks with no host reading, absent during early
crashes, drops on reset, and A1's 3V3 back-feed puts the module regulator in
contention with IC3 on a powered board). Survives only as
`SAFTYFW_ENABLE_USB_STDIO`, default off, debug builds only.

Console capture (`console_capture.py`), per-processor + interleaved log files,
and PC-arrival-time as the common clock are built. Not yet exercised against a
real Pico/probe — only synthetic queued events tested.

**Debug-UART temperature telemetry (owner decision 2026-09-02: flash never
holds per-tick temps, that belongs on the debug UART) — PC side done.**
`telemetry_capture.py` gives `enable`/`disable`/`status`/`capture` (SYSTEM
subcommands 0x05/0x06, wired through `telemetry_log_set_enabled()`/
`telemetry_log_is_enabled()` — `uart_bridge_system.c`, host-tested only, not
flashed this session). `log_analysis.parse_profile_exec_uart_capture()` reads
a capture file straight into `PollRow`, per that module's own reserved seam.
Not yet exercised against a live board — fixture/synthetic tests only (bench
was mid-firing). Throughput: FIRE lines run ~38 B/s against a 921600-baud PC
link, ~0.02% of capacity — see `telemetry_capture.py`'s module docstring for
the full numbers and the shared-queue starvation risk from OTHER log
traffic.

- [ ] RTT-over-SWD console as the fallback path — **firmware side pending**
      Premise check 2026-10-09: RTT is the RP2040 (SaftyFW) path only -- the ESP32-S3
      has no SWD (JTAG/USB-Serial-JTAG), and SaftyFW has no RTT code today (only a
      UART console, `console_uart.h`). Firmware side = vendor the SEGGER RTT control
      block (BSD) into SaftyFW behind a CMake option default OFF, route `log_task`
      output to up-channel 0 when ON; PC side = read the block via OpenOCD
      `rtt setup/start/server` through `debug_probe`. Bench steps once built: (1) build
      with the option ON, flash via `debug_program(peer="pico")`; (2) find
      `_SEGGER_RTT` in the ELF map, `rtt setup <addr> 0x30 "SEGGER RTT"`, `rtt start`,
      `rtt server start 9090 0`; (3) connect to 9090 and confirm log lines appear;
      (4) confirm link/safety timing unchanged with the option OFF (identical image
      size/behavior).

## Firmware updates from here

Design: [`../../firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](../../firmware/CommonFW/docs/UPDATE_PROTOCOL.md).
`ota_get_challenge()`, `ota_update_esp()`, `ota_update_pico()`, `ota_status()`,
`ota_rollback_esp()`, and `ota_recovery_exit_esp()` all exist
(`mcp_server_ota.py`, backed by `ota_http_client.py`) and are unit-tested
against mocked HTTP (`tests/test_ota_http_client.py`,
`tests/test_ota_status_protocol_version.py`) — this whole section was stale,
not open. `ota_update_esp()` does NOT wait for the reboot or return the
post-reboot version itself (that stays a separate `ota_status()`/
`get_fw_version()` poll by the caller, by design — see that tool's
docstring); `ota_update_pico()` returns as soon as the relay *starts*, same
reasoning. Every push/rollback/recovery-exit call now logs the image's
SHA-256 (or, for rollback/recovery, just host+outcome) and logs refusals too,
never the password (`ota_http_client.py`'s `log.info`/`log.warning` calls,
proved by `PushImageLoggingTest`). `ota_status()` now reports the Pico's
`protocol_version`/`protocol_min_compatible` and tags a real mismatch
`INCOMPATIBLE PROTOCOL VERSION` up front rather than folding it into normal
status prose — the wire fields were already emitted by
`ota_pico_status_get_handler()`, just not read on this side.

None of the above has been exercised against real hardware — no reboot has
ever actually been triggered by these tools. First hardware step: a board
that is idle/cool/link-healthy (interlocks satisfied), then one real
`ota_update_esp()` call followed by a manual reboot and `ota_status()` poll
to confirm PENDING_VERIFY → confirmed actually happens as documented.

- [ ] Live-hardware verification of all of the above (needs a board, see first
      step above)

## What this does not become

- **Not a second control path that bypasses safety.** Every relay command from
  here goes through `relay_authority_on_blocked()`, exactly as
  `firmware/KilnFW/docs/SAFETY_MODEL.md` requires of the existing tools. The GPIO probe's
  deny-list exists so it cannot become a way around that.
- **Not a place for firmware logic.** It observes and commands; it does not
  decide anything safety-relevant.
- **Not a fourth protocol implementation.** It consumes `CommonFW`'s test
  vectors so its Python codec is checked against the C one
  (`firmware/CommonFW/README.md`).

---

## Completion checklist

**Two peers**

**Capabilities**

**Logging and consoles**
- RTT-over-SWD console as the fallback path -- still open; single tracker is the "Logging and consoles" item above (duplicate checkbox merged 2026-10-07)

**Firmware updates**
- Live-hardware verification of the firmware-update tools -- still open; single tracker is the "Firmware updates from here" item above (duplicate checkbox merged 2026-10-07). OT-E-series bench evidence: `firmware/KilnFW/TODO.md` 9.7

**Integrity**
