"""Update stager -- MCP surface for the application's `stage` partition
(docs/GITHUB_RELEASE_UPDATE_PLAN.md WP6; routes in
firmware/KilnFW/App/drivers/update/update_http.c, client in
update_http_client.py).

Read-only:   update_status
Mutating:    update_stage_upload, update_stage_clear   (confirm is True exactly)

There is deliberately NO ``update_apply`` yet: installing a staged image is
the recovery image's ``POST /api/recovery/apply_staged`` (WP5), which does
not exist. Nothing here reboots the board or touches the ``app`` partition.
Credentials never appear here; every request goes through http_auth.urlopen().
"""
from __future__ import annotations

import os
from typing import Optional

from . import mcp_server_core as _core
from . import update_http_client as uhc

_SOURCES = {0: "unknown", 1: "upload", 2: "github"}


def _resolve_host(host: Optional[str]) -> str:
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import
    return _ota_resolve_host(host)


def _refuse_unconfirmed(what: str) -> str:
    return f"REFUSED: {what} -- pass confirm=True (exactly True) to proceed"


def _fmt_status(st: dict) -> str:
    parts = [f"phase={st.get('phase')}", f"busy={st.get('busy')}", f"staged={st.get('staged')}",
             f"header={st.get('header')}"]
    if st.get("reason"):
        parts.append(f"reason={st.get('reason')}")
    if st.get("busy"):
        parts.append(f"progress={st.get('bytes_done')}/{st.get('bytes_total')}")
    if st.get("header") == "ok":
        parts.append(f"state={st.get('state')}")
        parts.append(f"semver={st.get('semver')}")
        if st.get("commit"):
            parts.append(f"commit={st.get('commit')}")
        parts.append(f"image_length={st.get('image_length')}")
        parts.append(f"sha256={st.get('sha256')}")
        parts.append(f"source={_SOURCES.get(st.get('source'), st.get('source'))}")
    parts.append(f"capacity={st.get('capacity')}")
    return ", ".join(parts)


@_core._tool()
def update_status(host: Optional[str] = None) -> str:
    """READ-ONLY. Report the application's update stage (GET /api/update/stage,
    ROUTE_TIER_ADMIN): whether a verified image is staged (valid header AND a
    matching sha256), its version/commit/length/sha256/source, any upload
    in flight (phase, bytes), and the stage capacity. A board still on the
    pre-WP2 partition table has no stage and answers 404 (reported as an
    error). An image from a manual upload is UNSIGNED: sha256 catches
    corruption, it does not authenticate the publisher (plan decision D4).
    Nothing here installs anything; the apply step (recovery image, WP5) has
    no tool yet."""
    resolved = _resolve_host(host)
    try:
        st = uhc.get_stage_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"error: could not read GET /api/update/stage (host={resolved}): {exc}"
    return f"ok - {_fmt_status(st)} (host={resolved})"


