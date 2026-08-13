"""Per-session file logging with semantic rollover and a retention count.

One log file per *session*, where a session starts when either

  1. :meth:`UartLink.connect` succeeds in the GUI, or
  2. the device reboots -- detected as an unsolicited FW-version push on task
     INFO, which ``info_boot_push_task()`` sends exactly once per boot.

Both triggers are semantic, which is why this uses a plain
:class:`logging.FileHandler` swapped out by hand rather than
``RotatingFileHandler`` / ``TimedRotatingFileHandler``: neither size nor time
is what rolls this over.

Files land in ``pc_tools/logs/session_YYYYmmdd_HHMMSS.log``. After each
rollover the oldest files beyond the configured keep-count are deleted. That
count is persisted in the shared ``settings.json`` (see :mod:`kilnctrl.settings`).
"""

from __future__ import annotations

import logging
import queue
from datetime import datetime
from pathlib import Path
from typing import Optional

from . import settings

log = logging.getLogger(__name__)

#: pc_tools/  (this file is pc_tools/src/kilnctrl/session_log.py)
PROJECT_DIR = Path(__file__).resolve().parents[2]
LOG_DIR = PROJECT_DIR / "logs"

LOG_FILENAME_PREFIX = "session_"
LOG_FILENAME_SUFFIX = ".log"
LOG_TIMESTAMP_FORMAT = "%Y%m%d_%H%M%S"

DEFAULT_KEEP_LOGS = 10
MIN_KEEP_LOGS = 1
MAX_KEEP_LOGS = 1000

_LOG_FORMAT = "%(asctime)s %(levelname)-7s %(message)s"
_LOG_DATE_FORMAT = "%Y-%m-%d %H:%M:%S"


def clamp_keep(value: int) -> int:
    """Clamp a retention count into [MIN_KEEP_LOGS, MAX_KEEP_LOGS]."""
    return max(MIN_KEEP_LOGS, min(MAX_KEEP_LOGS, int(value)))


# ---------------------------------------------------------------------------
# in-app log view feed
# ---------------------------------------------------------------------------
class QueueLogHandler(logging.Handler):
    """Handler that formats records into a queue for the Tk thread to drain.

    A handler is called on whatever thread logged, which for sends is a worker
    thread -- so it must not touch widgets. Handing formatted lines to a queue
    keeps that boundary the same way gui.py already marshals send results.
    """

    def __init__(self, sink: "queue.Queue[str]") -> None:
        super().__init__()
        self.sink = sink
        self.setFormatter(logging.Formatter(_LOG_FORMAT, _LOG_DATE_FORMAT))

    def emit(self, record: logging.LogRecord) -> None:
        try:
            self.sink.put_nowait(self.format(record))
        except queue.Full:  # pragma: no cover - view is behind; drop the line
            pass


class _ForwardToSessionHandler(logging.Handler):
    """Re-emits records from the ``kilnctrl`` package into a SessionLogger.

    Kept as a forwarder rather than just attaching the session's own
    FileHandler to the package logger, because SessionLogger swaps its file
    handler out at every session boundary -- forwarding through the logger
    means package records always land in whatever file is current, with no
    re-attaching needed on rollover.
    """

    def __init__(self, session: "SessionLogger") -> None:
        super().__init__()
        self.session = session

    def emit(self, record: logging.LogRecord) -> None:
        try:
            # Prefix with the originating module so these are visibly not
            # the app's own session lines.
            self.session.logger.log(record.levelno, "%s: %s", record.name, record.getMessage())
        except Exception:  # pragma: no cover - logging must never raise
            pass


