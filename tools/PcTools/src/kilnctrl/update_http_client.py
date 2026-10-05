#!/usr/bin/env python3
"""update_http_client.py -- pure HTTP client for the application's update
stager (firmware/KilnFW/App/drivers/update/update_http.c, WP4 of
docs/GITHUB_RELEASE_UPDATE_PLAN.md), all ROUTE_TIER_ADMIN:

  GET  /api/update/stage        status of the `stage` partition and any upload
  POST /api/update/stage        stream an ESP image into `stage` (raw body)
  POST /api/update/stage/clear  erase the stage header

Nothing here installs anything: applying a staged image is the recovery
image's job (WP5) and has no client yet. Same "stdlib urllib.request through
http_auth.urlopen()" convention as nvs_keys_http_client.py, unit-tested
against mocked HTTP (tests/test_update_http_client.py), no socket, no board.
Credentials come only from http_auth (env vars) and are never echoed here.
"""
from __future__ import annotations

import hashlib
import json
import re
import urllib.error
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
                 timeout: float = UPDATE_UPLOAD_TIMEOUT_S) -> dict:
    """POST /api/update/stage with `image` as the raw body. Optional
    ``X-Stage-Version`` / ``X-Stage-Commit`` headers; when the version is
    omitted the board reads it from the image's esp_app_desc. Raises
    UpdateHttpError (never retries; a refused upload may already have erased
    a previously good stage). Returns the board's reply body."""
    problem = validate_upload_args(image, version, commit)
    if problem:
        raise UpdateHttpError(f"refusing to upload: {problem}")
    req = urllib.request.Request(_url(host, STAGE_PATH), data=image, method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    if version:
        req.add_header("X-Stage-Version", version)
    if commit:
        req.add_header("X-Stage-Commit", commit)
    return _request(req, STAGE_PATH, timeout)


def clear_stage(host: str, timeout: float = UPDATE_CLEAR_TIMEOUT_S) -> dict:
    """POST /api/update/stage/clear (empty body); erases the stage header."""
    req = urllib.request.Request(_url(host, STAGE_CLEAR_PATH), data=b"", method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    return _request(req, STAGE_CLEAR_PATH, timeout)


def sha256_hex(image: bytes) -> str:
    return hashlib.sha256(image).hexdigest()
