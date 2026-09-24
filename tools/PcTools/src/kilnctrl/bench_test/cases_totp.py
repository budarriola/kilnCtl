"""TOTP password-reset suite (docs/TOTP_PASSWORD_RESET_PLAN.md sections 4/
6a). Unit-tested against a fake board ONLY -- never a real one. WT-A (the
firmware routes themselves) is being landed in a parallel session as of this
writing and did not exist when these cases were written; every case below is
written strictly against the wire contract recorded in
``totp_http_client.py``'s module docstring (sections 4/6a/6b), never against
an assumption about firmware behaviour this module's author cannot see.

Same shape as cases_web.py/cases_web_rw.py: each ``_case_tp_xxx(ctx)``
fetches (or, in tests, calls a fake injected via ``ctx``), then hands the
result to a pure judge function in judgments.py. Injectable ``ctx`` keys, all
optional (falling back to the real HTTP client when absent, exactly like
``_web_client()`` in cases_web.py falls back to a real ``WebUiClient``):

  * ``ctx["totp_status_fn"]``  -- ``() -> dict``, replaces
    ``totp_http_client.get_totp_status()``.
  * ``ctx["totp_forgot_fn"]``  -- ``(username, code) -> (status, body)``,
    replaces ``totp_http_client.post_forgot()``.
  * ``ctx["totp_reset_fn"]``   -- ``(username, reset_token, new_password) ->
    (status, body)``, replaces ``totp_http_client.post_reset()``.
  * ``ctx["totp_login_fn"]``   -- ``(origin, new_password) -> bool``, replaces
    the real ``http_auth.login()`` round trip TP-M01 uses to verify a reset.

TP-R01 and TP-R02 are strictly READ-ONLY: TP-R01 only ever GETs
``/api/auth/totp_status``; TP-R02 only ever POSTs a WRONG code to
``/api/auth/forgot`` and never calls ``/api/auth/reset`` with the resulting
token (memory project_smoke_case_wrote_estop_verification -- a read-only
case that quietly performs a write is exactly the failure class to avoid
here: a real ``/api/auth/reset`` call, even with a wrong code, still spends
the one-shot reset token and could plausibly perturb board state in some
future firmware revision, so this case simply never makes that call). TP-R03
also never completes a reset, for the same reason -- it only observes
whether ``forgot``/``reset`` refuse an unauthenticated caller, never
following through with a real token.

TP-M01 is the one mutating case, and it is opt-in the same way
``cases_ota.py``'s OT-E01 SKIPs without an ``ap_password``/image path and
``cases_web_rw.py``'s WEB-SEC-03/04 SKIP without their required env vars --
never a separate boolean flag. It SKIPs unless BOTH ``KILNCTL_TOTP_CODE``
and ``KILNCTL_WEB_PASSWORD_NEW`` are set in the environment. Neither value
is ever printed, logged, or included in any ``CaseResult`` field this module
builds -- only booleans/status codes, mirroring ``totp_reset_password()``'s
(mcp_server_totp.py) own discipline.
"""
from __future__ import annotations

import os
from typing import Optional

from . import judgments as J
from .registry import CaseResult, Verdict, get_case

#: Fixed, deliberately-wrong 6-digit code -- never a credential, never read
#: from the environment. TP-R02's whole point is that a WRONG code still
#: gets a 202/reset_token per the plan's anti-oracle design (section 6a).
_TP_R02_WRONG_CODE = "000000"

#: TP-R03 never completes a reset (see module docstring) -- these are
#: syntactically-plausible but obviously-fake values, the same convention
#: cases_web.py's WEB-SEC-05 uses for its deliberately-wrong password.
_TP_R03_WRONG_CODE = "000000"
_TP_R03_WRONG_TOKEN = "bench-test-deliberately-wrong-token"
_TP_R03_WRONG_PASSWORD = "bench-test-deliberately-wrong-password"

