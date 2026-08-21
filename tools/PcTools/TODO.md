# PcTools — one GUI and MCP server for both processors

> **Status:** planning · **Last reviewed:** 2026-08-21
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
| **USB-TTL adapter → isolated UART** | Pico directly | For bring-up before the ESP side works, or when the ESP is the thing under suspicion. **Must invert** — see `firmware/SaftyFW/docs/HARDWARE.md` §1 |
| **SWD/RTT → Pico** | Pico directly | Development and flashing. Also the only path when the Pico will not talk |

- [ ] `Peer` abstraction (`ESP` | `SAFETY`) threaded through `link_hub.py`; every
      tool takes a peer argument
- [ ] Pico-through-the-ESP path working end to end (no second cable) — blocked
      on the isolated link itself, currently dead on the bench
      (`link_status`: `frames_received: 0`)
- [ ] GUI grows a safety column rather than a second application

## Capabilities to add, in priority order

**Hardware-gated, bench status**: the coordinated two-board GPIO test
(`firmware/SaftyFW/docs/HARDWARE.md` §1 Steps A/B/C) has its pin/direction
mapping confirmed manually (ESP pins 4/6 outputs, Pico pins 7/14 inputs, Pico
pin 6 → ESP pin 5), but the **electrical propagation half is still open**: no
edge has been captured crossing either direction in any attempt, and ESP
GPIO6 (fault) cannot be driven at all (hard-denied by design). This is blocked
on a real bug — `pico_gpio_probe.py`'s `write()` sets `CTRL.FUNCSEL` correctly
(confirmed via Saleae) but does not reliably move the physical pad; root cause
not found. Needs a scope/multimeter cross-check against a spare Pico GPIO to
rule out firmware contention before assuming the tool is at fault.

### 2. Saleae capture

`kilnlink` frame decoding for a capture is **not done** — deliberately: a
decoder built against zero real captures risks one nobody has validated.
Needs a board and analyzer on the bench at once.

### 3. Everything reachable headlessly

- [ ] A page-by-page audit of `gui.py` against `mcp_server.py` is still open —
      the pass done so far diffed client instantiations, not every control.

### 4. One-call board snapshot

`get_board_state()` still only has one processor's data — `SaftyFW` still has
no data source to add to it (no board, link is dead).

### 5. Software peer stub

Done both directions (`fake_peer.py`), including fault injection tied to
specific `LINK_PROTOCOL.md` rules.

## Debug and programming — OpenOCD wrapper

`kilnctl.debug_probe` wraps OpenOCD for both chips (ESP32-S3 JTAG, RP2040 SWD
via CMSIS-DAP), exposed as MCP `debug_*` tools taking a `peer` argument. This
is done and covers: program/reset/halt/resume/step/read/write memory/read
registers, `openocd.exe` path resolution, halt refused on the ESP mid-profile,
flash writes requiring explicit confirm, every halt/reset/write logged.

- [ ] The Pico-ARMED write-refusal gate has only been read-only smoke-tested
      against a board that was NOT confirmed to actually be `ARMED` at the
      time (the live SWD read came back an invalid `relay_owner_state_t` byte,
      so the gate correctly fail-closed rather than proving the true-ARMED
      case). Do a live build+flash+arm sequence before relying on this gate
      for anything beyond defense-in-depth.

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

- [ ] Pico logs emitted as `kilnlink` LOG frames (device `SAFETY`, task 5),
      relayed by the ESP — the primary path once the link is up; still needs
      the link itself
- [ ] RTT-over-SWD console as the fallback path
- [ ] Pico USB CDC explicitly reported as absent unless
      `SAFTYFW_ENABLE_USB_STDIO` was built in
- [ ] Transport marked per line (relayed / probe-UART / RTT) — today only the
      source processor is tagged, since only one transport per processor
      exists in code
- [ ] Per-peer level filter
- [ ] Runtime log-level control for the safety processor over the link,
      default warnings+errors
- [ ] Transport availability shown honestly as build-time capability, not a
      toggle
- [ ] Dropped-log-frame counter surfaced from the diagnostic frame
- [ ] Pico log emission best-effort and droppable — never blocking, per
      no-hang rule 3

## Firmware updates from here

Design: [`../../firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](../../firmware/CommonFW/docs/UPDATE_PROTOCOL.md).
`ota_rollback_esp()`/`ota_rollback_pico()` exist and are unit-tested (neither
exercised against real hardware — no reboot has ever actually been triggered
by these tools). SWD recovery is documented as the path for a Pico that will
not boot into either app slot or its own bootloader at all.

- [ ] `ota_status()` — both processors: running version, build commit, active
      slot, the version in the inactive slot, and **why an update is currently
      refused**, naming the blocker
- [ ] `ota_update_esp(image_path, password)` — streams, waits for the reboot,
      returns the version actually running afterwards
- [ ] `ota_update_pico(image_path, password)` — same, relayed over the link,
      with progress surfaced at least every 2 s
- [ ] Every call logged with the image's SHA-256, and refusals logged too
- [ ] The password is never written to the log or to a settings file
- [ ] A protocol-version mismatch between a Pico image and the running ESP is a
      hard error here, not a warning
- [ ] `ota_status()` reports **both** processors' protocol version and
      `min_compatible`, and says plainly whether they are compatible and which
      side is older
- [ ] No PC-side sender yet for `SAFETY_CMD_SET_CT_CAL`/`GET_CT_CAL` — the
      codecs and firmware-side handling exist (`firmware/SaftyFW/TODO.md`
      Phase 9), but no MCP tool or script calls them yet;
      `firmware/SimFW/tools/ct_calibration/` still only writes a local JSON file.

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
- [ ] `Peer` abstraction (`ESP` | `SAFETY`) threaded through `link_hub.py`
- [ ] Every tool takes a peer argument
- [ ] Pico-through-the-ESP path working (no second cable)
- [ ] Direct USB-TTL path documented, **with the inversion requirement stated**
- [ ] SWD/RTT path documented for flashing and for a Pico that will not talk
- [ ] GUI grows a safety column rather than a second application

**Capabilities**
- [ ] 1c. Coordinated two-board test script: electrical propagation half still
      open (see hazard note above) — pin/direction mapping half is confirmed.
- [ ] 2. `kilnlink` frame decoding for a Saleae capture — not built,
      deliberately, until a board + analyzer are on the bench together.
- [ ] 3. GUI-vs-MCP capability audit — not exhaustive; a full page-by-page
      pass is still open.
- [ ] 4. `get_board_state()` — only one processor's data exists to report.

**Logging and consoles**
- [ ] Pico logs emitted as `kilnlink` LOG frames, relayed by the ESP
- [ ] RTT-over-SWD console as the fallback path
- [ ] Pico USB CDC **not** offered as a transport; reported as absent unless built in
- [ ] Transport marked per line (relayed / probe-UART / RTT)
- [ ] Per-peer level filter
- [ ] Runtime log-level control for the safety processor over the link, default warnings+errors
- [ ] Transport availability shown honestly
- [ ] Dropped-log-frame counter surfaced from the diagnostic frame
- [ ] Pico log emission best-effort and droppable — never blocking

**Firmware updates**
- [ ] `ota_status`, `ota_update_esp`, `ota_update_pico`
- [ ] Tools call the ESP's endpoints; no second transfer implementation
- [ ] Image SHA-256 logged on every call, refusals included
- [ ] Password never persisted to log or settings
- [ ] Protocol-version mismatch is a hard error, not a warning

**Integrity**
- [ ] Python codec checked against `firmware/CommonFW/test/vectors/`
- [ ] No relay path here bypasses `relay_authority_on_blocked()`
