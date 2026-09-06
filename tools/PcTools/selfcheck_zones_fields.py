"""B7: zones top-level field-table drift check.

``kilnctrl.zones_http_client`` hand-types ``_TOP_FIELD_FORM_KEY`` (the
top-level scalar fields a POST round-trips) and
``_TOP_READONLY_OR_STRUCTURAL_KEYS`` (top-level keys GET emits that this
client deliberately never echoes back -- structural arrays/objects handled
by their own dedicated logic, or read-only telemetry with no POST field at
all). Together those two dicts/sets are supposed to cover EXACTLY the
top-level JSON keys ``zones_get_handler`` (``firmware/KilnFW/App/drivers/http/zones_http_get.c``)
and ``zones_post_handler`` (``firmware/KilnFW/App/drivers/http/zones_http_post.c``)
actually emit/accept -- these two handlers used to share one
``zones_http_handlers.c`` file; a 2026-09-04 split moved GET and POST into
their own files (see this module's ``_GET_C_PATH``/``_POST_C_PATH`` below).
Nothing
enforced that -- a firmware change to either handler's top-level key set
could drift from the client silently (see this module's sibling checks for
the same class of bug: ``_firmware_uart_protocol_version``).

EXTRACTION STRATEGY (robust to reformatting, not just line numbers):
GET side -- ``zones_get_handler`` builds its JSON response entirely through
a sequence of ``APPEND("...", args...)`` calls (a ``snprintf``-append
macro). Each call's leading, C-concatenated string-literal run is exactly
the JSON template text emitted at that point (with ``%u``/``%s``/etc. value
placeholders). Concatenating those template fragments IN SOURCE ORDER and
walking them with a small brace/bracket-depth tracker recovers the JSON
object's real nesting -- a ``"key":`` token is a TOP-LEVEL key only when it
appears at depth 1 (immediately inside the outermost ``{``), which is what
distinguishes ``"thermo_count"`` (top-level) from ``"link_up"`` (nested
inside the ``"safety_wiring"`` object) or ``"index"`` (nested inside each
``zones[]``/``timing_profiles[]`` element) without needing to special-case
any of those substructures by name.

POST side -- ``zones_post_handler`` reads each top-level scalar field with a
literal field-name argument to ``http_form_find_field(body, "name", ...)``
or ``zones_config_json_parse_{u8,float}_field(body, "name", ...)``; every
PER-ZONE/PER-PROFILE/PER-RELAY field instead builds its key at runtime into
a local ``key`` variable (``snprintf(key, ..., "z%u_...", i)``) and passes
that variable, never a literal, to the same helpers -- so a plain regex for
literal-string first arguments naturally picks up exactly the top-level
scalar fields and nothing per-zone, with no extra filtering needed.
"""
from __future__ import annotations

import pathlib
import re

from kilnctrl.zones_http_client import (
    _TOP_FIELD_FORM_KEY,
    _TOP_READONLY_OR_STRUCTURAL_KEYS,
)

from selfcheck_common import check

_DRIVERS_DIR = (
    pathlib.Path(__file__).resolve().parents[2]
    / "firmware" / "KilnFW" / "App" / "drivers"
)
_ZONES_GET_C_PATH = _DRIVERS_DIR / "zones_http_get.c"
_ZONES_POST_C_PATH = _DRIVERS_DIR / "zones_http_post.c"

_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
#: Between two adjacent (C-concatenated) string-literal fragments of one
#: APPEND() call there can be arbitrary whitespace AND/OR a /* ... */ block
#: comment -- e.g. the multi-line comment explaining relay_zone_owned_mask
#: that sits between the "ease_off_window_mult" and "relay_zone_owned_mask"
#: fragments in zones_get_handler. Skip both, not just whitespace, or the
#: capture silently truncates at the first comment and every key after it
#: goes undetected.
_GAP_RE = r"(?:\s|/\*.*?\*/)*"
_APPEND_CALL_RE = re.compile(r'APPEND\(' + _GAP_RE + r'((?:"(?:[^"\\]|\\.)*"' + _GAP_RE + r')+)', re.DOTALL)
_STRING_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
_FN_START_RE = re.compile(r"\nesp_err_t\s+(\w+)\s*\(")


