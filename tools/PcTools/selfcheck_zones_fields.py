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
    _ZONE_FIELD_FORM_KEY,
    _ZONE_READONLY_KEYS,
    _ZONES_DIAG_MODEL_FIT_KEYS,
)

from selfcheck_common import check

_DRIVERS_DIR = (
    pathlib.Path(__file__).resolve().parents[2]
    / "firmware" / "KilnFW" / "App" / "drivers"
)
_ZONES_GET_C_PATH = _DRIVERS_DIR / "http" / "zones_http_get.c"
_ZONES_POST_C_PATH = _DRIVERS_DIR / "http" / "zones_http_post.c"
_ZONES_POST_PARSE_C_PATH = _DRIVERS_DIR / "http" / "zones_http_post_parse.c"

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


# ---------------------------------------------------------------------------
# PER-ZONE field-table drift check (added 2026-09-08, same day as the on/off
# zone fields -- zone_type/failsafe_state/hyst_c/min_on_s/min_off_s -- shipped
# in the firmware's per-zone JSON but were never added to
# zones_http_client._ZONE_FIELD_FORM_KEY, breaking every whole-page zones
# save with ZonesHttpUnknownFieldError. This is the SECOND time in one day a
# firmware zones field landed with no matching PC client entry (the first was
# progress_band_c, e5375594) -- the check above only ever covered the
# TOP-LEVEL field tables (_TOP_FIELD_FORM_KEY/_TOP_READONLY_OR_STRUCTURAL_KEYS),
# never the PER-ZONE one (_ZONE_FIELD_FORM_KEY/_ZONE_READONLY_KEYS), so it
# could not have caught either incident. This check closes that gap.
#
# WHAT WOULD INVALIDATE THIS CHECK: a rename of zones_get_handler's per-zone
# loop guard (MAX31856_CHANNEL_COUNT) or of the "zones":[ / "z%u_" literal
# markers this extraction keys on; a restructure that stops emitting each
# zone's fields via a single `for (...MAX31856_CHANNEL_COUNT...)` loop in
# zones_http_get.c; or a POST-side helper other than snprintf(key, ...,
# "z%u_<suffix>", i) being used to build per-zone form-field names in
# zones_http_post_parse.c. None of those are expected to change without a
# deliberate zones-wire-format rewrite, in which case this check's extractors
# need updating alongside it -- same standing as the top-level check above.
#
# EXTRACTION STRATEGY: symbol/marker-keyed, not line-number-keyed.
# GET side -- slice zones_get_handler's body down to just the per-zone loop
# by finding the literal `for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT;
# i++) {` marker and brace-matching to its close, then reuse
# _scan_template_keys() (the same depth tracker the top-level check uses) on
# that slice: a `"key":` token at depth 1 (immediately inside the zone
# object's own `{`) is a real per-zone JSON field. Dynamic keys built with a
# second `%u`/`%s` placeholder (coupling_c%u, coupling_tau_c%u,
# coupling_dead_time_c%u, and settings_source_groups' "%s":%u loop) never
# match _IDENT_RE (which requires a clean identifier, no `%`), so they drop
# out of the extraction automatically -- exactly like the top-level check
# already relies on for array/relay fields, no special-casing needed here
# either. settings_source_groups itself (the outer key) DOES match, and is
# listed in _ZONE_READONLY_KEYS'-adjacent client set below alongside the
# other real per-zone keys.
# POST side -- zones_http_parse_zone_fields() builds every per-zone POST
# field the same way: `snprintf(key, sizeof(key), "z%u_<suffix>", i)` with a
# literal suffix. A single regex over that function's body recovers every
# suffix. Two suffixes are themselves dynamic (a further %u/%s follows the
# literal part): "z%u_coupling_c%u" truncates to "coupling_c", and
# "z%u_settings_source_%s" truncates to "settings_source_" -- both handled on
# the client side by _ZONE_COUPLING_CELL_RE / the _SRC_GROUP_NAMES loop
# rather than a flat _ZONE_FIELD_FORM_KEY entry, so both are excluded from
# the comparison set below by name, same as the GET side's %-placeholder
# keys drop out on their own.
_ZONE_LOOP_START_RE = re.compile(
    # The C source has this as a string literal, so the JSON quotes around
    # "zones" are themselves backslash-escaped in the raw file text (this
    # regex runs on the RAW source, before _STRING_LITERAL_RE/dequoting) --
    # match \"zones\": literally, not "zones":.
    r'\\"zones\\":\[.*?for\s*\(uint8_t\s+i\s*=\s*0;\s*i\s*<\s*MAX31856_CHANNEL_COUNT;\s*i\+\+\)\s*\{',
    re.DOTALL,
)
_ZONE_POST_FIELD_LITERAL_RE = re.compile(
    r'snprintf\(\s*(?:key|gkey)\s*,\s*sizeof\((?:key|gkey)\)\s*,\s*"z%u_([A-Za-z_][A-Za-z0-9_]*)"'
)
#: Dynamic POST suffixes truncated by the regex above at the point a second
#: format placeholder appears -- see this section's own header comment.
#: Excluded from the firmware set rather than the client set, since the
#: client's coverage for these is a regex/loop, not a flat dict entry.
_ZONE_POST_DYNAMIC_SUFFIXES = {"coupling_c", "settings_source_"}


