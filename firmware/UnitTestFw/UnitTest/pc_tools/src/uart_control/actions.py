"""Named-action registry: one entry per GUI button.

Backs the MCP server's generic ``press_button`` tool, which mirrors every
control the human GUI offers (``gui.py``) instead of requiring one bespoke
``@mcp.tool()`` per button. This is deliberately decoupled from ``mcp_server.py``
(no dependency on the ``mcp`` package here) so it can be exercised directly by
``pc_tools/selfcheck.py`` without hardware or an MCP client.

gui.py's own widget ``command=`` callbacks are left exactly as they were --
already tested, low blast-radius to leave alone -- this module doesn't
replace them, it's a second, generic front door onto the same underlying
``devices.py``/``info.py`` calls, useful for scripting/automation that would
otherwise need one dedicated tool per button.

Every entry's ``name`` matches the corresponding GUI button/menu-item label
(prefixed with its popup/menu, e.g. ``"OLED: Write & Show"``) so "press this
button" maps onto "call this action" with no translation needed.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

from . import devices
from .expander import ExpanderClient, ExpanderQueryError
from .info import InfoClient, InfoQueryError
from .protocol import (
    UART_TASK_ID_AD9833,
    UART_TASK_ID_DAC,
    UART_TASK_ID_OLED,
    UART_TASK_ID_PCF8575,
    UART_TASK_ID_SYSTEM,
)
from .serial_link import UartLink, list_ports


@dataclass
class ActionContext:
    """Everything an action needs to actually do something."""

    link: UartLink
    info: InfoClient
    session_log: object  # SessionLogger; typed loosely to avoid an import cycle in type checkers
    #: Owns task 7's inbox, needed only by the two PCF8575 *query* actions
    #: (read port / scan addresses). Optional so a caller that only drives
    #: write-style actions doesn't have to stand one up; those two report the
    #: absence rather than raising.
    expander: Optional[ExpanderClient] = None


@dataclass(frozen=True)
class Action:
    name: str
    description: str
    #: param name -> (type, required). Purely descriptive/self-documenting;
    #: the real validation happens inside devices.py's builders, which
    #: press_button's caller (mcp_server.py) surfaces as ValueError/TypeError.
    params: "dict[str, type]" = field(default_factory=dict)
    run: Callable[..., str] = field(repr=False, default=None)  # type: ignore[assignment]


ACTIONS: "dict[str, Action]" = {}


def _register(name: str, description: str, params: "dict[str, type]", run: Callable[..., str]) -> None:
    if name in ACTIONS:  # pragma: no cover - programmer error, not a runtime path
        raise ValueError(f"duplicate action name: {name!r}")
    ACTIONS[name] = Action(name=name, description=description, params=params, run=run)


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


# ---------------------------------------------------------------------------
# MCP4728 DAC
# ---------------------------------------------------------------------------
_register(
    "DAC: Set Channel Percent",
    "Set one DAC channel (0-3) to a 0-100 percent full-scale output.",
    {"channel": int, "percent": float},
    lambda ctx, channel, percent: _send(
        ctx, UART_TASK_ID_DAC, devices.dac_set_channel_percent(channel, percent)
    ),
)
_register(
    "DAC: Set All",
    "Set all four DAC channels at once, each 0-100 percent full scale.",
    {"p0": float, "p1": float, "p2": float, "p3": float},
    lambda ctx, p0, p1, p2, p3: _send(
        ctx, UART_TASK_ID_DAC, devices.dac_set_all_percent([p0, p1, p2, p3])
    ),
)
_register(
    "DAC: Power Down",
    "Power down DAC channel (0-3) with power-down mode 0-3.",
    {"channel": int, "mode": int},
    lambda ctx, channel, mode=0: _send(
        ctx, UART_TASK_ID_DAC, devices.dac_power_down(channel, mode)
    ),
)

# ---------------------------------------------------------------------------
# AD9833
# ---------------------------------------------------------------------------
_register(
    "AD9833: Set Frequency",
    "Write a frequency in Hz into FREQ0 (reg=0) or FREQ1 (reg=1).",
    {"reg": int, "freq_hz": float},
    lambda ctx, reg, freq_hz: _send(
        ctx, UART_TASK_ID_AD9833, devices.ad9833_set_frequency(reg, freq_hz)
    ),
)
_register(
    "AD9833: Select Active Freq Reg",
    "Select which frequency register drives the output (0=FREQ0, 1=FREQ1).",
    {"reg": int},
    lambda ctx, reg: _send(ctx, UART_TASK_ID_AD9833, devices.ad9833_select_freq_reg(reg)),
)
_register(
    "AD9833: Set Phase",
    "Write a phase offset in degrees into PHASE0 (reg=0) or PHASE1 (reg=1).",
    {"reg": int, "degrees": float},
    lambda ctx, reg, degrees: _send(
        ctx, UART_TASK_ID_AD9833, devices.ad9833_set_phase(reg, degrees)
    ),
)
_register(
    "AD9833: Select Active Phase Reg",
    "Select which phase register drives the output (0=PHASE0, 1=PHASE1).",
    {"reg": int},
    lambda ctx, reg: _send(ctx, UART_TASK_ID_AD9833, devices.ad9833_select_phase_reg(reg)),
)
_register(
    "AD9833: Apply Waveform",
    "Set the output waveform: 0=sine, 1=triangle, 2=square, 3=square/2.",
    {"waveform": int},
    lambda ctx, waveform: _send(
        ctx, UART_TASK_ID_AD9833, devices.ad9833_set_waveform(waveform)
    ),
)
_register(
    "AD9833: Reset (hold)",
    "Assert the AD9833 reset bit (outputs quiesced).",
    {},
    lambda ctx: _send(ctx, UART_TASK_ID_AD9833, devices.ad9833_reset(True)),
)
_register(
    "AD9833: Reset (release)",
    "Release the AD9833 reset bit.",
    {},
    lambda ctx: _send(ctx, UART_TASK_ID_AD9833, devices.ad9833_reset(False)),
)
_register(
    "AD9833: Apply Sleep",
    "Power down the AD9833's internal DAC and/or its MCLK.",
    {"dac_power_down": bool, "mclk_power_down": bool},
    lambda ctx, dac_power_down, mclk_power_down: _send(
        ctx, UART_TASK_ID_AD9833, devices.ad9833_sleep(dac_power_down, mclk_power_down)
    ),
)

# ---------------------------------------------------------------------------
# SSD1306 OLED
# ---------------------------------------------------------------------------
_register(
    "OLED: Clear",
    "Clear the OLED's in-RAM framebuffer (does not touch the panel yet).",
    {},
    lambda ctx: _send(ctx, UART_TASK_ID_OLED, devices.oled_clear()),
)
_register(
    "OLED: Set Cursor",
    "Move the OLED's text cursor to (col, row) in text-cell coordinates.",
    {"col": int, "row": int},
    lambda ctx, col, row: _send(ctx, UART_TASK_ID_OLED, devices.oled_set_cursor(col, row)),
)
_register(
    "OLED: Print",
    "Write text into the OLED's framebuffer at the cursor (call \"OLED: Display\" to show it).",
    {"text": str},
    lambda ctx, text: _send(ctx, UART_TASK_ID_OLED, devices.oled_print(text)),
)
_register(
    "OLED: Display",
    "Flush the OLED's framebuffer to the panel.",
    {},
    lambda ctx: _send(ctx, UART_TASK_ID_OLED, devices.oled_display()),
)
_register(
    "OLED: Set Contrast",
    "Set OLED contrast, 0-255.",
    {"contrast": int},
    lambda ctx, contrast: _send(ctx, UART_TASK_ID_OLED, devices.oled_set_contrast(contrast)),
)
_register(
    "OLED: Apply Invert",
    "Invert (or restore) the OLED's display polarity.",
    {"invert": bool},
    lambda ctx, invert: _send(ctx, UART_TASK_ID_OLED, devices.oled_set_invert(invert)),
)
_register(
    "OLED: Power On",
    "Turn the OLED panel on.",
    {},
    lambda ctx: _send(ctx, UART_TASK_ID_OLED, devices.oled_set_power(True)),
)
_register(
    "OLED: Power Off",
    "Turn the OLED panel off (sleep).",
    {},
    lambda ctx: _send(ctx, UART_TASK_ID_OLED, devices.oled_set_power(False)),
)


def _oled_write_and_show(ctx: ActionContext, text: str, col: int = 0, row: int = 0) -> str:
    """Clear -> set cursor -> print -> display, same compound action as the
    GUI's "Write & Show" button (gui.py: _oled_write_and_show), stopping at
    the first step that doesn't come back OK."""
    for step_name, payload in (
        ("clear", devices.oled_clear()),
        ("set cursor", devices.oled_set_cursor(col, row)),
        ("print", devices.oled_print(text)),
        ("display", devices.oled_display()),
    ):
        outcome = _send(ctx, UART_TASK_ID_OLED, payload)
        if not outcome.startswith("ok"):
            return f"{step_name}: {outcome}"
    return "ok - write & show complete"


