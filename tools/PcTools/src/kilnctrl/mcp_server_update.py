"""Update stager -- MCP surface for the application's `stage` partition
(docs/GITHUB_RELEASE_UPDATE_PLAN.md WP6; routes in
firmware/KilnFW/App/drivers/update/update_http.c, client in
update_http_client.py).

Read-only:   update_status, update_fetch_status, update_get_settings (GET only)
Job-starting: update_check -- NOT read-only: it POSTs /api/update/check, which
             starts a TLS job on the board (heap, network, a board-side job slot;
             nothing is stored and it needs no confirm). Use update_fetch_status
             for a pure GET of the last job.
Mutating:    update_stage_upload, update_stage_clear, update_stage_release,
             update_fetch_cancel, update_set_settings   (confirm is True exactly)

The GitHub side (WP8 routes in update_fetch.c, WP9 setting in
update_settings_http.c, tools from WP10): update_check runs the board's own
release check (a TLS job on the board; nothing is stored),
update_stage_release downloads the release into the stage,
update_fetch_status/update_fetch_cancel read and stop the board's fetch job,
update_get_settings/update_set_settings read and set the repo. Release
signing was removed (owner decision 2026-10-07); the board accepts an image only
when it is for this project (esp_app_desc project_name).

There is deliberately NO ``update_apply``: the recovery image's
``POST /api/recovery/apply_staged`` (WP5, ``recovery_apply_staged``) is the manual
path. The unattended path is ``update_install_staged`` (POST /api/update/install):
the board reboots into recovery, applies the VERIFIED stage by itself and boots the
new app. Every other tool here only stages; none touches the ``app`` partition.
Credentials never appear here; every request goes through http_auth.urlopen().
"""
from __future__ import annotations

import os
import threading
import time
from typing import Optional

from mcpkit import build_jobs

from . import dashboard_http_client as dhc
from . import http_auth
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
    if st.get("fetch_writer_wedged"):
        parts.append("fetch_writer_wedged=True (WARNING: the board's flash writer is wedged; reboot required "
                     "before another update can run)")
    elif "fetch_writer_wedged" in st:
        parts.append("fetch_writer_wedged=False")
    return ", ".join(parts)


@_core._tool()
def update_status(host: Optional[str] = None) -> str:
    """READ-ONLY. Report the application's update stage (GET /api/update/stage,
    ROUTE_TIER_ADMIN): whether a verified image is staged (valid header AND a
    matching sha256), its version/commit/length/sha256/source, any upload
    in flight (phase, bytes), and the stage capacity. A board still on the
    pre-WP2 partition table has no stage and answers 404 (reported as an
    error). The board refuses an image whose project name is not
    KilnCtrl (wrong_project); sha256 catches corruption.
    Nothing here installs anything; the apply step runs in the recovery
    image: recovery_enter(), then recovery_apply_staged()."""
    resolved = _resolve_host(host)
    try:
        st = uhc.get_stage_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"error: could not read GET /api/update/stage (host={resolved}): {exc}"
    return f"ok - {_fmt_status(st)} (host={resolved})"


