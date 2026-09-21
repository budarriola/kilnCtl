"""Resolve the default board host for kilnctrl's HTTP clients.

WHY THIS EXISTS. Every ``*_http_client.py`` in this package (and the GUI
modules) used to hardcode its own ``*_AP_DEFAULT_HOST = "192.168.4.1"``
constant -- the board's softAP fallback address -- as the default ``host``
argument. That is correct only for a board that has never joined a LAN. A
board provisioned onto the bench LAN (e.g. 192.168.1.156) answers there
instead, and nothing in this package remembered that fact between calls:
every tool defaulted back to the AP address, and a caller who didn't
already know the LAN address had to rediscover it (in one real incident, by
sweeping a /24) every session.

RESOLUTION ORDER, cheapest/most-authoritative first:

  1. ``KILNCTL_HOST`` environment variable, if set -- an explicit operator
     override, always wins.
  2. The last host any kilnctrl HTTP client actually got a response from,
     persisted in ``settings.json`` (see :mod:`kilnctrl.settings`) next to
     this file, gitignored. Updated by :func:`record_host_seen`, which
     :mod:`kilnctrl.http_auth` calls after every successful request that
     goes through its ``urlopen`` seam -- which is nearly all of them.
  3. The board's own AP fallback address, ``192.168.4.1`` -- unchanged
     behaviour for a from-scratch board nothing has ever talked to.

This module never *chooses* a host to try; it only says what the *default*
for an unspecified ``host`` argument should be right now. A caller that
knows better (an explicit ``host=`` argument, or ``flash_firmware()``'s own
ordered candidate list) is untouched by this module and always wins.

RECORDING IS OPT-IN, DEFAULT OFF. :func:`record_host_seen` is a no-op
until :func:`enable_recording` has been called once in this process
(``enable_recording(False)`` turns it back off, e.g. for test teardown).
Real entry points (the MCP server's own startup, the GUI's startup) call
it; nothing else should. This is what keeps a test that mocks
``urllib.request.urlopen`` and calls a client with a fixture host (e.g.
"kiln.local") from silently writing that fixture host into the real,
shared ``settings.json`` -- which would then poison every
``*_AP_DEFAULT_HOST`` module constant (each resolved once, at import
time) for whichever module happens to import next in a LATER process.
Earlier revisions of this module instead special-cased "running under
pytest" (``PYTEST_CURRENT_TEST``); that was fragile (anything importing
this package from a test runner other than pytest would still leak) and
backwards (recording should default off everywhere until a real caller
turns it on, not default on everywhere except one test framework).
"""

from __future__ import annotations

import os
import urllib.parse
from pathlib import Path
from typing import Optional

from . import settings

#: Explicit operator override. Checked first, always wins.
HOST_ENV = "KILNCTL_HOST"

#: The board's own softAP fallback address -- last resort, unchanged from
#: every module's previous hardcoded default.
FALLBACK_HOST = "192.168.4.1"

#: Opt-in switch for :func:`record_host_seen`. Default OFF: see the module
#: docstring's "RECORDING IS OPT-IN" section. Set by :func:`enable_recording`.
_recording_enabled = False


def enable_recording(enabled: bool = True) -> None:
    """Turn :func:`record_host_seen`'s writes on (the default) or, passing
    ``enabled=False``, back off for the rest of this process. Call with no
    argument exactly once, from a real entry point (the MCP server's
    startup, the GUI's startup) -- never from library code, and never from
    a test unless that test is specifically exercising this wiring (in
    which case it should also patch ``host_resolve.settings.SETTINGS_PATH``
    or pass an explicit ``path=`` to avoid touching the real file).

    ``enabled=False`` is a test/teardown helper: it used to be a separate
    ``disable_recording()`` function, folded into this one so the toggle
    has a single production entry point instead of two public names where
    only one is ever reached outside tests."""
    global _recording_enabled
    _recording_enabled = enabled


def resolve_default_host(path: Optional[Path] = None) -> str:
    """The host a caller should use when none was given explicitly.

    See the module docstring for the three-step order. Never raises: a
    missing or corrupt settings file (see :func:`kilnctrl.settings.load`)
    falls through to the AP fallback exactly like never having a cache.

    ``path`` defaults to :data:`kilnctrl.settings.SETTINGS_PATH`, resolved
    at CALL time (not import time) so tests can patch it out from under
    every caller -- including the module-level ``*_AP_DEFAULT_HOST``
    constants below, each of which calls this with no argument.
    """
    env = os.environ.get(HOST_ENV)
    if env and env.strip():
        return env.strip()
    cached = settings.get_last_host(path if path is not None else settings.SETTINGS_PATH)
    if cached:
        return cached
    return FALLBACK_HOST


def record_host_seen(host: str, path: Optional[Path] = None) -> None:
    """Remember that ``host`` just answered a real request.

    A no-op until :func:`enable_recording` has been called in this process
    -- see the module docstring's "RECORDING IS OPT-IN" section.

    ``host`` may be a bare host, a "host:port" netloc, or a full origin
    ("http://host:port") -- only the hostname is persisted, never a port or
    scheme, so it composes with every client's own ``f"http://{host}..."``
    URL-building. Silently ignored if ``host`` doesn't parse to a hostname
    (never raises -- this is a best-effort cache update piggybacked on
    another call's success, not a call any caller is making on purpose).
    """
    if not _recording_enabled:
        return
    name = _hostname_only(host)
    if not name:
        return
    settings.set_last_host(name, path if path is not None else settings.SETTINGS_PATH)


def _hostname_only(host: str) -> Optional[str]:
    if not host:
        return None
    value = host.strip()
    if "//" not in value:
        value = "//" + value  # give urlsplit something with a netloc to parse
    try:
        parsed = urllib.parse.urlsplit(value)
    except ValueError:
        return None
    return parsed.hostname or None