#: Same env vars totp_reset_password() (mcp_server_totp.py) reads -- reused
#: rather than inventing new names, per that module's own reasoning for why
#: a username reuses KILNCTL_WEB_USERNAME.
_USERNAME_ENV = "KILNCTL_WEB_USERNAME"
TOTP_CODE_ENV = "KILNCTL_TOTP_CODE"
NEW_PASSWORD_ENV = "KILNCTL_WEB_PASSWORD_NEW"


def _resolved_host(ctx: dict) -> Optional[str]:
    host = ctx.get("host")
    if host:
        return host
    try:
        from .. import mcp_server_ota  # local import: avoids importing kilnctrl.mcp_server at module load
        return mcp_server_ota._ota_resolve_host(None)
    except Exception:  # noqa: BLE001 - no board reachable is a legitimate "no host" case
        return None


def _totp_status(ctx: dict) -> dict:
    fn = ctx.get("totp_status_fn")
    if fn is not None:
        return fn()
    from .. import totp_http_client as thc
    host = _resolved_host(ctx)
    if not host:
        raise thc.TotpHttpError("no host configured")
    return thc.get_totp_status(host)


def _totp_forgot(ctx: dict, username: str, code: str) -> "tuple[Optional[int], dict]":
    fn = ctx.get("totp_forgot_fn")
    if fn is not None:
        return fn(username, code)
    from .. import totp_http_client as thc
    host = _resolved_host(ctx)
    if not host:
        raise thc.TotpHttpError("no host configured")
    return thc.post_forgot(host, username, code)


def _totp_reset(ctx: dict, username: str, reset_token: str, new_password: str) -> "tuple[Optional[int], dict]":
    fn = ctx.get("totp_reset_fn")
    if fn is not None:
        return fn(username, reset_token, new_password)
    from .. import totp_http_client as thc
    host = _resolved_host(ctx)
    if not host:
        raise thc.TotpHttpError("no host configured")
    return thc.post_reset(host, username, reset_token, new_password)


def _totp_login(ctx: dict, origin: str, new_password: str) -> bool:
    fn = ctx.get("totp_login_fn")
    if fn is not None:
        return fn(origin, new_password)
    from .. import http_auth
    http_auth._SESSIONS.pop(origin, None)
    env_backup = os.environ.get(http_auth.PASSWORD_ENV)
    os.environ[http_auth.PASSWORD_ENV] = new_password
    try:
        http_auth.login(origin)
        return True
    except http_auth.HttpAuthError:
        return False
    finally:
        if env_backup is None:
            os.environ.pop(http_auth.PASSWORD_ENV, None)
        else:
            os.environ[http_auth.PASSWORD_ENV] = env_backup


# ---------------------------------------------------------------------------
# TP-R01 -- GET /api/auth/totp_status (read-only).
# ---------------------------------------------------------------------------

def _case_tp_r01(ctx: dict) -> CaseResult:
    from .. import totp_http_client as thc
    try:
        data = _totp_status(ctx)
    except thc.TotpHttpError as exc:
        if getattr(exc, "status", None) == 404:
            # The route may not be flashed yet (WT-A landing in parallel) --
            # this is not evidence of a defect, so it is never a FAIL.
            return CaseResult(
                Verdict.INCONCLUSIVE,
                reason="GET /api/auth/totp_status returned 404 -- route may not be flashed yet",
                observed={"status": 404},
            )
        return CaseResult(
            Verdict.FAIL,
            reason=f"GET /api/auth/totp_status failed: {exc}",
            observed={"status": getattr(exc, "status", None)},
        )
    return J.judge_totp_status(data)


# ---------------------------------------------------------------------------
# TP-R02 -- POST /api/auth/forgot with a wrong code (read-only: never calls
# /api/auth/reset).
# ---------------------------------------------------------------------------