@_core._tool()
def update_stage_upload(image_path: str, version: str = "", commit: str = "", confirm: bool = False,
                        host: Optional[str] = None, ack_no_safety: bool = False,
                        force: bool = False, allow_downgrade: bool = False,
                        confirm_downgrade: str = "") -> str:
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
    flight; the result notes when a previously staged image may be replaced (the
    board erases the previous stage only after the image head passes the
    project, version and downgrade-gate checks, so an upload refused by
    those leaves the old stage byte-identical; a later failure never leaves
    a half-valid stage, but a previously good one is gone). After the POST it
    re-reads GET /api/update/stage and only reports ok when the board says
    staged with a verified header, the image length equals the file size and
    the board's sha256 equals the one computed locally; anything else is
    FAILED, never trusted from the POST reply alone. A reply lost mid-upload
    is UNKNOWN (read update_status). The board refuses a wrong-project
    image.

    HTTP 428 means the board's OTA interlock sees the safety processor not
    answering: the upload is refused unless the operator acknowledges it.
    Pass ``ack_no_safety=True`` (exactly True, still behind ``confirm=True``) to
    send ``X-Ota-Ack-No-Safety: 1`` and proceed anyway; it is never sent
    otherwise. A 409 (firing, hot zone, another update running) is final and
    cannot be acknowledged away.

    Downgrade gate (docs/GITHUB_RELEASE_UPDATE_PLAN.md section 6), applied by
    the board before any image byte is written: an image with the running
    commit needs ``force=True``; an older version or a lower config schema is
    refused with 409 ``downgrade_refused`` (the reply names the rollback
    hazard: an older firmware runs on default PID gains after a zones_cfg
    schema bump, so read back control_get_zones before heating). The only
    override is ``allow_downgrade=True`` together with ``confirm_downgrade``
    equal to the image's version (typed confirm). An image without the
    embedded schema identity record also needs ``force=True`` plus the typed
    confirm; a declared ``version`` must equal the image's own. The flags must be exactly
    True and still sit behind ``confirm=True``. A refused upload may already
    have erased the previous stage."""
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
    if not isinstance(confirm_downgrade, str):
        return "REFUSED: confirm_downgrade must be a string (the image version)"
    if allow_downgrade is True and not confirm_downgrade:
        return ("REFUSED: allow_downgrade needs confirm_downgrade set to the image version "
                "(typed confirm); after a downgrade read back control_get_zones before heating")
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
        reply = uhc.upload_stage(resolved, image, version, commit, ack_no_safety=(ack_no_safety is True),
                                 force=(force is True), allow_downgrade=(allow_downgrade is True),
                                 confirm_downgrade=confirm_downgrade)
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
                    "that you choose for this image; the previously staged image is untouched "
                    f"(host={resolved})")
        if exc.status == 409 and name in ("downgrade_refused", "needs_force"):
            body = uhc.refusal_body(exc)
            cand = body.get("candidate_version") or "?"
            run = body.get("running_version") or "?"
            if body.get("needs_typed_confirm") is True:
                hint = (f"Override needs the typed confirm: confirm_downgrade={cand}, plus force=True "
                        "(unknown schema / unknown running version) or allow_downgrade=True (downgrade); "
                        "after any downgrade read back control_get_zones before heating.")
            else:
                hint = "Override with force=True (no typed confirm needed)."
            return (f"FAILED: board refused the upload by the downgrade gate: {name} -- "
                    f"{uhc.refusal_reason(exc)} (candidate {cand}, running {run}). {hint} "
                    f"The previously staged image is untouched (host={resolved})")
        if exc.status == 409 and name == "version_mismatch":
            return ("FAILED: the declared version differs from the version inside the image; omit version= "
                    f"or pass the image's own (host={resolved})")
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
    return (f"ok - staged and verified by read-back: {_fmt_status(after)}; "
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
        if st.get("http_status"):
            parts.append(f"http_status={st.get('http_status')}")
        if st.get("error") == "http_status" and st.get("http_status") == 404:
            parts.append("(404 = no release published in this repo)")
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
    heap = _fmt_heap(st.get("heap"))
    if heap:
        parts.append(heap)
    return ", ".join(parts)


def _fmt_heap(h: Optional[dict]) -> str:
    """The job's internal-RAM readings (UPDFX7): admission reading, the worst settle result over all redirect hops and the low-waters."""
    if not isinstance(h, dict) or not h.get("sampled"):
        return ""
    text = (f"heap[start free={h.get('start_free')} dma={h.get('start_dma')} rtc={h.get('rtc_free')}; "
            f"hop={h.get('hop')}")
    if h.get("hop") in ("admitted", "refused"):
        text += (f" (heap settle: worst over all hops, not one hop's result; "
                 f"lowest free={h.get('hop_free')}, most re-samples={h.get('hop_settle_tries')})")
    text += (f"; low-water free={h.get('min_free')} dma={h.get('min_dma')} "
             f"largest_block={h.get('min_block')}]")
    return text


