"""Update stager -- MCP surface for the application's `stage` partition
(docs/GITHUB_RELEASE_UPDATE_PLAN.md WP6; routes in
firmware/KilnFW/App/drivers/update/update_http.c, client in
update_http_client.py).

Read-only:   update_status, update_check, update_fetch_status, update_get_settings
Mutating:    update_stage_upload, update_stage_clear, update_stage_release,
             update_fetch_cancel, update_set_settings   (confirm is True exactly)

The GitHub side (WP8 routes in update_fetch.c, WP9 setting in
update_settings_http.c, tools from WP10): update_check runs the board's own
release check (a TLS job on the board; nothing is stored),
update_stage_release downloads the release into the stage,
update_fetch_status/update_fetch_cancel read and stop the board's fetch job,
update_get_settings/update_set_settings read and set the repo. Every release
is UNSIGNED in v1 (plan D4/D5).

There is deliberately NO ``update_apply``: installing a staged image is the
recovery image's ``POST /api/recovery/apply_staged`` (WP5, wrapped by the
``recovery_apply_staged`` tool). Nothing here reboots the board or touches the
``app`` partition.
Credentials never appear here; every request goes through http_auth.urlopen().
"""
from __future__ import annotations

import os
import time
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
                        host: Optional[str] = None, ack_no_safety: bool = False) -> str:
    """Upload an ESP application image (KilnCtrl.bin, which embeds both
    processors' firmware) into the `stage` partition (POST /api/update/stage,
    ROUTE_TIER_ADMIN). Staging only: the running application and the `app`
    partition are untouched, nothing reboots, nothing is installed.

    Without ``confirm=True`` (exactly) this is a DRY RUN: it reads the file,
    runs the local checks (absolute path, non-empty, ESP magic 0xE9, optional
    ``version`` plain semver, ``commit`` 40 lowercase hex), computes the
    sha256 and reads the board's current stage status, then sends nothing.
    ``version``/``commit`` are optional; the board reads the version from the
    image's esp_app_desc when omitted. If that embedded version is not semver
    the board answers 400 bad_version; the result then says so and tells you
    to retry with an explicit ``version="x.y.z"`` -- this tool never invents
    one.

    With confirm: refuses while the board reports an upload already in
    flight; the result notes when a previously staged image was erased (an
    upload erases the previous stage first; a failed upload never leaves a
    half-valid stage, but a previously good one is gone). After the POST it
    re-reads GET /api/update/stage and only reports ok when the board says
    staged with a verified header, the image length equals the file size and
    the board's sha256 equals the one computed locally; anything else is
    FAILED, never trusted from the POST reply alone. A reply lost mid-upload
    is UNKNOWN (read update_status). The staged image is UNSIGNED (sha256
    only).

    HTTP 428 means the board's OTA interlock sees the safety processor not
    answering: the upload is refused unless the operator acknowledges it.
    Pass ``ack_no_safety=True`` (exactly True, still behind ``confirm=True``) to
    send ``X-Ota-Ack-No-Safety: 1`` and proceed anyway; it is never sent
    otherwise. A 409 (firing, hot zone, another update running) is final and
    cannot be acknowledged away."""
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
        reply = uhc.upload_stage(resolved, image, version, commit, ack_no_safety=(ack_no_safety is True))
    except uhc.UpdateHttpError as exc:
        if exc.status is None:
            return (f"UNKNOWN: the upload reply was lost or the board was unreachable ({exc}); "
                    f"read update_status -- NOT confirmed (host={resolved})")
        name = uhc.error_name(exc)
        if exc.status == 400 and name == "bad_version":
            if version:
                why = (f"the version you passed ({version!r}) was not accepted as semver "
                       "(or is 32+ characters)")
            else:
                why = ("no version was passed, so the board read the version embedded in the "
                       "image's esp_app_desc, and that string is not valid semver (or is empty/"
                       "too long)")
            return (f"FAILED: board refused the upload: HTTP 400 bad_version -- {why}. "
                    "Nothing was invented on your behalf. Retry with an explicit "
                    "version=\"x.y.z\" (e.g. \"1.4.0\"; an optional leading v is stripped) "
                    "that you choose for this image; a previously staged image may have been "
                    f"erased (host={resolved})")
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
def update_stage_clear(confirm: bool = False, host: Optional[str] = None, ack_no_safety: bool = False) -> str:
    """Erase the stage header (POST /api/update/stage/clear, ROUTE_TIER_ADMIN)
    so a staged image can never be installed. Refused by the board during a
    firing/autotune/restore or while another update operation holds the
    single update claim. Without ``confirm=True`` (exactly) this only reports
    what is staged and sends nothing. After the POST it re-reads the status
    and FAILS unless staged is false. A 428 (safety processor not answering) is
    refused unless ``ack_no_safety=True`` (exactly True, still behind
    ``confirm=True``) sends ``X-Ota-Ack-No-Safety: 1``; a 409 is final."""
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
        reply = uhc.clear_stage(resolved, ack_no_safety=(ack_no_safety is True))
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


