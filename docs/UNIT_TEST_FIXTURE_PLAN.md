# UnitTestFixture — control-device plan (2026-09-05)

Owner decision, ROADMAP.md ~line 216: revive `UnitTestFixture` as a control
device only — a PcTools/MCP surface that flips its relays through the I/O
expanders. Nothing else the old fixture firmware exposes (AD9833, MCP4728,
SSD1306) is in scope; that code stays in place, untouched.

## What the schematic actually has today

`hardware/UnitTestFixture/UnitTestFixture.kicad_sch` (read via the kicad
MCP + a direct grep, since `list_kicad_components` only accepts `.kicad_pro`/
`.kicad_pcb`) contains exactly five parts: one `ESP32-S3-DevKitC` (U1), one
`MCP4728` DAC (U2), one `AD9833xRM` function generator (U3), and **two**
`PCF8575DBR` I/O expanders — U4 and U5. The board file
(`UnitTestFixture.kicad_pcb`) is 79 bytes — no footprints placed, no layout
done.

**There is no relay board here yet.** U4/U5's sixteen P0x/P1x pins each are
broken out in the schematic but wired to nothing else — no relay coils, no
connectors, no thermocouple short/open network, no SSR-emulation circuit.
The "flip relays through the I/O expanders" capability the owner asked for
is real at the protocol/firmware level (see below) but has no matching
relay hardware today; that hardware is the "future use" the owner decision
explicitly defers. This plan therefore treats the relay map as **placeholder
and data-driven** — generic `U<n>:P<pin>` names — so it can be relabeled
with function names ("heater1_open", "tc2_short") with zero code changes
once the relay board is actually designed and its wiring is known.

U5's I2C address is not confirmed from the schematic (no address-strap
resistor net was traced) and is assumed 0x21 pending a bench `SCAN`. U4 is
assumed at the Kconfig default, 0x20.

## What the firmware already exposes

`firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md` + `PCF8575.md`: the
PCF8575 task (task 7) already supports everything a relay-control surface
needs — no firmware change was needed for this plan:

| Subcmd | Name | Use |
|---|---|---|
| `0x01` `WRITE_PORT` | full 16-bit write | `all_off()` |
| `0x02` `WRITE_PIN` | one pin | `set_relay()` |
| `0x06` `READ_PORT` | query: pins + shadow + current address | `get_relays()` |
| `0x07` `SET_ADDRESS` | retarget 0x20–0x27 without reflashing | reach U5 |
| `0x08` `SCAN` | which of 0x20–0x27 ACK | bench bring-up |