def _refusal_text(exc: uhc.UpdateHttpError) -> str:
    name = uhc.error_name(exc)
    return f"HTTP {exc.status} {name or exc.detail or exc}"


_MAX_WAIT_S = 200.0


def _wait_job(host: str, wait_s: float) -> "tuple[Optional[dict], Optional[str]]":
    """Poll GET /api/update/fetch until the job is no longer busy or wait_s runs out.
    Returns (status, None) or (last status or None, error text)."""
    # The client aborts a call after 300 s of silence; leave room for the
    # prechecks and read-back around the poll. A job still busy at the clamp
    # is reported UNKNOWN by the caller, never ok.
    wait_s = min(max(0.0, float(wait_s)), _MAX_WAIT_S)
    deadline = time.monotonic() + wait_s
    last: Optional[dict] = None
    while True:
        try:
            last = uhc.get_fetch_status(host)
        except uhc.UpdateHttpError as exc:
            return last, f"status read failed: {exc}"
        if not last.get("busy"):
            return last, None
        if time.monotonic() >= deadline:
            return last, f"still running after {wait_s:g}s (wait_s is clamped to {_MAX_WAIT_S:g}s)"
        time.sleep(_POLL_S)


@_core._tool()
def update_check(wait_s: float = 60.0, host: Optional[str] = None, allow_prerelease: bool = False) -> str:
    """NOT read-only: starts a board job (POST /api/update/check, no confirm gate;
    stores nothing, stage untouched). For a pure GET of the last job use
    update_fetch_status. Ask the board to check GitHub for the newest release of the configured
    repository (POST /api/update/check then GET /api/update/fetch, ROUTE_TIER_ADMIN)
    and report the version, the policy verdict (upgrade / up to date / downgrade /
    needs force / refused) and the release sha256. The board does the TLS work
    itself; nothing is stored and the stage is untouched. Refused by the board
    (409) during a firing or autotune, while another job runs, or until its clock
    has synced from the internet. The release's sha256 comes from its
    own manifest. ``wait_s`` bounds how long this call polls the
    job; a check normally takes a few seconds. ``allow_prerelease=True`` makes the
    board read the releases list and pick the highest-semver non-draft release, so a
    GitHub pre-release is found (plain /releases/latest never returns one)."""
    resolved = _resolve_host(host)
    try:
        if allow_prerelease is True:
            uhc.start_check(resolved, allow_prerelease=True)
        else:
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
    return f"ok - {_fmt_fetch(st)}; (host={resolved})"


