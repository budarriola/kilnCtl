#!/usr/bin/env python3
"""Wire-layout regression test for AUTOTUNE_CMD_GET_STATUS/ACCEPT -- item 1
of the 2026-09-02 round-2 review, and item 3 of the round-3 follow-up.

Round 2: a draft of the model_settled byte was inserted BEFORE the
length-prefixed abort_reason string in uart_bridge_ext.c's
autotune_build_status(), silently shifting abort_reason's own length-prefix
offset by one byte without updating this file's decoder
(devices.parse_autotune_response()) or bumping UART_PROTOCOL_VERSION --
every abort reason this frame carried would have decoded as "" (settled==0)
or one garbage byte (settled==1), with no exception raised.

Round 3: the FIRST version of this test built its synthetic payload with a
hand-written Python replica of autotune_build_status() -- it exercised the
Python decoder against Python-authored bytes and never read, parsed, or
compiled the C source at all, so reintroducing the exact round-2 ordering
bug in uart_bridge_ext.c would have passed this test unchanged. Fixed by
reading and regex-parsing the ACTUAL C function body (same
read-the-C-source-with-regex technique test_uart_version_independence.py
already uses in this repo for UART_PROTOCOL_VERSION), extracting the real
sequence of wire-writing statements and their sizes, and cross-checking the
resulting byte offsets against devices.py's own hardcoded offsets -- so a
byte inserted anywhere in the C function, including before abort_reason's
lstring, changes what this test computes and fails it.

No real link and no live board is used or required; nothing here compiles
or links the firmware, only parses its source text.

Run with: python -m pytest tools/PcTools/tests/test_autotune_wire_layout.py -q
"""
from __future__ import annotations

import re
import struct
from pathlib import Path

from kilnctrl import devices
from kilnctrl.devices import AUTOTUNE_CMD_GET_STATUS

from _drivers_layout import resolve_driver_file

_REPO_ROOT = Path(__file__).resolve().parents[3]
_UART_BRIDGE_EXT_C = resolve_driver_file(_REPO_ROOT, "uart_bridge_ext.c")
# uart_bridge_ext.c (2026-09-04, ROADMAP.md's 1500-line rule) was split into
# uart_bridge_ext.c plus three siblings; autotune_build_status() -- the exact
# function this file exists to regression-test -- moved into
# uart_bridge_ext_autotune.c. A path still pointed at uart_bridge_ext.c alone
# would silently find nothing (see _load_layout()'s own assert) rather than
# comparing against the function's real, current text.
_UART_BRIDGE_EXT_AUTOTUNE_C = resolve_driver_file(_REPO_ROOT, "uart_bridge_ext_autotune.c")

# Fixed-size wire writers this function uses, and how many bytes each
# advances `o` by. Mirrors uart_bridge_ext_put_*_le()'s own definitions
# (uart_bridge_ext.c) -- renamed from the old bx_put_*_le() spelling by the
# 2026-09-04 uart_bridge_ext.c split's widening rename. A NEW writer helper
# introduced later that isn't listed here makes _parse_build_status_layout()
# raise instead of silently mis-measuring it.
_FIELD_WIDTHS = {
    "out[o++]": 1,
    "uart_bridge_ext_put_u16_le": 2,
    "uart_bridge_ext_put_u32_le": 4,
    "uart_bridge_ext_put_f32_le": 4,
}


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//.*", "", text)
    return text


def _extract_function_body(code: str, name: str) -> str:
    """Pulls the brace-balanced body of `static ... name(...) { ... }` out
    of already-comment-stripped C source. Brace-counting, not a single
    regex, because the body contains its own nested braces (none here, but
    a future edit adding an `if` must not silently truncate the parse)."""
    header_match = re.search(rf"\b{re.escape(name)}\s*\([^)]*\)\s*\{{", code)
    assert header_match is not None, f"could not find {name}()'s definition"
    depth = 1
    i = header_match.end()
    start = i
    while depth > 0:
        assert i < len(code), f"unbalanced braces scanning {name}()"
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
        i += 1
    return code[start : i - 1]