def _slice_matching_braces(text: str, open_brace_pos: int) -> str:
    """Return text[open_brace_pos : close+1] for the ``{`` at
    ``open_brace_pos``, found by depth counting (handles nested braces)."""
    depth = 0
    for idx in range(open_brace_pos, len(text)):
        if text[idx] == "{":
            depth += 1
        elif text[idx] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace_pos : idx + 1]
    raise AssertionError("unbalanced braces slicing the zones GET per-zone loop")


def _extract_get_per_zone_keys(text: str, source_path: pathlib.Path,
                                handler_name: str = "zones_get_handler") -> set:
    """Extract the per-zone JSON keys one GET handler's "zones":[ array
    emits. `handler_name` defaults to zones_get_handler (GET /api/zones) but
    is also used for zones_diag_get_handler (GET /api/zones_diag,
    docs/audits/zones_diag_endpoint_split_2026-09-14.md) -- both handlers
    share the same "zones":[ + `for (uint8_t i = 0; i < MAX31856_CHANNEL_
    COUNT; i++) {` marker shape by construction, so the same extractor
    covers both without duplicating this logic."""
    body = _function_body(text, handler_name, source_path)
    m = _ZONE_LOOP_START_RE.search(body)
    if m is None:
        raise AssertionError(
            f"per-zone loop marker not found in {handler_name}() ({source_path}) -- "
            "has the loop guard or the \"zones\":[ marker been renamed?"
        )
    loop_open_brace = m.end() - 1
    loop_body = _slice_matching_braces(body, loop_open_brace)
    keys: set = set()
    depth = 0
    for am in _APPEND_CALL_RE.finditer(loop_body):
        dequoted = "".join(
            part.replace('\\"', '"') for part in _STRING_LITERAL_RE.findall(am.group(1))
        )
        depth = _scan_template_keys(dequoted, depth, keys)
    return keys


def _extract_post_per_zone_suffixes(text: str, source_path: pathlib.Path) -> set:
    body = _function_body_c(text, "zones_http_parse_zone_fields", source_path)
    suffixes = set(_ZONE_POST_FIELD_LITERAL_RE.findall(body))
    return suffixes - _ZONE_POST_DYNAMIC_SUFFIXES


def _function_body_c(text: str, name: str, source_path: pathlib.Path) -> str:
    """Same as _function_body(), but for a ``bool``-returning function
    (zones_http_parse_zone_fields is declared ``bool``, not ``esp_err_t``)."""
    m = re.search(rf"\bbool\s+{re.escape(name)}\s*\(", text)
    if m is None:
        raise AssertionError(f"{name}() not found in {source_path}")
    start = m.start()
    # Bound on the next top-level function start (bool/esp_err_t/static/void
    # return type at column 0) or end of file -- this file only defines the
    # one function plus SRC_GROUP_NAMES, so end-of-file is the common case.
    nxt = re.compile(r"\n(?:bool|esp_err_t|void|static)\s+\w+\s*\(").search(text, start + 1)
    end = nxt.start() if nxt else len(text)
    return text[start:end]


