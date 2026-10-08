#!/usr/bin/env python3
"""update_http_client.py -- pure HTTP client for the application's update
stager (firmware/KilnFW/App/drivers/update/update_http.c, WP4 of
docs/GITHUB_RELEASE_UPDATE_PLAN.md), all ROUTE_TIER_ADMIN:

  GET  /api/update/stage        status of the `stage` partition and any upload
  POST /api/update/stage        stream an ESP image into `stage` (raw body)
  POST /api/update/stage/clear  erase the stage header

and (WP10) the GitHub fetch job of update_fetch.c / the repo setting of
update_settings_http.c, also ADMIN:

  POST /api/update/check            start a release check (202, async)
  POST /api/update/download         start a download into the stage (202, async)
  GET  /api/update/fetch            job status and the last check's verdict
  POST /api/update/fetch/cancel     cancel a running job
  GET/POST /api/update/settings     the update repo (form body repo=owner/name)

Nothing here installs anything: applying a staged image is the recovery
image's job (WP5) and has no client yet. Same "stdlib urllib.request through
http_auth.urlopen()" convention as nvs_keys_http_client.py, unit-tested
against mocked HTTP (tests/test_mcp_server_update.py), no socket, no board.
Credentials come only from http_auth (env vars) and are never echoed here.
"""
from __future__ import annotations

import hashlib
import json
import re
import urllib.error
import urllib.parse
import urllib.request
from typing import Optional

from . import http_auth

UPDATE_STATUS_TIMEOUT_S = 8.0
#: Erasing 4 MiB, receiving, hashing and read-back verifying a ~2.6 MB image
#: on a single httpd task; generous because a premature client timeout would
#: not stop the board and would leave the outcome unknown.
UPDATE_UPLOAD_TIMEOUT_S = 240.0
UPDATE_CLEAR_TIMEOUT_S = 30.0

STAGE_PATH = "/api/update/stage"
STAGE_CLEAR_PATH = "/api/update/stage/clear"
FETCH_PATH = "/api/update/fetch"
CHECK_PATH = "/api/update/check"
DOWNLOAD_PATH = "/api/update/download"
SETTINGS_PATH = "/api/update/settings"
FETCH_CANCEL_PATH = "/api/update/fetch/cancel"

ESP_IMAGE_MAGIC = 0xE9
#: stage_header.h STAGE_SEMVER_FIELD_LEN / STAGE_COMMIT_HEX_LEN.
SEMVER_FIELD_LEN = 32
COMMIT_HEX_LEN = 40
_SEMVER_RE = re.compile(r"^[0-9A-Za-z.+-]+$")
_COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")