Pin semantics (PCF8575.md): writing 1 = weak pull-up ("input", power-on
default); writing 0 = driven low ("energized" by this plan's convention).
Only one physical PCF8575 instance is ever addressed at a time — reaching
U5 means `SET_ADDRESS(0x21)` first, same call the firmware already answers.

## What existing PC tooling has

`firmware/UnitTestFw/UnitTest/pc_tools/` (the fixture's *own*, historical PC
package: `src/uart_control/expander.py`, `protocol.py`, `serial_link.py`,
`mcp_server.py`) is a full GUI + stdio MCP client for this board, covering
DAC/AD9833/OLED/PCF8575 alike. It is **not** reused directly here: it is a
separate stdio server (no facade, no HTTP), tied to the old fixture's wider
scope, and out of this task's file allowlist. Instead, this plan adds a
narrow PCF8575-only surface into `tools/PcTools` (the kilnCtl-wide HTTP MCP
package on 8767), because that is where every other board control surface
already lives and where `kiln_find`/`kiln_call` already reach.

The wire framing is byte-identical between the two (SLIP + CRC-16/CCITT-
FALSE + ACK/NACK/retry — both descend from the same hardened UART protocol
design), so the new code reuses `kilnctrl.serial_link.UartLink` and
`kilnctrl.protocol.Frame` verbatim, opening a **second, independent**
`UartLink` instance at 115200 baud (the fixture firmware's baud, vs. the
main board's 921600) against the fixture's own port. It never shares a link
or a task-id namespace with the main-board connection.

## MCP surface (implemented)

Four tools, `tools/PcTools/src/kilnctrl/mcp_server_fixture.py`, behind the
existing `kiln_find`/`kiln_call` facade (`fixture_` prefix, new `fixture`
group in `mcp_facade.py`):

- `fixture_list_relays()` — every known relay name (no connection needed)
- `fixture_set_relay(name, on)` — refuses an unknown name before sending
  anything
- `fixture_get_relays()` — live `READ_PORT` per distinct address
- `fixture_all_off()` — de-energize everything

The client (`tools/PcTools/src/kilnctrl/fixture.py`) connects lazily on
first use and de-energizes all mapped relays both on connect and on
disconnect/shutdown (`mcp_server.py`'s `_close()` now also calls
`_close_fixture()`), matching the owner's de-energised-all-off-by-default
requirement.

## PC connection identity

Confirmed by bench USB enumeration, 2026-09-05, with both boards plugged in
at once:

| Board | Port role | VID:PID | Serial |
|---|---|---|---|
| Fixture | native USB-Serial-JTAG | 303A:1001 | 68:B6:B3:29:D0:B8 (COM7) |
| Fixture | UART bridge (CH340K) | 1A86:7522 | *(none reported)* (COM14) |
| Main board | native USB-Serial-JTAG | 303A:1001 | 1C:DB:D4:92:F4:7C (COM3) |
| Main board | UART bridge (CH343) | 1A86:55D3 | 552E006806 (COM6) |

The fixture and the main board use **different UART bridge silicon**
(CH340K vs. CH343), so `fixture.recommend_fixture_port()` can and does key
on VID:PID (`1A86:7522`) alone to find the fixture's control port, while
also excluding the main board's `1A86:55D3` and any JTAG/CMSIS-DAP
descriptor by name as a second, independent check. The CH340K reports no
per-device serial number at all (not a Windows MI_xx-stripping artifact —
there is nothing to strip), so if a second CH340K-based device ever joins
the bench this VID:PID match stops being sufficient and an explicit port
(`KILNCTL_FIXTURE_PORT` env var or `port=`) will be required again.

The two ESP32-S3 native JTAG/debug ports share VID:PID 303A:1001 and are
told apart only by serial number — irrelevant to this plan's PCF8575-only
UART surface, but load-bearing for whichever tool programs the boards over
JTAG (flagged below, out of this plan's scope).

**Flagged, not fixed here:** `flash_firmware()` (`mcp_server_flash.py`)
passes no `adapter serial` to OpenOCD, so with both boards' JTAG present it
binds to whichever 303A:1001 unit it finds first — a real risk of flashing
the wrong image onto the wrong board now that both are on the bench
simultaneously. Pinning that (and adding a parallel `fixture_flash`) touches
`mcp_server_flash.py` and `serial_link.py`, both outside this task's file
allowlist (shared, board-wide files; `serial_link.py`'s port-scoring is used
by every existing tool). Left for a follow-up task with its own explicit
scope grant.

## Verification run this session

- `tools/PcTools/tests/test_fixture.py` — real SLIP/CRC encode-decode against
  a fake serial peer that plays the fixture firmware's role: connect-time
  all-off, `set_relay`/`get_relays` round-trip, address retargeting to U5,
  disconnect-time all-off, and a negative test (unknown relay name refused,
  nothing written to the wire) plus a negative port-selection test (the main
  board's CH343 port is never chosen as the fixture's). All pass:
  `.venv/Scripts/python.exe -m pytest tests/test_fixture*.py -q` → 7 passed.
- `tools/PcTools/selfcheck.py` — run after the above; see run log for result.
- No firmware change was needed (protocol already covers every operation
  used), so no `build_kilnfw`-equivalent build was required for
  `UnitTestFw`. `kiln_find(query="unit test fixture build")` was checked;
  no dedicated build tool exists for `UnitTestFw` today — its own
  `README.md`'s plain `idf.py build` invocation would be the fallback if a
  firmware change is ever needed here, but none was.

## Owner-gated: bench validation (not run this session)

1. Restart the `kilnctrl` MCP server (`mcp_servers.ps1 restart`) so
   `fixture_*` is published — it is new code the running server has not
   loaded.
2. With only the fixture attached, confirm `fixture_list_relays()` then
   `fixture_get_relays()` succeed and read all-high (de-energized) at boot.
3. `fixture_set_relay("U4:P00", true)` then `fixture_get_relays()` — confirm
   the reported bit flips; there is nothing externally wired to observe yet.
4. Run `SCAN` (once exposed, or via the fixture's own `pc_tools` GUI) to
   confirm U5's address is actually 0x21 and not some other value — this
   plan's `DEFAULT_RELAY_MAP` assumed it without hardware confirmation.
5. Do this with the main board also attached at least once, and confirm
   `fixture_*` calls never touch COM6/COM3 (the main board's ports) — the
   port-selection unit tests cover the *logic*, not the real USB stack.
6. Once real relay hardware exists: replace `DEFAULT_RELAY_MAP` in
   `fixture.py` with the actual function-named map and re-verify polarity
   (does "pin driven low" really energize the relay coil, per this plan's
   assumption) against a multimeter or continuity check.