@_core._tool()
def update_stage_release(confirm: bool = False, allow_prerelease: bool = False, force: bool = False,
                         allow_downgrade: bool = False, confirm_downgrade: str = "",
                         wait_s: float = 200.0, host: Optional[str] = None,
                         ack_no_safety: bool = False) -> str:
    """Download the newest GitHub release of the configured repository into the
    `stage` partition (POST /api/update/download, ROUTE_TIER_ADMIN). The board
    fetches it itself; the running application and the `app` partition are
    untouched and nothing reboots or installs. Installing is a separate, human
    step (the web page's Install button, then Apply on the recovery page).

    Without ``confirm=True`` (exactly) this is a DRY RUN: it reads the fetch and
    stage status and the repo, shows the query it would send, and sends nothing.
    With confirm: refuses while any job or stage operation is in flight, then
    starts the download and polls up to ``wait_s`` (clamped to 200 s so the call stays under the MCP client's 300 s abort; the UNKNOWN text names the clamped value). The board decides policy
    (a downgrade needs ``allow_downgrade=True`` AND ``confirm_downgrade`` equal
    to the release tag; the same version needs ``force``; a pre-release needs
    ``allow_prerelease``, which also makes the board pick from the releases list
    instead of /releases/latest; on a board whose running version is unknown, a
    dev build, ``force`` also needs ``confirm_downgrade`` equal to the tag, and it
    is sent with ``force`` alone for that reason). Success is claimed only after a read-back of
    GET /api/update/stage shows a verified header with source github whose
    sha256 equals the one the board reported for the release; anything else is
    FAILED. A 409 (firing, hot zone, clock not
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
            f"nothing installed.{warn} (host={resolved})")


@_core._tool()
def update_get_settings(host: Optional[str] = None) -> str:
    """READ-ONLY. Report the GitHub repository the board checks for releases
    (GET /api/update/settings, ROUTE_TIER_ADMIN): the current owner/name, the
    compiled-in default, and whether the current value is the default."""
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
    request). Needs no typed confirmation (plan D5)."""
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
    (tag, size, sha256, policy verdict/allowed/needs_typed_confirm). Never starts a job."""
    resolved = _resolve_host(host)
    try:
        st = uhc.get_fetch_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"error: could not read GET {uhc.FETCH_PATH} (host={resolved}): {exc}"
    extra = f", http_status={st.get('http_status')}" if st.get("error") else ""
    # The wedge flag lives on the stage route (review 7 L1); best-effort read, never fails this tool.
    try:
        stg = uhc.get_stage_status(resolved)
    except uhc.UpdateHttpError:
        stg = {}
    if stg.get("fetch_writer_wedged"):
        extra += ", fetch_writer_wedged=True (WARNING: flash writer wedged; reboot required)"
    elif "fetch_writer_wedged" in stg:
        extra += ", fetch_writer_wedged=False"
    return f"ok - {_fmt_fetch(st)}, busy={st.get('busy')}{extra}; (host={resolved})"


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


def _install_line(st: dict) -> str:
    li = st.get("last_install") or {}
    return (f"running={st.get('running_version')!r}, staged={st.get('staged')} "
            f"({st.get('staged_version')!r}), last_install={li.get('state')}/{li.get('code')}"
            f"/{li.get('reason')}/{li.get('version')!r}")


#: Pending install evidence per host, kept across calls so a follow-up
#: update_install_staged(confirm=False) can use the same reboot proof as the
#: call that ran out of time (the install takes ~4 minutes on the bench, longer
#: than one synchronous call may run).
_PENDING_INSTALL: dict = {}

#: Hosts whose pending record expired: host -> requested version. Lets a
#: follow-up say "cannot confirm: record expired" instead of offering a fresh install.
_EXPIRED_INSTALL: dict = {}

#: Hosts with a confirmed install call in flight in THIS process (single flight:
#: a second confirmed call for the same host is refused, never a second POST).
#: The bench board lock is deliberately not taken: the release bench suite holds
#: it while it calls this tool, so taking it here would refuse that run.
_INSTALL_INFLIGHT: set = set()
_INSTALL_INFLIGHT_LOCK = threading.Lock()

#: A pending record older than this (seconds past its pre-POST read) is dropped:
#: past the longest wait any call makes (``_BG_WAIT_S``), so a late reboot
#: cannot count as this install.
_PENDING_TTL_S = 900.0

#: Slack for the whole-second truncation of two uptime_s readings: each reading
#: is floored, so (after - before) can be off by just under 1 s either way, and
#: the sum of both errors stays below 2 s.
_UPTIME_SLACK_S = 2.0

#: Synchronous call cap: the MCP client aborts a call after 300 s of silence.
_SYNC_WAIT_CAP_S = 270.0

#: Background (update_install_start) wait bound. Bench: the app answered again
#: at board uptime ~231-235 s after the POST in benchupd4/benchupd5 (recovery
#: boot, apply with up to 3 attempts, reboot, app boot, Wi-Fi), so 600 s is
#: about 2.5x the observed time and still well inside the pending TTL.
_BG_WAIT_S = 600.0

#: Identity of "no id": legacy records, or a board predating the id field.
_NO_ID = "00000000"


def _read_uptime(host: str, timeout: float = 5.0) -> Optional[float]:
    """Board uptime in seconds from GET /api/status, or None when unreadable."""
    try:
        v = dhc.get_status(host, timeout=timeout).get("uptime_s")
    except Exception:  # noqa: BLE001 - any failure means "no evidence"
        return None
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return None
    return float(v)


def _reboot_proven(host: str, pend: dict, deadline: Optional[float] = None) -> bool:
    """True when the uptime now is lower than the uptime before the POST plus
    the time elapsed since then, i.e. the board restarted in between. The read
    is charged to ``deadline`` (its timeout is clamped to what is left; none
    left means no read and no proof)."""
    before_up = pend.get("before_up")
    if before_up is None:
        return False
    timeout = 5.0
    if deadline is not None:
        timeout = min(5.0, deadline - time.monotonic())
        if timeout < 0.5:
            return False
    # "now" is taken BEFORE the read is sent, and t_pre AFTER the before-read
    # returned: request transit time is then never counted as board time, so
    # elapsed is a lower bound and a slow link cannot fake a reboot.
    elapsed = time.monotonic() - pend["t_pre"]
    after = _read_uptime(host, timeout)
    if after is None:
        return False
    return after + _UPTIME_SLACK_S < before_up + elapsed


def _id_of(li: dict) -> str:
    """The record's install identity (8 hex chars), '' when absent or all-zero."""
    v = li.get("id")
    if isinstance(v, str) and len(v) == 8 and v != _NO_ID:
        return v.lower()
    return ""