# ---- WP10: GitHub release check / stage, repo setting --------------------------------------

#: Seconds between status polls of a running board-side job; tests patch it to 0.
_POLL_S = 1.5


def _fmt_fetch(st: dict) -> str:
    parts = [f"state={st.get('state')}", f"kind={st.get('kind')}", f"repo={st.get('repo')}"]
    if st.get("stage"):
        parts.append(f"stage={st.get('stage')}")
    if st.get("error"):
        parts.append(f"error={st.get('error')}")
    if st.get("tag"):
        parts.append(f"tag={st.get('tag')}")
        parts.append(f"prerelease={st.get('prerelease')}")
        parts.append(f"running={st.get('running') or '(unknown)'}")
        parts.append(f"app_size={st.get('app_size')}")
        parts.append(f"verdict={st.get('verdict')}")
        parts.append(f"allowed={st.get('allowed')}")
        if st.get("needs_typed_confirm"):
            parts.append("needs_typed_confirm=True")
        if st.get("reason"):
            parts.append(f"reason={st.get('reason')}")
        if st.get("zones_cfg_lower"):
            parts.append("WARNING zones_cfg_lower=True (installing may reset PID gains to defaults)")
        if st.get("sha256"):
            parts.append(f"sha256={st.get('sha256')}")
    if st.get("busy") and st.get("bytes_total"):
        parts.append(f"progress={st.get('bytes_done')}/{st.get('bytes_total')}")
    return ", ".join(parts)


def _refusal_text(exc: uhc.UpdateHttpError) -> str:
    name = uhc.error_name(exc)
    return f"HTTP {exc.status} {name or exc.detail or exc}"


def _wait_job(host: str, wait_s: float) -> "tuple[Optional[dict], Optional[str]]":
    """Poll GET /api/update/fetch until the job is no longer busy or wait_s runs out.
    Returns (status, None) or (last status or None, error text)."""
    deadline = time.monotonic() + max(0.0, wait_s)
    last: Optional[dict] = None
    while True:
        try:
            last = uhc.get_fetch_status(host)
        except uhc.UpdateHttpError as exc:
            return last, f"status read failed: {exc}"
        if not last.get("busy"):
            return last, None
        if time.monotonic() >= deadline:
            return last, "still running when the wait ended"
        time.sleep(_POLL_S)


@_core._tool()
def update_check(wait_s: float = 60.0, host: Optional[str] = None) -> str:
    """Ask the board to check GitHub for the newest release of the configured
    repository (POST /api/update/check then GET /api/update/fetch, ROUTE_TIER_ADMIN)
    and report the version, the policy verdict (upgrade / up to date / downgrade /
    needs force / refused) and the release sha256. The board does the TLS work
    itself; nothing is stored and the stage is untouched. Refused by the board
    (409) during a firing or autotune, while another job runs, or until its clock
    has synced from the internet. The release is UNSIGNED in v1 (sha256 from the
    release's own manifest only). ``wait_s`` bounds how long this call polls the
    job; a check normally takes a few seconds."""
    resolved = _resolve_host(host)
    try:
        uhc.start_check(resolved)
    except uhc.UpdateHttpError as exc:
        return f"REFUSED: check not started: {_refusal_text(exc)} (host={resolved})"
    st, err = _wait_job(resolved, wait_s)
    if st is None:
        return f"UNKNOWN: check started but {err} (host={resolved})"
    if err:
        return f"UNKNOWN: {err}: {_fmt_fetch(st)}; read update_fetch_status (host={resolved})"
    if st.get("state") != "done":
        return f"FAILED: check did not finish ok: {_fmt_fetch(st)} (host={resolved})"
    return f"ok - {_fmt_fetch(st)}; UNSIGNED (host={resolved})"


