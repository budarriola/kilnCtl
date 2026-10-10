"""TOTP password-reset -- MCP surface for docs/TOTP_PASSWORD_RESET_PLAN.md's
WT-C tranche, wrapping the two OPEN-tier reset routes plus a read of
enrollment status, over :mod:`totp_http_client`. Registered the way the
most recent tools in this package were (``estop_verify``, ``boot_guard_get``
-- see ``mcp_server_info.py``): a small, self-contained module, imported for
its ``@_core._tool()`` side effects by ``mcp_server.py``.

The firmware routes (``auth_totp_http.c``) exist; see
``totp_http_client.py``'s module docstring for the request/response
contract and ``docs/TOTP_PASSWORD_RESET_PLAN.md``.

Enrollment itself is deliberately NOT exposed here -- the plan's owner
decision is that enrollment (generating a secret, showing the QR/manual
key, verifying the first code before committing it to NVS) stays a
settings-page-only flow (docs/TOTP_PASSWORD_RESET_PLAN.md section 2), never
an MCP call: an MCP tool that could mint or replace the board's TOTP secret
would be a strictly more sensitive capability than anything else in this
module, with no bench-testing need this plan identifies.

Credentials for the reset flow come ONLY from two environment variables,
per this task's instruction -- never a call parameter, so a live TOTP code
or a new password is never visible in an MCP call log or transcript, the
same rule this repo already applies to KILNCTL_WEB_USERNAME/
KILNCTL_WEB_PASSWORD (see http_auth.py and CLAUDE.md's Credentials
section):

  * ``KILNCTL_TOTP_CODE`` -- the 6-digit code from the operator's own
    authenticator app, for ``POST /api/auth/forgot``.
  * ``KILNCTL_WEB_PASSWORD_NEW`` -- the new password to set, for
    ``POST /api/auth/reset``. Deliberately a DIFFERENT variable from
    ``KILNCTL_WEB_PASSWORD`` (the CURRENT/expected credential every other
    ADMIN-tier tool in this package logs in with) -- conflating them would
    mean a successful reset silently invalidates every other tool's stored
    credential expectation for the rest of the process, and there is
    exactly one call site (``verify_new_password_login()`` below) that
    needs to try logging in with the NEW value specifically.

Neither value is ever printed, logged, echoed, or included in a returned
string -- every result string is built from booleans/status codes/generic
text only, checked by this module's own tests
(``tools/PcTools/tests/test_mcp_server_totp.py``).
"""
from __future__ import annotations

import os
from typing import Optional

from . import http_auth
from . import mcp_server_core as _core
from . import totp_http_client as thc

#: Per this module's docstring -- deliberately not KILNCTL_TOTP-prefixed
#: aliases of the existing web-auth env vars, and deliberately not call
#: parameters.
TOTP_CODE_ENV = "KILNCTL_TOTP_CODE"
NEW_PASSWORD_ENV = "KILNCTL_WEB_PASSWORD_NEW"

#: A username is required by both routes' request bodies (section 4). This
#: package's existing ADMIN-tier tools all assume a single administrator
#: identified by KILNCTL_WEB_USERNAME -- reused here rather than inventing a
#: third env var, since "whose password is this resetting" is exactly the
#: same identity every other tool in this package already assumes.
_USERNAME_ENV = http_auth.USERNAME_ENV