def zones_per_zone_field_table_checks() -> None:
    """Two endpoints now, since docs/audits/zones_diag_endpoint_split_2026-
    09-14.md moved coupling_tau_c%u/coupling_dead_time_c%u and model_fit_
    temp_c/model_fit_ambient_c off GET /api/zones (zones_get_handler) onto
    GET /api/zones_diag (zones_diag_get_handler) -- both handlers live in the
    same zones_http_get.c, so both are extracted from the one file read.
    Each endpoint's real per-zone JSON keys are checked against ITS OWN
    reference set below -- a field checked against the wrong endpoint's
    reference set would show up as spurious "missing"/"extra" on both sides
    rather than silently passing, so a future re-split (or a field moving
    back) that isn't reflected here fails loudly rather than being missed."""
    get_text = _ZONES_GET_C_PATH.read_text(encoding="utf-8")
    post_text = _ZONES_POST_PARSE_C_PATH.read_text(encoding="utf-8")

    fw_get_keys = _extract_get_per_zone_keys(get_text, _ZONES_GET_C_PATH, "zones_get_handler")
    fw_diag_keys = _extract_get_per_zone_keys(get_text, _ZONES_GET_C_PATH, "zones_diag_get_handler")
    fw_post_suffixes = _extract_post_per_zone_suffixes(post_text, _ZONES_POST_PARSE_C_PATH)

    check("firmware per-zone GET extractor found keys", len(fw_get_keys) > 10, True)
    check("firmware per-zone GET /api/zones_diag extractor found keys", len(fw_diag_keys) > 1, True)
    check("firmware per-zone POST extractor found suffixes", len(fw_post_suffixes) > 10, True)

    # GET /api/zones side: every real per-zone JSON key must be either a
    # field this client can round-trip (_ZONE_FIELD_FORM_KEY), a known
    # read-only key (_ZONE_READONLY_KEYS -- this still includes model_fit_
    # temp_c/model_fit_ambient_c, since a caller that merge_zones_diag()'d
    # them back onto a zone dict must still never try to repost them, but
    # they are no longer expected to be found by THIS extractor since they
    # moved off zones_get_handler -- excluded below by name, not by removing
    # them from _ZONE_READONLY_KEYS, which stays the single source of truth
    # for "never POST this"), or the settings_source_groups nested object
    # (handled by its own dedicated logic in _encode_zone(), not a flat
    # dict entry).
    client_get_keys = ((set(_ZONE_FIELD_FORM_KEY) | set(_ZONE_READONLY_KEYS) | {"settings_source_groups"})
                       - _ZONES_DIAG_MODEL_FIT_KEYS)
    missing_from_client_get = fw_get_keys - client_get_keys
    extra_in_client_get = client_get_keys - fw_get_keys
    check(
        "zones per-zone GET keys: client dicts match firmware "
        f"(missing from client -- would raise ZonesHttpUnknownFieldError on every "
        f"zones save: {sorted(missing_from_client_get)}, "
        f"extra in client: {sorted(extra_in_client_get)})",
        (missing_from_client_get, extra_in_client_get),
        (set(), set()),
    )

    # GET /api/zones_diag side (docs/audits/zones_diag_endpoint_split_2026-
    # 09-14.md): "index" plus exactly the fields the split moved here --
    # coupling_tau_c%u/coupling_dead_time_c%u don't show up in this
    # extraction at all (dynamic "coupling_tau_c%u" keys never match
    # _IDENT_RE, same reason coupling_c%u never has on the /api/zones side --
    # see this module's header comment), so only the two literal-identifier
    # keys are checked here.
    client_diag_keys = set(_ZONES_DIAG_MODEL_FIT_KEYS) | {"index"}
    missing_from_client_diag = fw_diag_keys - client_diag_keys
    extra_in_client_diag = client_diag_keys - fw_diag_keys
    check(
        "zones_diag per-zone GET keys: client's _ZONES_DIAG_MODEL_FIT_KEYS matches firmware "
        f"(missing from client: {sorted(missing_from_client_diag)}, "
        f"extra in client: {sorted(extra_in_client_diag)})",
        (missing_from_client_diag, extra_in_client_diag),
        (set(), set()),
    )

    # POST side: every per-zone form-field suffix the firmware parses must be
    # produced by _ZONE_FIELD_FORM_KEY's values (what _encode_zone() actually
    # emits onto the wire).
    client_post_suffixes = set(_ZONE_FIELD_FORM_KEY.values())
    missing_from_client_post = fw_post_suffixes - client_post_suffixes
    extra_in_client_post = client_post_suffixes - fw_post_suffixes
    check(
        "zones per-zone POST suffixes: _ZONE_FIELD_FORM_KEY matches firmware "
        f"(missing from client: {sorted(missing_from_client_post)}, "
        f"extra in client: {sorted(extra_in_client_post)})",
        (missing_from_client_post, extra_in_client_post),
        (set(), set()),
    )
