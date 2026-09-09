# UnitTestFixture — control-device plan (2026-09-05)

Owner decision, ROADMAP.md ~line 216: revive `UnitTestFixture` as a control
device only — a PcTools/MCP surface that flips its relays through the I/O
expanders. Nothing else the old fixture firmware exposes (AD9833, MCP4728,
SSD1306) is in scope; that code stays in place, untouched.

## What the schematic actually has today

`hardware/UnitTestFixture/UnitTestFixture.kicad_sch` (read via the kicad MCP
+ a direct grep, since `list_kicad_components` only accepts `.kicad_pro`/
`.kicad_pcb`) has exactly five parts: `ESP32-S3-DevKitC` (U1), `MCP4728` DAC
(U2), `AD9833xRM` (U3), and **two** `PCF8575DBR` expanders (U4, U5). The
board file is 79 bytes — no footprints placed or layout done.

**There is no relay board here yet.** U4/U5's 32 pins are broken out but
wired to nothing — no relay coils, connectors, thermocouple short/open
network, or SSR-emulation circuit. That hardware is the "future use" the
owner decision explicitly defers, so the relay map here is **placeholder
and data-driven** — generic `U<n>:P<pin>` names — relabelable to function
names ("heater1_open") with zero code changes once real wiring exists. U4 is
assumed at the Kconfig default 0x20; U5's address is unconfirmed (no strap
resistors traced) and provisionally 0x21 pending a bench `SCAN`. Every switch
to a non-cached address is confirmed against the firmware's own `READ_PORT`
address byte before any op on it is trusted — see `fixture.py`'s module
docstring for why a bare protocol ACK isn't proof.

## What the firmware/PC tooling already have

`firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md` + `PCF8575.md`: task 7
(PCF8575) already covers everything needed — `WRITE_PORT`/`WRITE_PIN` (write),
`READ_PORT` (query: pins + shadow + live address), `SET_ADDRESS` (retarget
0x20–0x27), `SCAN`. No firmware change was needed. Pin semantics: writing 1 =
weak pull-up ("de-energized", power-on default), writing 0 = driven low
("energized" by this plan's convention, not yet confirmed against real drive
polarity).

`firmware/UnitTestFw/UnitTest/pc_tools/` is the fixture's own historical
GUI + stdio MCP client (DAC/AD9833/OLED/PCF8575 alike) — not reused here: a
separate stdio server tied to the old fixture's wider scope. Instead this
plan adds a narrow PCF8575-only surface into `tools/PcTools` (the kilnCtl-
wide HTTP MCP package, port 8767), reusing `kilnctrl.serial_link.UartLink`/
`kilnctrl.protocol.Frame` verbatim (the wire framing is byte-identical
between the two boards) via a **second, independent** `UartLink` at 115200
baud against the fixture's own port — never sharing a link or task-id
namespace with the main-board connection.

## MCP surface (implemented)

Four tools, `tools/PcTools/src/kilnctrl/mcp_server_fixture.py`, behind the
`kiln_find`/`kiln_call` facade (`fixture_` prefix, new `fixture` group):

- `fixture_list_relays()` — every known relay name (no connection needed)
- `fixture_set_relay(name, on)` — refuses an unknown name up front; verifies
  the write by read-back and raises rather than reporting a bare ACK as
  success; a failed energize attempts a fail-safe `all_off()` before raising
- `fixture_get_relays()` — live `READ_PORT` per distinct address
- `fixture_all_off()` — de-energize everything, verified by read-back

`fixture.py` connects lazily on first use, and de-energizes with a verifying
read-back on both connect and disconnect/shutdown — `connect()` **fails**
(no warn-and-continue) if that initial de-energize can't be confirmed, so a
caller is never handed a "connected" fixture whose safe state was assumed.
`mcp_server.py`'s `_close()` calls the new `_close_fixture()`.

No client-side shadow of "what we last wrote" is kept: every relay/address
operation is verified by reading the device back, because a transport ACK
only proves delivery to the firmware's inbox, not that the I2C transfer (or
an address switch) actually happened — see `fixture.py`'s module docstring.

## PC connection identity

**Serial number is the source of truth.** A port whose USB descriptor
carries a serial number is identified by that serial and by nothing else;
VID:PID is consulted *only* for a port that reports no serial at all. VID:PID
names a chip family, not a unit — see the correction below for what happens
when it is treated as an identity.

Bench USB enumeration, **corrected 2026-09-09**:

| Board | Port role | VID:PID | Serial |
|---|---|---|---|
| Fixture | native JTAG | 303A:1001 | 68:B6:B3:29:D0:B8 |
| Fixture | UART bridge (CH343) | 1A86:55D3 | 552E006806 |
| Main board | native JTAG | 303A:1001 | 1C:DB:D4:92:F4:7C (= its Wi-Fi STA MAC) |
| Main board | UART bridge (CH340K) | 1A86:7522 | **none** (COM14) |

`recommend_fixture_port()` resolves the fixture's bridge by serial
`552E006806`, and additionally excludes 303A:1001 and the main board's
`1A86:7522` by VID:PID and by text hints. The main board's CH340K is the one
device on this bench with no serial number at all, so `1A86:7522` is the only
handle it has; if a second CH340K-family device ever joins the bench, that
stops being sufficient and `KILNCTL_FIXTURE_PORT` / `port=` is the fallback.