@_core._tool()
def update_stage_upload(image_path: str, version: str = "", commit: str = "", confirm: bool = False,
                        host: Optional[str] = None) -> str:
    """Upload an ESP application image (KilnCtrl.bin, which embeds both
    processors' firmware) into the `stage` partition (POST /api/update/stage,
    ROUTE_TIER_ADMIN). Staging only: the running application and the `app`
    partition are untouched, nothing reboots, nothing is installed.

    Without ``confirm=True`` (exactly) this is a DRY RUN: it reads the file,
    runs the local checks (absolute path, non-empty, ESP magic 0xE9, optional
    ``version`` plain semver, ``commit`` 40 lowercase hex), computes the
    sha256 and reads the board's current stage status, then sends nothing.
    ``version``/``commit`` are optional; the board reads the version from the
    image's esp_app_desc when omitted.

    With confirm: refuses while the board reports an upload already in
    flight; the result notes when a previously staged image was erased (an
    upload erases the previous stage first; a failed upload never leaves a
    half-valid stage, but a previously good one is gone). After the POST it
    re-reads GET /api/update/stage and only reports ok when the board says
    staged with a verified header, the image length equals the file size and
    the board's sha256 equals the one computed locally; anything else is
    FAILED, never trusted from the POST reply alone. A reply lost mid-upload
    is UNKNOWN (read update_status). The staged image is UNSIGNED (sha256
    only)."""
    if not isinstance(image_path, str) or not os.path.isabs(image_path):
        return "REFUSED: image_path must be an absolute path"
    try:
        with open(image_path, "rb") as fh:
            image = fh.read()
    except OSError as exc:
        return f"REFUSED: cannot read image_path: {exc}"
    problem = uhc.validate_upload_args(image, version, commit)
    if problem:
        return f"REFUSED: {problem}"
    local_sha = uhc.sha256_hex(image)
    resolved = _resolve_host(host)
    try:
        before = uhc.get_stage_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"error: could not read GET /api/update/stage (host={resolved}): {exc}"
    if before.get("busy"):
        return f"REFUSED: an update operation is already in flight ({_fmt_status(before)})"
    cap = before.get("capacity")
    if isinstance(cap, int) and cap > 0 and len(image) > cap:
        return f"REFUSED: image is {len(image)} bytes, larger than the stage capacity ({cap} bytes)"
    if confirm is not True:
        return (f"DRY RUN (pass confirm=True to upload) -- {len(image)} bytes, sha256={local_sha}; "
                f"board stage before: {_fmt_status(before)} (host={resolved})")
    warn = ""
    if before.get("staged"):
        warn = (f" NOTE: the previously staged image ({before.get('semver')}, "
                f"sha256={before.get('sha256')}) was erased by this upload.")
    try:
        reply = uhc.upload_stage(resolved, image, version, commit)
    except uhc.UpdateHttpError as exc:
        if exc.status is None:
            return (f"UNKNOWN: the upload reply was lost or the board was unreachable ({exc}); "
                    f"read update_status -- NOT confirmed (host={resolved})")
        name = uhc.error_name(exc)
        return (f"FAILED: board refused the upload: HTTP {exc.status} {name or exc.detail!r}; "
                f"a previously staged image may have been erased (host={resolved})")
    try:
        after = uhc.get_stage_status(resolved)
    except uhc.UpdateHttpError as exc:
        return (f"UNKNOWN: POST replied {reply!r} but the confirming status read failed: {exc} "
                f"(host={resolved})")
    problems = []
    if after.get("staged") is not True:
        problems.append(f"board does not report staged ({after.get('reason')!r})")
    if after.get("header") != "ok" or after.get("state") != "verified":
        problems.append(f"header={after.get('header')!r} state={after.get('state')!r}, expected ok/verified")
    if after.get("image_length") != len(image):
        problems.append(f"image_length {after.get('image_length')} != file size {len(image)}")
    if after.get("sha256") != local_sha:
        problems.append(f"board sha256 {after.get('sha256')!r} != local {local_sha}")
    if problems:
        return (f"FAILED: POST replied {reply!r} but the read-back disagrees: {'; '.join(problems)} "
                f"(host={resolved}). Do not trust this stage.")
    return (f"ok - staged and verified by read-back: {_fmt_status(after)}; UNSIGNED (sha256 only); "
            f"nothing installed.{warn} (host={resolved})")


@_core._tool()
def update_stage_clear(confirm: bool = False, host: Optional[str] = None) -> str:
    """Erase the stage header (POST /api/update/stage/clear, ROUTE_TIER_ADMIN)
    so a staged image can never be installed. Refused by the board during a
    firing/autotune/restore or while another update operation holds the
    single update claim. Without ``confirm=True`` (exactly) this only reports
    what is staged and sends nothing. After the POST it re-reads the status
    and FAILS unless staged is false."""
    resolved = _resolve_host(host)
    try:
        before = uhc.get_stage_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"error: could not read GET /api/update/stage (host={resolved}): {exc}"
    if before.get("busy"):
        return f"REFUSED: an update operation is in flight ({_fmt_status(before)})"
    if confirm is not True:
        return _refuse_unconfirmed(f"clearing the stage would discard: {_fmt_status(before)} (host={resolved})")
    try:
        reply = uhc.clear_stage(resolved)
    except uhc.UpdateHttpError as exc:
        name = uhc.error_name(exc)
        return f"FAILED: stage clear refused or lost: HTTP {exc.status} {name or exc.detail or exc} (host={resolved})"
    try:
        after = uhc.get_stage_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"UNKNOWN: POST replied {reply!r} but the confirming status read failed: {exc} (host={resolved})"
    if after.get("staged") is not False or after.get("header") == "ok":
        return (f"FAILED: POST replied {reply!r} but the board still reports a stage: "
                f"{_fmt_status(after)} (host={resolved})")
    return f"ok - stage cleared and confirmed by read-back: {_fmt_status(after)} (host={resolved})"