def _case_tp_r02(ctx: dict) -> CaseResult:
    username = ctx.get("username") or os.environ.get(_USERNAME_ENV) or "bench"
    from .. import totp_http_client as thc
    try:
        status, body = _totp_forgot(ctx, username, _TP_R02_WRONG_CODE)
    except thc.TotpHttpError as exc:
        return CaseResult(
            Verdict.FAIL,
            reason=f"POST /api/auth/forgot failed: {exc}",
            observed={"status": getattr(exc, "status", None)},
        )
    return J.judge_totp_forgot_probe(status, body)


# ---------------------------------------------------------------------------
# TP-R03 -- forgot/reset must not require a session (OPEN tier). Never
# completes a real reset (see module docstring).
# ---------------------------------------------------------------------------

def _case_tp_r03(ctx: dict) -> CaseResult:
    username = ctx.get("username") or os.environ.get(_USERNAME_ENV) or "bench"
    from .. import totp_http_client as thc

    try:
        forgot_status, _forgot_body = _totp_forgot(ctx, username, _TP_R03_WRONG_CODE)
    except thc.TotpHttpError as exc:
        forgot_status = getattr(exc, "status", None)

    try:
        reset_status, _reset_body = _totp_reset(ctx, username, _TP_R03_WRONG_TOKEN, _TP_R03_WRONG_PASSWORD)
    except thc.TotpHttpError as exc:
        reset_status = getattr(exc, "status", None)

    return J.judge_totp_open_tier(forgot_status, reset_status)


# ---------------------------------------------------------------------------
# TP-M01 -- the full reset round trip. Mutating; opt-in via env credentials,
# never a separate flag (see module docstring).
# ---------------------------------------------------------------------------

def _case_tp_m01(ctx: dict) -> CaseResult:
    username = ctx.get("username") or os.environ.get(_USERNAME_ENV) or ""
    code = ctx.get("totp_code") if "totp_code" in ctx else os.environ.get(TOTP_CODE_ENV)
    new_password = ctx.get("totp_new_password") if "totp_new_password" in ctx else os.environ.get(NEW_PASSWORD_ENV)

    missing = []
    if not username:
        missing.append(_USERNAME_ENV)
    if not code:
        missing.append(TOTP_CODE_ENV)
    if not new_password:
        missing.append(NEW_PASSWORD_ENV)
    if missing:
        return CaseResult(
            Verdict.SKIP,
            reason=f"missing required environment variable(s): {', '.join(missing)}",
            observed={},
        )

    from .. import totp_http_client as thc

    try:
        forgot_status, forgot_body = _totp_forgot(ctx, username, code)
    except thc.TotpHttpError as exc:
        return CaseResult(
            Verdict.FAIL,
            reason=f"POST /api/auth/forgot failed: {exc}",
            observed={"status": getattr(exc, "status", None)},
        )

    reset_token = forgot_body.get("reset_token") if isinstance(forgot_body, dict) else None
    reset_token_present = isinstance(reset_token, str) and bool(reset_token)

    reset_status: Optional[int] = None
    login_ok = False
    if forgot_status == 202 and reset_token_present:
        try:
            reset_status, _reset_body = _totp_reset(ctx, username, reset_token, new_password)
        except thc.TotpHttpError as exc:
            return CaseResult(
                Verdict.FAIL,
                reason=f"POST /api/auth/reset failed: {exc}",
                observed={"status": getattr(exc, "status", None)},
            )
        if reset_status == 200:
            host = _resolved_host(ctx)
            origin = f"http://{host}" if host else ""
            login_ok = _totp_login(ctx, origin, new_password)

    return J.judge_totp_reset_roundtrip(forgot_status, reset_token_present, reset_status, login_ok)


#: Wire this suite's cases into the shared REGISTRY (see registry.py's
#: module docstring for why ids are declared there and wired up here at
#: import time -- same convention as cases_web.py's _CASE_FUNCS tail).
_CASE_FUNCS = {
    "TP-R01": _case_tp_r01,
    "TP-R02": _case_tp_r02,
    "TP-R03": _case_tp_r03,
    "TP-M01": _case_tp_m01,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
