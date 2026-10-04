"""WEB AUTH SETUP -- MCP surface for bootstrapping the board's web admin
credential and turning web authentication on, matched exactly against
security_backend_web_auth.c/security_http.c and the /settings/security
page's own JS (net/security_page.html). Part of the mcp_server.py split
pattern -- see that module's docstring for the overall map. This tool never
reads a credential from anywhere but the environment, and never writes,
logs, or echoes one back.

Also home to ``web_auth_logout()``, the PC-side counterpart: it ends
whatever admin session ``http_auth.urlopen()``'s own 401-retry login
established for this process, via ``http_auth.logout()``. Kept in this
module rather than a new file since it is a one-line wrapper over the same
``/api/auth`` surface and the same env-credential-only, never-echo rules
``web_auth_setup()`` already documents above.
"""
from __future__ import annotations

import os
from typing import Optional

from . import http_auth
from . import mcp_server_core as _core
from . import web_auth_setup_http_client as wac
from .http_auth import PASSWORD_ENV, USERNAME_ENV


def _describe_login_error(exc: wac.WebAuthSetupHttpError) -> str:
    """Brand a 429 distinctly as the login lockout (project_login_lockout_
    saturation_accepted -- ~1 request/19s per off-subnet address, an owner-
    accepted saturation point, not a bug) rather than letting it read as a
    generic, unexplained failure."""
    if exc.status == 429:
        return f"HTTP 429 (login rate-limited/locked out -- wait before retrying): {exc}"
    return str(exc)


# security_timeout_minutes_is_valid() (security_http_core.c): -1 means
# "never expire", 1-60 is minutes, anything else is rejected by set_policy
# as a 400. These are the substitutes used when the pre-fetch reads -1 for
# a reason other than a deliberate "never expire" choice -- see
# _resolve_timeout()'s docstring.
_DEFAULT_WEB_TIMEOUT_MIN = 30
_DEFAULT_LCD_TIMEOUT_MIN = 10


def _timeout_out_of_range(value: Optional[int]) -> bool:
    return value is not None and value != -1 and not (1 <= value <= 60)


def _resolve_timeout(label: str, default: int, raw, override: Optional[int]):
    """Pick the value to send to set_policy for one timeout field, and a
    human-readable note when that value was not simply echoed from the
    pre-fetch.

    ``web_auth_backend_get_config()`` (security_backend_web_auth.c:293-294)
    collapses "no policy record yet" (the state right after an NVS erase)
    onto the same -1 sentinel that ``security_timeout_minutes_is_valid()``
    (security_http_core.c:26-31) treats as a deliberate, valid "never
    expire". Blindly echoing a pre-fetched -1 back into set_policy silently
    persists never-expire on a board that never actually asked for it.

    ``override`` (an explicit caller-supplied value) always wins, including
    an explicit -1 -- that is a deliberate choice and is honored, just
    flagged loudly. Otherwise, a raw pre-fetch value of -1 is treated as the
    ambiguous "no record" case and replaced with ``default``.
    """
    if override is not None:
        if override == -1:
            return override, (f"{label}=-1 requested explicitly -- sessions will NEVER expire")
        return override, None
    if raw == -1:
        return default, (f"{label} read -1 from the board (this after an NVS erase means 'no "
                          f"policy record yet', not a deliberate 'never expire' choice -- "
                          f"security_backend_web_auth.c's ABSENT case collapses onto the same "
                          f"sentinel security_http_core.c treats as valid) -- substituted "
                          f"default {default}")
    return raw, None