### CORRECTION 2026-09-09: the two UART bridges were recorded backwards

From `402ab01a` (2026-09-05) until `HEAD` today, the table above had the two
UART bridges swapped: the main board's CH340K was recorded as the fixture's
and hard-excluded from main-board autodiscovery, while the fixture's CH343
was recorded as the main board's. `402ab01a`'s message says it "Confirmed via
bench USB enumeration that the fixture's CH340K (1A86:7522) and the main
board's CH343 (1A86:55D3) are different silicon" — the enumeration confirmed
they were *different silicon*, never *which board owned which*, and no round
trip was ever run to settle it. `2545936e` then built the exclusion on that
assumption and `3530e598` inherited it.

Symptom, 2026-09-09: with the fixture's bridge unplugged, `recommend_port()`
returned `None` on a bench whose only UART bridge was the main board's, and
every `kilnctrl` call reported "no serial port open". Conversely
`recommend_fixture_port()` would have returned the **kiln's own** UART port
for relay commands.

Evidence that settles it (the hardware did not change; the record did):

- Live `get_fw_version` over COM14 (the CH340K) answers as **KilnFW**, the
  main board's firmware, at the main board's commit.
- COM14 was already confirmed as the main board's PC↔ESP link on 2026-09-04
  in `d99784c3`, a day *before* the fixture was revived and before
  `1A86:55D3`/`552E006806` appears anywhere in this repo's history.
- Every 2026-09-08 audit of the main board's CONTROL link
  (`docs/audits/control_uart_link_dead_2026-09-08.md` and siblings) names
  COM14, and `settings.py`'s `last_port` is COM14.
- The main board's live STA IP `192.168.1.156` resolves by ARP to MAC
  `1C:DB:D4:92:F4:7C`, matching `MAIN_BOARD_JTAG_SERIAL` — the JTAG half of
  the table was right all along; only the UART half was wrong.

Descriptor reprogramming was considered and ruled out: WCH bridge descriptors
are writable, but nothing here reprogrammed one, and Windows names a device
from its driver/INF and instance path rather than from a reflashed string
anyway (see the "Windows USB names vs descriptors" finding).

**Fixed in `2545936` (2026-09-06 follow-up task):** the two identical
VID:PID (303A:1001) native JTAG ports are now told apart everywhere by USB
serial number. `serial_link.py` gained a board-identity table
(`MAIN_BOARD_JTAG_SERIAL`/`MAIN_BOARD_UART_VID_PID`/`FIXTURE_JTAG_SERIAL`/
`FIXTURE_UART_SERIAL`, `identify_port()` and its
`is_main_board_port()`/`is_fixture_port()` wrappers — note the constant names
changed with the 2026-09-09 correction above);
`recommend_port()` (the MAIN board's own picker) now explicitly excludes
fixture ports — confirmed bug: it previously picked the fixture's bridge
when the main board was unplugged, instead of refusing.
`recommend_fixture_port()` now also excludes by VID:PID/serial directly
(a 303A:1001 port with a generic, non-"jtag" description was not excluded by
the old text-only check) and never falls back to an unidentified port.
`mcp_server_flash.py`'s `flash_firmware()` now passes `adapter serial
<main board's serial>` to OpenOCD and refuses, before calling OpenOCD at
all, if that serial isn't currently enumerated; a new `fixture_flash()` tool
does the same pinned to the fixture's serial. Tests:
`tests/test_serial_link_board_identity.py`, `tests/test_flash_board_pinning.py`,
plus additions to `tests/test_fixture.py`. No firmware was flashed as part
of this fix.

## Verification run this session

- `tools/PcTools/tests/test_fixture.py` — real SLIP/CRC encode-decode against
  a fake serial peer whose command bytes are copied by hand from
  UART_PROTOCOL.md (not the client's own constants, so a drift in one can't
  hide behind the other). Covers connect/disconnect all-off, set_relay/
  get_relays round-trip, address retargeting, and negative tests: unknown
  relay name, a `SET_ADDRESS` nothing ACKs to, a write whose I2C leg fails
  silently (read-back catches both), the fail-safe `all_off()` after a
  failed energize, `connect()` failing loud on an unconfirmable all-off, and
  port selection never picking the main board's port.
  `.venv/Scripts/python.exe -m pytest tests/test_fixture*.py -q` → 14 passed.
- `tools/PcTools/selfcheck.py` — run clean after the above.
- No firmware build was needed; `kiln_find(query="unit test fixture build")`
  found no dedicated `UnitTestFw` build tool — its README's `idf.py build`
  is the fallback.

## Owner-gated: bench validation (not run this session)

1. Restart the `kilnctrl` MCP server so `fixture_*` is published.
2. Fixture only: `fixture_list_relays()` then `fixture_get_relays()` should
   read all-high (de-energized) at boot.
3. `fixture_set_relay("U4:P00", true)` then `fixture_get_relays()` — confirm
   the bit flips (nothing externally wired to observe yet).
4. Run `SCAN` to confirm U5's real address — `DEFAULT_RELAY_MAP` assumed
   0x21 without hardware confirmation.
5. With the main board attached too, confirm `fixture_*` never touches the main board's COM14/COM3 — the unit tests cover the logic, not the real USB stack.
6. Once real relay hardware exists: replace `DEFAULT_RELAY_MAP` with the
   real map and verify drive polarity against a multimeter.
