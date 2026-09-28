#!/usr/bin/env python3
"""backup_import_http_client.py -- pure HTTP client for POST
/api/backup/import (firmware/KilnFW/App/drivers/http/backup_import.c's
backup_import_post_handler(), ROUTE_TIER_ADMIN per route_tier_table.h).

Unlike POST /api/kiln_configs/apply, this route is SYNCHRONOUS -- there is
no job id and no separate status-poll route. backup_import_post_handler()
validates and commits (or refuses) the whole body within the one POST/
response round trip (backup_import_apply(), a two-pass validate-then-commit
parser); the response IS the outcome. ``post_import()`` below reports
whichever of these it gets back:

2026-09-28 (docs/HTTP_POST_OWNER_MIGRATION_PLAN.md slice A4): the slow tail
(reading the body and running backup_import_apply()) now runs on its own
task via http_async_job.c, off esp_http_server's single shared httpd_worker
-- so a long import no longer starves every OTHER request (dashboard polls,
the Stop button) for its duration, which is what a bench A4 run
(2026-09-28) actually observed ("board HTTP unresponsive for several polls
after each" import). The wire contract this client speaks did NOT change:
still one POST, still one response, on the SAME connection, no job id, no
poll -- A4 reuses A1's helper shape (same status/body the handler already
sent), not the kiln_configs/apply 202-and-poll shape. What DID change is
how long a caller must be willing to wait for that one response:
BACKUP_IMPORT_HTTP_TIMEOUT_S below was raised from 30s after that same bench
run's POST timed out client-side on a live, uncommitted-looking import that
the board had, in fact, already committed (confirmed by a follow-up
GET /api/zones read showing the new config in place) -- a client-side
timeout on this route answers only "did the response arrive within this
many seconds", never "did the board apply the change". A caller whose POST
times out here MUST NOT retry blindly: retrying an import whose first
attempt actually committed re-applies it a second time (harmless for an
idempotent merge/mirror of the same body, but never assume that without
checking) instead of confirming the real outcome. Read back the config
(``GET /api/zones``, ``GET /api/profiles``, ``GET /api/kiln_configs`` as
appropriate) after a timeout, the same way the firmware gotchas section on
``ota_rollback_esp()`` already tells a caller to do after a rollback,
rather than re-POSTing.

  * 200, ``{"ok":true}`` -- committed (or, if the request set
    ``X-Kiln-Config-Dry-Run: 1``, the plan text was returned instead of
    JSON -- see ``dry_run`` below).
  * 400 -- validation refused the whole import before writing anything
    (``err_msg``, plain text).
  * 500 "out of memory" (that exact body) -- a buffer allocation failed
    before anything was parsed or written; nothing changed
    (``REFUSAL_OUT_OF_MEMORY``).
  * any other 500 -- a PARTIAL write landed (``kiln_configs[]`` committed before
    profiles/zones failed) -- backup_import_post_handler() distinguishes
    this from the all-nothing 400 case explicitly, and so does this client
    (``BackupImportHttpError.partial_write``).
  * 409 -- refused, for one of THREE distinct reasons, all plain text:
      - the system_mode_gate refusal (a firing or autotune run is active),
        checked first, before the body is even read -- see
        ``zones_http_client.is_system_mode_gate_refusal()``, reused here
        rather than duplicating its marker string.
      - an ordinary OTA-interlock refusal (profile running, heater on, zone
        over temperature, safety link down with no ack, another update in
        flight) that does not need an operator acknowledgement.
      - "another commissioning operation is running" -- an unrelated
        http_async_job (ct_auto_zero's measurement) is mid-commit; this
        exact string is the only reliable discriminator
        (``ASYNC_BUSY_MARKER`` below).
  * 428 -- the ONE interlock precondition a restore may proceed past: the
    safety link is down and the operator has not yet acknowledged
    ``X-Ota-Ack-No-Safety``. ``ack_no_safety`` below sends that header.

``classify_refusal()`` turns a caught error into one of these categories so
a caller can report which refusal happened without re-deriving the string
matching here.

Same "stdlib urllib.request, no framework" convention as the other bench
HTTP clients in this package, going through :mod:`http_auth` for the ADMIN
session seam every other admin-tier write tool here uses.
"""
from __future__ import annotations

import urllib.error
import urllib.request
from typing import Optional

from . import http_auth
from . import zones_http_client

#: Raised from 30.0 (2026-09-28, bench A4 finding): a live board with 100
#: profile/zone/kiln_config slots plus a coupling matrix restore can push
#: safety_cfg_write_apply_pairs()'s own stage/COMMIT_CONFIG/read-back-confirm
#: round trip through the Pico link, the same worst-case-tens-of-seconds
#: shape bench_preset_job() already documents for 32 SET_PARAM calls plus one
#: commit -- 30s was measured too short on the bench and the client's own
#: read timed out while the board was still working (and, per the module
#: docstring above, had in fact already committed). This is a read timeout
#: on one still-synchronous POST/response round trip, not a poll interval.
BACKUP_IMPORT_HTTP_TIMEOUT_S = 90.0

_API_PATH = "/api/backup/import"

#: backup_import_post_handler()'s own literal text for the http_async_job
#: busy refusal (backup_import.c) -- the only reliable way to tell this 409
#: apart from the system_mode_gate 409 and the ordinary OTA-interlock 409,
#: none of which share a status-code-only signature.
ASYNC_BUSY_MARKER = "another commissioning operation is running"

