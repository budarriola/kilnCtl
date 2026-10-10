"""factory_reset_guard.py -- backup-first rule for every PcTools FACTORY_RESET path.

docs/audits/KILN_NVS_LOSS_2026-10-09.md: an untraced host factory_reset wiped
zones/estop/crash record on the bench with no backup to restore from. Every
PcTools path that sends SYSTEM_CMD_FACTORY_RESET (the "System: Factory Reset"
action, factory_default_then_load_preset, the GUI Danger Zone) calls
:func:`backup_before_reset` first and refuses to send when it fails, unless the
caller passed ``skip_backup=True`` explicitly.

Pure helper: no board writes, GET /api/backup/export only.
"""
from __future__ import annotations

import os
import time
from typing import Callable, Optional, Tuple

from . import backup_export_http_client

#: Same directory backup_export() defaults to (gitignored).
BACKUP_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))))), "logs", "backup_export")


def backup_before_reset(
    host: Optional[str] = None,
    skip_backup: bool = False,
    *,
    exporter: "Optional[Callable[[str], Tuple[str, dict]]]" = None,
    out_dir: Optional[str] = None,
) -> "Tuple[bool, str]":
    """Export a backup before a factory reset.

    Returns ``(ok, text)``. ``ok`` False means REFUSE the reset; ``text`` is the
    error. ``ok`` True: ``text`` is the saved path (or a note that the backup was
    explicitly skipped). Only ``skip_backup is True`` exactly skips.
    """
    if skip_backup is True:
        return True, "backup SKIPPED (skip_backup=True)"
    if not host:
        from . import ota_http
        host = ota_http.OTA_AP_DEFAULT_HOST
    fetch = exporter or backup_export_http_client.get_export
    try:
        raw_text, _parsed = fetch(host)
    except Exception as exc:  # any failure must refuse the reset
        return False, (f"error: factory reset refused -- pre-reset backup export failed "
                       f"(host={host}): {exc}. Fix the export or pass skip_backup=True "
                       f"to reset without a backup.")
    directory = out_dir or BACKUP_DIR
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    path = os.path.join(directory, f"kilnctl_backup_prereset_{stamp}.json")
    try:
        os.makedirs(directory, exist_ok=True)
        with open(path, "wb") as f:
            f.write(raw_text.encode("utf-8"))
    except OSError as exc:
        return False, f"error: factory reset refused -- could not save the pre-reset backup: {exc}"
    return True, path
