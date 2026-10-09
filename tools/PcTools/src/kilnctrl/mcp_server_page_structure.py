"""UI tool -- structural inventory of one of the board's web pages, over HTTP.

Answers "is the page there, did it arrive whole, and does it declare the
elements it should?" without pulling the page itself into the caller's
context: ``/settings/zones`` alone is about 240 KB. Host resolution mirrors
mcp_server_adaptive_tune.py's ``_adaptive_tune_resolve_host()`` (explicit
``host`` wins, else the board's current station IP over the UART WIFI query,
else the fallback-AP address); see page_structure.py for what is measured and
how an enabled web-auth gate is passed rather than worked around.
"""
from __future__ import annotations

from typing import Optional

from . import page_structure
from .wifi_uart import WifiUartQueryError

from . import mcp_server_core as _core


def _page_resolve_host(host: Optional[str]) -> str:
    """Explicit ``host`` always wins; otherwise the board's current Wi-Fi
    station IP, else the fallback-AP address."""
    if host:
        return host
    try:
        status = _srv._wifi.get_status()
        if status.sta_connected and status.sta_ip:
            return status.sta_ip
    except WifiUartQueryError:
        pass
    return page_structure.PAGE_AP_DEFAULT_HOST


@_core._tool()
def board_page_structure(path: str = page_structure.DEFAULT_PAGE_PATH,
                         host: Optional[str] = None,
                         timeout_s: float = page_structure.DEFAULT_TIMEOUT_S,
                         max_ids: int = 60) -> str:
    """Fetch one board web page and report its STRUCTURE, not its text.

    The motivating case: confirming the zone kiln graphic is present and
    complete on a freshly flashed board. ``/settings/zones`` (the default
    ``path``) is roughly 240 KB of HTML and inline SVG; reading it to answer
    a handful of yes/no questions is pure context cost. This returns:

    * the HTTP status and Content-Type;
    * the byte size **both on the wire and decompressed**, plus whether the
      board actually gzipped it -- a wire size alone cannot tell a complete
      compressed page from a truncated one;
    * every ``id=`` the page declares;
    * the CSS class prefixes present, counted TWO ways: over the parsed
      ``class=`` attributes, and over the whole page text (stylesheet rules
      and inline script included). The second is the one that answers "is
      the kiln graphic there": that graphic is built by the page's own
      script, so its ``kg-`` classes live in CSS rules and JS string
      literals, not in static attributes -- 77 source occurrences against 0
      parsed class attributes on the bench board, 2026-09-18. An
      attribute-only count would have called a complete page empty;
    * inline-SVG ``<symbol>`` ids and the ``<use href="#...">`` references to
      them, with a WARNING naming any icon referenced but never defined
      (12 ``kgIcon*`` symbol ids on the bench board, 2026-09-18).

    ``host`` defaults to the board's current station IP (fallback AP address
    if Wi-Fi is down); pass a bare address or a full ``http://...`` origin.
    ``max_ids`` caps how many ids are printed, and the cap is stated.

    If the board has web authentication enabled, the request is authenticated
    through the usual ``http_auth`` seam using ``KILNCTL_WEB_USERNAME`` /
    ``KILNCTL_WEB_PASSWORD``. When no credential is available this comes back
    as one clear sentence naming both variables -- not a traceback, and not a
    misleading "the page is missing".

    Read-only: one GET, nothing on the board is written or energized.
    """
    try:
        page = page_structure.analyze(
            host=_page_resolve_host(host), path=path, timeout=timeout_s)
    except page_structure.PageStructureError as exc:
        return f"error: {exc}"
    return page_structure.format_structure(page, max_ids=max_ids)

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