def _stale_id(li: dict, pend: dict) -> bool:
    """A record carrying a non-zero identity different from the staged image's
    belongs to some other install and is never evidence about this one."""
    rid = _id_of(li)
    return bool(rid and pend.get("id") and rid != pend["id"])


def _ok_is_new(host: str, st: dict, pend: dict, deadline: Optional[float] = None) -> bool:
    """Is a matching `ok` record evidence of THIS install (not a stale one)?"""
    li = st.get("last_install") or {}
    want = pend["want"]
    if not (st.get("running_version") == want and li.get("state") == "recorded" and li.get("code") == "ok"):
        return False
    if _stale_id(li, pend):
        return False
    rid = _id_of(li)
    if rid and rid == pend.get("id") and pend.get("before_ok_id") != rid:
        return True  # an ok record for this image that was not there before the POST
    if not pend["post_unknown"]:
        return True  # the POST was answered: the board accepted this request
    if pend["before_running"] != want:
        return True  # the running version changed from an older one to the requested one
    return _reboot_proven(host, pend, deadline)


def _failure_is_this(li: dict, pend: dict) -> bool:
    if not (li.get("state") == "recorded" and li.get("code") not in ("ok", None)
            and not _stale_id(li, pend)):
        return False
    if pend.get("post_unknown") and pend.get("before_fail") == _fail_key(li):
        # The POST got no answer and this record is identical to the one read before it:
        # it may be a stale earlier failure, not evidence about this install.
        return False
    return True


def _fail_key(li: dict) -> tuple:
    return (li.get("code"), li.get("reason"), _id_of(li))


def _staged_id(host: str) -> str:
    """First 4 bytes of the staged image's sha256 as 8 hex chars, '' when unreadable."""
    try:
        sha = uhc.get_stage_status(host).get("sha256")
    except uhc.UpdateHttpError:
        return ""
    if isinstance(sha, str) and len(sha) >= 8:
        try:
            int(sha[:8], 16)
        except ValueError:
            return ""
        return sha[:8].lower()
    return ""