def _function_body(text: str, name: str, source_path: pathlib.Path) -> str:
    """Slice ``text`` down to one ``esp_err_t <name>(...)`` function's body,
    ending right before the next top-level ``esp_err_t`` function starts (or
    end of file). Tolerant of the exact braces/whitespace around the
    function -- it only needs the *next function's* start position as a
    bound, not to fully parse C scoping."""
    m = re.search(rf"esp_err_t\s+{re.escape(name)}\s*\(", text)
    if m is None:
        raise AssertionError(f"{name}() not found in {source_path}")
    start = m.start()
    nxt = _FN_START_RE.search(text, start + 1)
    end = nxt.start() if nxt else len(text)
    return text[start:end]


def _scan_template_keys(dequoted: str, depth: int, keys: set) -> int:
    """Walk one dequoted JSON-template fragment, tracking brace/bracket
    depth, and record any ``"key":`` token seen at depth 1 into ``keys``.
    Returns the depth at the end of the fragment, so callers can carry it
    across consecutive APPEND() calls that jointly build one JSON stream."""
    i, n = 0, len(dequoted)
    while i < n:
        c = dequoted[i]
        if c == '"':
            j = dequoted.find('"', i + 1)
            if j == -1:
                break
            token = dequoted[i + 1 : j]
            k = j + 1
            while k < n and dequoted[k] in " \t":
                k += 1
            if depth == 1 and k < n and dequoted[k] == ":" and _IDENT_RE.match(token):
                keys.add(token)
            i = j + 1
        elif c in "{[":
            depth += 1
            i += 1
        elif c in "}]":
            depth -= 1
            i += 1
        else:
            i += 1
    return depth


def _extract_get_top_level_keys(text: str, source_path: pathlib.Path) -> set:
    body = _function_body(text, "zones_get_handler", source_path)
    depth = 0
    keys: set = set()
    for m in _APPEND_CALL_RE.finditer(body):
        dequoted = "".join(
            part.replace('\\"', '"') for part in _STRING_LITERAL_RE.findall(m.group(1))
        )
        depth = _scan_template_keys(dequoted, depth, keys)
    return keys


_POST_FIELD_LITERAL_RE = re.compile(
    r'(?:http_form_find_field|zones_config_json_parse_u8_field|'
    r'zones_config_json_parse_float_field)\(\s*body\s*,\s*"([A-Za-z_][A-Za-z0-9_]*)"'
)


def _extract_post_top_level_keys(text: str, source_path: pathlib.Path) -> set:
    body = _function_body(text, "zones_post_handler", source_path)
    return set(_POST_FIELD_LITERAL_RE.findall(body))


def zones_field_table_checks() -> None:
    # zones_get_handler and zones_post_handler used to share one
    # zones_http_handlers.c; a 2026-09-04 split moved each into its own file.
    get_text = _ZONES_GET_C_PATH.read_text(encoding="utf-8")
    post_text = _ZONES_POST_C_PATH.read_text(encoding="utf-8")
    fw_get_keys = _extract_get_top_level_keys(get_text, _ZONES_GET_C_PATH)
    fw_post_keys = _extract_post_top_level_keys(post_text, _ZONES_POST_C_PATH)

    client_get_keys = set(_TOP_FIELD_FORM_KEY) | set(_TOP_READONLY_OR_STRUCTURAL_KEYS)
    client_post_keys = set(_TOP_FIELD_FORM_KEY)

    # Sanity check on the extractor itself: it should have found a
    # non-trivial number of keys, not silently matched nothing because a
    # formatting change broke the regexes above.
    check("firmware GET top-level extractor found keys", len(fw_get_keys) > 5, True)
    check("firmware POST top-level extractor found keys", len(fw_post_keys) > 2, True)

    missing_from_client_get = fw_get_keys - client_get_keys
    extra_in_client_get = client_get_keys - fw_get_keys
    check(
        "zones GET top-level keys: client dicts match firmware "
        f"(missing from client: {sorted(missing_from_client_get)}, "
        f"extra in client: {sorted(extra_in_client_get)})",
        (missing_from_client_get, extra_in_client_get),
        (set(), set()),
    )

    missing_from_client_post = fw_post_keys - client_post_keys
    extra_in_client_post = client_post_keys - fw_post_keys
    check(
        "zones POST top-level fields: _TOP_FIELD_FORM_KEY matches firmware "
        f"(missing from client: {sorted(missing_from_client_post)}, "
        f"extra in client: {sorted(extra_in_client_post)})",
        (missing_from_client_post, extra_in_client_post),
        (set(), set()),
    )