@_core._tool()
def web_auth_setup(host: Optional[str] = None, confirm: bool = False,
                    enable_web_auth: bool = True,
                    web_timeout_min: Optional[int] = None,
                    lcd_timeout_min: Optional[int] = None) -> str:
    """Bootstrap the board's administrator web credential from the
    environment (``KILNCTL_WEB_USERNAME``/``KILNCTL_WEB_PASSWORD`` -- never
    a parameter, never logged; this report only ever says whether each is
    ``present``) and, by default, turn web authentication on.

    Reads GET /api/auth/config first, then does exactly one of three things
    depending on what it finds -- the same three states the
    ``/settings/security`` page itself distinguishes
    (security_backend_web_auth.c / security_http.c):

    1. Web auth is ON and no admin record exists yet
       (``web_auth_admin_bootstrap_needed()`` true on the board): POSTs
       /api/auth/bootstrap_password (ROUTE_TIER_ADMIN_BOOTSTRAP -- reachable
       with no session, and reachable ONLY in this state; the board answers
       409 the moment an admin record exists).
    2. Web auth is OFF and no admin record exists (the state after an NVS
       erase): sets the admin password via POST /api/auth/security
       (``cmd=set_web_password&role=admin``), then -- only if
       ``enable_web_auth`` is true -- POSTs ``cmd=set_policy`` to turn
       ``web_enabled`` on, preserving the board's current ``lcd_enabled``
       exactly as read back in step 1, and its ``web_timeout_min``/
       ``lcd_timeout_min`` UNLESS the pre-fetch read -1 for one -- see the
       "Timeouts" paragraph below (the page's own ``save()`` always echoes
       all four policy fields together; set_policy's parser requires
       web_timeout_min/lcd_timeout_min on every call). Per
       http_auth_check() (http_auth_enforce.c),
       ROUTE_TIER_ADMIN collapses to unconditional ALLOW while
       ``web_enabled`` is false, so setting the password here needs no
       session at all -- confirmed against the firmware source, not
       inferred from behaviour.
    3. An admin record already exists: this tool writes nothing. If the
       pre-fetch GET /api/auth/config itself succeeded while web auth reads
       ON, that success was only possible over an authenticated session for
       this exact credential (reused, or just established by
       ``http_auth.urlopen()``'s own 401-retry login) -- reported as
       "already configured, credentials valid" with no separate login POST,
       to avoid a second, redundant login against the same per-IP lockout
       the firmware enforces. Otherwise (web auth reads OFF, so the GET
       proved nothing about the credential) it logs in once explicitly
       (POST /api/auth/login, form-encoded, ``Accept-Encoding: identity``)
       and reports success only if that login succeeds. A 401 is reported
       as a failure and this tool stops -- it never retries a login and
       never guesses a different credential.

    If the pre-fetch GET itself is denied (401/403 -- the state a board with
    web auth already ON and no admin record yet produces, since that GET is
    itself ROUTE_TIER_ADMIN and no login can succeed with no admin record to
    check against), this tool treats it as case 1 above rather than failing
    outright: it proceeds straight to POST /api/auth/bootstrap_password,
    which requires no session at all and 409s harmlessly if an admin record
    turns out to already exist (that 409 is then resolved with exactly one
    login check). Only a pre-fetch failure whose message says the board is
    unreachable is treated as a hard error.

    REFUSES every write unless ``confirm=True`` -- without it, this is a
    dry run: it reports which of the three states the board is in and what
    it WOULD do, but sends no POST (case 3's login check still runs, since
    a login is not a write). Also refuses outright, before any HTTP call,
    if either environment variable is absent.

    Timeouts: ``web_auth_backend_get_config()``
    (security_backend_web_auth.c:293-294) reports -1 for both
    ``web_timeout_min``/``lcd_timeout_min`` when no policy record exists yet
    (e.g. right after an NVS erase) -- the exact same sentinel
    ``security_timeout_minutes_is_valid()`` (security_http_core.c:26-31)
    treats as a deliberate, valid "never expire". Echoing that -1 straight
    back into ``set_policy`` (case 2's own enable step) would silently
    persist never-expire on a board that never asked for it. So when the
    pre-fetch reads -1 for either field and the caller did not pass an
    explicit override below, this tool substitutes a default (30 for
    ``web_timeout_min``, 10 for ``lcd_timeout_min``) instead of echoing -1,
    and names the substitution in the result. Pass ``web_timeout_min``/
    ``lcd_timeout_min`` explicitly to override either default outright;
    each is validated against the firmware's own rule (-1, or 1-60) and
    refused before any HTTP call if out of range -- this validation always
    runs, regardless of which of the three cases the board turns out to be
    in. The resolved (post-substitution) values are also named in a DRY
    RUN's "would: ..." line. Only case 2 actually sends them anywhere
    (case 1's bootstrap_password route never calls set_policy, and case 3
    writes nothing), so outside case 2 these two parameters are validated
    but otherwise have no effect.

    After any write, re-reads GET /api/auth/config and fails loud (does not
    report success) if the result disagrees with what was requested --
    ``admin_password_set`` still false after setting it, or ``web_enabled``
    still false after asking to enable it -- the same "never trust a
    write's own ok:true alone" rule this codebase's other ADMIN-tier write
    tools already follow (see CLAUDE.md's boot_guard write-lies section).

    Host is auto-resolved the same way get_heap_status()/the OTA/control
    tools do; pass ``host`` explicitly for kilnctl.local or a board reachable
    only from a different network than this link's serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as get_heap_status()

    username = os.environ.get(USERNAME_ENV) or ""
    password = os.environ.get(PASSWORD_ENV) or ""
    have_username = bool(username)
    have_password = bool(password)
    presence = f"{USERNAME_ENV} present={have_username}, {PASSWORD_ENV} present={have_password}"
    if not have_username or not have_password:
        return (f"refused: missing credential in the environment ({presence}). Set both "
                f"{USERNAME_ENV} and {PASSWORD_ENV} in the shell that launches this MCP server.")

    for _label, _val in (("web_timeout_min", web_timeout_min), ("lcd_timeout_min", lcd_timeout_min)):
        if _timeout_out_of_range(_val):
            return (f"refused: {_label}={_val} is out of range -- the firmware accepts -1 "
                     f"(never expire) or 1-60 minutes only "
                     f"(security_timeout_minutes_is_valid(), security_http_core.c:26-31)")

    resolved = _ota_resolve_host(host)

    # GET /api/auth/config is itself ROUTE_TIER_ADMIN (http_auth_enforce.c).
    # With web auth already ON and no admin record yet, this pre-fetch
    # cannot succeed: there is no session, and http_auth's own 401-retry
    # login attempt cannot succeed either (there is no admin credential yet
    # for it to check against) -- it raises before ever reaching this
    # function, either as a plain 401 HTTPError (wac.get_auth_config's own
    # urllib.error.HTTPError branch) or wrapped from http_auth.HttpAuthError
    # (the retried login itself being refused). Both are indistinguishable
    # from "board unreachable" only by message text, so unreachable is ruled
    # out explicitly and everything else is treated as "web auth is ON and
    # this route is gated" -- exactly the state bootstrap_password() exists
    # for. That POST requires no session at all (ROUTE_TIER_ADMIN_BOOTSTRAP)
    # and answers 409 harmlessly if an admin record turns out to already
    # exist, so proceeding here is always safe, never destructive.
    config_readable = True
    before: dict = {}
    try:
        before = wac.get_auth_config(resolved)
    except wac.WebAuthSetupHttpError as exc:
        if "unreachable" in str(exc).lower():
            return f"error: could not read GET /api/auth/config (host={resolved}): {exc}"
        config_readable = False

    web_enabled = bool(before.get("web_enabled")) if config_readable else True
    admin_configured = bool(before.get("admin_password_set")) if config_readable else False
    lcd_enabled = bool(before.get("lcd_enabled"))
    # Raw values exactly as the board reported them (or -1 default if the
    # field/whole read is missing) -- kept separate from the *effective*
    # values computed below (which may substitute a default for a raw -1,
    # or honor an explicit caller override) so the "before" report always
    # reflects what the board actually said, not what this tool decided to
    # send back to it.
    raw_lcd_timeout_min = before.get("lcd_timeout_min", -1)
    raw_web_timeout_min = before.get("web_timeout_min", -1)
    # Computed here (not just before the set_policy call) so a DRY RUN --
    # which never reaches that call -- still reports the value this tool
    # would actually send and names any substitution, rather than echoing
    # the board's raw -1 with no explanation.
    eff_web_timeout_min, web_timeout_note = _resolve_timeout(
        "web_timeout_min", _DEFAULT_WEB_TIMEOUT_MIN, raw_web_timeout_min, web_timeout_min)
    eff_lcd_timeout_min, lcd_timeout_note = _resolve_timeout(
        "lcd_timeout_min", _DEFAULT_LCD_TIMEOUT_MIN, raw_lcd_timeout_min, lcd_timeout_min)
    timeout_notes = [n for n in (web_timeout_note, lcd_timeout_note) if n]
    if config_readable:
        state_line = (f"before: web_enabled={web_enabled} admin_password_set={admin_configured} "
                      f"lcd_enabled={lcd_enabled} web_timeout_min={raw_web_timeout_min} "
                      f"lcd_timeout_min={raw_lcd_timeout_min} ({presence}, host={resolved})")
    else:
        state_line = (f"before: GET /api/auth/config was refused -- treating this as web auth ON "
                      f"with no admin record yet (the only state that denies this ADMIN-tier read "
                      f"with no way to log in) ({presence}, host={resolved})")
    # Only case 2 (web auth confirmed OFF, no admin record yet) ever sends
    # these values anywhere (via set_policy, in the real write or named in
    # a dry run's "would:" line) -- appending the note in cases 1/3 too
    # would claim a substitution that never happens: case 1's raw_* is only
    # the before.get(..., -1) fallback over an unreadable config, and case 3
    # writes nothing at all regardless of what the board's timeouts read.
    in_case_2 = config_readable and not web_enabled and not admin_configured and enable_web_auth
    if timeout_notes and in_case_2:
        state_line += "\n" + "\n".join(f"NOTE: {n}" for n in timeout_notes)

    # Case 3: an admin record already exists. Whatever web_enabled reads,
    # this tool never bootstraps over an existing admin record -- it only
    # verifies the environment credential actually works.
    if admin_configured:
        # If the pre-fetch above succeeded on an ADMIN-tier route while
        # web_enabled is true, that success was only possible with a valid
        # session for this exact credential -- either reused from an
        # earlier call in this same long-running server process, or just
        # established by http_auth.urlopen()'s own 401-retry login. A
        # second POST /api/auth/login here would be a redundant login
        # attempt against the same per-IP lockout the firmware enforces
        # (see CLAUDE.md's login-lockout note) -- skip it. When web_enabled
        # reads false, GET /api/auth/config needed no session at all, so
        # this success proves nothing about the credential and the explicit
        # check below is still required.
        if config_readable and web_enabled:
            return (f"already configured, credentials valid (confirmed by the successful "
                    f"GET /api/auth/config read above -- no separate login was sent)\n{state_line}")
        try:
            ok = wac.try_login(resolved, username, password)
        except wac.WebAuthSetupHttpError as exc:
            return f"error: login check failed (host={resolved}): {_describe_login_error(exc)}\n{state_line}"
        if ok:
            return f"already configured, credentials valid\n{state_line}"
        return (f"failed: administrator credential already configured, but the environment "
                f"credential was refused (401) -- never retried\n{state_line}")

    # No admin record yet (or config was unreadable, which this tool treats
    # the same way -- see above). Case 1 (web auth already ON, or assumed ON
    # because the pre-fetch was denied) uses the bootstrap route; case 2
    # (web auth confirmed OFF) sets the password directly, since that route
    # requires no session at all while web_enabled is false.
    if not confirm:
        if not config_readable:
            action = "POST /api/auth/bootstrap_password (config unreadable; see above)"
        elif web_enabled:
            action = "POST /api/auth/bootstrap_password"
        else:
            action = ("POST /api/auth/security (set_web_password)"
                       + (f" then set_policy(web_enabled=1, web_timeout_min={eff_web_timeout_min}, "
                          f"lcd_timeout_min={eff_lcd_timeout_min})" if enable_web_auth else ""))
        return f"DRY RUN (pass confirm=True to actually set it up) -- would: {action}\n{state_line}"

    if not config_readable or web_enabled:
        try:
            wac.post_bootstrap_password(resolved, username, password)
        except wac.WebAuthSetupHttpError as exc:
            if exc.status == 409:
                if not config_readable:
                    # The unreadable pre-fetch above was ambiguous between
                    # "bootstrap needed" and "admin exists, wrong
                    # credential" -- this 409 resolves it to the latter.
                    # One login attempt now gives the caller a real signal
                    # instead of just "failed".
                    try:
                        ok = wac.try_login(resolved, username, password)
                    except wac.WebAuthSetupHttpError as login_exc:
                        return (f"failed: an administrator credential already exists (409 from "
                                f"bootstrap_password); login check also failed: "
                                f"{_describe_login_error(login_exc)}\n{state_line}")
                    if ok:
                        return (f"already configured, credentials valid (config was unreadable "
                                f"beforehand, resolved via bootstrap_password's 409)\n{state_line}")
                    return (f"failed: an administrator credential already exists (409 from "
                            f"bootstrap_password), and the environment credential was refused "
                            f"(401) -- never retried\n{state_line}")
                return (f"failed: board reports an administrator credential already exists (409) "
                        f"even though the pre-fetch above saw none\n{state_line}: {exc}")
            if exc.status == 400:
                return f"failed: password rejected (400, likely too weak)\n{state_line}: {exc}"
            return f"failed: POST /api/auth/bootstrap_password: {_describe_login_error(exc)}\n{state_line}"
    else:
        try:
            result = wac.post_security(resolved, {
                "cmd": "set_web_password", "role": "admin",
                "username": username, "password": password,
            })
        except wac.WebAuthSetupHttpError as exc:
            return f"failed: POST /api/auth/security (set_web_password): {_describe_login_error(exc)}\n{state_line}"
        if not result.get("ok"):
            return f"failed: set_web_password rejected: {result.get('error')!r}\n{state_line}"

        if enable_web_auth:
            # Never trust this write's own {"ok":true} alone (the same rule
            # CLAUDE.md's boot_guard write-lies section applies elsewhere):
            # read the config back and confirm the password actually stuck
            # BEFORE turning web_enabled on. Turning auth on over a password
            # that didn't really persist would strand the board -- gated,
            # with no credential that works.
            try:
                mid = wac.get_auth_config(resolved)
            except wac.WebAuthSetupHttpError as exc:
                return (f"set_web_password reported ok, but could not read back GET "
                        f"/api/auth/config to confirm it before enabling web auth -- refusing "
                        f"to enable web auth: {exc}\n{state_line}")
            if not mid.get("admin_password_set"):
                return (f"FAILED verification: set_web_password reported ok:true but "
                        f"admin_password_set still reads false -- refusing to enable web auth "
                        f"over an unconfirmed credential\n{state_line}")
            try:
                policy_result = wac.post_security(resolved, {
                    "cmd": "set_policy",
                    "web_enabled": "1",
                    "lcd_enabled": "1" if lcd_enabled else "0",
                    "web_timeout_min": str(eff_web_timeout_min),
                    "lcd_timeout_min": str(eff_lcd_timeout_min),
                })
            except wac.WebAuthSetupHttpError as exc:
                return (f"password set, but enabling web auth failed: POST /api/auth/security "
                        f"(set_policy): {exc}\n{state_line}")
            if not policy_result.get("ok"):
                return (f"password set, but enabling web auth was rejected: "
                        f"{policy_result.get('error')!r}\n{state_line}")

    try:
        after = wac.get_auth_config(resolved)
    except wac.WebAuthSetupHttpError as exc:
        return f"write(s) sent but could not read back GET /api/auth/config to verify: {exc}\n{state_line}"

    after_admin = bool(after.get("admin_password_set"))
    after_web_enabled = bool(after.get("web_enabled"))
    after_line = (f"after: web_enabled={after_web_enabled} admin_password_set={after_admin} "
                  f"lcd_enabled={after.get('lcd_enabled')} web_timeout_min={after.get('web_timeout_min')} "
                  f"lcd_timeout_min={after.get('lcd_timeout_min')}")

    if not after_admin:
        return (f"FAILED verification: admin_password_set still false after a reported-ok "
                f"write -- do not trust the {{'ok':true}} response alone\n{state_line}\n{after_line}")
    if enable_web_auth and not after_web_enabled:
        return (f"FAILED verification: web_enabled still false after requesting "
                f"enable_web_auth=True\n{state_line}\n{after_line}")

    return f"ok: administrator credential configured{' and web auth enabled' if enable_web_auth else ' (web auth left as-is)'}\n{state_line}\n{after_line}"


@_core._tool()
def web_auth_logout(host: Optional[str] = None) -> bool:
    """End this process's own remembered admin web session at ``host``, if
    any, via POST /api/auth/logout (web_auth_login_http.c's logout handler,
    ROUTE_TIER_USER -- any authenticated session).

    This is the PC-side counterpart of ``http_auth.urlopen()``'s own
    401-retry login: every other client in this package that calls a gated
    route logs in silently on the first 401 and then keeps reusing that
    session cookie for the rest of this process's life. Nothing previously
    called ``http_auth.logout()`` to ever give that session back, so it sat
    open on the board until it timed out or the board rebooted. This tool
    is the one caller.

    Not destructive -- ending a session the board itself treats as
    idempotent, best-effort, and always reversible by logging in again --
    so it refuses nothing and takes no ``confirm`` parameter, unlike the
    write tools elsewhere in this module.

    Returns ``True`` if this process held a remembered session for ``host``
    and the logout POST was sent (regardless of the board's own response
    to it -- see ``http_auth.logout()``'s own docstring for why that is
    still correct), ``False`` if this process had no remembered session to
    begin with. Never raises for a refused or unreachable logout POST --
    ``http_auth.logout()`` itself swallows that, since this side of the
    seam is done with the credential either way.

    Never prints, logs, or returns a credential value -- only the
    ``[bool]`` result described above.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as web_auth_setup()

    resolved = _ota_resolve_host(host)
    origin = f"http://{resolved}"
    return http_auth.logout(origin)

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