# ---------------------------------------------------------------------------
# session logger
# ---------------------------------------------------------------------------
class SessionLogger:
    """A logger whose file handler is swapped at each session boundary."""

    def __init__(
        self,
        name: str = "kilnctrl.session",
        log_dir: Path = LOG_DIR,
        settings_path: Path = settings.SETTINGS_PATH,
    ) -> None:
        self.log_dir = Path(log_dir)
        self.settings_path = Path(settings_path)
        self.keep = clamp_keep(settings.get_keep_logs(DEFAULT_KEEP_LOGS, self.settings_path))

        self.logger = logging.getLogger(name)
        self.logger.setLevel(logging.INFO)
        # Session lines are the GUI's own record; don't also push them through
        # the root logger's handlers (the MCP server configures those).
        self.logger.propagate = False

        self._handler: Optional[logging.FileHandler] = None
        self.path: Optional[Path] = None

    # -- feeds -------------------------------------------------------------
    def attach_view(self, sink: "queue.Queue[str]") -> QueueLogHandler:
        """Also feed every record into ``sink`` for the in-app log view."""
        handler = QueueLogHandler(sink)
        self.logger.addHandler(handler)
        return handler

    def capture_package_logs(self, package: str = "kilnctrl", level: int = logging.WARNING) -> None:
        """Route this package's own module-level logging into the session log.

        Modules like :mod:`kilnctrl.serial_link` and
        :mod:`kilnctrl.link_hub` log genuinely diagnostic things through
        their own ``logging.getLogger(__name__)`` -- a serial read failing,
        the reader thread exiting, the connection to the link hub dying.
        Without this, a host that never configures logging (the GUI does
        not) drops every one of them on the floor, so the session log shows
        a link going down with no reason recorded anywhere.

        Safe against recursion: this logger is itself under ``package``, but
        it sets ``propagate = False`` (see __init__), so records logged here
        never climb back up to the package logger this handler listens on.
        """
        forwarder = _ForwardToSessionHandler(self)
        forwarder.setLevel(level)
        package_logger = logging.getLogger(package)
        package_logger.addHandler(forwarder)
        # Don't lower an explicitly-configured level, but make sure the
        # package logger is permissive enough for `level` to get through --
        # a logger left at NOTSET/WARNING default would otherwise filter
        # these out before the handler ever sees them.
        if package_logger.level == logging.NOTSET or package_logger.level > level:
            package_logger.setLevel(level)

    # -- session lifecycle -------------------------------------------------
    def start_session(self, reason: str) -> Optional[Path]:
        """Close the current log file, open a new one, and prune old ones.

        ``reason`` is recorded as the new file's first line (e.g. "connected
        to COM7", "device reboot detected"). Returns the new path, or None if
        the file could not be opened -- a logging failure must never take the
        GUI down with it.
        """
        self._close_handler()

        name = (
            f"{LOG_FILENAME_PREFIX}"
            f"{datetime.now().strftime(LOG_TIMESTAMP_FORMAT)}"
            f"{LOG_FILENAME_SUFFIX}"
        )
        path = self.log_dir / name
        try:
            self.log_dir.mkdir(parents=True, exist_ok=True)
            handler = logging.FileHandler(path, encoding="utf-8")
        except OSError:
            log.warning("could not open log file %s", path, exc_info=True)
            self.path = None
            return None

        handler.setFormatter(logging.Formatter(_LOG_FORMAT, _LOG_DATE_FORMAT))
        self.logger.addHandler(handler)
        self._handler = handler
        self.path = path

        self.logger.info("=== session started: %s ===", reason)
        self.prune()
        return path

    def _close_handler(self) -> None:
        handler, self._handler = self._handler, None
        if handler is None:
            return
        self.logger.removeHandler(handler)
        try:
            handler.close()
        except Exception:  # pragma: no cover - defensive
            log.debug("error closing log handler", exc_info=True)

    def close(self) -> None:
        """Close the current file handler. Safe to call twice."""
        if self._handler is not None:
            self.logger.info("=== session ended ===")
        self._close_handler()

    # -- retention ---------------------------------------------------------
    def existing_logs(self) -> "list[Path]":
        """Session log files, oldest first (the timestamped name sorts right)."""
        try:
            files = [
                p
                for p in self.log_dir.glob(f"{LOG_FILENAME_PREFIX}*{LOG_FILENAME_SUFFIX}")
                if p.is_file()
            ]
        except OSError:  # pragma: no cover - directory vanished
            return []
        return sorted(files, key=lambda p: p.name)

    def prune(self) -> "list[Path]":
        """Delete the oldest files beyond :attr:`keep`; return what was deleted.

        The file currently open is never deleted, even if keep is set to a
        value smaller than 1 file's worth of history.
        """
        files = self.existing_logs()
        excess = len(files) - self.keep
        deleted: list[Path] = []
        for path in files[: max(0, excess)]:
            if self.path is not None and path == self.path:
                continue
            try:
                path.unlink()
                deleted.append(path)
            except OSError:  # pragma: no cover - locked/removed elsewhere
                log.warning("could not delete old log %s", path, exc_info=True)
        if deleted:
            self.logger.info(
                "retention: kept %d log(s), deleted %d (%s)",
                self.keep,
                len(deleted),
                ", ".join(p.name for p in deleted),
            )
        return deleted

    def set_keep(self, value: int) -> int:
        """Set the retention count, persist it, and prune immediately."""
        self.keep = clamp_keep(value)
        settings.set_keep_logs(self.keep, self.settings_path)
        self.logger.info("log retention set to %d file(s)", self.keep)
        self.prune()
        return self.keep

    # -- convenience -------------------------------------------------------
    def info(self, message: str, *args) -> None:
        self.logger.info(message, *args)

    def warning(self, message: str, *args) -> None:
        self.logger.warning(message, *args)

    def error(self, message: str, *args) -> None:
        self.logger.error(message, *args)
