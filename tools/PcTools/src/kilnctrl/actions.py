"""Named-action registry: one entry per GUI button.

Backs the MCP server's generic ``press_button`` tool, which mirrors every
control the human GUI offers (``gui.py``) instead of requiring one bespoke
``@mcp.tool()`` per button. This is deliberately decoupled from ``mcp_server.py``
(no dependency on the ``mcp`` package here) so it can be exercised directly by
``tools/PcTools/selfcheck.py`` without hardware or an MCP client.

gui.py's own widget ``command=`` callbacks are left exactly as they were --
already tested, low blast-radius to leave alone -- this module doesn't
replace them, it's a second, generic front door onto the same underlying
``devices.py``/``info.py`` calls, useful for scripting/automation that would
otherwise need one dedicated tool per button.

Every entry's ``name`` matches the corresponding GUI button/menu-item label
(prefixed with its popup/menu, e.g. ``"Thermo: Read All"``) so "press this
button" maps onto "call this action" with no translation needed.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

from . import devices
from .display import BlitError, DisplayClient, DisplayQueryError
from .info import InfoClient, InfoQueryError
from .io_expander import IoClient, IoQueryError
from .protocol import (
    THERMO_CHANNEL_ALL,
    UART_TASK_ID_DISPLAY,
    UART_TASK_ID_IO,
    UART_TASK_ID_SAFETY,
    UART_TASK_ID_SYSTEM,
    UART_TASK_ID_THERMO,
)
from .safety import SafetyClient, SafetyQueryError
from .serial_link import UartLink, list_ports
from .system import SystemClient, SystemQueryError
from .thermo import ThermoClient, ThermoQueryError


@dataclass
class ActionContext:
    """Everything an action needs to actually do something.

    The four device clients are optional so a caller that only drives
    write-style actions doesn't have to stand all of them up; the *query*
    actions report the absence rather than raising.
    """

    link: UartLink
    info: InfoClient
    session_log: object  # SessionLogger; typed loosely to avoid an import cycle in type checkers
    #: Owns task 1's inbox -- needed by the THERMO read/fault/register queries.
    thermo: Optional[ThermoClient] = None
    #: Owns task 2's inbox -- needed by the IO read/register/scan queries.
    io: Optional[IoClient] = None
    #: Owns task 4's inbox -- needed by READ_ID and by the blit helpers.
    display: Optional[DisplayClient] = None
    #: Owns task 7's inbox -- needed by the SAFETY status/link-stats queries.
    safety: Optional[SafetyClient] = None
    #: Owns task 6's inbox -- needed by the SYSTEM GET_WATCHDOG_PANIC_DISABLED query.
    system: Optional[SystemClient] = None


@dataclass(frozen=True)
class Action:
    name: str
    description: str
    #: param name -> type. press_button (mcp_server_actions.py) validates
    #: every supplied value against this before calling ``run`` -- a bool
    #: param must be a real bool (not "false"/"true"/0/1) and an int param a
    #: real int, so a stringly/loosely-typed ``confirm`` can never slip past
    #: a refusal gate the way it could when this was descriptive-only.
    params: "dict[str, type]" = field(default_factory=dict)
    #: Subset of ``params`` that must be supplied explicitly -- no implicit
    #: default. Purely descriptive today (surfaced by list_buttons so a
    #: caller can see "scope and confirm are required" before ever calling
    #: press_button); the actual requiredness is enforced by ``run``'s own
    #: signature raising TypeError on a missing arg, same as always.
    required: "frozenset[str]" = field(default_factory=frozenset)
    run: Callable[..., str] = field(repr=False, default=None)  # type: ignore[assignment]


ACTIONS: "dict[str, Action]" = {}


def _register(
    name: str,
    description: str,
    params: "dict[str, type]",
    run: Callable[..., str],
    required: "frozenset[str] | None" = None,
) -> None:
    if name in ACTIONS:  # pragma: no cover - programmer error, not a runtime path
        raise ValueError(f"duplicate action name: {name!r}")
    ACTIONS[name] = Action(
        name=name, description=description, params=params, run=run,
        required=required or frozenset(),
    )


def _send(ctx: ActionContext, dst_task: int, payload: bytes) -> str:
    """Shared send path for every device-command action.

    Mirrors mcp_server.py's ``_send()`` exactly (including the protocol
    version compatibility gate) -- duplicated rather than imported from
    there specifically so this module has no dependency on ``mcp_server.py``
    (or the ``mcp`` package it requires), keeping it independently testable.
    """
    if ctx.info.compatible is not True:
        reason = (
            "protocol version mismatch"
            if ctx.info.compatible is False
            else "firmware version not yet confirmed (press \"INFO: Get FW Version\" first)"
        )
        ctx.session_log.error("refused send to task %d: %s", dst_task, reason)
        return f"error: refused - {reason}"
    result = ctx.link.send(dst_task=dst_task, src_task=dst_task, payload=payload)
    ctx.session_log.info("send to task %d -> %s", dst_task, result.value)
    return f"{result.value} - {result.describe()}"


def _info_query(ctx: ActionContext, describe: Callable[[object], str], query: Callable[[InfoClient], object]) -> str:
    try:
        value = query(ctx.info)
    except InfoQueryError as exc:
        return f"error: {exc}"
    return describe(value)


def _client_query(
    client: object,
    task_name: str,
    error_type: type,
    describe: Callable[[object], str],
    query: Callable[..., object],
) -> str:
    """Run a query against one of the four device clients.

    Queries are device commands too -- unlike the INFO ones, which are exempt
    from the compatibility gate because they're how compatibility gets
    discovered. The gate here is the client's own: a client that was never
    registered means this context can't reach that task at all.
    """
    if client is None:
        return f"error: no {task_name} client registered in this context"
    try:
        value = query(client)
    except error_type as exc:  # type: ignore[misc]
        return f"error: {exc}"
    except ValueError as exc:
        # An out-of-range argument (channel 9, register length 17, ...) is
        # rejected by the devices.py builder the query calls, before any
        # frame exists. Report it the same way as a link failure rather than
        # letting it out of the action registry.
        return f"error: {exc}"
    return describe(value)


def _gated_client_query(
    ctx: ActionContext,
    client: object,
    task_name: str,
    error_type: type,
    describe: Callable[[object], str],
    query: Callable[..., object],
) -> str:
    """Same as ``_client_query``, but gated on the protocol-version
    compatibility check ``_send`` applies.

    The IoClient/ThermoClient/etc. wrapper methods talk to ``ctx.link``
    directly and have no notion of protocol-version compatibility -- they
    only guard against a FIRMWARE-level refusal (owned-by-profile, safety
    fault, OTA in progress). A device command reached through one of these
    wrappers before compatibility is confirmed (or after an incompatible
    version is observed) is exactly the same hazard ``_send``'s gate exists
    to prevent -- e.g. relay-authority commands must never reach the wrong
    firmware/protocol version. Relay actions must use this, not the
    ungated ``_client_query``, for that reason.
    """
    if ctx.info.compatible is not True:
        reason = (
            "protocol version mismatch"
            if ctx.info.compatible is False
            else "firmware version not yet confirmed (press \"INFO: Get FW Version\" first)"
        )
        ctx.session_log.error("refused %s command: %s", task_name, reason)
        return f"error: refused - {reason}"
    return _client_query(client, task_name, error_type, describe, query)


# ---------------------------------------------------------------------------
# THERMO -- 3x MAX31856 (task 1)
# ---------------------------------------------------------------------------
_register(
    "Thermo: Configure Channel",
    "Set a thermocouple channel's type (0=B,1=E,2=J,3=K,4=N,5=R,6=S,7=T), "
    "averaging (0=1,1=2,2=4,3=8,4=16 samples), 50/60Hz filter and conversion mode.",
    {"channel": int, "tc_type": int, "avg_mode": int, "filter_50hz": bool, "auto_convert": bool},
    lambda ctx, channel, tc_type=3, avg_mode=0, filter_50hz=False, auto_convert=True: _send(
        ctx,
        UART_TASK_ID_THERMO,
        devices.thermo_config_channel(channel, tc_type, avg_mode, filter_50hz, auto_convert),
    ),
)
_register(
    "Thermo: Set Thresholds",
    "Set one channel's thermocouple high/low trip temperatures (degC, float) and "
    "cold-junction high/low limits (degC, whole numbers).",
    {"channel": int, "tc_high": float, "tc_low": float, "cj_high": int, "cj_low": int},
    lambda ctx, channel, tc_high, tc_low, cj_high, cj_low: _send(
        ctx,
        UART_TASK_ID_THERMO,
        devices.thermo_set_thresholds(channel, tc_high, tc_low, cj_high, cj_low),
    ),
)
_register(
    "Thermo: Set CJ Offset",
    "Set one channel's cold-junction offset in degC (-8..+8, part resolution 0.0625).",
    {"channel": int, "offset_c": float},
    lambda ctx, channel, offset_c: _send(
        ctx, UART_TASK_ID_THERMO, devices.thermo_set_cj_offset(channel, offset_c)
    ),
)
_register(
    "Thermo: One Shot",
    "Trigger a single conversion on one channel. The result is not returned -- "
    'follow with "Thermo: Read All" or enable auto-reporting.',
    {"channel": int},
    lambda ctx, channel: _send(
        ctx, UART_TASK_ID_THERMO, devices.thermo_one_shot(channel)
    ),
)
_register(
    "Thermo: Clear Faults",
    "Pulse CR0.FAULTCLR on one channel. Only meaningful in interrupt fault mode; "
    "in the comparator mode this driver uses, fault bits clear themselves.",
    {"channel": int},
    lambda ctx, channel: _send(
        ctx, UART_TASK_ID_THERMO, devices.thermo_clear_faults(channel)
    ),
)
_register(
    "Thermo: Set Auto Report",
    "Push unsolicited readings for the masked channels (bit N = channel N) every "
    "period_ms milliseconds. period_ms=0 turns reporting off.",
    {"channel_mask": int, "period_ms": int},
    lambda ctx, channel_mask=0x07, period_ms=1000: _send(
        ctx,
        UART_TASK_ID_THERMO,
        devices.thermo_set_auto_report(channel_mask, period_ms),
    ),
)
_register(
    "Thermo: Write Register",
    "Raw MAX31856 register write on one channel (debug).",
    {"channel": int, "reg": int, "value": int},
    lambda ctx, channel, reg, value: _send(
        ctx, UART_TASK_ID_THERMO, devices.thermo_write_reg(channel, reg, value)
    ),
)


def _describe_readings(readings) -> str:
    if not readings:
        return "device reported no channels"
    return "\n".join(r.describe() for r in readings)


_register(
    "Thermo: Read All",
    "Read temperature, cold-junction temperature and decoded faults for one "
    "channel (0-2), or all three when channel is omitted.",
    {"channel": int},
    lambda ctx, channel=THERMO_CHANNEL_ALL: _client_query(
        ctx.thermo,
        "THERMO",
        ThermoQueryError,
        _describe_readings,
        lambda client: client.read(channel),
    ),
)
_register(
    "Thermo: Read Faults",
    "Read the fault status (SR) and MASK registers for one channel, or all "
    "three -- decoded to text, without triggering a conversion.",
    {"channel": int},
    lambda ctx, channel=THERMO_CHANNEL_ALL: _client_query(
        ctx.thermo,
        "THERMO",
        ThermoQueryError,
        lambda faults: "\n".join(f.describe() for f in faults) or "device reported no channels",
        lambda client: client.read_faults(channel),
    ),
)
_register(
    "Thermo: Read Register",
    "Raw MAX31856 register read on one channel (debug), 1-16 bytes.",
    {"channel": int, "reg": int, "length": int},
    lambda ctx, channel, reg, length=1: _client_query(
        ctx.thermo,
        "THERMO",
        ThermoQueryError,
        lambda regs: regs.describe(),
        lambda client: client.read_reg(channel, reg, length),
    ),
)

# ---------------------------------------------------------------------------
# IO -- SX1509 expander (task 2)
#
# Relay numbering is the schematic's (Relay1..4 = expander bits 0..3), which
# is NOT the K-designator order -- every description spells out the K and
# terminal-block names so a caller can't energize the wrong contactor by
# assuming Relay1 means K1.
# ---------------------------------------------------------------------------
def _describe_relay_result(result: "devices.RelayResult") -> str:
    if result.ok:
        return "ok"
    detail = f": {result.reason_text}" if result.reason_text and result.refusal is devices.RelayRefusal.OTHER else ""
    return f"refused ({result.refusal.value}){detail}"


_register(
    "IO: Set Relay",
    "Switch one relay: 1=K3/J8, 2=K1/J3, 3=K2/J4, 4=K5/J11. on=true energizes the coil. "
    "Waits briefly for a refusal reply (owned / safety / updating / out of range) before "
    "reporting success, since the firmware answers a rejected relay command but not a "
    "successful one.",
    {"relay": int, "on": bool},
    lambda ctx, relay, on: _gated_client_query(
        ctx, ctx.io, "IO", IoQueryError, _describe_relay_result,
        lambda client: client.set_relay(relay, on),
    ),
)
_register(
    "IO: Set Relay Mask",
    "Switch several relays in one atomic register write. Bits 0-3 = Relay1..4 "
    "(K3/J8, K1/J3, K2/J4, K5/J11) in both mask (which to change) and value. Same "
    "refusal-aware wait as \"IO: Set Relay\".",
    {"mask": int, "value": int},
    lambda ctx, mask, value: _gated_client_query(
        ctx, ctx.io, "IO", IoQueryError, _describe_relay_result,
        lambda client: client.set_relay_mask(mask, value),
    ),
)
_register(
    "IO: All Relays Off",
    "De-energize all four relays unconditionally -- the same state the firmware "
    "falls back to on link loss or a safety fault.",
    {},
    lambda ctx: _gated_client_query(
        ctx, ctx.io, "IO", IoQueryError, lambda result: result.describe(),
        lambda client: client.all_relays_off(),
    ),
)
_register(
    "IO: Set Digital Output",
    "Drive digital I/O 1-7 high or low (only meaningful when it is an output). "
    "IO1 is the opto input from J24, IO2 the opto output to J25, IO3/IO4 J20, "
    "IO5/IO6 J21, IO7 J23.",
    {"io": int, "level": bool},
    lambda ctx, io, level: _send(ctx, UART_TASK_ID_IO, devices.io_set_io(io, level)),
)
_register(
    "IO: Set Direction",
    "Set digital I/O 1-7 as input (is_input=true) or output, with an optional "
    "pull-up on inputs.",
    {"io": int, "is_input": bool, "pullup": bool},
    lambda ctx, io, is_input, pullup=False: _send(
        ctx, UART_TASK_ID_IO, devices.io_set_io_dir(io, is_input, pullup)
    ),
)
_register(
    "IO: Set Auto Report",
    "Push unsolicited expander state every period_ms ms (and immediately on every "
    "~INT edge). period_ms=0 turns reporting off.",
    {"period_ms": int},
    lambda ctx, period_ms=500: _send(
        ctx, UART_TASK_ID_IO, devices.io_set_auto_report(period_ms)
    ),
)
_register(
    "IO: Read",
    "Read the expander: raw registers, relay shadow, digital I/O levels and "
    "directions, and the three thermocouple DRDY bits.",
    {},
    lambda ctx: _client_query(
        ctx.io, "IO", IoQueryError, lambda state: state.describe(), lambda client: client.read()
    ),
)
_register(
    "IO: Scan Addresses",
    "Probe 0x3E/0x3F/0x70/0x71 on the device's I2C bus; report which answered. "
    "The board straps ADDR1/ADDR0 to GND, so only 0x3E should.",
    {},
    lambda ctx: _client_query(
        ctx.io,
        "IO",
        IoQueryError,
        lambda found: (
            ", ".join(f"0x{a:02X}" for a in found)
            if found
            else "no device answered (expected 0x3E)"
        ),
        lambda client: client.scan(),
    ),
)
_register(
    "Expander: Read Register",
    "Raw SX1509 register read (debug), 1-16 bytes.",
    {"reg": int, "length": int},
    lambda ctx, reg, length=1: _client_query(
        ctx.io,
        "IO",
        IoQueryError,
        lambda regs: regs.describe(),
        lambda client: client.read_reg(reg, length),
    ),
)
_register(
    "Expander: Write Register",
    "Raw SX1509 register write (debug).",
    {"reg": int, "value": int},
    lambda ctx, reg, value: _send(ctx, UART_TASK_ID_IO, devices.sx_write_reg(reg, value)),
)
_register(
    "Expander: Set Direction Mask",
    "Write RegDir directly: u16, bit N = 1 makes expander pin N an input.",
    {"mask": int},
    lambda ctx, mask: _send(ctx, UART_TASK_ID_IO, devices.sx_set_dir(mask)),
)
_register(
    "Expander: Set Pullup Mask",
    "Write RegPullUp directly: u16, bit N = 1 enables pin N's pull-up.",
    {"mask": int},
    lambda ctx, mask: _send(ctx, UART_TASK_ID_IO, devices.sx_set_pullup(mask)),
)
_register(
    "Expander: Set Open Drain Mask",
    "Write RegOpenDrain directly: u16, bit N = 1 makes pin N open-drain.",
    {"mask": int},
    lambda ctx, mask: _send(ctx, UART_TASK_ID_IO, devices.sx_set_opendrain(mask)),
)
_register(
    "Expander: Set Debounce",
    "Enable debounce on the masked pins; config 0-7 selects 0.5ms << config.",
    {"enable_mask": int, "config": int},
    lambda ctx, enable_mask, config=0: _send(
        ctx, UART_TASK_ID_IO, devices.sx_set_debounce(enable_mask, config)
    ),
)
_register(
    "Expander: Set Interrupt Mask",
    "Write RegInterruptMask (bit N = 1 DISABLES pin N's interrupt) and RegSense "
    "(2 bits per pin pair, as the part encodes them).",
    {"mask": int, "sense": int},
    lambda ctx, mask, sense=0: _send(
        ctx, UART_TASK_ID_IO, devices.sx_set_int_mask(mask, sense)
    ),
)
_register(
    "Expander: LED Driver",
    "Enable/disable the SX1509's LED driver on one pin (0-15) with an intensity "
    "0-255 (0 is full on for this part's sink driver).",
    {"pin": int, "enable": bool, "intensity": int},
    lambda ctx, pin, enable, intensity=0: _send(
        ctx, UART_TASK_ID_IO, devices.sx_led_driver(pin, enable, intensity)
    ),
)
_register(
    "Expander: Reset",
    "Reset the SX1509: hard=false is a software reset via RegReset, hard=true "
    "pulses the ~RESET pin on GPIO10.",
    {"hard": bool},
    lambda ctx, hard=False: _send(ctx, UART_TASK_ID_IO, devices.sx_reset(hard)),
)

# ---------------------------------------------------------------------------
# DISPLAY -- ILI9488 (task 4)
#
# Colors are RGB565 u16. Callers that think in 8-bit RGB should build one with
# devices.rgb565(r, g, b) rather than guessing at the packing.
# ---------------------------------------------------------------------------
_register(
    "Display: Reset",
    "Reset the panel: hard=false sends the software reset command, hard=true "
    "pulses ~RESET through the expander.",
    {"hard": bool},
    lambda ctx, hard=False: _send(
        ctx, UART_TASK_ID_DISPLAY, devices.display_reset(hard)
    ),
)
_register(
    "Display: Set Power",
    "Turn the panel on, or off (display-off + sleep-in).",
    {"on": bool},
    lambda ctx, on: _send(ctx, UART_TASK_ID_DISPLAY, devices.display_set_power(on)),
)
_register(
    "Display: Set Rotation",
    "MADCTL rotation 0-3: 0/2 portrait 320x480, 1/3 landscape 480x320.",
    {"rotation": int},
    lambda ctx, rotation: _send(
        ctx, UART_TASK_ID_DISPLAY, devices.display_set_rotation(rotation)
    ),
)
_register(
    "Display: Set Invert",
    "Invert (or restore) the panel's display polarity.",
    {"invert": bool},
    lambda ctx, invert: _send(
        ctx, UART_TASK_ID_DISPLAY, devices.display_set_invert(invert)
    ),
)
_register(
    "Display: Clear",
    "Fill the whole screen with one RGB565 color (default black).",
    {"color": int},
    lambda ctx, color=0x0000: _send(
        ctx, UART_TASK_ID_DISPLAY, devices.display_clear(color)
    ),
)
_register(
    "Display: Fill Rect",
    "Fill a rectangle with an RGB565 color.",
    {"x": int, "y": int, "w": int, "h": int, "color": int},
    lambda ctx, x, y, w, h, color: _send(
        ctx, UART_TASK_ID_DISPLAY, devices.display_fill_rect(x, y, w, h, color)
    ),
)
_register(
    "Display: Draw Rect",
    "Draw a 1px rectangle outline in an RGB565 color.",
    {"x": int, "y": int, "w": int, "h": int, "color": int},
    lambda ctx, x, y, w, h, color: _send(
        ctx, UART_TASK_ID_DISPLAY, devices.display_draw_rect(x, y, w, h, color)
    ),
)
_register(
    "Display: Draw Line",
    "Draw a line from (x0,y0) to (x1,y1) in an RGB565 color.",
    {"x0": int, "y0": int, "x1": int, "y1": int, "color": int},
    lambda ctx, x0, y0, x1, y1, color: _send(
        ctx, UART_TASK_ID_DISPLAY, devices.display_draw_line(x0, y0, x1, y1, color)
    ),
)
_register(
    "Display: Set Text Cursor",
    "Move the text cursor to a pixel position (top-left of the next glyph).",
    {"x": int, "y": int},
    lambda ctx, x, y: _send(
        ctx, UART_TASK_ID_DISPLAY, devices.display_set_text_cursor(x, y)
    ),
)
_register(
    "Display: Set Text Style",
    "Set text foreground/background RGB565 colors, integer scale 1-8, and whether "
    "the background is painted.",
    {"fg": int, "bg": int, "size": int, "opaque_background": bool},
    lambda ctx, fg, bg=0x0000, size=1, opaque_background=True: _send(
        ctx,
        UART_TASK_ID_DISPLAY,
        devices.display_set_text_style(fg, bg, size, opaque_background),
    ),
)
_register(
    "Display: Print",
    "Draw ASCII text at the cursor, which advances and wraps at the right edge.",
    {"text": str},
    lambda ctx, text: _send(ctx, UART_TASK_ID_DISPLAY, devices.display_print(text)),
)
_register(
    "Display: Read ID",
    "Read the panel's RDDID bytes and its current (rotated) width/height -- the "
    "only way to tell a wired-up panel from commands vanishing into thin air.",
    {},
    lambda ctx: _client_query(
        ctx.display,
        "DISPLAY",
        DisplayQueryError,
        lambda ident: ident.describe(),
        lambda client: client.read_id(),
    ),
)


def _display_blit(ctx: ActionContext, pixels: bytes, x: int, y: int, w: int, h: int) -> str:
    """Shared tail of the two image actions: stream and report.

    Not routed through _send: a blit is thousands of frames with a strict
    ordering contract, which DisplayClient.blit owns (see display.py).
    """
    if ctx.display is None:
        return "error: no DISPLAY client registered in this context"
    if ctx.info.compatible is not True:
        reason = (
            "protocol version mismatch"
            if ctx.info.compatible is False
            else "firmware version not yet confirmed"
        )
        ctx.session_log.error("refused blit: %s", reason)
        return f"error: refused - {reason}"
    try:
        frames = ctx.display.blit(x, y, w, h, pixels)
    except (BlitError, ValueError) as exc:
        ctx.session_log.error("blit failed: %s", exc)
        return f"error: {exc}"
    ctx.session_log.info("blit %dx%d at (%d,%d) -> %d frames", w, h, x, y, frames)
    return f"ok - streamed {w}x{h} at ({x},{y}) in {frames} frames"


def _display_send_image(
    ctx: ActionContext,
    path: str,
    x: int = 0,
    y: int = 0,
    width: int = devices.DISPLAY_NATIVE_WIDTH,
    height: int = devices.DISPLAY_NATIVE_HEIGHT,
    fit: bool = True,
) -> str:
    from .display import image_to_rgb565

    try:
        pixels, out_w, out_h = image_to_rgb565(path, width, height, fit=fit)
    except RuntimeError as exc:  # Pillow missing -- carries the install hint
        return f"error: {exc}"
    except (OSError, ValueError) as exc:
        return f"error: could not load {path}: {exc}"
    return _display_blit(ctx, pixels, x, y, out_w, out_h)


_register(
    "Display: Send Image",
    "Load an image file (PNG/JPEG/...), scale it to the given size and stream it "
    "to the panel with BLIT_BEGIN/DATA/END. Requires Pillow. fit=true letterboxes "
    "to preserve aspect ratio; fit=false stretches. NOTE: at 115200 baud a full "
    "480x320 frame takes on the order of a minute.",
    {"path": str, "x": int, "y": int, "width": int, "height": int, "fit": bool},
    _display_send_image,
)


def _display_test_pattern(
    ctx: ActionContext,
    x: int = 0,
    y: int = 0,
    width: int = devices.DISPLAY_NATIVE_WIDTH,
    height: int = devices.DISPLAY_NATIVE_HEIGHT,
) -> str:
    from .display import test_pattern_rgb565

    try:
        pixels = test_pattern_rgb565(width, height)
    except ValueError as exc:
        return f"error: {exc}"
    return _display_blit(ctx, pixels, x, y, width, height)


_register(
    "Display: Test Pattern",
    "Generate colour bars plus a grey ramp and stream them to the panel -- "
    "exercises the whole blit path and makes a swapped colour channel or a lossy "
    "RGB565 expansion obvious. Needs no image file and no Pillow.",
    {"x": int, "y": int, "width": int, "height": int},
    _display_test_pattern,
)

# ---------------------------------------------------------------------------
# SAFETY -- the isolated RP2040 link (task 7)
# ---------------------------------------------------------------------------
_register(
    "Safety: Get Status",
    "Read the cached safety status: link state, E-stop, safety relay K4, heating "
    "enable, the safety thermocouple and the three current-sense channels. The "
    "RP2040 firmware does not exist yet, so 'link up' being absent with age "
    "'never received' is the expected result today.",
    {},
    lambda ctx: _client_query(
        ctx.safety,
        "SAFETY",
        SafetyQueryError,
        lambda status: status.describe(),
        lambda client: client.get_status(),
    ),
)
_register(
    "Safety: Get Link Stats",
    "Read the ESP's own counters for the isolated UART (sent/received/errors/"
    "timeouts and the poll period). These move even while the far side is silent.",
    {},
    lambda ctx: _client_query(
        ctx.safety,
        "SAFETY",
        SafetyQueryError,
        lambda stats: stats.describe(),
        lambda client: client.get_link_stats(),
    ),
)
_register(
    "Safety: Request Enable",
    "Ask the safety processor to permit (or drop) heating. Advisory only -- the "
    "Pico can refuse, and its own interlocks always win.",
    {"enable": bool},
    lambda ctx, enable: _send(
        ctx, UART_TASK_ID_SAFETY, devices.safety_request_enable(enable)
    ),
)
_register(
    "Safety: Ping",
    "Force an immediate poll of the safety processor instead of waiting for the "
    "next poll tick.",
    {},
    lambda ctx: _send(ctx, UART_TASK_ID_SAFETY, devices.safety_ping()),
)
_register(
    "Safety: Set Poll Period",
    "Set how often the ESP polls the safety processor, in ms (0 stops polling).",
    {"period_ms": int},
    lambda ctx, period_ms: _send(
        ctx, UART_TASK_ID_SAFETY, devices.safety_set_poll_period(period_ms)
    ),
)
_register(
    "Safety: Set Fault Out",
    "Drive the isolated Fault line (ESP GPIO6 -> U1 -> the Pico's mainFault). "
    "This is an OUTPUT we assert to tell the safety processor the main controller "
    "has faulted; it is not a signal from the Pico. Manual override of a line the "
    "firmware otherwise asserts by itself on PC-link loss, a thermocouple fault, "
    "or the watchdog.",
    {"assert_fault": bool},
    lambda ctx, assert_fault: _send(
        ctx, UART_TASK_ID_SAFETY, devices.safety_set_fault_out(assert_fault)
    ),
)

# ---------------------------------------------------------------------------
# INFO queries
# ---------------------------------------------------------------------------
_register(
    "INFO: Get Pin Config",
    "Report which GPIOs the running firmware has wired up, and to what.",
    {},
    lambda ctx: _info_query(
        ctx,
        lambda entries: (
            "\n".join(f"GPIO{e.gpio}: {e.label} [{e.abbrev}]" for e in entries)
            if entries
            else "device reported no pin entries"
        ),
        lambda info: info.get_pin_config(),
    ),
)
_register(
    "INFO: Get FW Version",
    "Report firmware git commit/dirty flag/build time and protocol-version compatibility.",
    {},
    lambda ctx: _info_query(
        ctx,
        lambda version: (
            # PC<->ESP link number (UART_PROTOCOL_VERSION), not the
            # ESP<->Pico KILNLINK_PROTOCOL_VERSION -- see mcp_server_info.py.
            f"uart_protocol_version: {version.protocol_version}\n"
            f"compatible: {'yes' if version.compatible else 'NO'}\n"
            f"commit: {version.commit}\n"
            f"tree: {'dirty' if version.dirty else 'clean'}\n"
            f"built: {version.built}"
        ),
        lambda info: info.get_fw_version(),
    ),
)

# ---------------------------------------------------------------------------
# SYSTEM (link recovery)
# ---------------------------------------------------------------------------
_register(
    "System: Restart UART",
    "Flush the ESP's UART RX ring buffer and reset its rx-error counter -- "
    "a recovery lever for a stuck/desynced link, without power-cycling the board.",
    {},
    lambda ctx: _send(ctx, UART_TASK_ID_SYSTEM, devices.system_restart_uart()),
)
_register(
    "System: Factory Reset",
    "Erase NVS-backed configuration and reboot the device (SYSTEM_CMD_FACTORY_RESET). "
    "scope (required, no default -- pick deliberately): 0=wifi (saved networks, AP "
    "identity), 1=kiln (zones, PID), 2=profiles (fire profiles), 3=all. The GUI's "
    "Danger Zone button gates this behind an askyesno dialog plus typing RESET; "
    "confirm=True is this path's equivalent -- confirm=False (the default) is refused "
    "so a bare call can never erase anything. No reply frame; the device reboots "
    "~500ms after the ACK, so poll \"INFO: Get FW Version\" (or get_fw_version) to "
    "confirm it came back up. Unlike load_config_preset()'s factory-reset path, this "
    "does NOT apply any preset afterward -- it only erases.",
    {"scope": int, "confirm": bool},
    lambda ctx, scope, confirm=False: (
        "error: factory reset refused without confirm=True -- this erases NVS-backed "
        "configuration and reboots the device"
        if confirm is not True
        else _send(ctx, UART_TASK_ID_SYSTEM, devices.system_factory_reset(scope))
    ),
    required=frozenset({"scope", "confirm"}),
)
_register(
    "System: Get Watchdog Panic Disabled",
    "Query whether the ESP task-watchdog's PANIC half is disabled (dev-only bench "
    "escape hatch -- see watchdog_cfg.h). The watchdog's monitoring/logging and the "
    "RTC watchdog are unaffected either way.",
    {},
    lambda ctx: _client_query(
        ctx.system,
        "SYSTEM",
        SystemQueryError,
        lambda disabled: f"watchdog panic disabled: {disabled}",
        lambda system: system.get_watchdog_panic_disabled(),
    ),
)
_register(
    "System: Set Watchdog Panic Disabled",
    "Enable/disable the ESP task-watchdog's PANIC half. Development-only -- with it "
    "disabled a hung task leaves the board sitting hung with relays in whatever state "
    "they were last commanded instead of rebooting. The GUI gates this behind an "
    "askyesno dialog; confirm=True is this path's equivalent -- confirm=False (the "
    "default) is refused. No reply frame; poll \"System: Get Watchdog Panic Disabled\" "
    "afterward to confirm the applied value.",
    {"disabled": bool, "confirm": bool},
    lambda ctx, disabled, confirm=False: (
        "error: watchdog panic change refused without confirm=True -- this changes "
        "whether a hung task reboots the board"
        if confirm is not True
        else _send(ctx, UART_TASK_ID_SYSTEM, devices.system_set_watchdog_panic_disabled(disabled))
    ),
    required=frozenset({"disabled", "confirm"}),
)

# ---------------------------------------------------------------------------
# Port / connection (host-side, no device round trip)
# ---------------------------------------------------------------------------
def _connect(ctx: ActionContext, port: Optional[str] = None) -> str:
    from . import settings

    if ctx.link.is_connected:
        return f"already connected to {ctx.link.port}"
    try:
        opened = ctx.link.connect(port)
    except Exception as exc:  # noqa: BLE001 - surfaced to the caller as text
        ctx.session_log.error("connect to %s failed: %s", port or "<auto>", exc)
        return f"error: {exc}"
    ctx.session_log.start_session(f"connected to {opened} @ {ctx.link.baudrate} baud")
    settings.set_last_port(opened)
    return f"connected to {opened} @ {ctx.link.baudrate} baud"


def _disconnect(ctx: ActionContext) -> str:
    if not ctx.link.is_connected:
        return "not connected"
    port = ctx.link.port
    ctx.link.disconnect()
    ctx.session_log.info("disconnected from %s", port)
    return f"disconnected from {port}"


_register("Port: Connect", "Open the serial link. Omit port to autodiscover.", {"port": str}, _connect)
_register("Port: Disconnect", "Close the serial link.", {}, _disconnect)


def _refresh_ports(ctx: ActionContext) -> str:
    infos = list_ports()
    if not infos:
        return "no serial ports found"
    return "\n".join(
        f"{p.device}: {p.description or '<no description>'} score={p.score}"
        + ("  (recommended)" if p.recommended else "")
        for p in infos
    )


_register("Port: Refresh Ports", "List serial ports with autodiscovery scoring.", {}, _refresh_ports)

# ---------------------------------------------------------------------------
# Logs
# ---------------------------------------------------------------------------
def _set_keep_logs(ctx: ActionContext, count: int) -> str:
    kept = ctx.session_log.set_keep(count)
    return f"keeping the {kept} most recent log file(s)"


def _open_log_folder(ctx: ActionContext) -> str:
    import subprocess
    import sys

    directory = ctx.session_log.log_dir
    try:
        directory.mkdir(parents=True, exist_ok=True)
        if sys.platform == "win32":
            subprocess.Popen(["explorer", str(directory)])
        elif sys.platform == "darwin":  # pragma: no cover - not this project's target
            subprocess.Popen(["open", str(directory)])
        else:  # pragma: no cover
            subprocess.Popen(["xdg-open", str(directory)])
    except OSError as exc:
        return f"error: could not open {directory}: {exc}"
    return f"opened {directory}"


_register("Logs: Set Keep Logs", "Set how many session log files to retain.", {"count": int}, _set_keep_logs)
_register("Logs: Open Log Folder", "Open the session-log directory in the OS file manager.", {}, _open_log_folder)
