"""WEB AUTH SETUP -- MCP surface for bootstrapping the board's web admin
credential and turning web authentication on, matched exactly against
security_backend_web_auth.c/security_http.c and the /settings/security
page's own JS (net/security_page.html). Part of the mcp_server.py split
pattern -- see that module's docstring for the overall map. This tool never
reads a credential from anywhere but the environment, and never writes,
logs, or echoes one back.
"""
from __future__ import annotations

import os
from typing import Optional

from . import mcp_server as _srv
from . import web_auth_setup_http_client as wac
from .http_auth import PASSWORD_ENV, USERNAME_ENV


@_srv._tool()
def web_auth_setup(host: Optional[str] = None, confirm: bool = False,
                    enable_web_auth: bool = True) -> str:
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
       ``web_enabled`` on, preserving the board's current ``lcd_enabled``/
       ``lcd_timeout_min`` exactly as read back in step 1 (the page's own
       ``save()`` always echoes all four policy fields together;
       set_policy's parser requires web_timeout_min/lcd_timeout_min on
       every call). Per http_auth_check() (http_auth_enforce.c),
       ROUTE_TIER_ADMIN collapses to unconditional ALLOW while
       ``web_enabled`` is false, so setting the password here needs no
       session at all -- confirmed against the firmware source, not
       inferred from behaviour.
    3. An admin record already exists and web auth is ON: this tool writes
       nothing. It logs in once with the environment credentials
       (POST /api/auth/login, form-encoded, ``Accept-Encoding: identity``)
       and reports "already configured, credentials valid" only if that
       login succeeds. A 401 is reported as a failure and this tool stops
       -- it never retries a login and never guesses a different
       credential.

    An admin record that exists while web auth reads OFF, or any other
    combination GET /api/auth/config can report, is treated as "already
    configured" too (case 3's login check) rather than guessed at further --
    this tool only ever writes in the two specific bootstrap states above.

    REFUSES every write unless ``confirm=True`` -- without it, this is a
    dry run: it reports which of the three states the board is in and what
    it WOULD do, but sends no POST (case 3's login check still runs, since
    a login is not a write). Also refuses outright, before any HTTP call,
    if either environment variable is absent.

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

    resolved = _ota_resolve_host(host)

    try:
        before = wac.get_auth_config(resolved)
    except wac.WebAuthSetupHttpError as exc:
        return f"error: could not read GET /api/auth/config (host={resolved}): {exc}"

    web_enabled = bool(before.get("web_enabled"))
    admin_configured = bool(before.get("admin_password_set"))
    lcd_enabled = bool(before.get("lcd_enabled"))
    lcd_timeout_min = before.get("lcd_timeout_min", -1)
    web_timeout_min = before.get("web_timeout_min", -1)
    state_line = (f"before: web_enabled={web_enabled} admin_password_set={admin_configured} "
                  f"lcd_enabled={lcd_enabled} web_timeout_min={web_timeout_min} "
                  f"lcd_timeout_min={lcd_timeout_min} ({presence}, host={resolved})")

    # Case 3: an admin record already exists. Whatever web_enabled reads,
    # this tool never bootstraps over an existing admin record -- it only
    # verifies the environment credential actually works.
    if admin_configured:
        try:
            ok = wac.try_login(resolved, username, password)
        except wac.WebAuthSetupHttpError as exc:
            return f"error: login check failed (host={resolved}): {exc}\n{state_line}"
        if ok:
            return f"already configured, credentials valid\n{state_line}"
        return (f"failed: administrator credential already configured, but the environment "
                f"credential was refused (401) -- never retried\n{state_line}")

    # No admin record yet. Case 1 (web auth already ON) uses the bootstrap
    # route; case 2 (web auth OFF) sets the password directly, since that
    # route requires no session at all while web_enabled is false.
    if not confirm:
        action = ("POST /api/auth/bootstrap_password" if web_enabled
                   else "POST /api/auth/security (set_web_password)"
                   + (" then set_policy(web_enabled=1)" if enable_web_auth else ""))
        return f"DRY RUN (pass confirm=True to actually set it up) -- would: {action}\n{state_line}"

    if web_enabled:
        try:
            wac.post_bootstrap_password(resolved, username, password)
        except wac.WebAuthSetupHttpError as exc:
            if exc.status == 409:
                return (f"failed: board reports an administrator credential already exists (409) "
                        f"even though the pre-fetch above saw none\n{state_line}: {exc}")
            if exc.status == 400:
                return f"failed: password rejected (400, likely too weak)\n{state_line}: {exc}"
            return f"failed: POST /api/auth/bootstrap_password: {exc}\n{state_line}"
    else:
        try:
            result = wac.post_security(resolved, {
                "cmd": "set_web_password", "role": "admin",
                "username": username, "password": password,
            })
        except wac.WebAuthSetupHttpError as exc:
            return f"failed: POST /api/auth/security (set_web_password): {exc}\n{state_line}"
        if not result.get("ok"):
            return f"failed: set_web_password rejected: {result.get('error')!r}\n{state_line}"

        if enable_web_auth:
            try:
                policy_result = wac.post_security(resolved, {
                    "cmd": "set_policy",
                    "web_enabled": "1",
                    "lcd_enabled": "1" if lcd_enabled else "0",
                    "web_timeout_min": str(web_timeout_min),
                    "lcd_timeout_min": str(lcd_timeout_min),
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
