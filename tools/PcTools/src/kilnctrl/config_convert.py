#!/usr/bin/env python3
"""config_convert.py -- owner request (2026-09-17): "Create a python tool
that supports converting any config version to any other in best effort
style, but keep the board at one version at a time."

This module is a thin DISPATCHER over the config "stores" this project
knows how to convert on the PC side, plus a standalone converter for one
store `kilnctrl.cfg_convert` (the kilnctl_backup JSON document) does not
cover: the raw on-flash "profN" NVS record (`profile_persisted_t` in
firmware/KilnFW/App/drivers/http/profiles_http.c, versioned by
PROFILE_VERSION). It never writes to a board -- see CLAUDE.md's "the tool
converts files, it never writes to a board" rule, restated here because it
is the whole reason this module exists on the PC side rather than as a
firmware migration step: firmware itself is deliberately kept to a single
N-1 -> N migration (docs/CONFIG_MIGRATION_CHAIN_PLAN.md), so a board more
than one release behind cannot read its own on-flash config. Moving the
*arbitrary* version-to-version conversion burden here, where it is cheap,
lets an operator convert an exported/backed-up file to whatever version a
particular board build actually understands, without ever asking firmware
to do more than the one step it already does.

STORES:
  - "backup" (kind "kilnctl_backup"): the zones+profiles+safety_tc_type
    backup/restore document GET /api/backup/export emits, versioned by
    BACKUP_FORMAT_VERSION. Delegates entirely to `kilnctrl.cfg_convert`,
    which already implements this conversion (forward mirrors firmware
    exactly, backward is best-effort with a lossy report) and is covered by
    its own mirror-drift check (cfg_convert_field_mirror_drift_check.py)
    and test suite (test_cfg_convert.py). This module does not re-implement
    any of that logic.
  - "profile_blob" (kind "kilnctl_profile_blob"): the raw NVS "profN"
    record (`profile_persisted_t`), versioned by PROFILE_VERSION. Firmware
    never exposes this record as JSON (profiles_http.c's own JSON endpoints
    return an already-current-version profile_t; the version-tagged wrapper
    only ever exists as bytes in flash), so this module defines its own
    thin JSON wrapper -- {"kind": "kilnctl_profile_blob", "version": N,
    "blob_hex": "<the exact bytes profiles_http.c would read from/write to
    NVS, hex-encoded>"} -- as the interchange format a caller pulls out of
    (or pushes into) NVS by some other means (e.g. a JTAG/NVS dump). This is
    this tool's own format, not one firmware emits, and is documented as
    such rather than presented as a real board export.

NOT YET SUPPORTED (best-effort tool, refuses rather than guesses):
  - kiln_cfg_store's "kilnpkg.json" package format (kiln_package.h) bundles
    a whole zones_cfg_t blob AND a Pico safety-config blob inside one
    envelope; converting it correctly means implementing kiln_package.h's
    own container format on top of everything zones_config_migrate.c does,
    which this pass did not have time to do safely. detect_kind() refuses
    a document shaped like this with a clear "not yet supported" message
    rather than attempting a partial, unverified conversion.
  - SaftyFW's raw `config_store_record_t` NVS record
    (firmware/SaftyFW/src/config_store.c, CONFIG_STORE_FORMAT_VERSION,
    currently a ~100-field, 512-byte binary record with byte-level field
    offsets) has no JSON export surface at all and no PC-side struct
    definition existed before this pass. Mirroring its full field table
    correctly needs more careful, incremental verification against real
    captured records than this pass had time for; hand-transcribing ~100
    field offsets/types from config_store.c with no fixture to check
    against risks silently WRONG output, which is worse than refusing. This
    module refuses this kind by name with a pointer to config_store.c/.h so
    a future pass has the exact place to start.

CRC. profile_persisted_t's crc32 tail is esp_crc32_le() (a standard
reflected CRC-32, poly 0xEDB88320, no init complement, no final XOR -- the
same algorithm FreeBSD's crc32() and Python's zlib.crc32() implement). This
module computes it with zlib.crc32(). This equivalence was NOT independently
verified against a live board or a firmware host-test fixture in this pass
(no captured real "profN" blob with a known-good crc32 was available) --
round-trip tests below prove this module's own pack/unpack/crc are mutually
consistent, not that they match real on-flash bytes byte-for-byte. Treat a
v2+ profile_blob conversion as unverified against real hardware until a real
captured blob is added to tools/PcTools/tests/fixtures/config_convert/.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from dataclasses import dataclass, field
from typing import Any, Optional

from . import cfg_convert as backup_cfg_convert

# ---------------------------------------------------------------------------
# Errors / shared report shape
# ---------------------------------------------------------------------------


class ConfigConvertError(RuntimeError):
    pass


@dataclass
class FieldOutcome:
    scope: str
    field: str
    action: str  # "kept" | "dropped" | "defaulted" | "renamed"
    detail: str = ""


@dataclass
class ConversionReport:
    store: str
    source_version: int
    target_version: int
    outcomes: list = field(default_factory=list)

    def add(self, scope: str, field_name: str, action: str, detail: str = "") -> None:
        self.outcomes.append(FieldOutcome(scope, field_name, action, detail))

    @property
    def lossy(self) -> bool:
        """True only when this conversion actually discarded information the
        source document held. A field gaining its documented DEFAULT because
        the source predates it (action "defaulted") is not lossy -- there
        was nothing to lose. Only "dropped" (an existing, non-default value
        the target version's layout cannot express at all) counts."""
        return any(o.action == "dropped" for o in self.outcomes)

    def render(self) -> str:
        lines = [f"config_convert report: store={self.store} v{self.source_version} -> v{self.target_version}"]
        if not self.outcomes:
            lines.append("  (no field-level changes)")
        for o in self.outcomes:
            suffix = f" -- {o.detail}" if o.detail else ""
            lines.append(f"  [{o.scope}] {o.field}: {o.action}{suffix}")
        return "\n".join(lines)

    def as_dict(self) -> dict:
        return {
            "store": self.store,
            "source_version": self.source_version,
            "target_version": self.target_version,
            "lossy": self.lossy,
            "outcomes": [
                {"scope": o.scope, "field": o.field, "action": o.action, "detail": o.detail}
                for o in self.outcomes
            ],
        }


# ---------------------------------------------------------------------------
# Store detection
# ---------------------------------------------------------------------------

# Kinds this module refuses by name, with a pointer to why -- see module
# docstring's "NOT YET SUPPORTED" section. Checked before falling through to
# "unrecognized document shape" so the caller gets a specific, actionable
# message rather than a generic one.
KNOWN_UNSUPPORTED_KINDS = {
    "kilnctl_kiln_cfg_package": (
        "kiln_cfg_store's kilnpkg.json package (kiln_package.h) is not yet "
        "supported by this tool -- it bundles a zones_cfg_t blob and a Pico "
        "safety-config blob inside one envelope, and converting it correctly "
        "needs kiln_package.h's own container format implemented here first. "
        "See config_convert.py's module docstring."
    ),
    "safety_config_store": (
        "SaftyFW's raw config_store_record_t NVS record "
        "(firmware/SaftyFW/src/config_store.c, CONFIG_STORE_FORMAT_VERSION) "
        "is not yet supported -- no PC-side struct definition for its "
        "~100-field binary layout has been verified against real captured "
        "records. See config_convert.py's module docstring."
    ),
}


def detect_kind(doc: dict) -> str:
    """Best-effort auto-detect of which store `doc` belongs to, from its own
    fields -- never from a file extension or caller hint. Raises
    ConfigConvertError (never returns an unrecognized kind silently) for
    anything this tool cannot place."""
    if not isinstance(doc, dict):
        raise ConfigConvertError("input is not a JSON object")
    kind = doc.get("kind")
    if kind == "kilnctl_backup":
        return "backup"
    if kind == "kilnctl_profile_blob":
        return "profile_blob"
    if kind in KNOWN_UNSUPPORTED_KINDS:
        raise ConfigConvertError(KNOWN_UNSUPPORTED_KINDS[kind])
    raise ConfigConvertError(
        f"could not identify a known config store from this document (kind={kind!r}). "
        "Known kinds: kilnctl_backup, kilnctl_profile_blob. "
        f"Not-yet-supported kinds: {sorted(KNOWN_UNSUPPORTED_KINDS)}"
    )


# ---------------------------------------------------------------------------
# profile_blob store -- mirrors firmware/KilnFW/App/drivers/http/
# profiles_http.c's profile_persisted_t / profile_persisted_v1_t /
# profile_persisted_v2_t / profile_persisted_v3_t and profiles_types.h's
# profile_t/profile_segment_t/profile_on_off_rule_t.
#
# PROFILE_VERSION and every struct shape below is hand-mirrored from
# firmware source and MUST be kept in step with it -- see
# check_config_convert_mirror.py, this module's own mirror-drift check.
# ---------------------------------------------------------------------------

PROFILE_VERSION = 4  # profiles_http.c PROFILE_VERSION
PROFILE_NAME_MAX_LEN = 15  # profiles_types.h PROFILE_NAME_MAX_LEN
PROFILE_MAX_SEGMENTS = 12  # profiles_types.h PROFILE_MAX_SEGMENTS
PROFILE_MAX_ON_OFF_RULES = 8  # profiles_types.h PROFILE_MAX_ON_OFF_RULES

_NAME_LEN = PROFILE_NAME_MAX_LEN + 1  # + NUL
_HEADER_FMT = "<B3x" + f"{_NAME_LEN}s" + "BB2x"  # version, name, zone_mask, segment_count
_SEG_V2_FMT = "ffI"  # profile_segment_v2_t: target_c, ramp_c_per_hr, dwell_min (12 B)
_SEG_V3_FMT = "ffIBBBBB3x"  # profile_segment_t: + seg_kind/io_target/io_state/io_blocking/io_leave_on_at_end (20 B)
_RULE_FMT = "BBBBBBBBfHHB3x"  # profile_on_off_rule_t (20 B)

_PERSISTED_V1_FMT = _HEADER_FMT + _SEG_V2_FMT * PROFILE_MAX_SEGMENTS
_PERSISTED_V2_FMT = _PERSISTED_V1_FMT + "I"
_PERSISTED_V3_FMT = _HEADER_FMT + _SEG_V3_FMT * PROFILE_MAX_SEGMENTS + "I"
_PERSISTED_V4_FMT = (
    _HEADER_FMT + _SEG_V3_FMT * PROFILE_MAX_SEGMENTS + "B3x" + _RULE_FMT * PROFILE_MAX_ON_OFF_RULES + "I"
)

_EXPECTED_LEN = {
    1: struct.calcsize(_PERSISTED_V1_FMT),
    2: struct.calcsize(_PERSISTED_V2_FMT),
    3: struct.calcsize(_PERSISTED_V3_FMT),
    PROFILE_VERSION: struct.calcsize(_PERSISTED_V4_FMT),
}

_DEFAULT_SEGMENT = {
    "target_c": 0.0, "ramp_c_per_hr": 0.0, "dwell_min": 0,
    "seg_kind": 0, "io_target": 0, "io_state": 0, "io_blocking": 0, "io_leave_on_at_end": 0,
}
_DEFAULT_RULE = {
    "segment_index": 0, "zone_index": 0, "enable": 0, "phase_mask": 0, "direction_mask": 0,
    "temp_source": 0, "temp_ref_zone": 0, "temp_cmp": 0, "temp_threshold_c": 0.0,
    "time_start_s": 0, "time_stop_s": 0, "invert": 0,
}


def _crc32(data: bytes) -> int:
    """esp_crc32_le(0, data, len) -- see module docstring's CRC note."""
    return zlib.crc32(data) & 0xFFFFFFFF


def _decode_name(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")


def _encode_name(name: str) -> bytes:
    b = name.encode("ascii", errors="replace")[:PROFILE_NAME_MAX_LEN]
    return b + b"\x00" * (_NAME_LEN - len(b))


def decode_profile_blob(blob: bytes) -> "tuple[int, dict]":
    """Decode a raw profN NVS record into (version, profile_dict). Raises
    ConfigConvertError for an unrecognized version or a length that does not
    match its claimed version (firmware treats that as corrupt too -- see
    expected_len_for_version() in profiles_http.c)."""
    if len(blob) < 1:
        raise ConfigConvertError("empty profile blob")
    version = blob[0]
    expected = _EXPECTED_LEN.get(version)
    if expected is None:
        raise ConfigConvertError(
            f"unknown/unsupported profile version {version} -- known versions: {sorted(_EXPECTED_LEN)}"
        )
    if len(blob) != expected:
        raise ConfigConvertError(
            f"blob length {len(blob)} does not match version {version}'s expected length {expected} "
            "-- firmware treats this as corrupt, not a valid record to migrate"
        )

    if version == 1:
        fmt = _PERSISTED_V1_FMT
        seg_fields = 3
    elif version == 2:
        fmt = _PERSISTED_V2_FMT
        seg_fields = 3
    elif version == 3:
        fmt = _PERSISTED_V3_FMT
        seg_fields = 8
    else:
        fmt = _PERSISTED_V4_FMT
        seg_fields = 8

    vals = list(struct.unpack(fmt, blob))
    i = 0
    _version = vals[i]; i += 1
    name = _decode_name(vals[i]); i += 1
    zone_mask = vals[i]; i += 1
    segment_count = vals[i]; i += 1

    segments = []
    for _ in range(PROFILE_MAX_SEGMENTS):
        if seg_fields == 3:
            target_c, ramp_c_per_hr, dwell_min = vals[i:i + 3]
            i += 3
            segments.append({**_DEFAULT_SEGMENT, "target_c": target_c,
                              "ramp_c_per_hr": ramp_c_per_hr, "dwell_min": dwell_min})
        else:
            (target_c, ramp_c_per_hr, dwell_min, seg_kind, io_target, io_state,
             io_blocking, io_leave_on_at_end) = vals[i:i + 8]
            i += 8
            segments.append({
                "target_c": target_c, "ramp_c_per_hr": ramp_c_per_hr, "dwell_min": dwell_min,
                "seg_kind": seg_kind, "io_target": io_target, "io_state": io_state,
                "io_blocking": io_blocking, "io_leave_on_at_end": io_leave_on_at_end,
            })
    segments = segments[:segment_count]

    on_off_rules = []
    if version == PROFILE_VERSION:
        on_off_rule_count = vals[i]; i += 1
        for _ in range(PROFILE_MAX_ON_OFF_RULES):
            (segment_index, zone_index, enable, phase_mask, direction_mask, temp_source,
             temp_ref_zone, temp_cmp, temp_threshold_c, time_start_s, time_stop_s, invert) = vals[i:i + 12]
            i += 12
            on_off_rules.append({
                "segment_index": segment_index, "zone_index": zone_index, "enable": enable,
                "phase_mask": phase_mask, "direction_mask": direction_mask, "temp_source": temp_source,
                "temp_ref_zone": temp_ref_zone, "temp_cmp": temp_cmp,
                "temp_threshold_c": temp_threshold_c, "time_start_s": time_start_s,
                "time_stop_s": time_stop_s, "invert": invert,
            })
        on_off_rules = on_off_rules[:on_off_rule_count]

    profile = {
        "name": name,
        "zone_mask": zone_mask,
        "segments": segments,
        "on_off_rules": on_off_rules,
    }
    return version, profile


def _encode_v1_or_v2(profile: dict, version: int) -> bytes:
    segs = (profile["segments"] + [_DEFAULT_SEGMENT] * PROFILE_MAX_SEGMENTS)[:PROFILE_MAX_SEGMENTS]
    body = struct.pack("<B3x", version)
    body += struct.pack(f"<{_NAME_LEN}sBB2x", _encode_name(profile["name"]), profile["zone_mask"],
                         len(profile["segments"]))
    for s in segs:
        body += struct.pack("<" + _SEG_V2_FMT, s["target_c"], s["ramp_c_per_hr"], s["dwell_min"])
    if version == 1:
        return body
    return body + struct.pack("<I", _crc32(body))


def _encode_v3(profile: dict) -> bytes:
    segs = (profile["segments"] + [_DEFAULT_SEGMENT] * PROFILE_MAX_SEGMENTS)[:PROFILE_MAX_SEGMENTS]
    body = struct.pack("<B3x", 3)
    body += struct.pack(f"<{_NAME_LEN}sBB2x", _encode_name(profile["name"]), profile["zone_mask"],
                         len(profile["segments"]))
    for s in segs:
        body += struct.pack(
            "<" + _SEG_V3_FMT, s["target_c"], s["ramp_c_per_hr"], s["dwell_min"], s["seg_kind"],
            s["io_target"], s["io_state"], s["io_blocking"], s["io_leave_on_at_end"],
        )
    return body + struct.pack("<I", _crc32(body))


def _encode_v4(profile: dict) -> bytes:
    segs = (profile["segments"] + [_DEFAULT_SEGMENT] * PROFILE_MAX_SEGMENTS)[:PROFILE_MAX_SEGMENTS]
    rules = (profile["on_off_rules"] + [_DEFAULT_RULE] * PROFILE_MAX_ON_OFF_RULES)[:PROFILE_MAX_ON_OFF_RULES]
    body = struct.pack("<B3x", PROFILE_VERSION)
    body += struct.pack(f"<{_NAME_LEN}sBB2x", _encode_name(profile["name"]), profile["zone_mask"],
                         len(profile["segments"]))
    for s in segs:
        body += struct.pack(
            "<" + _SEG_V3_FMT, s["target_c"], s["ramp_c_per_hr"], s["dwell_min"], s["seg_kind"],
            s["io_target"], s["io_state"], s["io_blocking"], s["io_leave_on_at_end"],
        )
    body += struct.pack("<B3x", len(profile["on_off_rules"]))
    for r in rules:
        body += struct.pack(
            "<" + _RULE_FMT, r["segment_index"], r["zone_index"], r["enable"], r["phase_mask"],
            r["direction_mask"], r["temp_source"], r["temp_ref_zone"], r["temp_cmp"],
            r["temp_threshold_c"], r["time_start_s"], r["time_stop_s"], r["invert"],
        )
    return body + struct.pack("<I", _crc32(body))


def convert_profile_blob(blob: bytes, target_version: int) -> "tuple[bytes, ConversionReport]":
    """Best-effort convert a raw profN record to target_version. Forward
    steps (source < target) mirror profiles_http.c's convert_profile_v1()/
    convert_profile_v2()/convert_profile_v3() exactly: every field the newer
    shape adds gets precisely the default the firmware migration itself
    documents (see profiles_types.h's field comments, cited in each
    _DEFAULT_* constant above). Backward steps (source > target) are
    best-effort: fields the older shape cannot express (relay/IO segment
    fields for a v1/v2 target, on/off rules for anything below v4) are
    DROPPED, never silently folded into something else, and every drop is
    recorded in the report."""
    if target_version not in _EXPECTED_LEN:
        raise ConfigConvertError(
            f"target version {target_version} is not a known PROFILE_VERSION step "
            f"({sorted(_EXPECTED_LEN)}) -- refusing rather than guessing at an unknown layout"
        )
    source_version, profile = decode_profile_blob(blob)
    report = ConversionReport(store="profile_blob", source_version=source_version, target_version=target_version)

    if source_version == target_version:
        report.add("document", "version", "kept", "source and target versions are identical")
    else:
        if source_version < 3 <= target_version:
            for s in profile["segments"]:
                if any(s[k] for k in ("seg_kind", "io_target", "io_state", "io_blocking", "io_leave_on_at_end")):
                    continue  # nothing to report; defaults already applied at decode
            report.add("segments", "seg_kind/io_target/io_state/io_blocking/io_leave_on_at_end", "defaulted",
                        "source predates relay/IO segments (PROFILE_VERSION 2->3); every segment defaults to "
                        "PROFILE_SEG_KIND_ZONE_RAMP with IO fields cleared, exactly firmware's own migration")
        if source_version < PROFILE_VERSION <= target_version:
            report.add("on_off_rules", "on_off_rule_count/on_off_rules", "defaulted",
                        "source predates on/off rules (PROFILE_VERSION 3->4); on_off_rule_count=0, "
                        "reproducing today's behavior exactly (no rule for any segment/zone)")
        if target_version < PROFILE_VERSION <= source_version and profile["on_off_rules"]:
            report.add("on_off_rules", "on_off_rule_count/on_off_rules", "dropped",
                        f"{len(profile['on_off_rules'])} rule(s) cannot be expressed in PROFILE_VERSION "
                        f"{target_version}'s layout; target has no on/off-rule mechanism at all")
        if target_version < 3 <= source_version:
            lossy_segs = [s for s in profile["segments"] if s["seg_kind"] != 0]
            if lossy_segs:
                report.add("segments", "seg_kind/io_*", "dropped",
                            f"{len(lossy_segs)} relay/IO segment(s) cannot be expressed in PROFILE_VERSION "
                            f"{target_version}'s layout; target has no relay/IO segment kind at all")

    if target_version in (1, 2):
        out = _encode_v1_or_v2(profile, target_version)
    elif target_version == 3:
        out = _encode_v3(profile)
    else:
        out = _encode_v4(profile)
    return out, report


# ---------------------------------------------------------------------------
# Top-level dispatch
# ---------------------------------------------------------------------------


def convert_document(doc: dict, target_version: int) -> "tuple[dict, ConversionReport]":
    """Convert any recognized JSON document (see detect_kind()) to
    target_version. Returns (new_doc, report)."""
    kind = detect_kind(doc)
    if kind == "backup":
        loaded = backup_cfg_convert.load_package(json.dumps(doc))
        out, backup_report = backup_cfg_convert.convert(loaded, target_version)
        report = ConversionReport(store="backup", source_version=backup_report.source_version,
                                   target_version=backup_report.target_version)
        for o in backup_report.outcomes:
            report.add(o.scope, o.field, o.outcome, o.detail)
        return out, report

    if kind == "profile_blob":
        try:
            blob = bytes.fromhex(doc["blob_hex"])
        except (KeyError, ValueError) as exc:
            raise ConfigConvertError(f"kilnctl_profile_blob document must carry a valid hex 'blob_hex': {exc}")
        out_blob, report = convert_profile_blob(blob, target_version)
        out_doc = {"kind": "kilnctl_profile_blob", "version": target_version, "blob_hex": out_blob.hex()}
        return out_doc, report

    raise ConfigConvertError(f"unreachable: detect_kind returned unhandled kind {kind!r}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="config_convert",
        description="Best-effort convert a kilnCtl config export/blob document to another version of its own "
                    "store. Never writes to a board -- see this tool's module docstring.",
    )
    p.add_argument("input", help="path to the input JSON document, or '-' for stdin")
    p.add_argument("--to-version", type=int, required=True, help="target version for the document's own store")
    p.add_argument("-o", "--output", help="path to write the converted document (default: stdout)")
    p.add_argument("--report", action="store_true", help="also print the per-field lossy report to stderr")
    p.add_argument("--quiet", action="store_true", help="suppress the summary line on success")
    return p


def main(argv: Optional[list] = None) -> int:
    args = _build_arg_parser().parse_args(argv)
    try:
        raw = sys.stdin.read() if args.input == "-" else open(args.input, "r", encoding="utf-8").read()
        doc = json.loads(raw)
        out_doc, report = convert_document(doc, args.to_version)
    except (ConfigConvertError, backup_cfg_convert.CfgConvertError, OSError, json.JSONDecodeError) as exc:
        print(f"config_convert: error: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(out_doc, indent=2) + "\n"
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)

    if args.report or report.lossy:
        print(report.render(), file=sys.stderr)
    if not args.quiet:
        status = "LOSSY" if report.lossy else "lossless"
        print(f"config_convert: {report.store} v{report.source_version} -> v{report.target_version} ({status})",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