def _parse_build_status_layout(body: str) -> "list[tuple[str, int | None]]":
    """Walks autotune_build_status()'s body in source order and returns a
    list of (field_label, byte_width_or_None) -- None marks the one
    variable-width field, the abort_reason lstring. Anything in the body
    that is not one of the two recognized write forms (a fixed-size write
    or the lstring call) is left alone; only write statements contribute an
    entry."""
    entries: "list[tuple[str, int | None]]" = []
    for line in body.splitlines():
        line = line.strip()
        if not line:
            continue
        if "uart_bridge_ext_put_lstring" in line:
            entries.append(("abort_reason (lstring)", None))
            continue
        m = re.match(r"out\[o\+\+\]\s*=\s*(.+?);", line)
        if m:
            entries.append((m.group(1), _FIELD_WIDTHS["out[o++]"]))
            continue
        m = re.match(r"(uart_bridge_ext_put_\w+_le)\(&out\[o\],\s*(.+?)\);\s*o\s*\+=\s*(\d+);", line)
        if m:
            fn, arg, width = m.group(1), m.group(2), int(m.group(3))
            assert fn in _FIELD_WIDTHS, f"unrecognized wire writer {fn!r} -- add it to _FIELD_WIDTHS"
            assert _FIELD_WIDTHS[fn] == width, (
                f"{fn}({arg}) advances o by {width}, expected {_FIELD_WIDTHS[fn]} -- "
                "either the helper's width changed or this is a bug at the call site"
            )
            entries.append((arg, width))
            continue
    return entries


def _offsets_after(entries: "list[tuple[str, int | None]]") -> "dict[str, int]":
    """field label -> byte offset immediately AFTER that field (i.e. the
    offset the NEXT field starts at) -- computed by summing widths in
    order. A variable-width (lstring) entry maps to -1, a sentinel: "the
    offset after it" is not a fixed number by construction."""
    out: "dict[str, int]" = {}
    o = 0
    for label, width in entries:
        if width is None:
            out[label] = -1
            continue
        o += width
        out[label] = o
    return out


def _load_layout() -> "list[tuple[str, int | None]]":
    code = _strip_comments(_UART_BRIDGE_EXT_AUTOTUNE_C.read_text(encoding="utf-8"))
    body = _extract_function_body(code, "autotune_build_status")
    entries = _parse_build_status_layout(body)
    assert entries, "parsed zero write statements out of autotune_build_status() -- parser regex is stale"
    return entries


def test_uart_bridge_ext_source_file_exists():
    assert _UART_BRIDGE_EXT_C.is_file(), _UART_BRIDGE_EXT_C


def test_model_settled_is_written_after_the_abort_reason_lstring():
    """THE regression this test exists to catch: whatever else changes in
    this function, the model_settled write must come strictly AFTER the
    abort_reason lstring in source (and therefore wire) order -- never
    before it. This is what a byte inserted before the lstring (round 2's
    actual defect) flips."""
    entries = _load_layout()
    labels = [label for label, _ in entries]
    lstring_idx = next(i for i, l in enumerate(labels) if "lstring" in l)
    settled_idx = next(
        i for i, l in enumerate(labels) if "model.settled" in l or "st.model.settled" in l
    )
    assert settled_idx > lstring_idx, (
        f"model.settled (entry {settled_idx}: {labels[settled_idx]!r}) must be written AFTER "
        f"the abort_reason lstring (entry {lstring_idx}), not before it -- see this test's own "
        "module docstring for the corruption this ordering prevents"
    )


def test_extrapolation_converged_and_tau_consistent_are_written_after_settled():
    """2026-09-02 (round-3 follow-up, UART_PROTOCOL_VERSION 9->10): two
    more trailing fields, extrapolation_converged and tau_consistent_with_
    gain, must be written after model.settled (and therefore also after the
    abort_reason lstring, transitively) -- same append-only discipline, same
    corruption class if violated. Order between the two new fields relative
    to EACH OTHER is not asserted (either order is wire-compatible with
    itself), only that both come strictly after settled."""
    entries = _load_layout()
    labels = [label for label, _ in entries]
    settled_idx = next(
        i for i, l in enumerate(labels) if "model.settled" in l or "st.model.settled" in l
    )
    converged_idx = next(
        i for i, l in enumerate(labels) if "extrapolation_converged" in l
    )
    tau_consistent_idx = next(
        i for i, l in enumerate(labels) if "tau_consistent_with_gain" in l
    )
    assert converged_idx > settled_idx, (
        f"extrapolation_converged (entry {converged_idx}) must be written AFTER model.settled "
        f"(entry {settled_idx})"
    )
    assert tau_consistent_idx > settled_idx, (
        f"tau_consistent_with_gain (entry {tau_consistent_idx}) must be written AFTER model.settled "
        f"(entry {settled_idx})"
    )


