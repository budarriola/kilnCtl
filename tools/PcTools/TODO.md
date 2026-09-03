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
| **USB-TTL adapter → isolated UART** | Pico directly | For bring-up before the ESP side works, or when the ESP is the thing under suspicion. **Must invert** — see `firmware/SaftyFW/docs/HARDWARE.md` §1 |
| **SWD/RTT → Pico** | Pico directly | Development and flashing. Also the only path when the Pico will not talk |

- [ ] `Peer` abstraction (`ESP` | `SAFETY`) threaded through `link_hub.py`; every
      tool takes a peer argument
- [x] Pico-through-the-ESP path working end to end (no second cable) —
      unblocked 2026-08-23: the isolated link was capped at 9600 baud by the
      TCMT1109 optocouplers (115200 delivered zero frames, ever), not dead.
      With the baud corrected on both sides, `link_status` shows real frames
      received and `safety_get_status()` returns live telemetry. See
      `firmware/SaftyFW/docs/HARDWARE.md` §1
- [ ] GUI grows a safety column rather than a second application

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

### 2. Saleae capture

`kilnlink` frame decoding for a capture is **not done** — deliberately: a
decoder built against zero real captures risks one nobody has validated.
Needs a board and analyzer on the bench at once.

### 3. Everything reachable headlessly

- [ ] A page-by-page audit of `gui.py` against `mcp_server.py` is still open —
      the pass done so far diffed client instantiations, not every control.

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
- [ ] `min_compatible` field / cross-check itself is still open on the
      **firmware** side (`UPDATE_PROTOCOL.md`'s own checklist,
      "`min_compatible` field added to both version frames" / "Both sides
      check both directions of `peer.protocol >= self.min_compatible`") —
      out of scope here, owned by `firmware/CommonFW`

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
- [x] 1c. Coordinated two-board test: both halves confirmed electrically
      2026-08-23. A reusable *script* for it is still unwritten — the run was
      driven tool-call by tool-call.
- [ ] 2. `kilnlink` frame decoding for a Saleae capture — not built,
      deliberately, until a board + analyzer are on the bench together.
- [ ] 3. GUI-vs-MCP capability audit — not exhaustive; a full page-by-page
      pass is still open.
- [x] 4. `get_board_state()` — both processors reported (`safety_status`/
      `safety_link_stats` alongside the ESP sections); this closed for free
      once the isolated link's baud fix landed.

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
- [x] `ota_status`, `ota_update_esp`, `ota_update_pico`
- [x] Tools call the ESP's endpoints; no second transfer implementation
- [x] Image SHA-256 logged on every call, refusals included
- [x] Password never persisted to log or settings
- [x] `ota_status()` surfaces a Pico protocol-version mismatch as an explicit
      `INCOMPATIBLE` flag, not folded into normal-status prose — the actual
      version-negotiation enforcement (`peer.protocol >= self.min_compatible`
      on both sides) is firmware work, tracked in `UPDATE_PROTOCOL.md`, not
      here
- [ ] Live-hardware verification (no board exercised yet)

**Integrity**
- [ ] Python codec checked against `firmware/CommonFW/test/vectors/`
- [ ] No relay path here bypasses `relay_authority_on_blocked()`