def _install_impl(confirm: object, wait_s: float, host: Optional[str], cap_s: float) -> str:
    t0 = time.monotonic()
    # Whole call (initial GET, POST, settle sleep and polling) stays under the
    # caller's silence limit: deadline cap_s from entry, every read clamped to it.
    deadline = t0 + max(10.0, min(float(wait_s), cap_s))
    resolved = _resolve_host(host)
    try:
        before = uhc.get_install_status(resolved)
    except uhc.UpdateHttpError as exc:
        return f"error: could not read GET {uhc.INSTALL_STATUS_PATH} (host={resolved}): {exc}"
    if confirm is not True:
        pend = _PENDING_INSTALL.get(resolved)
        if pend is not None and time.monotonic() - pend["t_pre"] > _PENDING_TTL_S:
            _PENDING_INSTALL.pop(resolved, None)  # expired: cannot confirm an old install
            _EXPIRED_INSTALL[resolved] = pend["want"]
            pend = None
        if pend is not None:
            if _ok_is_new(resolved, before, pend):
                _PENDING_INSTALL.pop(resolved, None)
                return (f"ok - installed {pend['want']!r} and running in the application "
                        f"(confirmed by a follow-up read): {_install_line(before)} (host={resolved})")
            li = before.get("last_install") or {}
            if _failure_is_this(li, pend):
                _PENDING_INSTALL.pop(resolved, None)
                return f"FAILED: install recorded a failure: {_install_line(before)} (host={resolved})"
            return (f"UNKNOWN: an earlier install of {pend['want']!r} is still pending, not yet proven: "
                    f"{_install_line(before)}; poll again with update_install_staged(confirm=False) "
                    f"(host={resolved})")
        lost = _EXPIRED_INSTALL.get(resolved)
        bli0 = before.get("last_install") or {}
        if lost is None and not before.get("staged") and bli0.get("state") != "recorded":
            # Plain status read: nothing staged, no install ever recorded, no record of ours.
            return (f"ok - nothing staged and no install recorded (read-only, nothing sent): "
                    f"{_install_line(before)} (host={resolved})")
        if lost is not None or not before.get("staged"):
            what = (f"the pending record for {lost!r} expired" if lost is not None
                    else "this server has no pending install record")
            return (f"UNKNOWN: cannot confirm: {what} (records last {int(_PENDING_TTL_S)} s past the POST and "
                    f"are lost when the MCP server restarts), so whether an install happened is not "
                    f"provable from here: {_install_line(before)}; compare running_version with the "
                    f"version you meant to install via update_status (host={resolved})")
        return _refuse_unconfirmed(f"would install the staged image: {_install_line(before)} (host={resolved})")
    with _INSTALL_INFLIGHT_LOCK:
        if resolved in _INSTALL_INFLIGHT:
            return (f"REFUSED: another confirmed install call for this host is already in flight in this "
                    f"server; poll it (bench_test_job_status or update_install_staged(confirm=False)) "
                    f"(host={resolved})")
        _INSTALL_INFLIGHT.add(resolved)
    old_pend = _PENDING_INSTALL.get(resolved)
    try:
        return _install_confirmed(resolved, before, deadline)
    except Exception as exc:  # noqa: BLE001
        cur = _PENDING_INSTALL.get(resolved)
        if cur is not None and cur is not old_pend:
            # This call's pending record exists, so the POST was already sent: the
            # install may be running. Never let the wrapper's "error:" read as "never started".
            return (f"UNKNOWN: install may be running; {type(exc).__name__} after the install POST was sent: "
                    f"{exc}; poll again with update_install_staged(confirm=False) (host={resolved})")
        raise
    finally:
        with _INSTALL_INFLIGHT_LOCK:
            _INSTALL_INFLIGHT.discard(resolved)


