#!/usr/bin/env python3
"""page_structure.py -- fetch one board web page and report its STRUCTURE.

WHY THIS EXISTS. The question "is the zone kiln graphic present and complete
on this freshly flashed board?" was previously answered by pulling
``/settings/zones`` -- about 240 KB of HTML, CSS and inline SVG -- into a
conversation and reading it. The answer is a handful of facts (does the page
exist, did it arrive whole, are the ``kg-`` classed elements and the icon
``<symbol>`` ids there), so this module computes those facts on the PC side
and returns only them.

WHAT IT REPORTS.
  * HTTP status, and the byte size BOTH on the wire and after decompression
    -- KilnFW serves several of these pages gzipped, and a wire size alone
    cannot tell a complete compressed page from a truncated one.
  * ``gzipped``: whether the board actually applied Content-Encoding, not
    whether we asked for it.
  * ``element_ids``: every ``id=`` the page declares, in document order.
  * ``class_prefixes``: each CSS class prefix (the part up to and including
    its first ``-``) with how many *parsed* ``class=`` tokens carry it.
  * ``source_prefixes``: the same prefixes counted over the WHOLE page text,
    stylesheet rules and inline script included. Both are reported because
    on the real /settings/zones page they answer different questions and
    only the second one answers the motivating one: that page's kiln
    graphic is BUILT BY SCRIPT, so its ``kg-`` classes live in CSS rules and
    in JavaScript string literals, not in static ``class=`` attributes.
    Measured 2026-09-18 on the bench board: 77 ``kg-`` occurrences in the
    page source against 0 parsed ``kg-`` class attributes. A tool that
    reported only the parsed count would have said the graphic was absent
    from a page that carries it in full -- the exact false negative this
    tool exists to prevent.
  * ``symbol_ids`` / ``use_refs``: inline-SVG ``<symbol id=...>`` ids and the
    ``<use href="#...">`` references to them, so a page that references an
    icon it never defines is visible as a mismatch rather than as a blank
    square on the panel.

AUTHENTICATION. The board's web auth may be on, and these pages are not
ROUTE_TIER_OPEN. Requests go through :mod:`kilnctrl.http_auth`, the same seam
every other client here uses, so an enabled gate is passed with the
credential from the environment rather than worked around. When no credential
is available the failure is reported as one clear sentence naming the two
environment variables -- never as a traceback, and never as a misleading
"page is missing".

Stdlib only (urllib + gzip + html.parser), same convention as
zones_http_client.py and web_ui_client.py. Unit tested against synthetic
HTML with no socket (tools/PcTools/tests/test_page_structure.py).
"""

from __future__ import annotations

import collections
import dataclasses
import gzip
import re
import urllib.error
import urllib.parse
import urllib.request
from html.parser import HTMLParser
from typing import Dict, List, Optional, Tuple

from . import http_auth

#: Default wall-clock budget for the fetch. These pages are large but local.
DEFAULT_TIMEOUT_S = 15.0

#: Same fallback-AP address every other HTTP client in this package names.
PAGE_AP_DEFAULT_HOST = "192.168.4.1"

#: The page this tool exists for: the zones settings page carries the kiln
#: graphic. Not special-cased anywhere -- just the default argument.
DEFAULT_PAGE_PATH = "/settings/zones"


class PageStructureError(RuntimeError):
    """The page could not be fetched or could not be read. Carries a
    caller-actionable sentence, never a traceback."""


@dataclasses.dataclass
class PageStructure:
    """One page's structural inventory. No page text is retained."""

    url: str
    status: int
    wire_bytes: int
    decoded_bytes: int
    gzipped: bool
    content_type: str
    element_ids: List[str]
    class_prefixes: Dict[str, int]
    class_token_count: int
    source_prefixes: Dict[str, int]
    symbol_ids: List[str]
    use_refs: List[str]

    @property
    def undefined_use_refs(self) -> List[str]:
        """``<use href="#x">`` with no ``<symbol id="x">`` on the page."""
        defined = set(self.symbol_ids)
        return [ref for ref in dict.fromkeys(self.use_refs) if ref not in defined]


