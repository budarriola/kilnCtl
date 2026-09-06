#!/usr/bin/env python3
"""safety_cfg_http_client.py -- pure HTTP client for
GET/POST /api/safety/commissioning
(firmware/KilnFW/App/drivers/http/safety_cfg_http.c), the write path for the
SAFETY-PROCESSOR (SaftyFW) commissioning parameters that config_presets.py's
docstring used to describe as out of scope. Same "stdlib urllib.request, no
framework" convention as zones_http_client.py/ota_http_client.py, and in its
own module for the same reason: everything here is unit-tested against
mocked HTTP responses (tools/PcTools/tests/test_safety_cfg_http_client.py),
no real socket and no live board.

WHY THIS IS NOT zones_http_client.py AGAIN. The two endpoints have OPPOSITE
POST conventions, and getting that backwards is a hazard in each direction:

  * POST /api/zones is a WHOLE-PAGE SUBMIT -- an omitted field means "set
    this to zero", so zones_http_client.py must GET the whole config and
    echo every field back.
  * POST /api/safety/commissioning is INCREMENTAL -- the body is a sequence
    of adjacent ``id=<decimal>&value=<v>`` token pairs plus an optional
    ``commit=1``, each pair staged on the Pico via SET_PARAM. A parameter
    NOT named in the body is not touched at all (parse_set_param_body() /
    apply_pairs() in safety_cfg_http.c). So this module deliberately sends
    ONLY the fields the caller asked for. Echoing the whole parameter page
    back the way the zones client does would re-commit values nobody asked
    to change and -- far worse -- turn every currently-UNSET field into an
    explicitly-SET one. config_store.h is explicit that the
    CONFIG_STORE_SET_* bit is the ONLY thing distinguishing a commissioned
    value from a compiled default; clearing calibration_missing by echoing
    defaults back would make "commissioned" a claim rather than a fact
    (commissioning_gate.h's own wording).

WE STILL READ FIRST, for a different purpose than merging: the GET supplies
each id's WIRE TYPE (u8/u16/f32/bool) and its current set/value state, so
this module can (a) refuse a field the board does not know, (b) format the
value the way that id's type requires, and (c) report what a write actually
changed. get_commissioning() is the first call every writer here makes.

AN ACK IS NOT PROOF. This is the lesson zones_http_client's
_verify_against_preset() and zones_http.c's zone_sweep_confirm_ct_map_landed()
both exist for, and it applies doubly here: SET_PARAM and COMMIT_CONFIG are
fire-and-forget broadcasts over the isolated UART, and a
COMMIT_CONFIG_REJECTED reply that misses its reply window used to be silently
discarded. The firmware now runs its own confirm_commit_landed() live
re-fetch before answering {"ok":true} -- and this module STILL re-reads
afterwards and compares every submitted field, because a client that trusts
the server's self-report has no independent evidence of anything. Both
checks failing closed is the point, not redundancy.

WHAT THIS MODULE WILL NOT DO. It has no "write every required field until
the board reports commissioned" convenience. Which fields are safe to write
is a question about physical hardware, answered per bench in the preset data
(config_presets/*.json), not here -- see bench_fixture.json's own comment
block for the live example: this bench has no CT fitted, so ct_channel_map
has no measured truth and lives in a separate, opt-in-only backup section
rather than being written.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Optional

#: A commit is a real blocking round trip to the Pico (SET_PARAM xN,
#: COMMIT_CONFIG, then the firmware's own live GET_CONFIG_PAGE re-fetch), so
#: this is longer than the zones endpoint's 8 s.
SAFETY_CFG_HTTP_TIMEOUT_S = 20.0

_API_PATH = "/api/safety/commissioning"


class SafetyCfgHttpError(Exception):
    """Any transport or protocol failure talking to
    GET/POST /api/safety/commissioning -- unreachable host, non-2xx, or a
    response shape this client does not understand."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


class SafetyCfgUnknownParamError(SafetyCfgHttpError):
    """Raised when a caller names a parameter the live board's own table
    does not carry. Refused loudly rather than skipped: a silently-dropped
    commissioning field is a field the operator believes is set and the
    guard reading it does not."""


def _url(host: str) -> str:
    return f"http://{host}{_API_PATH}"