_register(
    "OLED: Write & Show",
    'Compound action: clear, set cursor, print text, then display -- the OLED "one call" convenience.',
    {"text": str, "col": int, "row": int},
    lambda ctx, text, col=0, row=0: _oled_write_and_show(ctx, text, col, row),
)

# ---------------------------------------------------------------------------
# PCF8575 I/O expander
#
# Writes go through _send like every other device command. The two queries
# (read port / scan) need the ExpanderClient that owns task 7's inbox, since
# their answers come back as separate DATA frames -- same shape as INFO.
# ---------------------------------------------------------------------------
def _expander_query(ctx: ActionContext, describe: Callable[[object], str],
                    query: Callable[[ExpanderClient], object]) -> str:
    if ctx.expander is None:
        return "error: no expander client registered for task 7 in this context"
    try:
        value = query(ctx.expander)
    except ExpanderQueryError as exc:
        return f"error: {exc}"
    return describe(value)


_register(
    "Expander: Write Port",
    "Write all 16 PCF8575 pins at once (bit N = pin N; 1 = weak-high/input, 0 = driven low).",
    {"value": int},
    lambda ctx, value: _send(ctx, UART_TASK_ID_PCF8575, devices.pcf8575_write_port(value)),
)
_register(
    "Expander: Write Pin",
    "Drive one PCF8575 pin (0-15) high (weak pull-up / input) or low.",
    {"pin": int, "level": bool},
    lambda ctx, pin, level: _send(
        ctx, UART_TASK_ID_PCF8575, devices.pcf8575_write_pin(pin, level)
    ),
)
_register(
    "Expander: Set Mask",
    "Take the masked PCF8575 pins high (weak pull-up / input), leaving the rest alone.",
    {"mask": int},
    lambda ctx, mask: _send(ctx, UART_TASK_ID_PCF8575, devices.pcf8575_set_mask(mask)),
)
_register(
    "Expander: Clear Mask",
    "Drive the masked PCF8575 pins low, leaving the rest alone.",
    {"mask": int},
    lambda ctx, mask: _send(ctx, UART_TASK_ID_PCF8575, devices.pcf8575_clear_mask(mask)),
)
_register(
    "Expander: Toggle Mask",
    "Invert the masked PCF8575 pins, leaving the rest alone.",
    {"mask": int},
    lambda ctx, mask: _send(ctx, UART_TASK_ID_PCF8575, devices.pcf8575_toggle_mask(mask)),
)
_register(
    "Expander: Set Address",
    "Re-target the running firmware at another PCF8575 address (0x20-0x27, i.e. 32-39).",
    {"addr": int},
    lambda ctx, addr: _send(ctx, UART_TASK_ID_PCF8575, devices.pcf8575_set_address(addr)),
)
_register(
    "Expander: Read Port",
    "Read the PCF8575's live pin states, plus the firmware's output shadow and current address.",
    {},
    lambda ctx: _expander_query(
        ctx, lambda port: port.describe(), lambda client: client.read_port()
    ),
)
_register(
    "Expander: Scan Addresses",
    "Probe 0x20-0x27 on the device's I2C bus and report which addresses answered.",
    {},
    lambda ctx: _expander_query(
        ctx,
        lambda found: (
            ", ".join(f"0x{a:02X}" for a in found)
            if found
            else "no device answered in 0x20-0x27"
        ),
        lambda client: client.scan(),
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
            f"protocol_version: {version.protocol_version}\n"
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
