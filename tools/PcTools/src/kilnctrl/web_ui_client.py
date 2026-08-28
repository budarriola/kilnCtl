"""Client for the web half of the UI regression-test framework: drives the
KilnFW HTTP dashboard (main_page.html) the same way a browser's click
handler would, but from Python. Stdlib only (urllib.request + html.parser) --
no browser-automation dependency, same reasoning as zones_http_client.py and
ota_http_client.py using plain urllib rather than requests.

find_element()/click() are deliberately narrow: this is not a general DOM
engine, only enough to (a) read back a page's rendered HTML for an
assert_text-style check and (b) fire the exact fetch() calls the real page's
buttons already make, from a static table read out of main_page.html rather
than guessed.
"""

from __future__ import annotations

import urllib.error
import urllib.parse
import urllib.request
from html.parser import HTMLParser
from typing import Optional

WEB_UI_TIMEOUT_S = 5.0


class WebUiError(RuntimeError):
    """Raised for a request that could not be completed, or an unmapped click target."""


#: target -> (HTTP method, endpoint path). Populated from the real
#: onclick/fetch() calls in firmware/KilnFW/App/drivers/main_page.html --
#: read-only reference, never edited here. Extend this table, never invent a
#: URL not actually wired to a button in that file.
_WEB_CLICK_ENDPOINTS = {
    # runBtn's click handler POSTs id=<profile> as an urlencoded form body --
    # this table only carries the fixed (method, path) half; a caller
    # needing the id passes it via click()'s `body` argument.
    "runBtn": ("POST", "/api/profile_exec/start"),
    "stopBtn": ("POST", "/api/profile_exec/stop"),
    "clearTripBtn": ("POST", "/api/safety/clear_trip"),
}


class _ElementFinder(HTMLParser):
    """Finds the first tag whose id attribute matches, and collects its text
    and hidden-ness. Not a general tree walker -- good enough for the flat,
    non-nested ids main_page.html actually uses for its buttons/labels."""

    def __init__(self, target_id: str) -> None:
        super().__init__(convert_charrefs=True)
        self.target_id = target_id
        self.found: Optional[dict] = None
        self._depth = 0
        self._capture_depth: Optional[int] = None
        self._text_parts: "list[str]" = []

    def handle_starttag(self, tag: str, attrs) -> None:
        attr_map = dict(attrs)
        self._depth += 1
        if self._capture_depth is None and attr_map.get("id") == self.target_id:
            self._capture_depth = self._depth
            style = attr_map.get("style") or ""
            hidden = "hidden" in attr_map or "display:none" in style.replace(" ", "")
            self.found = {"tag": tag, "text": "", "hidden": hidden}

    def handle_startendtag(self, tag: str, attrs) -> None:
        attr_map = dict(attrs)
        if self._capture_depth is None and attr_map.get("id") == self.target_id:
            style = attr_map.get("style") or ""
            hidden = "hidden" in attr_map or "display:none" in style.replace(" ", "")
            self.found = {"tag": tag, "text": "", "hidden": hidden}

    def handle_endtag(self, tag: str) -> None:
        if self._capture_depth is not None and self._depth == self._capture_depth:
            self.found["text"] = "".join(self._text_parts).strip()
            self._capture_depth = None
            self._text_parts = []
        self._depth = max(0, self._depth - 1)

    def handle_data(self, data: str) -> None:
        if self._capture_depth is not None:
            self._text_parts.append(data)


class WebUiClient:
    """Drives the HTTP dashboard over urllib. One instance per base_url."""

    def __init__(self, base_url: str, timeout: float = WEB_UI_TIMEOUT_S) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.current_html: str = ""
        self.current_path: str = ""

    def goto(self, path: str) -> str:
        """GET ``path`` off ``base_url``, store and return the response HTML."""
        url = self.base_url + path
        try:
            with urllib.request.urlopen(url, timeout=self.timeout) as resp:
                body = resp.read()
        except (urllib.error.URLError, OSError) as exc:
            raise WebUiError(f"GET {url} failed: {exc}") from exc
        self.current_html = body.decode("utf-8", errors="replace")
        self.current_path = path
        return self.current_html

    def find_element(self, id_or_testid: str) -> "Optional[dict]":
        """``{"tag":str,"text":str,"hidden":bool}`` for the element with this
        id in the last page fetched by :meth:`goto`, or None if absent."""
        parser = _ElementFinder(id_or_testid)
        parser.feed(self.current_html)
        return parser.found

    def click(self, target: str, body: "Optional[str]" = None) -> str:
        """Fire the real request the named button's click handler makes.

        Raises :class:`WebUiError` (KeyError-style message) for any target
        not in :data:`_WEB_CLICK_ENDPOINTS` -- this client never guesses a
        URL. ``body`` is an urlencoded form string, passed through for
        targets (like runBtn) whose real handler sends one.
        """
        entry = _WEB_CLICK_ENDPOINTS.get(target)
        if entry is None:
            raise WebUiError(
                f"no click mapping for '{target}' -- add an entry to _WEB_CLICK_ENDPOINTS"
            )
        method, endpoint = entry
        url = self.base_url + endpoint
        data = body.encode("utf-8") if body is not None else None
        headers = {"Content-Type": "application/x-www-form-urlencoded"} if data else {}
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise WebUiError(f"{method} {url} -> HTTP {exc.code}: {detail}") from exc
        except (urllib.error.URLError, OSError) as exc:
            raise WebUiError(f"{method} {url} failed: {exc}") from exc