def _http_error_detail(exc: Exception) -> "tuple[Optional[int], str]":
    if isinstance(exc, urllib.error.HTTPError):
        try:
            detail = exc.read().decode("utf-8", errors="replace").strip()
        except Exception:
            detail = ""
        return exc.code, detail
    if isinstance(exc, urllib.error.URLError):
        return None, f"unreachable: {exc.reason}"
    return None, str(exc)


def get_commissioning(host: str, timeout: float = SAFETY_CFG_HTTP_TIMEOUT_S) -> dict:
    """GET /api/safety/commissioning -- the ESP's view of the Pico's config
    record, as commissioning_get_handler() builds it: ``{link_up,
    live_config_crc, cached_config_crc, stale, commissioned, fetched_ms_ago,
    unset_reporting_reliable, params:[{id,name,type,set,value?}]}``.

    Note ``value`` is OMITTED, not zeroed, for an unset parameter -- that
    omission is deliberate on the firmware side so no caller can mistake an
    uncommissioned field for a real 0 (0 on abs_max_temp_c means the
    overtemperature guard never trips). The helpers below preserve that
    distinction rather than defaulting the value."""
    req = urllib.request.Request(_url(host), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise SafetyCfgHttpError(f"GET {_API_PATH} failed: {detail}", status, detail) from exc
    try:
        data = json.loads(body_text)
    except Exception as exc:
        raise SafetyCfgHttpError(
            f"GET {_API_PATH} response was not valid JSON: {body_text!r}") from exc
    if not isinstance(data, dict) or not isinstance(data.get("params"), list):
        raise SafetyCfgHttpError(f"GET {_API_PATH} response has no params list: {body_text!r}")
    return data


def params_by_name(current: dict) -> "dict[str, dict]":
    """The GET's params list keyed by the firmware's own field name
    (safety_cfg_store.c's table -- ``abs_max_temp_c``, ``ct_channel_map[0]``,
    ...). Names, not ids, are what a preset spells, so a preset stays
    readable and a renumbered id cannot silently retarget a value at a
    different field."""
    return {p["name"]: p for p in current.get("params", []) if "name" in p}


def params_by_id(current: dict) -> "dict[int, dict]":
    return {int(p["id"]): p for p in current.get("params", []) if "id" in p}


def format_value(name: str, wire_type: str, value: Any) -> str:
    """One parameter's value, rendered the way safety_cfg_http.c's
    parse_value_for_type() parses it back:

      * ``bool`` -> "1"/"0" (strtol, and ONLY 0 or 1 is accepted)
      * ``u8``/``u16`` -> a plain decimal integer, never a float repr like
        "3.0" (strtol stops at the '.' and the handler rejects the pair)
      * ``f32`` -> ``repr(float(...))``; NaN/Inf are refused here rather than
        sent. The firmware rejects them too, but a NaN threshold is worth
        failing on the PC side with the field's own name attached.
    """
    if wire_type == "bool":
        if isinstance(value, float) or not isinstance(value, (bool, int)):
            raise SafetyCfgHttpError(f"{name}: bool parameter needs true/false or 0/1, got {value!r}")
        return "1" if value else "0"
    if wire_type in ("u8", "u16"):
        if isinstance(value, bool):
            raise SafetyCfgHttpError(f"{name}: {wire_type} parameter got a bool, {value!r}")
        as_int = int(value)
        if as_int != value:
            raise SafetyCfgHttpError(f"{name}: {wire_type} parameter needs a whole number, got {value!r}")
        limit = 0xFF if wire_type == "u8" else 0xFFFF
        if as_int < 0 or as_int > limit:
            raise SafetyCfgHttpError(f"{name}: {value!r} does not fit a {wire_type}")
        return str(as_int)
    if wire_type == "f32":
        if isinstance(value, bool):
            raise SafetyCfgHttpError(f"{name}: f32 parameter got a bool, {value!r}")
        as_float = float(value)
        if as_float != as_float or as_float in (float("inf"), float("-inf")):
            raise SafetyCfgHttpError(f"{name}: {value!r} is not a finite number")
        return repr(as_float)
    raise SafetyCfgHttpError(f"{name}: unknown wire type {wire_type!r} reported by the board")


def build_post_body(current: dict, fields: "dict[str, Any]", commit: bool = True) -> str:
    """The ``id=<decimal>&value=<v>`` body for exactly `fields`, and nothing
    else -- see the module docstring on why this endpoint must NOT be sent a
    whole-page echo.

    Pairs are emitted in the order `fields` iterates, with each ``id`` token
    IMMEDIATELY followed by its ``value`` token: safety_cfg_http.c's
    parse_set_param_body() is an order-preserving tokenizer that pairs each
    "id" with the next "value" and returns -1 (HTTP 400 for the whole
    request) on two ids in a row. urlencode() over a list of tuples
    preserves that order; a dict body could not express it at all, since the
    keys repeat.

    Raises SafetyCfgUnknownParamError for a name the live GET did not
    report, so a typo -- or a field from a newer firmware -- fails here
    rather than being quietly dropped from the write."""
    known = params_by_name(current)
    pairs: "list[tuple[str, str]]" = []
    for name, value in fields.items():
        entry = known.get(name)
        if entry is None:
            raise SafetyCfgUnknownParamError(
                f"the board's parameter table has no field named {name!r} -- refusing to write a "
                "commissioning value this firmware would not recognise")
        pairs.append(("id", str(int(entry["id"]))))
        pairs.append(("value", format_value(name, entry.get("type", ""), value)))
    if not pairs and not commit:
        raise SafetyCfgHttpError("nothing to write: no fields and no commit")
    if commit:
        pairs.append(("commit", "1"))
    return urllib.parse.urlencode(pairs)


def post_commissioning(host: str, body: str, timeout: float = SAFETY_CFG_HTTP_TIMEOUT_S) -> dict:
    """POST /api/safety/commissioning. The handler answers JSON on both
    success paths -- ``{"ok":true}`` or ``{"ok":false,"reason":"..."}`` --
    where the reason is the firmware's own account of what went wrong (a
    rejected commit names the offending field and the rule it broke; an
    unconfirmed one says so in those words). A 4xx/5xx is plain text instead
    (httpd_resp_send_err), surfaced verbatim in SafetyCfgHttpError.detail,
    the same convention as the other two HTTP clients here.

    Returns the decoded JSON, ok or not -- an ``{"ok":false}`` body is a real
    answer from a reachable board, not a transport failure, so the caller
    reports it as a failed write rather than an exception."""
    data = body.encode("ascii")
    req = urllib.request.Request(
        _url(host), data=data, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded",
                 "Content-Length": str(len(data))},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise SafetyCfgHttpError(f"POST {_API_PATH} refused: HTTP {status}: {detail}",
                                  status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise SafetyCfgHttpError(f"POST {_API_PATH} unreachable: {detail}") from exc
    try:
        return json.loads(text)
    except Exception as exc:
        raise SafetyCfgHttpError(
            f"POST {_API_PATH} returned 200 but the body was not JSON: {text!r}") from exc


@dataclass(frozen=True)
class SafetyApplyResult:
    ok: bool
    #: Fields this call wrote AND independently confirmed by a fresh GET
    #: read-back. Never populated from the POST's own {"ok":true}.
    confirmed: "list[str]" = field(default_factory=list)
    #: Human-readable disagreements found on read-back -- empty when ok.
    mismatches: "list[str]" = field(default_factory=list)
    #: The firmware's own reason string from an {"ok":false} POST, if any.
    post_reason: str = ""
    #: `commissioned` as the board reported it on the read-back GET -- the
    #: authoritative answer to "did the gate open", straight from
    #: commissioning_gate.c's calibration_missing via Frame B, NOT inferred
    #: here from the field values.
    commissioned_after: Optional[bool] = None
    #: Required-for-commissioning fields still unset after this write. Empty
    #: does NOT by itself mean commissioned -- see commissioned_after.
    still_unset: "list[str]" = field(default_factory=list)

    def describe(self) -> str:
        lines = []
        if self.ok:
            lines.append("safety config written and CONFIRMED by a fresh read-back: "
                          + (", ".join(self.confirmed) or "(no fields)"))
        else:
            lines.append("safety config write did NOT verify:")
            if self.post_reason:
                lines.append(f"  board said: {self.post_reason}")
            lines.extend(f"  {m}" for m in self.mismatches)
            if self.confirmed:
                lines.append("  confirmed anyway: " + ", ".join(self.confirmed))
        if self.commissioned_after is not None:
            lines.append(f"  board reports commissioned: {self.commissioned_after}")
        if self.still_unset:
            lines.append("  still UNSET (required for commissioning): " + ", ".join(self.still_unset))
        return "\n".join(lines)


#: The fields SaftyFW's commissioning_gate.c requires before it will allow a
#: heat enable -- config_params_all_required_set()'s CONFIG_STORE_SET_* list,
#: spelled with safety_cfg_store.c's own names. ct_channel_map is three wire
#: ids but ONE gate bit, derived by the Pico's
#: config_params_finalize_ct_channel_map() once all three land, so all three
#: are listed. Used only for REPORTING what is still missing; nothing here
#: writes a field just because it appears on this list.
REQUIRED_FOR_COMMISSIONING = (
    "tc_source", "borrowed_zone_index", "tc_placement_mode", "abs_max_temp_c",
    "tc_type", "ct_installed",
    "ct_channel_map[0]", "ct_channel_map[1]", "ct_channel_map[2]",
    "max_rate_c_per_min", "mains_voltage_v",
)

#: The three ids that stop being required once ``ct_installed`` is answered
#: "no CTs fitted" -- see SaftyFW's config_params_all_required_set(), which
#: makes exactly this group conditional and nothing else. ``ct_installed``
#: ITSELF is never conditional: an unanswered question keeps the strict rule.
CT_MAP_FIELDS_CONDITIONAL_ON_CT_INSTALLED = (
    "ct_channel_map[0]", "ct_channel_map[1]", "ct_channel_map[2]",
)


def unset_required_fields(current: dict) -> "list[str]":
    """Which REQUIRED_FOR_COMMISSIONING fields the board currently reports as
    unset. Reads ``set``, never the presence of a value -- and when the GET
    says ``unset_reporting_reliable`` is false (a peer too old, or of unknown
    version, to have ever set KILNLINK_CONFIG_PAGE_UNSET_BIT), every field is
    reported unset, because on such a peer this board genuinely cannot tell a
    commissioned value from a defaulted one."""
    known = params_by_name(current)
    reliable = bool(current.get("unset_reporting_reliable"))

    # Mirror config_params_all_required_set()'s ONE conditional branch. The
    # CT map drops off the list only on an EXPLICIT, committed answer of "no
    # CTs fitted" -- `set` true AND value 0. An unset ct_installed, a missing
    # ct_installed (older firmware), or unreliable unset-reporting all leave
    # the strict list in force, which is the same order-independence the Pico
    # itself has: there is no state in which an unanswered question relaxes a
    # requirement.
    ct = known.get("ct_installed")
    cts_absent = bool(
        reliable and ct is not None and ct.get("set") and int(ct.get("value", 1)) == 0
    )

    out = []
    for name in REQUIRED_FOR_COMMISSIONING:
        if cts_absent and name in CT_MAP_FIELDS_CONDITIONAL_ON_CT_INSTALLED:
            continue
        entry = known.get(name)
        if entry is None:
            continue  # a field this firmware does not carry; not this module's to invent
        if not reliable or not entry.get("set"):
            out.append(name)
    return out


def _values_agree(wire_type: str, expected: Any, actual: Any) -> bool:
    if wire_type == "bool":
        return bool(actual) == bool(expected)
    if wire_type in ("u8", "u16"):
        return int(actual) == int(expected)
    if wire_type == "f32":
        # The board prints f32 with %.6g, so an exact == on the round trip is
        # wrong for a value like 0.0333333 -- compare with the tolerance that
        # six significant figures implies, not an arbitrary epsilon.
        return abs(float(actual) - float(expected)) <= max(1e-6, abs(float(expected)) * 1e-6)
    return False


def verify_fields(after: dict, fields: "dict[str, Any]") -> "tuple[list[str], list[str]]":
    """Compares a fresh GET against exactly the fields that were submitted.
    Returns ``(confirmed, mismatches)``. A field that reads back UNSET is a
    mismatch, not a pass -- an unset field is precisely the "ACKed but never
    landed" outcome this verification exists to catch."""
    known = params_by_name(after)
    confirmed: "list[str]" = []
    mismatches: "list[str]" = []
    reliable = bool(after.get("unset_reporting_reliable"))
    for name, expected in fields.items():
        entry = known.get(name)
        if entry is None:
            mismatches.append(f"{name}: not present in the read-back at all")
            continue
        if not reliable:
            mismatches.append(
                f"{name}: the board cannot report whether anything is really set "
                "(unset_reporting_reliable=false) -- treating the write as UNCONFIRMED")
            continue
        if not entry.get("set"):
            mismatches.append(f"{name}: reads back UNSET after the commit -- the write did not land")
            continue
        if "value" not in entry:
            mismatches.append(f"{name}: reads back set but with no value -- cannot confirm")
            continue
        if not _values_agree(entry.get("type", ""), expected, entry["value"]):
            mismatches.append(f"{name}: expected {expected!r}, board reports {entry['value']!r}")
            continue
        confirmed.append(name)
    return confirmed, mismatches


def apply_safety_fields(host: str, fields: "dict[str, Any]",
                        timeout: float = SAFETY_CFG_HTTP_TIMEOUT_S,
                        verify: bool = True) -> SafetyApplyResult:
    """The whole GET/POST/read-back-verify cycle for one set of named safety
    parameters. Writes ONLY `fields`; every other parameter on the board --
    set or unset -- is left exactly as it was.

    `verify=True` (the default) re-reads the config afterwards and confirms
    each submitted field now reads back SET and equal to what was sent,
    independently of the POST's own ``{"ok":true}``. It also records the
    board's own ``commissioned`` verdict and which required fields remain
    unset, so a caller never has to infer commissioning state from raw
    values.

    Never touches relays, never requests enable, never resets."""
    current = get_commissioning(host, timeout)
    if not current.get("link_up"):
        raise SafetyCfgHttpError(
            "the safety link is down -- refusing to stage commissioning values the Pico cannot receive")
    body = build_post_body(current, fields, commit=True)
    response = post_commissioning(host, body, timeout)
    reason = str(response.get("reason", "")) if not response.get("ok") else ""

    if not verify:
        return SafetyApplyResult(ok=bool(response.get("ok")), post_reason=reason)

    after = get_commissioning(host, timeout)
    confirmed, mismatches = verify_fields(after, fields)
    ok = bool(response.get("ok")) and not mismatches
    return SafetyApplyResult(
        ok=ok, confirmed=confirmed, mismatches=mismatches, post_reason=reason,
        commissioned_after=bool(after.get("commissioned")),
        still_unset=unset_required_fields(after),
    )


#: The preset section holding real, verified-for-this-bench safety values.
SAFETY_SECTION = "safety"
#: The preset section holding an ASSUMED, UNMEASURED CT-to-zone map, applied
#: only on an explicit opt-in. See apply_safety_preset() below.
SAFETY_CT_MAP_BACKUP_SECTION = "safety_ct_channel_map_backup"


def apply_safety_preset(host: str, preset: dict, timeout: float = SAFETY_CFG_HTTP_TIMEOUT_S,
                        verify: bool = True,
                        use_ct_map_backup: bool = False) -> SafetyApplyResult:
    """Applies a config_presets.py preset's ``"safety"`` section.

    ``use_ct_map_backup`` (default False) ALSO applies the preset's
    ``"safety_ct_channel_map_backup"`` section. That section holds an
    ASSUMED, UNMEASURED CT-to-zone map for a bench where no CT is fitted;
    writing it makes the board report itself commissioned on the strength of
    a mapping nobody verified, which is why it takes a separate, explicit
    opt-in rather than riding along with the rest of the preset. See
    config_presets/bench_fixture.json's own comment block."""
    fields = dict(preset.get(SAFETY_SECTION) or {})
    if use_ct_map_backup:
        fields.update(preset.get(SAFETY_CT_MAP_BACKUP_SECTION) or {})
    if not fields:
        return SafetyApplyResult(ok=True, confirmed=[])
    return apply_safety_fields(host, fields, timeout=timeout, verify=verify)