def _install_confirmed(resolved: str, before: dict, deadline: float) -> str:
    if not before.get("staged") or before.get("stage_busy"):
        return f"REFUSED: no verified stage ready (staged={before.get('staged')}, stage_busy={before.get('stage_busy')}) (host={resolved})"
    _EXPIRED_INSTALL.pop(resolved, None)
    want = before.get("staged_version")
    inst_id = _staged_id(resolved)
    bli = before.get("last_install") or {}
    before_ok_id = _id_of(bli) if (bli.get("state") == "recorded" and bli.get("code") == "ok") else ""
    before_up = _read_uptime(resolved)
    t_pre = time.monotonic()  # after the read returned: transit time must not count as board time
    post_unknown = False
    try:
        uhc.start_install(resolved)
    except uhc.UpdateHttpError as exc:
        if exc.status is not None:
            return f"FAILED: install refused or lost: {_refusal_text(exc)} (host={resolved})"
        if isinstance(exc.__cause__, http_auth.HttpAuthError):
            # Relogin refused, lockout or no credential: the handler never ran. Definite, not "possibly applied".
            return f"FAILED: install not sent, authentication refused: {exc} (host={resolved})"
        # Transport failure/timeout: the board may have accepted the request and
        # rebooted. Do not call it failed; let the follow-up reads decide.
        post_unknown = True
    pend = {"want": want, "post_unknown": post_unknown, "before_running": before.get("running_version"),
            "before_up": before_up, "t_pre": t_pre, "id": inst_id, "before_ok_id": before_ok_id,
            "before_fail": _fail_key(bli) if (bli.get("state") == "recorded" and bli.get("code") not in ("ok", None)) else None}
    _PENDING_INSTALL[resolved] = pend
    time.sleep(min(15.0, max(0.0, deadline - time.monotonic())))  # let the board go down before polling
    last = None
    n404 = 0
    while time.monotonic() < deadline:
        try:
            st = uhc.get_install_status(resolved, timeout=max(1.0, min(5.0, deadline - time.monotonic())))
        except uhc.UpdateHttpError as exc:
            # 404: the recovery image answers the port and has no /api/update/status
            # while it applies the stage; that is progress, not absence.
            if exc.status == 404:
                n404 += 1
            time.sleep(min(5.0, max(0.0, deadline - time.monotonic())))
            continue
        last = st
        li = st.get("last_install") or {}
        if _ok_is_new(resolved, st, pend, deadline):
            # After a POST with no HTTP answer an `ok` may predate this call (same-version reinstall):
            # it counts only with proof of a reboot (uptime lower than before + elapsed).
            _PENDING_INSTALL.pop(resolved, None)
            return f"ok - installed {want!r} and running in the application: {_install_line(st)} (host={resolved})"
        if _failure_is_this(li, pend):
            _PENDING_INSTALL.pop(resolved, None)
            return f"FAILED: install recorded a failure: {_install_line(st)} (host={resolved})"
        time.sleep(min(5.0, max(0.0, deadline - time.monotonic())))
    if last:
        tail = _install_line(last)
    elif n404:
        if _read_uptime(resolved) is not None:
            tail = (f"HTTP 404 on {uhc.INSTALL_STATUS_PATH} x{n404} while GET /api/status answers: the board runs "
                    f"an application WITHOUT the install route (a downgrade?), not the recovery image applying")
        else:
            tail = f"HTTP 404 on {uhc.INSTALL_STATUS_PATH} x{n404}: the recovery image is still applying the stage"
    else:
        tail = "board never answered the application API"
    note = " The install POST got no HTTP answer (possibly applied)." if post_unknown else ""
    if (post_unknown and last and (last.get("last_install") or {}).get("code") == "ok"
            and last.get("running_version") == want):
        note += (" The board shows an `ok` install for this version but no reboot is proven (uptime did not drop),"
                 " so it may be a stale result from an earlier install, not this one.")
    return (f"UNKNOWN: install may still be running, not confirmed within this call's wait: board not confirmed on "
            f"{want!r} ({tail});{note} the bench install took about 4 minutes before the application answered "
            f"again, which can exceed one call: use update_install_start(confirm=True) (background job, waits up "
            f"to {int(_BG_WAIT_S)} s, poll bench_test_job_status) next time, and for this install poll again with "
            f"update_install_staged(confirm=False) (host={resolved})")