MODE_GATE_HEADER = "X-Kiln-Config-Mode"
DRY_RUN_HEADER = "X-Kiln-Config-Dry-Run"
ACK_DELETE_HEADER = "X-Kiln-Config-Ack-Delete"
ACK_NO_SAFETY_HEADER = "X-Ota-Ack-No-Safety"

REFUSAL_MODE_GATE = "mode_gate"
REFUSAL_ASYNC_BUSY = "async_busy"
REFUSAL_INTERLOCK_NEEDS_ACK = "interlock_needs_ack"
REFUSAL_INTERLOCK = "interlock"
REFUSAL_VALIDATION = "validation"
REFUSAL_PARTIAL_WRITE = "partial_write"
REFUSAL_OUT_OF_MEMORY = "out_of_memory"

#: The exact body of backup_import_post_handler()'s pre-write allocation
#: failures (httpd_resp_send_err(..., HTTPD_500_INTERNAL_SERVER_ERROR,
#: "out of memory")) -- a 500 that wrote nothing, unlike every other 500.
OUT_OF_MEMORY_NOTHING_WRITTEN = "out of memory"
REFUSAL_OTHER = "other"


class BackupImportHttpError(Exception):
    """Any transport or protocol failure talking to POST
    /api/backup/import -- unreachable host, non-2xx, or a response shape
    this client does not understand. `.status`/`.detail` carry the board's
    own reported status code and plain-text body."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str) -> str:
    return f"http://{host}{_API_PATH}"


def _http_error_detail(exc: Exception) -> "tuple[Optional[int], str]":
    if isinstance(exc, urllib.error.HTTPError):
        try:
            detail = exc.read().decode("utf-8", errors="replace")
        except Exception:
            detail = ""
        return exc.code, detail
    if isinstance(exc, urllib.error.URLError):
        return None, f"unreachable: {exc.reason}"
    return None, str(exc)


def classify_refusal(status: Optional[int], detail: str) -> str:
    """Turns a (status, body) pair from a non-2xx POST /api/backup/import
    response into one of the REFUSAL_* categories above, by status code
    first and then, for the ambiguous 409s, by body text -- see this
    module's own header comment for why status code alone cannot
    distinguish the three 409 reasons."""
    if status == 428:
        return REFUSAL_INTERLOCK_NEEDS_ACK
    if status == 409:
        if ASYNC_BUSY_MARKER in (detail or ""):
            return REFUSAL_ASYNC_BUSY
        if zones_http_client.is_system_mode_gate_refusal(detail):
            return REFUSAL_MODE_GATE
        return REFUSAL_INTERLOCK
    if status == 400:
        return REFUSAL_VALIDATION
    if status == 500:
        # backup_import_post_handler() sends a bare "out of memory" 500 via
        # httpd_resp_send_err() from three allocation checks that all run
        # BEFORE backup_import_apply() writes anything (body buffer, plan
        # buffer, dry-run output buffer). Every partial-write 500 instead
        # carries backup_import_apply()'s own err_msg, whose post-commit
        # out-of-memory variants all say "... -- kiln configs were already
        # restored" -- never the bare string. Anything else at 500 stays
        # partial_write: an unrecognised 500 must fail loud, not quiet.
        if (detail or "").strip() == OUT_OF_MEMORY_NOTHING_WRITTEN:
            return REFUSAL_OUT_OF_MEMORY
        return REFUSAL_PARTIAL_WRITE
    return REFUSAL_OTHER


def post_import(
    host: str,
    body_text: str,
    mode: str = "merge",
    dry_run: bool = False,
    ack_delete_count: Optional[int] = None,
    ack_no_safety: bool = False,
    timeout: float = BACKUP_IMPORT_HTTP_TIMEOUT_S,
) -> "tuple[int, str]":
    """POST /api/backup/import with ``body_text`` as the raw JSON body.
    Returns ``(status, body_text)`` for EVERY response, 2xx or not, so a
    400/409/428/500 reaches the caller with the board's own explanation
    intact rather than being folded into a generic exception -- same
    convention as kiln_configs_apply_http_client.post_apply(). Only a
    transport-level failure (unreachable host, no credential) raises
    BackupImportHttpError.

    ``mode`` is "merge" (default, the safer choice) or "mirror" -- sent as
    ``X-Kiln-Config-Mode``. ``dry_run=True`` sends
    ``X-Kiln-Config-Dry-Run: 1`` (the response is then a plain-text plan,
    not JSON, and nothing is written). ``ack_delete_count`` is sent as
    ``X-Kiln-Config-Ack-Delete`` only when not None -- required for a
    non-dry-run MIRROR restore that would delete kiln config slots.
    ``ack_no_safety=True`` sends ``X-Ota-Ack-No-Safety: 1``, the one
    interlock precondition this route may proceed past."""
    data = body_text.encode("utf-8")
    req = urllib.request.Request(_url(host), data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    if mode.lower() == "mirror":
        req.add_header(MODE_GATE_HEADER, "mirror")
    if dry_run:
        req.add_header(DRY_RUN_HEADER, "1")
    if ack_delete_count is not None:
        req.add_header(ACK_DELETE_HEADER, str(ack_delete_count))
    if ack_no_safety:
        req.add_header(ACK_NO_SAFETY_HEADER, "1")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        return status, detail
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise BackupImportHttpError(f"POST {_API_PATH} unreachable: {detail}") from exc
    except (http_auth.HttpAuthError, OSError) as exc:
        raise BackupImportHttpError(f"POST {_API_PATH} failed: {exc}") from exc
