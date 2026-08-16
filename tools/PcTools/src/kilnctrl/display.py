"""Client for the firmware's DISPLAY task (task 4): the ILI9488 TFT on J2.

Nearly everything here is fire-and-forget drawing. The one *query* is
``READ_ID``, which has the same shape as the queries on task INFO: the request
DATA frame is ACKed for delivery only, and the answer arrives afterwards as a
**separate DATA frame** from ``(ESP, UART_TASK_ID_DISPLAY)`` addressed back to
whoever asked -- so the PC has to be registered on task 4 itself. Nothing is
ever pushed unsolicited on this task, and the reply echoes its subcommand in
byte0, so this client is the simplest of the four: one outstanding query at a
time, matched by subcommand.

The other reason this module exists rather than everything going through bare
``link.send()`` calls is :meth:`DisplayClient.blit`. A full 480x320 frame is
307200 bytes of RGB565, i.e. ~2440 protocol frames; that is a multi-second
streaming operation with a strict ordering contract (BLIT_BEGIN, then only
BLIT_DATA, then BLIT_END -- anything else in between aborts the blit on the
firmware side). Keeping it here means the ordering, the chunking and the
abort-on-failure behaviour live in one tested place instead of being
open-coded in both the GUI and the MCP server.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Callable, Optional

from . import devices
from .devices import DisplayId, DisplayResponseError
from .protocol import (
    DISPLAY_CMD_READ_ID,
    UART_TASK_ID_DISPLAY,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: How long to wait for the reply DATA frame *after* the request was ACKed.
#: READ_ID is one short SPI read plus the two expander transfers the D/C
#: toggle costs.
DEFAULT_REPLY_TIMEOUT_S = 2.0

#: Hard ceiling on a blit's geometry. Coordinates are u16 on the wire, so
#: without this a caller (or a model) could ask for 65535x65535 -- 8.6 GB of
#: RGB565 built in Python, and 68 million protocol frames, i.e. an
#: out-of-memory kill or a transfer measured in months. Generous next to the
#: 480x320 panel but far below anything that can hurt: even the maximum here
#: is a ~34-minute stream at 115200 baud.
MAX_BLIT_DIMENSION = 2048
MAX_BLIT_PIXELS = 1 << 20


def _check_blit_size(width, height) -> "tuple[int, int]":
    """Validate a blit/conversion geometry before anything allocates."""
    try:
        width = int(width)
        height = int(height)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"blit size must be two integers, got {width!r}x{height!r}") from exc
    if width <= 0 or height <= 0:
        raise ValueError(f"blit size must be positive, got {width}x{height}")
    if width > MAX_BLIT_DIMENSION or height > MAX_BLIT_DIMENSION:
        raise ValueError(
            f"blit size {width}x{height} exceeds the {MAX_BLIT_DIMENSION} px "
            "per-side limit"
        )
    if width * height > MAX_BLIT_PIXELS:
        raise ValueError(
            f"blit of {width}x{height} = {width * height} pixels exceeds the "
            f"{MAX_BLIT_PIXELS}-pixel limit"
        )
    return width, height


class DisplayQueryError(RuntimeError):
    """Raised when a DISPLAY query cannot be completed.

    :attr:`send_result` is set when the failure was at the delivery layer (the
    request never got an ACK), and None when the request was delivered but no
    valid reply came back in time.
    """

    def __init__(self, message: str, send_result: Optional[SendResult] = None) -> None:
        super().__init__(message)
        self.send_result = send_result


class BlitError(RuntimeError):
    """Raised when a blit could not be streamed to completion.

    The firmware aborts a blit on the first thing that isn't BLIT_DATA or
    BLIT_END, and a dropped frame mid-stream would silently shift every
    subsequent pixel, so :meth:`DisplayClient.blit` stops at the first failed
    send rather than pressing on and painting garbage.
    """


class _Pending:
    """A single outstanding query: what we asked for, and where to put it."""

    def __init__(self, subcommand: int) -> None:
        self.subcommand = subcommand
        self.event = threading.Event()
        self.value: object = None


class DisplayClient:
    """Owns task :data:`UART_TASK_ID_DISPLAY` on the PC side of a link.

    Thread-safety: every method here blocks on at least one UART round trip
    (:meth:`blit` on thousands) and must not be called from a GUI thread.
    """

    def __init__(self, link: UartLink, task_id: int = UART_TASK_ID_DISPLAY) -> None:
        self.link = link
        self.task_id = task_id

        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        #: Serializes queries so at most one reply is ever outstanding.
        self._query_lock = threading.RLock()
        #: Held for the whole BLIT_BEGIN..BLIT_END sequence: a second sender
        #: slipping a drawing command in between would abort the blit on the
        #: firmware side, which is exactly the failure this prevents.
        self._blit_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-display-rx", daemon=True
        )
        self._consumer.start()

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Stop the consumer thread and release task 4. Safe to call twice."""
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- writes ------------------------------------------------------------
    def send(self, payload: bytes) -> SendResult:
        """Send one non-query subcommand payload (built by ``devices.py``)."""
        return self.link.send(
            dst_task=self.task_id, src_task=self.task_id, payload=payload
        )

    # -- blit --------------------------------------------------------------
    def blit(
        self,
        x: int,
        y: int,
        width: int,
        height: int,
        pixels: bytes,
        progress: Optional[Callable[[int, int], None]] = None,
    ) -> int:
        """Stream an RGB565 image into a window on the panel.

        ``pixels`` is row-major RGB565, u16 LE -- exactly ``width * height * 2``
        bytes (see :func:`kilnctrl.devices.rgb565` and the Pillow conversion in
        :func:`image_to_rgb565`). Returns the number of BLIT_DATA frames sent.

        ``progress`` is called as ``(frames_sent, frames_total)`` after each
        chunk, so a GUI can show a bar for what is inherently a slow transfer:
        at 115200 baud a full 480x320 frame is on the order of a minute, and
        the panel is filled top-to-bottom as it arrives.

        Raises :class:`BlitError` on the first send that doesn't come back OK,
        after attempting a BLIT_END so the firmware isn't left with a window
        open.
        """
        width, height = _check_blit_size(width, height)
        expected = width * height * 2
        if len(pixels) != expected:
            raise ValueError(
                f"{width}x{height} needs {expected} bytes of RGB565, got {len(pixels)}"
            )

        # Built before BLIT_BEGIN on purpose: a chunking failure must happen
        # while no window is open on the panel.
        chunks = list(devices.iter_blit_chunks(pixels))
        total = len(chunks)
        with self._blit_lock:
            result = self.send(devices.display_blit_begin(x, y, width, height))
            if not result.ok:
                raise BlitError(f"BLIT_BEGIN failed: {result.describe()}")
            try:
                for index, chunk in enumerate(chunks, start=1):
                    result = self.send(chunk)
                    if not result.ok:
                        raise BlitError(
                            f"BLIT_DATA frame {index}/{total} failed: "
                            f"{result.describe()} -- blit aborted (every later "
                            "pixel would have landed one chunk early)"
                        )
                    if progress is not None:
                        progress(index, total)
            finally:
                # Always try to close the window, even on failure: leaving a
                # blit open would make the *next* command an error on the
                # firmware side too.
                self.send(devices.display_blit_end())
        return total

    # -- queries -----------------------------------------------------------
    def read_id(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> DisplayId:
        """Read the panel's RDDID bytes and its current (rotated) geometry.

        This is the only way to tell "the panel is wired up and answering"
        from "every drawing command has been vanishing into an unconnected
        connector", since drawing itself is fire-and-forget.
        """
        value = self._query(DISPLAY_CMD_READ_ID, devices.display_read_id(), timeout)
        return value  # type: ignore[return-value]

    def _query(self, subcommand: int, payload: bytes, timeout: float) -> object:
        with self._query_lock:
            pending = _Pending(subcommand)
            with self._pending_lock:
                self._pending = pending
            try:
                result = self.link.send(
                    dst_task=self.task_id,
                    src_task=self.task_id,
                    payload=payload,
                    dst_device=Device.ESP,
                )
                if not result.ok:
                    raise DisplayQueryError(
                        f"DISPLAY request 0x{subcommand:02X} not delivered: "
                        f"{result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise DisplayQueryError(
                        f"DISPLAY request 0x{subcommand:02X} was ACKed but no reply "
                        f"arrived within {timeout:.1f} s"
                    )
                return pending.value
            finally:
                with self._pending_lock:
                    if self._pending is pending:
                        self._pending = None

    # -- receive -----------------------------------------------------------
    def _consume_loop(self) -> None:
        while not self._stop.is_set():
            try:
                frame = self._inbox.get(timeout=0.2)
            except queue.Empty:
                continue  # poll interval so close() is noticed promptly
            try:
                self._handle_reply(frame)
            except Exception:  # pragma: no cover - never kill the consumer
                log.exception("error handling DISPLAY frame")

    def _handle_reply(self, frame: Frame) -> None:
        try:
            subcommand, value = devices.parse_display_response(frame.payload)
        except DisplayResponseError as exc:
            log.warning("dropping malformed DISPLAY response: %s", exc)
            return

        with self._pending_lock:
            pending = self._pending
        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
            return

        # Nothing outstanding: a stale reply to a query we already gave up on.
        # Nothing on this task is ever pushed unsolicited.
        log.debug("ignoring unsolicited DISPLAY response 0x%02X", subcommand)


# ---------------------------------------------------------------------------
# image conversion (Pillow)
#
# Pillow is a declared dependency (pyproject.toml) but imported lazily and
# only here: it is needed for exactly one feature -- "send this PNG to the
# panel" -- and the rest of the package (protocol, link, GUI, MCP server)
# must stay importable and usable on a machine where the wheel didn't
# install. The About window's pinout diagram deliberately still uses
# tk.PhotoImage, which reads PNG natively on Tk 8.6+.
# ---------------------------------------------------------------------------
PILLOW_MISSING_HINT = (
    "Pillow is required to send an image to the display. "
    "Install it with: uv sync --project pc_tools  (or: pip install pillow)"
)


def pillow_available() -> bool:
    """Whether image loading/scaling is usable in this environment."""
    try:
        import PIL.Image  # noqa: F401
    except ImportError:
        return False
    return True


def image_to_rgb565(
    path,
    width: int,
    height: int,
    fit: bool = True,
    background: "tuple[int, int, int]" = (0, 0, 0),
) -> "tuple[bytes, int, int]":
    """Load an image file and convert it to RGB565 bytes for :meth:`blit`.

    Returns ``(pixels, width, height)`` -- the returned size is what the caller
    should pass to BLIT_BEGIN. With ``fit`` (the default) the image is scaled
    to fit inside ``width x height`` preserving aspect ratio and letterboxed
    onto ``background``, so a photo of the wrong shape isn't silently
    stretched; with ``fit=False`` it is resampled straight to the requested
    size.

    Raises :class:`RuntimeError` with an install hint if Pillow is missing,
    and :class:`ValueError` for an absurd target size, a file that is not an
    image, or a decode that fails part-way (including Pillow's
    decompression-bomb guard, which is neither an OSError nor a ValueError of
    its own and would otherwise escape every caller's except clause).
    """
    try:
        from PIL import Image
    except ImportError as exc:  # pragma: no cover - environment dependent
        raise RuntimeError(PILLOW_MISSING_HINT) from exc

    width, height = _check_blit_size(width, height)

    try:
        with Image.open(path) as source:
            image = source.convert("RGB")
            if fit:
                canvas = Image.new("RGB", (width, height), background)
                scaled = image.copy()
                scaled.thumbnail((width, height), Image.LANCZOS)
                canvas.paste(
                    scaled, ((width - scaled.width) // 2, (height - scaled.height) // 2)
                )
                image = canvas
            else:
                image = image.resize((width, height), Image.LANCZOS)

            out = bytearray(width * height * 2)
            for index, (r, g, b) in enumerate(image.getdata()):
                color = devices.rgb565(r, g, b)
                out[index * 2] = color & 0xFF  # u16 LE
                out[index * 2 + 1] = (color >> 8) & 0xFF
    except (OSError, ValueError):
        raise  # already the shape every caller handles
    except Exception as exc:  # noqa: BLE001 - Pillow's own error taxonomy
        raise ValueError(f"could not decode {path} as an image: {exc}") from exc
    return bytes(out), width, height


def test_pattern_rgb565(width: int, height: int) -> bytes:
    """Generate a colour-bar + gradient test pattern, no Pillow required.

    Deliberately built from things that make specific failures obvious rather
    than from something pretty: eight saturated vertical bars catch a swapped
    or truncated colour channel, and the grey ramp underneath catches an
    RGB565/RGB666 expansion that has lost its low bits. Also the only way to
    exercise the whole BLIT_BEGIN/DATA/END path on a machine without Pillow.
    """
    width, height = _check_blit_size(width, height)

    bars = (
        (255, 255, 255),
        (255, 255, 0),
        (0, 255, 255),
        (0, 255, 0),
        (255, 0, 255),
        (255, 0, 0),
        (0, 0, 255),
        (0, 0, 0),
    )
    split = max(1, (height * 3) // 4)  # bars on top, ramp below
    out = bytearray(width * height * 2)
    index = 0
    for y in range(height):
        for x in range(width):
            if y < split:
                r, g, b = bars[(x * len(bars)) // width]
            else:
                level = (x * 255) // max(1, width - 1)
                r = g = b = level
            color = devices.rgb565(r, g, b)
            out[index] = color & 0xFF
            out[index + 1] = (color >> 8) & 0xFF
            index += 2
    return bytes(out)