@_core._tool()
def update_stage_release(confirm: bool = False, allow_prerelease: bool = False, force: bool = False,
                         allow_downgrade: bool = False, confirm_downgrade: str = "",
                         wait_s: float = 300.0, host: Optional[str] = None,
                         ack_no_safety: bool = False) -> str:
    """Download the newest GitHub release of the configured repository into the
    `stage` partition (POST /api/update/download, ROUTE_TIER_ADMIN). The board
    fetches it itself; the running application and the `app` partition are
    untouched and nothing reboots or installs. Installing is a separate, human
    step (the web page's Install button, then Apply on the recovery page).

    Without ``confirm=True`` (exactly) this is a DRY RUN: it reads the fetch and
    stage status and the repo, shows the query it would send, and sends nothing.
    With confirm: refuses while any job or stage operation is in flight, then
    starts the download and polls up to ``wait_s``. The board decides policy
    (a downgrade needs ``allow_downgrade=True`` AND ``confirm_downgrade`` equal
    to the release tag; the same version needs ``force``; a pre-release needs
    ``allow_prerelease``). Success is claimed only after a read-back of
    GET /api/update/stage shows a verified header with source github whose
    sha256 equals the one the board reported for the release; anything else is
    FAILED. The staged image is UNSIGNED. A 409 (firing, hot zone, clock not
    synced, another job) is final; a 428 (safety processor not answering) is
    refused unless ``ack_no_safety=True``, still behind ``confirm=True``."""
    resolved = _resolve_host(host)
    cd = confirm_downgrade if isinstance(confirm_downgrade, str) else ""
    query = uhc.download_query(allow_prerelease is True, force is True, allow_downgrade is True, cd)
    try:
        fetch_before = uhc.get_fetch_status(resolved)
        stage_before = uhc.get_stage_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"error: could not read the update status (host={resolved}): {exc}"
    if fetch_before.get("busy") or stage_before.get("busy"):
        return (f"REFUSED: an update operation is already in flight (fetch: {_fmt_fetch(fetch_before)}; "
                f"stage: {_fmt_status(stage_before)})")
    if confirm is not True:
        return (f"DRY RUN (pass confirm=True to download) -- would POST {uhc.DOWNLOAD_PATH}{query}; "
                f"fetch: {_fmt_fetch(fetch_before)}; stage before: {_fmt_status(stage_before)} (host={resolved})")
    warn = ""
    if stage_before.get("staged"):
        warn = (f" NOTE: the previously staged image ({stage_before.get('semver')}, "
                f"sha256={stage_before.get('sha256')}) is erased by this download.")
    try:
        uhc.start_download(resolved, allow_prerelease is True, force is True, allow_downgrade is True, cd,
                           ack_no_safety=(ack_no_safety is True))
    except uhc.UpdateHttpError as exc:
        return f"REFUSED: download not started: {_refusal_text(exc)} (host={resolved})"
    st, err = _wait_job(resolved, wait_s)
    if st is None or err:
        return (f"UNKNOWN: download started but {err}: "
                f"{_fmt_fetch(st) if st else 'no status'}; the job may still be running -- read "
                f"update_fetch_status / update_status or call update_fetch_cancel (host={resolved}).{warn}")
    if st.get("state") != "done":
        return f"FAILED: download did not finish ok: {_fmt_fetch(st)} (host={resolved}).{warn}"
    try:
        after = uhc.get_stage_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"UNKNOWN: job reported done but the confirming stage read failed: {exc} (host={resolved})"
    problems = []
    if after.get("staged") is not True or after.get("header") != "ok" or after.get("state") != "verified":
        problems.append(f"stage not verified (staged={after.get('staged')!r} header={after.get('header')!r} "
                        f"state={after.get('state')!r})")
    if after.get("source") != 2:
        problems.append(f"stage source is {_SOURCES.get(after.get('source'), after.get('source'))!r}, expected github")
    if not st.get("sha256"):
        problems.append("the board reported no release sha256 to compare against")
    elif after.get("sha256") != st.get("sha256"):
        problems.append(f"stage sha256 {after.get('sha256')!r} != release sha256 {st.get('sha256')!r}")
    if st.get("app_size") and after.get("image_length") != st.get("app_size"):
        problems.append(f"image_length {after.get('image_length')} != release app_size {st.get('app_size')}")
    if problems:
        return (f"FAILED: the job reported done but the read-back disagrees: {'; '.join(problems)} "
                f"(host={resolved}). Do not trust this stage.")
    return (f"ok - release {st.get('tag')} staged and verified by read-back: {_fmt_status(after)}; "
            f"UNSIGNED (sha256 from the release manifest only); nothing installed.{warn} (host={resolved})")


@_core._tool()
def update_get_settings(host: Optional[str] = None) -> str:
    """READ-ONLY. Report the GitHub repository the board checks for releases
    (GET /api/update/settings, ROUTE_TIER_ADMIN): the current owner/name, the
    compiled-in default, and whether the current value is the default. A
    non-default repository's releases are always UNSIGNED (plan D5)."""
    resolved = _resolve_host(host)
    try:
        st = uhc.get_settings(resolved)
    except uhc.UpdateHttpError as exc:
        return f"error: could not read GET /api/update/settings (host={resolved}): {exc}"
    return (f"ok - repo={st.get('repo')}, default_repo={st.get('default_repo')}, "
            f"is_default={st.get('is_default')} (host={resolved})")