class UpdateHttpError(Exception):
    """Transport or protocol failure talking to the update routes.
    ``.status``/``.detail`` carry the board's status code and body."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str, path: str) -> str:
    return f"http://{host}{path}"


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


def _request(req: urllib.request.Request, path: str, timeout: float) -> dict:
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise UpdateHttpError(f"{req.get_method()} {path} failed: HTTP {status}: {detail}"
                              if status is not None else f"{req.get_method()} {path} failed: {detail}",
                              status, detail) from exc
    try:
        data = json.loads(text)
    except Exception as exc:
        raise UpdateHttpError(f"{req.get_method()} {path} returned a non-JSON body: {text!r}") from exc
    if not isinstance(data, dict):
        raise UpdateHttpError(f"{req.get_method()} {path} returned a non-object body: {text!r}")
    return data


def error_name(exc: UpdateHttpError) -> str:
    """The board's ``{"ok":false,"error":"<name>"}`` name when the detail is
    that shape, else ''."""
    try:
        body = json.loads(exc.detail)
    except Exception:
        return ""
    if isinstance(body, dict) and isinstance(body.get("error"), str):
        return body["error"]
    return ""


def get_stage_status(host: str, timeout: float = UPDATE_STATUS_TIMEOUT_S) -> dict:
    """GET /api/update/stage. Keys: ok, phase, busy, bytes_done, bytes_total,
    staged, reason, header, capacity, image_length, state, semver, commit,
    sha256, source (1 upload, 2 github)."""
    req = urllib.request.Request(_url(host, STAGE_PATH), method="GET")
    data = _request(req, STAGE_PATH, timeout)
    if "staged" not in data or "phase" not in data:
        raise UpdateHttpError(f"GET {STAGE_PATH} response lacks staged/phase: {data!r}")
    return data


def validate_upload_args(image: bytes, version: str = "", commit: str = "") -> Optional[str]:
    """Local pre-flight; returns a refusal reason or None. The board repeats
    every check (and more: chip id, app descriptor, capacity) -- this only
    saves a round trip and an erase of a previously good stage."""
    if not image:
        return "image is empty"
    if image[0] != ESP_IMAGE_MAGIC:
        return f"image does not start with the ESP image magic 0x{ESP_IMAGE_MAGIC:02x} (got 0x{image[0]:02x})"
    if version:
        v = version[1:] if version[:1] == "v" else version
        if not v or len(v) >= SEMVER_FIELD_LEN or not _SEMVER_RE.match(v):
            return f"version {version!r} is not a plain semver string under {SEMVER_FIELD_LEN} characters"
    if commit and not _COMMIT_RE.match(commit):
        return "commit must be 40 lowercase hex characters (or empty)"
    return None


def upload_stage(host: str, image: bytes, version: str = "", commit: str = "",
                 timeout: float = UPDATE_UPLOAD_TIMEOUT_S, ack_no_safety: bool = False,
                 force: bool = False, allow_downgrade: bool = False, confirm_downgrade: str = "") -> dict:
    """POST /api/update/stage with `image` as the raw body. Optional
    ``X-Stage-Version`` / ``X-Stage-Commit`` headers; when the version is
    omitted the board reads it from the image's esp_app_desc. Raises
    UpdateHttpError (never retries; a refused upload may already have erased
    a previously good stage). ``ack_no_safety=True`` adds ``X-Ota-Ack-No-Safety: 1``,
    the operator acknowledgement that lets the board proceed while the safety
    processor is not answering (otherwise HTTP 428). Returns the board's reply body.

    The board applies the downgrade gate (docs/GITHUB_RELEASE_UPDATE_PLAN.md
    section 6) before writing any image byte: a same-commit image needs
    ``force``; an older version or a lower config schema is refused with HTTP
    409 ``downgrade_refused`` unless ``allow_downgrade`` is set AND
    ``confirm_downgrade`` equals the image's version (typed confirm). They go
    out as ``X-Stage-Force`` / ``X-Stage-Allow-Downgrade`` / ``X-Stage-Confirm``."""
    problem = validate_upload_args(image, version, commit)
    if problem:
        raise UpdateHttpError(f"refusing to upload: {problem}")
    req = urllib.request.Request(_url(host, STAGE_PATH), data=image, method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    if version:
        req.add_header("X-Stage-Version", version)
    if commit:
        req.add_header("X-Stage-Commit", commit)
    if ack_no_safety:
        req.add_header("X-Ota-Ack-No-Safety", "1")
    if force:
        req.add_header("X-Stage-Force", "1")
    if allow_downgrade:
        req.add_header("X-Stage-Allow-Downgrade", "1")
    if confirm_downgrade:
        req.add_header("X-Stage-Confirm", confirm_downgrade)
    return _request(req, STAGE_PATH, timeout)


def refusal_reason(exc: "UpdateHttpError") -> str:
    """The board's gate ``reason`` text from a 409 body, else ''."""
    try:
        body = json.loads(exc.detail)
    except Exception:
        return ""
    if isinstance(body, dict) and isinstance(body.get("reason"), str):
        return body["reason"]
    return ""