@_core._tool()
def totp_enroll_status(host: Optional[str] = None) -> str:
    """READ-ONLY: report whether TOTP is enrolled for the board's
    administrator account (GET /api/auth/totp_status, ROUTE_TIER_ADMIN --
    docs/TOTP_PASSWORD_RESET_PLAN.md section 7, firmware route in
    ``auth_totp_http.c``). Reports ``{"enrolled": bool}`` plus the
    board's reported time and SNTP sync state -- the plan's section 3 makes
    TOTP verification meaningless without a synced clock, so an operator
    deciding whether the reset flow will even work needs both facts
    together, not enrollment alone.

    NEVER reports a secret, a seed, or a QR payload -- there is no such
    field in this route's response to begin with (see
    totp_http_client.py's docstring for the exact contract), and this tool
    would refuse to print one even if the board's JSON grew one later.

    Host is auto-resolved the same way get_readiness()/estop_verify() do;
    pass `host` explicitly for kilnctl.local or a board reachable only
    from a different network than this link's serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as get_readiness()

    resolved = _ota_resolve_host(host)
    try:
        data = thc.get_totp_status(resolved)
    except thc.TotpHttpError as exc:
        return f"error reading TOTP enrollment status over HTTP (host={resolved}): {exc}"

    enrolled = data.get("enrolled")
    # Never surface any key besides the ones this tool documents, even if
    # the board's JSON grows an unexpected field later.
    board_time = data.get("board_time_utc")
    sntp_synced = data.get("sntp_synced")
    lines = [
        f"host={resolved}",
        f"enrolled={enrolled!r}",
    ]
    if board_time is not None:
        lines.append(f"board_time_utc={board_time!r}")
    if sntp_synced is not None:
        lines.append(f"sntp_synced={sntp_synced!r}")
        if sntp_synced is False:
            lines.append(
                "WARNING: board clock is not SNTP-synced -- TOTP codes cannot be verified "
                "reliably (or at all) until it is, per docs/TOTP_PASSWORD_RESET_PLAN.md section 3")
    lines.append(f"summary: enrolled={enrolled!r}")
    return "\n".join(lines)


@_core._tool()
def totp_reset_password(confirm: bool = False, host: Optional[str] = None) -> str:
    """Run the TOTP-based "forgot password" flow end to end: POST
    /api/auth/forgot (verifies a TOTP code, issues a short-lived reset
    token) then POST /api/auth/reset (consumes that token, sets a new
    password) -- docs/TOTP_PASSWORD_RESET_PLAN.md section 4. Both routes
    are ROUTE_TIER_OPEN by design (that is the whole point of a
    forgot-password flow), so this tool sends no session cookie for either
    call.

    THE CODE AND THE NEW PASSWORD ARE NEVER CALL PARAMETERS. They come only
    from two environment variables, per this task's explicit instruction,
    kept out of any MCP call log the same way every other credential in
    this package is kept out (CLAUDE.md's Credentials section):

      * ``KILNCTL_TOTP_CODE`` -- the current 6-digit code from the
        operator's own authenticator app.
      * ``KILNCTL_WEB_PASSWORD_NEW`` -- the new password to set. NOT the
        same variable as ``KILNCTL_WEB_PASSWORD`` (see this module's
        docstring for why).

    REFUSES UNLESS ``confirm`` IS EXACTLY ``True`` -- same confirm-gate
    convention as ``estop_verify()``/``crash_report_ack()``. Without it,
    this is a dry run: it reports which of the two required environment
    variables are present (as booleans only, never values) and does
    nothing else -- no HTTP request of any kind is sent.

    REFUSES IF EITHER REQUIRED ENVIRONMENT VARIABLE IS MISSING OR EMPTY,
    regardless of ``confirm`` -- naming ONLY the missing variable's NAME,
    never attempting a partial reset with one credential present and the
    other absent (a code with no destination password, or a password with
    no verifying code, are both meaningless requests to send).

    Reports outcomes as booleans only -- never the board's raw response
    body, which could otherwise leak the opaque reset-token value or the
    (deliberately generic) failure text into a transcript for no
    diagnostic benefit; a status code alone is enough to know what
    happened, per the plan's documented response shapes (200/400/429/503).

    After a successful ``/api/auth/reset`` (HTTP 200), this then attempts
    an actual login with the NEW credential via the existing
    :mod:`http_auth` login path (the same one every ADMIN-tier tool in this
    package relies on) and reports that as a final boolean -- an
    ``{"ok": true}`` reset response is not trusted alone, same rule
    ``estop_verify()``/``crash_report_ack()`` already apply to a board's
    own self-reported success (CLAUDE.md's boot_guard write-lies section:
    an unverified success report is exactly the failure class this project
    has been bitten by before).

    Host is auto-resolved the same way get_readiness()/estop_verify() do;
    pass `host` explicitly for kilnctl.local or a board reachable only
    from a different network than this link's serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as get_readiness()

    username = os.environ.get(_USERNAME_ENV, "")
    code = os.environ.get(TOTP_CODE_ENV, "")
    new_password = os.environ.get(NEW_PASSWORD_ENV, "")

    missing = []
    if not username:
        missing.append(_USERNAME_ENV)
    if not code:
        missing.append(TOTP_CODE_ENV)
    if not new_password:
        missing.append(NEW_PASSWORD_ENV)
    if missing:
        return (f"refused: missing required environment variable(s): "
                f"{', '.join(missing)} -- set them and retry (values are never accepted as "
                f"call parameters)")

    if confirm is not True:
        return (
            f"DRY RUN (pass confirm=True, exactly, to actually run the reset) -- "
            f"{_USERNAME_ENV} present=True, {TOTP_CODE_ENV} present=True, "
            f"{NEW_PASSWORD_ENV} present=True"
        )

    resolved = _ota_resolve_host(host)

    try:
        forgot_status, forgot_body = thc.post_forgot(resolved, username, code)
    except thc.TotpHttpError as exc:
        return f"failed: POST /api/auth/forgot error (host={resolved}): {exc}"

    if forgot_status == 429:
        return f"failed: POST /api/auth/forgot rate-limited (HTTP 429) (host={resolved})"
    if forgot_status == 503:
        return (f"failed: POST /api/auth/forgot refused (HTTP 503) -- board clock not "
                f"SNTP-synced yet, TOTP cannot be verified (host={resolved})")
    if forgot_status != 202:
        return (f"failed: POST /api/auth/forgot returned unexpected HTTP {forgot_status} "
                f"(host={resolved})")

    reset_token = forgot_body.get("reset_token")
    if not reset_token:
        return (f"failed: POST /api/auth/forgot returned HTTP 202 but no reset_token in "
                f"its body (host={resolved})")

    try:
        reset_status, _reset_body = thc.post_reset(resolved, username, reset_token, new_password)
    except thc.TotpHttpError as exc:
        return f"failed: POST /api/auth/reset error (host={resolved}): {exc}"

    if reset_status == 429:
        return f"failed: POST /api/auth/reset rate-limited (HTTP 429) (host={resolved})"
    if reset_status == 503:
        return (f"failed: POST /api/auth/reset refused (HTTP 503) -- board clock not "
                f"SNTP-synced yet (host={resolved})")
    if reset_status != 200:
        return (f"failed: POST /api/auth/reset returned HTTP {reset_status} -- code and/or "
                f"new password rejected (host={resolved})")

    origin = f"http://{resolved}"
    # The old password's session (if any) is now stale -- forget it so the
    # login attempt below cannot accidentally reuse a cookie issued under
    # the credential that was just replaced.
    http_auth._SESSIONS.pop(origin, None)
    # Never touch os.environ (other threads' logins would see the new
    # password mid-flight): hand the credential straight to the login.
    try:
        http_auth.login(origin, password_override=new_password)
        login_ok = True
    except http_auth.HttpAuthError:
        login_ok = False

    if login_ok:
        return f"ok - password reset via TOTP and verified by login (host={resolved})"
    return (f"FAILED: POST /api/auth/reset reported success (HTTP 200), but a login attempt "
            f"with the new credential did not succeed -- do not trust the reset as complete "
            f"(host={resolved})")

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