def test_fixed_field_offsets_match_the_python_decoder():
    """Cross-checks every FIXED-size field's offset, as computed from the
    actual C source, against devices.parse_autotune_response()'s own
    hardcoded offsets (4, 8, 10, 14, 15, 19, 20, 32, 44, 45, 49, 50, 62/63)
    -- if a field is added, removed, or reordered in the C function without
    a matching change on the Python side, this fails."""
    entries = _load_layout()

    assert entries[0][1] == 1 and entries[1][1] == 1 and entries[2][1] == 1 and entries[3][1] == 1
    offset = sum(w for _, w in entries[0:4])
    assert offset == 4, f"subcmd/state/method/zone must total 4 bytes, got {offset}"

    after = _offsets_after(entries)

    elapsed_label = entries[4][0]
    assert "elapsed_s" in elapsed_label, entries[4]
    assert after[elapsed_label] == 8

    sample_label = entries[5][0]
    assert "sample_count" in sample_label, entries[5]
    assert after[sample_label] == 10

    actual_c_label = entries[6][0]
    assert "actual_c" in actual_c_label, entries[6]
    assert after[actual_c_label] == 14

    # actual_valid (u8) -> offset 15 (this is payload[14] in devices.py)
    actual_valid_label = entries[7][0]
    assert "actual_valid" in actual_valid_label, entries[7]
    assert after[actual_valid_label] == 15

    duty_label = entries[8][0]
    assert "duty" in duty_label, entries[8]
    assert after[duty_label] == 19

    # model.valid (u8) -> offset 20 (payload[19] in devices.py)
    model_valid_label = entries[9][0]
    assert "model.valid" in model_valid_label, entries[9]
    assert after[model_valid_label] == 20

    # k_gain, tau_s, dead_time_s (3x f32) -> offset 32 (payload[20:32])
    dead_time_label = entries[12][0]
    assert "dead_time_s" in dead_time_label, entries[12]
    assert after[dead_time_label] == 32

    # kp, ki, kd (3x f32) -> offset 44
    kd_label = entries[15][0]
    assert "kd" in kd_label, entries[15]
    assert after[kd_label] == 44

    # rule (u8) -> offset 45 (payload[44] in devices.py)
    rule_label = entries[16][0]
    assert "rule" in rule_label, entries[16]
    assert after[rule_label] == 45

    ramp_label = entries[17][0]
    assert "predicted_max_ramp" in ramp_label, entries[17]
    assert after[ramp_label] == 49

    # relay.valid (u8) -> offset 50 (payload[49] in devices.py)
    relay_valid_label = entries[18][0]
    assert "relay.valid" in relay_valid_label, entries[18]
    assert after[relay_valid_label] == 50

    # ku, tu_s, amplitude_c (3x f32) -> offset 62 (payload[50:62])
    amplitude_label = entries[21][0]
    assert "amplitude_c" in amplitude_label, entries[21]
    assert after[amplitude_label] == 62

    # abort_reason's length byte is payload[62] in devices.py -- i.e. the
    # lstring call must begin exactly where the fixed-size fields end.
    lstring_idx = next(i for i, (l, w) in enumerate(entries) if w is None)
    assert lstring_idx == 22, f"abort_reason lstring must be entry 22 (right after amplitude_c), got {lstring_idx}"
    running = sum(w for _, w in entries[:lstring_idx] if w is not None)
    assert running == 62, f"abort_reason length-prefix byte must land at offset 62, got {running}"


# ---------------------------------------------------------------------------
# Round-2 regression coverage: a synthetic payload, built from the layout
# READ FROM THE C SOURCE (not a hand-maintained copy of it), decoded through
# the REAL devices.parse_autotune_response(), at several abort_reason
# lengths.
# ---------------------------------------------------------------------------


