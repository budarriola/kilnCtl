"""KilnCtrl -- PC-side implementation of the ESP32-S3 hardened UART protocol.

Mirrors App/drivers/espInterfaces/uart_protocol.{c,h} and
App/drivers/uart_task_ids.h. See protocol.py for the wire format, and
docs/HARDWARE.md for what is actually on the other end of each task.
"""

from __future__ import annotations

from .protocol import (
    Device,
    Frame,
    FrameDecoder,
    FrameError,
    INFO_CMD_GET_FW_VERSION,
    INFO_CMD_GET_PIN_CONFIG,
    INFO_CMD_GET_STACK_MARGIN,
    MsgType,
    AvgMode,
    PinFunction,
    SafetyFlag,
    StackMarginLevel,
    TcType,
    ThermoFault,
    ThermoReadFlag,
    UART_PROTOCOL_VERSION,
    UART_TASK_ID_DISPLAY,
    UART_TASK_ID_INFO,
    UART_TASK_ID_IO,
    UART_TASK_ID_LOG,
    UART_TASK_ID_SAFETY,
    UART_TASK_ID_SYSTEM,
    UART_TASK_ID_THERMO,
    crc16_ccitt_false,
)
from .devices import (
    DisplayId,
    ExpanderRegisters,
    FirmwareVersion,
    IoState,
    LogLine,
    PinConfigEntry,
    SafetyLinkStats,
    SafetyStatus,
    StackMarginEntry,
    ThermoFaultStatus,
    ThermoReading,
    ThermoRegisters,
)
from .display import DisplayClient, DisplayQueryError
from .info import InfoClient, InfoQueryError
from .io_expander import IoClient, IoQueryError
from .safety import SafetyClient, SafetyQueryError
from .serial_link import PortInfo, SendResult, UartLink, list_ports, recommend_port
from .thermo import ThermoClient, ThermoQueryError

__version__ = "0.2.0"

__all__ = [
    "AvgMode",
    "Device",
    "DisplayClient",
    "DisplayId",
    "DisplayQueryError",
    "ExpanderRegisters",
    "FirmwareVersion",
    "Frame",
    "FrameDecoder",
    "FrameError",
    "INFO_CMD_GET_FW_VERSION",
    "INFO_CMD_GET_PIN_CONFIG",
    "INFO_CMD_GET_STACK_MARGIN",
    "InfoClient",
    "InfoQueryError",
    "IoClient",
    "IoQueryError",
    "IoState",
    "LogLine",
    "MsgType",
    "PinConfigEntry",
    "PinFunction",
    "PortInfo",
    "SafetyClient",
    "SafetyFlag",
    "SafetyLinkStats",
    "SafetyQueryError",
    "SafetyStatus",
    "SendResult",
    "StackMarginEntry",
    "StackMarginLevel",
    "TcType",
    "ThermoClient",
    "ThermoFault",
    "ThermoFaultStatus",
    "ThermoQueryError",
    "ThermoReadFlag",
    "ThermoReading",
    "ThermoRegisters",
    "UART_PROTOCOL_VERSION",
    "UART_TASK_ID_DISPLAY",
    "UART_TASK_ID_INFO",
    "UART_TASK_ID_IO",
    "UART_TASK_ID_LOG",
    "UART_TASK_ID_SAFETY",
    "UART_TASK_ID_SYSTEM",
    "UART_TASK_ID_THERMO",
    "UartLink",
    "crc16_ccitt_false",
    "list_ports",
    "recommend_port",
]