def clear_stage(host: str, timeout: float = UPDATE_CLEAR_TIMEOUT_S, ack_no_safety: bool = False) -> dict:
    """POST /api/update/stage/clear (empty body); erases the stage header."""
    req = urllib.request.Request(_url(host, STAGE_CLEAR_PATH), data=b"", method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    if ack_no_safety:
        req.add_header("X-Ota-Ack-No-Safety", "1")
    return _request(req, STAGE_CLEAR_PATH, timeout)


def get_fetch_status(host: str, timeout: float = UPDATE_STATUS_TIMEOUT_S) -> dict:
    """GET /api/update/fetch. Keys: state (idle/checking/downloading/done/failed),
    kind, stage, error, http_status, bytes_done, bytes_total, busy, repo,
    tag, prerelease, app_size, running, commit, sha256, verdict, reason, allowed,
    needs_typed_confirm, zones_cfg_lower."""
    req = urllib.request.Request(_url(host, FETCH_PATH), method="GET")
    data = _request(req, FETCH_PATH, timeout)
    if "state" not in data or "busy" not in data:
        raise UpdateHttpError(f"GET {FETCH_PATH} response lacks state/busy: {data!r}")
    return data


def _empty_post(host: str, path: str, timeout: float, ack_no_safety: bool = False) -> dict:
    req = urllib.request.Request(_url(host, path), data=b"", method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    if ack_no_safety:
        req.add_header("X-Ota-Ack-No-Safety", "1")
    return _request(req, path, timeout)


def start_check(host: str, timeout: float = UPDATE_CLEAR_TIMEOUT_S,
                allow_prerelease: bool = False) -> dict:
    """POST /api/update/check; the board answers 202 {"ok":true,"started":true}.
    allow_prerelease reads the releases list (GitHub's /releases/latest hides pre-releases)."""
    return _empty_post(host, CHECK_PATH + ("?allow_prerelease=1" if allow_prerelease else ""), timeout)


def download_query(allow_prerelease: bool = False, force: bool = False,
                   allow_downgrade: bool = False, confirm_downgrade: str = "") -> str:
    """The query string for POST /api/update/download: only set flags are sent, and
    confirm_downgrade only together with allow_downgrade or force. The board needs the
    typed tag with force too when its running version is unknown (a dev build,
    update_policy_decide_typed()); the tag alone never enables a downgrade, because
    the board ANDs it with allow_downgrade=1."""
    q = []
    if allow_prerelease:
        q.append("allow_prerelease=1")
    if force:
        q.append("force=1")
    if allow_downgrade:
        q.append("allow_downgrade=1")
    if confirm_downgrade and (allow_downgrade or force):
        q.append("confirm_downgrade=" + urllib.parse.quote(confirm_downgrade, safe=""))
    return ("?" + "&".join(q)) if q else ""


def start_download(host: str, allow_prerelease: bool = False, force: bool = False,
                   allow_downgrade: bool = False, confirm_downgrade: str = "",
                   timeout: float = UPDATE_CLEAR_TIMEOUT_S, ack_no_safety: bool = False) -> dict:
    """POST /api/update/download. 202 when the job started; a refusal raises
    UpdateHttpError (409 mode gate/claim/clock, 428 safety link not answering)."""
    return _empty_post(host, DOWNLOAD_PATH + download_query(allow_prerelease, force, allow_downgrade,
                                                            confirm_downgrade), timeout, ack_no_safety)

def cancel_fetch(host: str, timeout: float = UPDATE_STATUS_TIMEOUT_S) -> dict:
    """POST /api/update/fetch/cancel; {"ok":true,"cancelling":bool}."""
    return _empty_post(host, FETCH_CANCEL_PATH, timeout)


def get_settings(host: str, timeout: float = UPDATE_STATUS_TIMEOUT_S) -> dict:
    """GET /api/update/settings: ok, repo, default_repo, is_default."""
    req = urllib.request.Request(_url(host, SETTINGS_PATH), method="GET")
    data = _request(req, SETTINGS_PATH, timeout)
    if "repo" not in data:
        raise UpdateHttpError(f"GET {SETTINGS_PATH} response lacks repo: {data!r}")
    return data


def set_settings(host: str, repo: str, timeout: float = UPDATE_CLEAR_TIMEOUT_S) -> dict:
    """POST /api/update/settings with form body repo=<owner/name> (empty resets
    to the default). The board validates; a 400 raises UpdateHttpError."""
    req = urllib.request.Request(_url(host, SETTINGS_PATH),
                                 data=("repo=" + urllib.parse.quote(repo, safe="")).encode("ascii"),
                                 method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    return _request(req, SETTINGS_PATH, timeout)


def sha256_hex(image: bytes) -> str:
    return hashlib.sha256(image).hexdigest()