def _build_status_payload(
    *,
    state: int = 5,
    method: int = 0,
    zone: int = 1,
    elapsed_s: int = 1234,
    sample_count: int = 60,
    actual_c: float = 210.5,
    actual_valid: bool = True,
    duty: float = 0.75,
    model_valid: bool = True,
    k_gain: float = 123.4,
    tau_s: float = 456.7,
    dead_time_s: float = 12.0,
    kp: float = 0.01,
    ki: float = 0.001,
    kd: float = 0.0001,
    rule: int = 0,
    predicted_max_ramp: float = 987.0,
    relay_valid: bool = False,
    ku: float = 0.0,
    tu_s: float = 0.0,
    amplitude_c: float = 0.0,
    abort_reason: str = "",
    model_settled: bool = True,
    model_extrapolation_converged: bool = True,
    model_tau_consistent: bool = True,
) -> bytes:
    """Builds a payload using the layout _load_layout() just read from the
    C source -- so this helper cannot silently drift from
    uart_bridge_ext.c the way a hand-maintained replica could (the round-3
    review's actual finding about the first version of this file). Field
    values are supplied by keyword in natural field order; this walks the
    parsed entries and packs each one from `values` in turn.

    Everything after the abort_reason lstring (settled, extrapolation_
    converged, tau_consistent_with_gain, in whatever order the C source
    actually writes them) is packed generically by NAME, not by a fixed
    positional assumption -- so this helper stays correct even if a future
    change reorders the trailer fields among themselves (only their
    position relative to the lstring is asserted elsewhere, by
    test_extrapolation_converged_and_tau_consistent_are_written_after_
    settled())."""
    values = [
        state, method, zone, elapsed_s, sample_count, actual_c,
        1 if actual_valid else 0, duty, 1 if model_valid else 0,
        k_gain, tau_s, dead_time_s, kp, ki, kd, rule, predicted_max_ramp,
        1 if relay_valid else 0, ku, tu_s, amplitude_c,
    ]
    trailer_values_by_label_substring = {
        "extrapolation_converged": model_extrapolation_converged,
        "tau_consistent_with_gain": model_tau_consistent,
        # model.settled matched last -- it's a substring-free "settled"
        # check would also match "extrapolation_converged"/"tau_consistent"
        # if either were ever renamed to contain "settled", so this is
        # deliberately the most specific (least likely to collide) key.
        "model.settled": model_settled,
    }

    out = bytearray(struct.pack("<B", AUTOTUNE_CMD_GET_STATUS))
    entries = _load_layout()
    lstring_idx = next(i for i, (l, w) in enumerate(entries) if w is None)
    vi = 0
    # entries[0] is the subcmd byte, already packed above -- body runs
    # entries[1:lstring_idx] (the fixed-size fields), entries[lstring_idx]
    # is the lstring itself, and entries[lstring_idx+1:] is the trailer
    # (settled/converged/tau_consistent), packed by name below.
    for label, width in entries[1:lstring_idx]:
        v = values[vi]
        vi += 1
        if width == 1:
            out += struct.pack("<B", int(v))
        elif width == 2:
            out += struct.pack("<H", int(v))
        elif width == 4 and isinstance(v, float):
            out += struct.pack("<f", v)
        elif width == 4:
            out += struct.pack("<I", int(v))
        else:
            raise AssertionError(f"unhandled width {width} for {label!r}")

    reason_bytes = abort_reason.encode("ascii")
    out += struct.pack("<B", len(reason_bytes))
    out += reason_bytes

    for label, width in entries[lstring_idx + 1 :]:
        assert width == 1, f"unexpected trailer field width for {label!r}: {width}"
        matched = [v for key, v in trailer_values_by_label_substring.items() if key in label]
        assert len(matched) == 1, f"trailer field {label!r} did not match exactly one known name"
        out += struct.pack("<B", 1 if matched[0] else 0)
    return bytes(out)


def test_get_status_decodes_with_no_abort_reason() -> None:
    payload = _build_status_payload(abort_reason="", model_settled=True)
    subcmd, status = devices.parse_autotune_response(payload)
    assert subcmd == AUTOTUNE_CMD_GET_STATUS
    assert status.abort_reason == ""
    assert status.model_settled is True