@_core._tool()
def update_set_settings(repo: str, confirm: bool = False, host: Optional[str] = None) -> str:
    """Set the GitHub repository the board checks for releases (POST
    /api/update/settings, ROUTE_TIER_ADMIN). ``repo`` is ``owner/name`` (the
    board validates it again and answers 400 for anything else); the empty
    string resets to the compiled-in default. Without ``confirm=True`` (exactly)
    only reports the current value and what would change. The board refuses
    (409) during a firing or autotune. After the POST it re-reads the setting
    and FAILS unless it equals the requested value (or the default for an empty
    request). Needs no typed confirmation (plan D5); releases from a
    non-default repository are always shown UNSIGNED."""
    if not isinstance(repo, str):
        return "REFUSED: repo must be a string"
    resolved = _resolve_host(host)
    try:
        before = uhc.get_settings(resolved)
    except uhc.UpdateHttpError as exc:
        return f"error: could not read GET /api/update/settings (host={resolved}): {exc}"
    want = repo.strip() or before.get("default_repo")
    if confirm is not True:
        return _refuse_unconfirmed(f"would change the update repo from {before.get('repo')!r} to {want!r} (host={resolved})")
    try:
        uhc.set_settings(resolved, repo.strip())
    except uhc.UpdateHttpError as exc:
        return f"FAILED: setting refused or lost: {_refusal_text(exc)} (host={resolved})"
    try:
        after = uhc.get_settings(resolved)
    except uhc.UpdateHttpError as exc:
        return f"UNKNOWN: POST replied but the confirming read failed: {exc} (host={resolved})"
    if after.get("repo") != want:
        return f"FAILED: read-back repo {after.get('repo')!r} != requested {want!r} (host={resolved})"
    return (f"ok - update repo is now {after.get('repo')} (default={after.get('is_default')}), "
            f"confirmed by read-back (host={resolved})")


@_core._tool()
def update_fetch_status(host: Optional[str] = None) -> str:
    """READ-ONLY. Report the board's GitHub fetch job (GET /api/update/fetch,
    ROUTE_TIER_ADMIN): state (idle/checking/downloading/done/failed), stage,
    error and http_status, byte progress, and the last release it looked at
    (tag, size, sha256, policy verdict/allowed/needs_typed_confirm). Every
    release is UNSIGNED in v1 (sha256 only). Never starts a job."""
    resolved = _resolve_host(host)
    try:
        st = uhc.get_fetch_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"error: could not read GET {uhc.FETCH_PATH} (host={resolved}): {exc}"
    extra = f", http_status={st.get('http_status')}" if st.get("error") else ""
    return f"ok - {_fmt_fetch(st)}, busy={st.get('busy')}{extra}; UNSIGNED (host={resolved})"


@_core._tool()
def update_fetch_cancel(confirm: bool = False, host: Optional[str] = None) -> str:
    """Ask a running GitHub check/download job to stop (POST
    /api/update/fetch/cancel, ROUTE_TIER_ADMIN). A cancelled download leaves
    no half-valid stage but may have erased the previous one. Without
    ``confirm=True`` (exactly) it only reports the job and sends nothing.
    Reports whether the board said it was cancelling, then re-reads the
    status; the job ends asynchronously, so a still-busy re-read is noted,
    not a failure."""
    resolved = _resolve_host(host)
    try:
        before = uhc.get_fetch_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"error: could not read GET {uhc.FETCH_PATH} (host={resolved}): {exc}"
    if not before.get("busy"):
        return f"ok - nothing to cancel, no fetch job is running: {_fmt_fetch(before)} (host={resolved})"
    if confirm is not True:
        return _refuse_unconfirmed(f"cancelling would stop the running job: {_fmt_fetch(before)} (host={resolved})")
    try:
        reply = uhc.cancel_fetch(resolved)
    except uhc.UpdateHttpError as exc:
        return f"FAILED: cancel refused or lost: {_refusal_text(exc)} (host={resolved})"
    if reply.get("cancelling") is not True:
        return f"ok - the job had already ended when the cancel arrived (reply {reply!r}) (host={resolved})"
    try:
        after = uhc.get_fetch_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"UNKNOWN: cancel accepted but the confirming status read failed: {exc} (host={resolved})"
    note = " (job still winding down; read update_fetch_status)" if after.get("busy") else ""
    return f"ok - cancel requested: {_fmt_fetch(after)}{note} (host={resolved})"