class _StructureParser(HTMLParser):
    """Collects ids, class tokens and SVG symbol/use references.

    Deliberately not a tree builder: these pages are large and the questions
    asked of them are flat ("which ids exist", "how many kg- classes"), so a
    streaming tag scan is both sufficient and cheap.
    """

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.ids: List[str] = []
        self.class_tokens: List[str] = []
        self.symbol_ids: List[str] = []
        self.use_refs: List[str] = []

    def _handle(self, tag: str, attrs) -> None:
        attr_map = {name.lower(): (value or "") for name, value in attrs}
        element_id = attr_map.get("id", "").strip()
        if element_id:
            self.ids.append(element_id)
            if tag.lower() == "symbol":
                self.symbol_ids.append(element_id)
        classes = attr_map.get("class", "")
        if classes:
            self.class_tokens.extend(tok for tok in classes.split() if tok)
        if tag.lower() == "use":
            # SVG 2 uses href; SVG 1.1 used xlink:href. Both appear in the
            # wild and both are read here.
            ref = attr_map.get("href") or attr_map.get("xlink:href") or ""
            ref = ref.strip()
            if ref.startswith("#") and len(ref) > 1:
                self.use_refs.append(ref[1:])

    def handle_starttag(self, tag: str, attrs) -> None:
        self._handle(tag, attrs)

    def handle_startendtag(self, tag: str, attrs) -> None:
        self._handle(tag, attrs)


def class_prefix(token: str) -> str:
    """The prefix a class token belongs to: everything up to and including
    its first ``-`` (so ``kg-shell`` -> ``kg-``), or the whole token when it
    carries no ``-`` at all."""
    head, sep, _ = token.partition("-")
    return head + sep if sep else token


#: A hyphenated identifier prefix anywhere in the page text: ``kg-`` out of
#: ``kg-shell``, whether that appeared in a class attribute, a CSS selector or
#: a JavaScript string literal. Deliberately source-text, not markup-aware --
#: see the module docstring for why the markup-only count is not sufficient.
_SOURCE_PREFIX_RE = re.compile(r"\b([A-Za-z][A-Za-z0-9]*-)[A-Za-z0-9_-]+")

#: Prefixes below this many occurrences are noise on a page this size (every
#: hyphenated English word in prose reaches the regex above).
SOURCE_PREFIX_MIN_COUNT = 3


def source_prefix_counts(html: str, min_count: int = SOURCE_PREFIX_MIN_COUNT) -> Dict[str, int]:
    """Count hyphenated identifier prefixes over the whole page text.

    This is what answers "is the kiln graphic present", because the graphic's
    ``kg-`` classes are emitted by the page's own script at render time and so
    never appear as a static ``class=`` attribute for the HTML parser to see.
    """
    counts = collections.Counter(_SOURCE_PREFIX_RE.findall(html))
    return dict(sorted(((prefix, n) for prefix, n in counts.items() if n >= min_count),
                       key=lambda kv: (-kv[1], kv[0])))


def inventory(html: str) -> Tuple[List[str], Dict[str, int], int, List[str], List[str]]:
    """Parse ``html`` into ``(ids, prefix_counts, class_token_count,
    symbol_ids, use_refs)``. Prefix counts are ordered most-common first."""
    parser = _StructureParser()
    parser.feed(html)
    parser.close()
    counts: Dict[str, int] = {}
    for token in parser.class_tokens:
        prefix = class_prefix(token)
        counts[prefix] = counts.get(prefix, 0) + 1
    ordered = dict(sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])))
    return (list(dict.fromkeys(parser.ids)), ordered, len(parser.class_tokens),
            list(dict.fromkeys(parser.symbol_ids)), parser.use_refs)


def build_url(host: str, path: str) -> str:
    """``host`` may be a bare address or a full origin; ``path`` is the
    page path. Both spellings a caller is likely to type resolve the same."""
    host = (host or PAGE_AP_DEFAULT_HOST).strip()
    if not host.startswith("http://") and not host.startswith("https://"):
        host = "http://" + host
    if not path.startswith("/"):
        path = "/" + path
    return host.rstrip("/") + path