def test_get_status_decodes_with_a_short_abort_reason_and_settled_false() -> None:
    # The exact combination the corruption defect hid: a non-empty
    # abort_reason together with a model_settled byte.
    payload = _build_status_payload(
        state=6,
        abort_reason="fit failed: rise too small",
        model_settled=False,
    )
    subcmd, status = devices.parse_autotune_response(payload)
    assert subcmd == AUTOTUNE_CMD_GET_STATUS
    assert status.abort_reason == "fit failed: rise too small"
    assert status.model_settled is False


def test_get_status_decodes_with_a_long_abort_reason() -> None:
    reason = "fit failed: gain implies 45.0C max at full duty, below zone limit 1200.0C -- fit is wrong"
    payload = _build_status_payload(abort_reason=reason, model_settled=True)
    subcmd, status = devices.parse_autotune_response(payload)
    assert subcmd == AUTOTUNE_CMD_GET_STATUS
    assert status.abort_reason == reason
    assert status.model_settled is True


def test_get_status_decodes_extrapolation_converged_and_tau_consistent_independently() -> None:
    """The three trailer booleans must decode independently, not as one
    collapsed bit -- exercises all four combinations of the two NEW flags
    (settled held True throughout, since this test is specifically about
    the other two)."""
    for converged, tau_ok in [(True, True), (True, False), (False, True), (False, False)]:
        payload = _build_status_payload(
            abort_reason="",
            model_settled=True,
            model_extrapolation_converged=converged,
            model_tau_consistent=tau_ok,
        )
        _, status = devices.parse_autotune_response(payload)
        assert status.model_extrapolation_converged is converged, (converged, tau_ok)
        assert status.model_tau_consistent is tau_ok, (converged, tau_ok)


def test_get_status_all_numeric_fields_round_trip() -> None:
    payload = _build_status_payload(
        elapsed_s=7200,
        sample_count=720,
        actual_c=999.25,
        duty=0.42,
        k_gain=88.5,
        tau_s=333.0,
        dead_time_s=44.0,
        kp=1.5,
        ki=0.25,
        kd=0.05,
        predicted_max_ramp=456.0,
        abort_reason="",
        model_settled=True,
        model_extrapolation_converged=False,
        model_tau_consistent=False,
    )
    _, status = devices.parse_autotune_response(payload)
    assert status.elapsed_s == 7200
    assert status.sample_count == 720
    assert status.actual_c == 999.25
    assert abs(status.duty - 0.42) < 1e-5
    assert abs(status.model.k_gain_c_per_duty - 88.5) < 1e-3
    assert abs(status.model.tau_s - 333.0) < 1e-3
    assert abs(status.model.dead_time_s - 44.0) < 1e-3
    assert status.model_settled is True
    assert status.model_extrapolation_converged is False
    assert status.model_tau_consistent is False


def test_get_status_from_a_pre_version_9_firmware_defaults_all_three_flags_false() -> None:
    """A v8 firmware sends none of the three trailer bytes at all."""
    payload = _build_status_payload(abort_reason="", model_settled=True)
    # Strip settled + extrapolation_converged + tau_consistent_with_gain --
    # all three trailer bytes -- to reproduce a v8 (pre-Version-9) reply.
    trailer_len = 3
    payload_without_any_trailer = payload[:-trailer_len]
    _, status = devices.parse_autotune_response(payload_without_any_trailer)
    assert status.model_settled is False
    assert status.model_extrapolation_converged is False
    assert status.model_tau_consistent is False


def test_get_status_from_a_pre_version_10_firmware_defaults_new_flags_false() -> None:
    """A v9 firmware sends settled but not the two Version-10 bytes."""
    payload = _build_status_payload(
        abort_reason="",
        model_settled=True,
        model_extrapolation_converged=True,
        model_tau_consistent=True,
    )
    trailer_len_for_v10_fields = 2
    payload_from_v9 = payload[:-trailer_len_for_v10_fields]
    _, status = devices.parse_autotune_response(payload_from_v9)
    assert status.model_settled is True
    assert status.model_extrapolation_converged is False
    assert status.model_tau_consistent is False
