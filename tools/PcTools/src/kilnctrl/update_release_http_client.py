#!/usr/bin/env python3
"""update_release_http_client.py -- the one PC-side request the OT-G bench
cases need that update_http_client.py cannot make: a stage upload that is cut
off mid-body (OT-G02). urllib always sends the body it declares, so this uses
http.client directly. Everything else the OT-G cases call (settings, check,
download, fetch status, stage read/clear) lives in update_http_client.py.

Credentials are never echoed; the ADMIN session comes from http_auth.
"""
from __future__ import annotations

from typing import Optional

from . import update_http_client as _u
from .update_http_client import UpdateHttpError


def upload_stage_truncated(host: str, image: bytes, send_fraction: float = 0.6,
                           timeout: float = 60.0) -> "tuple[Optional[int], str]":
    """POST /api/update/stage declaring the FULL Content-Length but sending
    only ``send_fraction`` of the bytes, then half-closing the socket: a real
    cut-off upload (urllib cannot do this, it always sends what it declares).
    Returns (HTTP status, body) if the board answers before closing, else
    (None, "<ExcName>: ...") for a transport-level close/timeout. Uses the
    ADMIN session from http_auth. Bench-untested (written for OT-G02)."""
    import http.client
    import socket
    from . import http_auth
    err = _u.validate_upload_args(image)
    if err:
        raise UpdateHttpError(f"refusing to upload: {err}")
    cookie = http_auth.login(f"http://{host}", timeout)
    cut = max(1, min(len(image) - 1, int(len(image) * send_fraction)))
    conn = http.client.HTTPConnection(host, timeout=timeout)
    try:
        conn.putrequest("POST", _u.STAGE_PATH)
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(len(image)))
        conn.putheader("Cookie", f"kiln_sid={cookie}")
        conn.endheaders()
        conn.send(image[:cut])
        try:
            conn.sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        resp = conn.getresponse()
        return resp.status, resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        return None, f"{type(exc).__name__}: {exc}"
    finally:
        conn.close()
