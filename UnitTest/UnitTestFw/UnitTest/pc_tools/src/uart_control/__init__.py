"""PC-side implementation of the ESP32-S3 hardened UART protocol.

Mirrors App/drivers/espInterfaces/uart_protocol.{c,h} and
App/drivers/uart_task_ids.h. See protocol.py for the wire format.
"""

from __future__ import annotations

from .protocol import (
    Device,
    Frame,
    FrameDecoder,
    FrameError,
    INFO_CMD_GET_FW_VERSION,
    INFO_CMD_GET_PIN_CONFIG,
    MsgType,
    PinFunction,
    UART_TASK_ID_AD9833,
    UART_TASK_ID_DAC,
    UART_TASK_ID_INFO,
    Waveform,
    crc16_ccitt_false,
)
from .devices import FirmwareVersion, PinConfigEntry
from .info import InfoClient, InfoQueryError
from .serial_link import PortInfo, SendResult, UartLink, list_ports, recommend_port

__version__ = "0.1.0"

__all__ = [
    "Device",
    "FirmwareVersion",
    "Frame",
    "FrameDecoder",
    "FrameError",
    "INFO_CMD_GET_FW_VERSION",
    "INFO_CMD_GET_PIN_CONFIG",
    "InfoClient",
    "InfoQueryError",
    "MsgType",
    "PinConfigEntry",
    "PinFunction",
    "PortInfo",
    "SendResult",
    "UartLink",
    "UART_TASK_ID_AD9833",
    "UART_TASK_ID_DAC",
    "UART_TASK_ID_INFO",
    "Waveform",
    "crc16_ccitt_false",
    "list_ports",
    "recommend_port",
]