@_core._tool()
def update_install_staged(confirm: bool = False, wait_s: float = 240.0, host: Optional[str] = None) -> str:
    """Install the VERIFIED staged image unattended (POST /api/update/install,
    ROUTE_TIER_ADMIN, form confirm=1). The board writes a one-shot request,
    reboots into the recovery image, which applies the stage (bounded to 3
    attempts), boots the new app and records the result. No Wi-Fi joining.
    Without ``confirm=True`` (exactly) it reads GET /api/update/status
    (including the last install result) and sends nothing; if an earlier call
    of this tool timed out with the install still unconfirmed, that read also
    decides it: ok once the requested version runs with an ok record and, where
    the POST had no answer, the board's uptime proves a reboot. The record
    carries the first 4 bytes of the installed image's sha256 (``id``); a
    record with a different non-zero id is another install's and never counts.
    With no pending record (expired after 15 minutes, or the MCP server
    restarted) and nothing staged it answers UNKNOWN "cannot confirm".
    Refuses unless the stage reads staged. A firing or autotune makes the board
    answer 409. After the POST it waits up to ``wait_s`` (clamped to 270, total
    call under 280 s) for the board to answer again in the application with
    running_version equal to the staged version, then reports ok only if that
    matches and last_install is ok. The whole install can take about 4 minutes,
    longer than one call: prefer ``update_install_start`` (background job).
    UNKNOWN means it may still be running, so poll again with
    update_install_staged(confirm=False). FAILED means a recorded failure."""
    if confirm is not True:
        return _install_impl(False, wait_s, host, _SYNC_WAIT_CAP_S)
    return _install_impl(True, wait_s, host, _SYNC_WAIT_CAP_S)


def classify_install_report(report: str) -> str:
    """Job state from an install report: ``ok`` / ``failed`` (FAILED, REFUSED,
    error) / ``unknown`` (UNKNOWN: not confirmed within the wait)."""
    first = (report or "").lstrip()
    if first.startswith("ok"):
        return "ok"
    if first.startswith("UNKNOWN"):
        return "unknown"
    return "failed"


@_core._tool()
def update_install_start(confirm: bool = False, host: Optional[str] = None) -> str:
    """Background twin of `update_install_staged`: returns a job id at once.

    The job calls the real install routine with ``confirm`` unchanged, so the
    exactly-True gate, the stage precheck and every refusal still apply inside
    it and come back as the job's report. An unconfirmed call (confirm not
    exactly True) is answered synchronously (the same read-only status /
    pending-record answer as `update_install_staged(confirm=False)`) and
    starts no job. The job waits up to 600 s for the application to answer
    with the new version (the bench install took ~4 minutes: benchupd4/5 came
    back at board uptime 231-235 s; a 404 on /api/update/status meanwhile
    means the recovery image is still applying). Poll
    `bench_test_job_status(job_id, wait_s=100)`: OK / FAILED / UNKNOWN. A
    server restart loses the job (the install itself continues on the board;
    check update_status)."""
    if confirm is not True:
        return _install_impl(confirm, 0.0, host, _SYNC_WAIT_CAP_S)
    job_id = build_jobs.start_job(
        "update_install_staged",
        lambda: _install_impl(True, _BG_WAIT_S, host, _BG_WAIT_S),
        {"host": host, "wait_s": _BG_WAIT_S},
        classify=classify_install_report)
    return (f"bench-job {job_id}: STARTED (update_install_staged). Poll "
            f"bench_test_job_status(job_id=\"{job_id}\", wait_s=100); the install keeps "
            f"going on the board if you stop polling; waits up to {int(_BG_WAIT_S)} s.")