def fetch_page(url: str, timeout: float = DEFAULT_TIMEOUT_S) -> Tuple[int, int, str, bool, str]:
    """GET ``url`` and return ``(status, wire_bytes, text, gzipped, content_type)``.

    ``Accept-Encoding: gzip`` is offered explicitly so the wire-vs-decoded
    comparison is meaningful; urllib does not offer it on its own, and a
    board that never compresses simply reports ``gzipped=False``.
    """
    request = urllib.request.Request(url, method="GET", headers={
        "Accept-Encoding": "gzip",
        "Accept": "text/html,*/*",
    })
    try:
        with http_auth.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            status = getattr(response, "status", None) or response.getcode()
            headers = response.headers
    except http_auth.HttpAuthError as exc:
        raise PageStructureError(
            f"{url}: web authentication is enabled on the board and the request could not "
            f"be authenticated -- {exc}") from exc
    except urllib.error.HTTPError as exc:
        if exc.code in (401, 403):
            raise PageStructureError(
                f"{url}: the board refused the request with HTTP {exc.code} (web "
                f"authentication). Set {http_auth.USERNAME_ENV} and {http_auth.PASSWORD_ENV} "
                "in the environment of the process making this call, then retry.") from exc
        raise PageStructureError(f"{url}: HTTP {exc.code} {exc.reason}") from exc
    except urllib.error.URLError as exc:
        raise PageStructureError(f"{url}: not reachable -- {exc.reason}") from exc
    except OSError as exc:
        raise PageStructureError(f"{url}: request failed -- {exc}") from exc

    encoding = (headers.get("Content-Encoding") or "").lower()
    content_type = (headers.get("Content-Type") or "").split(";")[0].strip()
    wire_bytes = len(raw)
    gzipped = "gzip" in encoding
    if gzipped:
        try:
            body = gzip.decompress(raw)
        except (OSError, EOFError) as exc:
            raise PageStructureError(
                f"{url}: the board declared Content-Encoding: gzip but the {wire_bytes} "
                f"received bytes do not decompress ({exc}) -- the response is truncated "
                "or corrupt.") from exc
    else:
        body = raw
    return status, wire_bytes, body.decode("utf-8", errors="replace"), gzipped, content_type


def analyze(host: str = PAGE_AP_DEFAULT_HOST, path: str = DEFAULT_PAGE_PATH,
            timeout: float = DEFAULT_TIMEOUT_S) -> PageStructure:
    """Fetch one page and return its :class:`PageStructure`."""
    url = build_url(host, path)
    status, wire_bytes, text, gzipped, content_type = fetch_page(url, timeout)
    ids, prefixes, token_count, symbol_ids, use_refs = inventory(text)
    return PageStructure(
        url=url, status=status, wire_bytes=wire_bytes, decoded_bytes=len(text.encode("utf-8")),
        gzipped=gzipped, content_type=content_type, element_ids=ids,
        class_prefixes=prefixes, class_token_count=token_count,
        source_prefixes=source_prefix_counts(text),
        symbol_ids=symbol_ids, use_refs=use_refs,
    )


def format_structure(page: PageStructure, max_ids: int = 60,
                     max_prefixes: int = 25) -> str:
    """Render the inventory compactly. Long lists are capped and the cap is
    stated -- the whole point of this tool is to not dump the page."""
    lines = [
        f"{page.url}: HTTP {page.status} {page.content_type or '(no content-type)'}",
        f"  bytes: wire={page.wire_bytes} decoded={page.decoded_bytes} "
        f"gzipped={'yes' if page.gzipped else 'no'}",
        f"  element ids: {len(page.element_ids)}",
    ]
    shown_ids = page.element_ids[:max_ids]
    if shown_ids:
        lines.append("    " + ", ".join(shown_ids))
        if len(page.element_ids) > len(shown_ids):
            lines.append(f"    ... {len(page.element_ids) - len(shown_ids)} more id(s) not shown")
    lines.append(
        f"  class prefixes (parsed class= attributes): {len(page.class_prefixes)} distinct "
        f"over {page.class_token_count} class token(s)")
    for prefix, count in list(page.class_prefixes.items())[:max_prefixes]:
        lines.append(f"    {prefix:<20} {count}")
    if len(page.class_prefixes) > max_prefixes:
        lines.append(f"    ... {len(page.class_prefixes) - max_prefixes} more prefix(es) not shown")
    lines.append(
        f"  source prefixes (whole page text, CSS + script included): "
        f"{len(page.source_prefixes)} distinct")
    for prefix, count in list(page.source_prefixes.items())[:max_prefixes]:
        attr = page.class_prefixes.get(prefix, 0)
        lines.append(f"    {prefix:<20} {count:<5} (as class attribute: {attr})")
    if len(page.source_prefixes) > max_prefixes:
        lines.append(
            f"    ... {len(page.source_prefixes) - max_prefixes} more prefix(es) not shown")
    lines.append(f"  svg symbol ids: {len(page.symbol_ids)}")
    if page.symbol_ids:
        lines.append("    " + ", ".join(page.symbol_ids))
    lines.append(f"  <use> references: {len(page.use_refs)}")
    missing = page.undefined_use_refs
    if missing:
        lines.append(
            "  WARNING: referenced but never defined on this page: " + ", ".join(missing))
    return "\n".join(lines)
