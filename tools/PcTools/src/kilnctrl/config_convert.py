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

  - "safety_config_blob" (kind "kilnctl_safety_config_blob"): (added
    2026-09-23) the raw flash-sector record for SaftyFW's
    `config_store_record_t` (firmware/SaftyFW/src/config_store.c/.h,
    CONFIG_STORE_FORMAT_VERSION) -- a fixed-size slot in the RP2040's own
    flash, not an NVS key/value entry (NVS is ESP-IDF-specific; SaftyFW has
    no NVS). Unlike that struct's in-RAM compiler layout (never touched
    here), the WIRE format firmware actually reads/writes is fully
    deterministic and already hand-explicit in C: `config_store_pack()`/
    `unpack_v2_fields()` place every field at a named `REC_OFF_*` byte
    offset with `put_u16_le()`/`put_u32_le()`/`put_f32_le()` (never a raw
    struct memcpy), and the tail is `bootloader_crc32()` -- confirmed
    equivalent to `zlib.crc32()` by reading its implementation plus
    firmware's own host-test vector (`bootloader_crc32("123456789") ==
    0xCBF43926`, the standard CRC-32/ISO-HDLC check value). This is a
    stronger claim than the ESP-side `esp_crc32_le()` used by
    profile_blob/backup below, which remains an unverified stand-in --
    never checked against a real captured hardware blob for either. This
    module mirrors `config_store_pack()`/
    `config_store_unpack_ex()` byte-for-byte for versions 1, 2 and current
    (3), including the v1->v3 and v2->v3 forward migrations
    (`config_store_default()`'s compiled defaults, and
    `config_store_derive_zone_ct_channel()`'s ct_topology ->
    zone_ct_channel[] backfill). Wraps the raw bytes the same way
    profile_blob does: {"kind": "kilnctl_safety_config_blob", "version": N,
    "blob_hex": "<hex>"}. Encoding is supported ONLY to
    CONFIG_STORE_FORMAT_VERSION (today 3): firmware has no v1/v2 *pack* path
    any more (only the *unpack*-side forward migrations above), so writing a
    v1- or v2-shaped record would invent a wire format nothing in firmware
    ever produces or reads, not convert one that exists --
    convert_safety_config_blob() refuses a target below the current format
    version by name.

ZONES_CFG_T / KILN_PACKAGE (landed 2026-09-24, see the "zones_blob store" and
"kiln_package store" section comments below for the full derivation):
  - "zones_blob" (kind "kilnctl_zones_blob"): the ESP's raw zones_cfg_t
    NVS/blob record (firmware/KilnFW/App/drivers/persist/
    zones_config_json.h, ZONES_CFG_VERSION, currently 26). STAGE 1:
    decodes/encodes the CURRENT version byte-exactly (offsets cross-checked
    against zone_cfg_v25_t's real `_Static_assert`s, and pinned field by
    field by a firmware-generated golden: KilnFW's host test
    test_zones_blob_golden.c writes a sentinel-filled zones_cfg_t through the
    real nvs_save() into tests/fixtures/config_convert/zones_cfg_golden.txt,
    and tests/test_config_convert_zones_golden.py decodes it). STAGE 2 (2026-10-05): v21..v25 also decode
    (CRC-verified) and upgrade to the current layout, because each hop
    v21->v26 is a pure tail-append prefix (zones_config_migrate.c cases
    21..25); verified against the firmware v26 golden truncated to each
    older shape, plus a mirror test of the frozen `_Static_assert` sizes.
    v20 and older still refuse by name: v20->v21 grew settings_source
    MID-struct and no blob of those shapes is available to verify against.
  - "kiln_package" (kind "kilnctl_kiln_package" -- KILN_PKG_KIND;
    previously misnamed "kilnctl_kiln_cfg_package" in this docstring, which
    never matched the real macro): kiln_cfg_store's "kilnpkg.json" envelope.
    STAGE 3, built on zones_blob support immediately above: its
    `esp_blob_hex` field is a raw zones_cfg_t blob, decoded/re-encoded with
    the same functions, and `pkg_hash` is recomputed the same way
    `kiln_package_compute_hash()` does. `pico` and `source_board_id` pass
    through unchanged. Inherits zones_blob's range: a package whose
    esp_blob_hex is v21..current converts; older refuses with the zones_blob
    message.

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

CRC RANGE. firmware's compute_profile_crc() (profiles_http.c:487-492, and the
v2/v3 checks near :583/:597) computes the CRC over the WHOLE persisted
struct -- body bytes AND the crc32 field itself, zeroed to 4 zero bytes --
never over the body alone. This module reproduces that exactly: every
encoder appends `_crc32(body + b"\\x00\\x00\\x00\\x00")`, and
decode_profile_blob() checks an incoming v2+ blob's stored CRC the same way
before trusting it.
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
# message rather than a generic one. Empty as of 2026-09-24: every kind this
# module used to refuse outright (kilnctl_zones_blob, kilnctl_kiln_package)
# is now at least partially supported -- a document at an unsupported
# *version* within a recognized kind still refuses, just later, from
# convert_zones_blob()/convert_document(), with a version-specific message.
KNOWN_UNSUPPORTED_KINDS = {}


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
    if kind == "kilnctl_safety_config_blob":
        return "safety_config_blob"
    if kind == "kilnctl_zones_blob":
        return "zones_blob"
    if kind == "kilnctl_kiln_package":
        return "kiln_package"
    if kind in KNOWN_UNSUPPORTED_KINDS:
        raise ConfigConvertError(KNOWN_UNSUPPORTED_KINDS[kind])
    raise ConfigConvertError(
        f"could not identify a known config store from this document (kind={kind!r}). "
        "Known kinds: kilnctl_backup, kilnctl_profile_blob, kilnctl_safety_config_blob, "
        "kilnctl_zones_blob, kilnctl_kiln_package. "
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
    expected_len_for_version() in profiles_http.c). v2+ blobs also get their
    stored crc32 checked, over the same range firmware's compute_profile_crc()
    uses (body + the crc32 field itself zeroed) -- v1 predates the crc32 tail
    and has no check here, mirroring profiles_http.c's version==1 branch."""
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

    if version != 1:
        stored_crc = int.from_bytes(blob[-4:], "little")
        computed_crc = _crc32(blob[:-4] + b"\x00\x00\x00\x00")
        if stored_crc != computed_crc:
            raise ConfigConvertError(
                f"CRC mismatch for profile version {version} blob "
                f"(stored 0x{stored_crc:08x}, computed 0x{computed_crc:08x}) -- treating as corrupt, "
                "same as profiles_http.c's own load path"
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
    if segment_count > PROFILE_MAX_SEGMENTS:
        raise ConfigConvertError(
            f"segment_count {segment_count} exceeds PROFILE_MAX_SEGMENTS ({PROFILE_MAX_SEGMENTS}) -- "
            "treating as corrupt rather than silently clamping"
        )

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
    return body + struct.pack("<I", _crc32(body + b"\x00\x00\x00\x00"))


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
    return body + struct.pack("<I", _crc32(body + b"\x00\x00\x00\x00"))


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
    return body + struct.pack("<I", _crc32(body + b"\x00\x00\x00\x00"))


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
# safety_config_blob store -- mirrors firmware/SaftyFW/src/config_store.c's
# REC_OFF_* byte layout, config_store_pack()/unpack_v2_fields()/
# config_store_unpack_ex()/config_store_default() exactly. All offsets below
# are transcribed verbatim from config_store.c's own #define table (the
# comment block above it, and the constants themselves) -- see
# check_config_convert_mirror.py for the drift check that keeps
# SAFETY_CONFIG_STORE_FORMAT_VERSION in step with firmware's
# CONFIG_STORE_FORMAT_VERSION.
#
# Deliberately NOT replicated here: config_params_validate_ranges(), the
# load-time re-check config_store_unpack_ex() runs on every branch. That is a
# SAFETY re-validation (are these values sane to arm guards with), not a
# FORMAT concern (are these bytes this store's shape) -- this module's job
# stops at the latter, same as it never re-validates a profile_blob's
# segment temperatures. A decoded document that would fail firmware's own
# range check is not flagged by this tool.
# ---------------------------------------------------------------------------

SAFETY_CONFIG_STORE_FORMAT_VERSION = 3  # config_store.h CONFIG_STORE_FORMAT_VERSION
SAFETY_CONFIG_STORE_FORMAT_VERSION_V2 = 2
SAFETY_CONFIG_STORE_FORMAT_VERSION_V1 = 1
_SC_MAGIC = 0x4B4C4331  # CONFIG_STORE_MAGIC
_SC_RECORD_LEN = 512  # CONFIG_STORE_RECORD_LEN

_SC_DEFAULT_TC_TYPE = 0x03  # CONFIG_STORE_DEFAULT_TC_TYPE (K)
_SC_TC_TYPE_MAX_REAL = 0x07  # CONFIG_STORE_TC_TYPE_MAX_REAL
_SC_CT_TOPOLOGY_PER_ZONE = 0
_SC_CT_TOPOLOGY_SUMMED = 1
_SC_SAFETY_TC_INSTALLED_MARKER_INSTALLED = 0x01
_SC_SAFETY_TC_INSTALLED_MARKER_NOT_INSTALLED = 0xA5
_SC_CT_INSTALLED_MARKER_INSTALLED = 0x01
_SC_CT_INSTALLED_MARKER_NOT_INSTALLED = 0xA5
_SC_ESTOP_ACTIVE_HIGH = 0  # DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH
_SC_ESTOP_ACTIVE_LOW = 1  # DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW

# v3 (current) REC_OFF_* -- config_store.c's own layout-table comment.
_SC = {
    "MAGIC": 0, "FORMAT_VERSION": 4, "SEQ": 8, "FIELDS_SET": 12,
    "TC_SOURCE": 16, "BORROWED_ZONE_INDEX": 17, "TC_PLACEMENT_MODE": 18,
    "ABS_MAX_TEMP_C": 19, "TC_TYPE": 23, "CT_CHANNEL_MAP": 24,
    "CALIBRATION_MISSING": 27, "FIRING_MARGIN_C": 28, "OVERSHOOT_MARGIN_C": 32,
    "OVERSHOOT_TIME_S": 36, "MAX_RATE_C_PER_MIN": 40, "RATE_WINDOW_S": 44,
    "BLIND_GRACE_S": 48, "FROZEN_WINDOW_S": 52, "TC_DISAGREEMENT_C": 56,
    "TC_DISAGREEMENT_TIME_S": 60, "TC_EXPECTED_OFFSET_C": 64, "CJ_WARN_C": 68,
    "CJ_MAX_C": 72, "CJ_TIME_S": 76, "BORROWED_STALE_S": 80,
    "BORROWED_STALE_TRIP_S": 84, "BORROWED_TYPE_EXPECTED": 88,
    "I_PRESENT_A": 89, "ZERO_COUNTS": 93, "CORRELATION_WINDOW_S": 99,
    "STUCK_ON_TIME_S": 103, "TRIP_VERIFY_S": 107, "K_CT_V_PER_A": 111,
    "GAIN": 123, "MAINS_VOLTAGE_V": 135, "POWER_WINDOW_S": 139,
    "CONTEXT_MAX_AGE_S": 143, "LINK_TIMEOUT_S": 147, "LINK_DEAD_HARD_S": 151,
    "MAINFAULT_DEBOUNCE_MS": 155, "TELEMETRY_PERIOD_MS": 159,
    "STARTUP_GRACE_S": 163, "ESTOP_DEBOUNCE_MS": 167, "WATCHDOG_TIMEOUT_MS": 171,
    "CONFIG_CHECK_PERIOD_S": 175, "CT_CAL": 179, "SAFETY_TC_INSTALLED": 206,
    "MAX_EXPECTED_POWER_W": 207, "I_NORMAL_A": 211, "OVERCURRENT_PCT": 223,
    "OVERCURRENT_TIME_S": 225, "CT_INSTALLED": 229, "CT_TOPOLOGY": 230,
    "I_PRESENT_A_MANUAL": 231, "TC_OFFSET_C": 232, "ESTOP_ACTIVE_LEVEL": 236,
    "ZONE_CT_CHANNEL": 237, "RESERVED": 240, "CRC": 504,
}
_SC_CT_CAL_CHANNEL_LEN = 9  # calibrated u8(1) + gain f32(4) + offset f32(4)
_SC_RESERVED_LEN = 264

# v2 (legacy) -- frozen, only used to migrate a v2 record into v3 shape.
_SC_V2_OFF_FIELDS_SET = 12
_SC_V2_OFF_TC_SOURCE = 14
_SC_V2_OFF_RESERVED = 235
_SC_V2_RESERVED_LEN = 269
_SC_V2_OFF_CRC = 504

# v1 (legacy) -- frozen, only used to migrate a v1 record forward.
_SC_V1_OFF_MAGIC = 0
_SC_V1_OFF_FORMAT_VERSION = 4
_SC_V1_OFF_SEQ = 8
_SC_V1_OFF_TC_TYPE = 12
_SC_V1_OFF_CALIBRATION_MISSING = 13
_SC_V1_OFF_CT_CAL = 16
_SC_V1_CT_CAL_CHANNEL_LEN = 9
_SC_V1_OFF_CRC = 248


def _sc_crc32(data: bytes) -> int:
    """bootloader_crc32() -- confirmed standard CRC-32/ISO-HDLC via
    firmware's own host-test vector (bootloader_crc32("123456789") ==
    0xCBF43926), i.e. exactly zlib.crc32()."""
    return zlib.crc32(data) & 0xFFFFFFFF


def _sc_pack_ct_cal(channels: "list[dict]") -> bytes:
    out = bytearray(_SC_CT_CAL_CHANNEL_LEN * 3)
    for ch, c in enumerate(channels):
        base = ch * _SC_CT_CAL_CHANNEL_LEN
        out[base] = 1 if c["calibrated"] else 0
        struct.pack_into("<f", out, base + 1, c["gain"])
        struct.pack_into("<f", out, base + 5, c["offset"])
    return bytes(out)


def _sc_unpack_ct_cal(buf: bytes, base_off: int, channel_len: int) -> "list[dict]":
    out = []
    for ch in range(3):
        base = base_off + ch * channel_len
        out.append({
            "calibrated": buf[base] == 1,  # only wire byte 1 means calibrated
            "gain": struct.unpack_from("<f", buf, base + 1)[0],
            "offset": struct.unpack_from("<f", buf, base + 5)[0],
        })
    return out


def safety_config_default_fields() -> dict:
    """config_store_default() -- the exact compiled defaults, used as the v2
    baseline a migrated v1 record is overlaid onto."""
    return {
        "format_version": SAFETY_CONFIG_STORE_FORMAT_VERSION, "seq": 0, "fields_set": 0,
        "tc_source": 0, "borrowed_zone_index": 0, "tc_placement_mode": 0, "abs_max_temp_c": 0.0,
        "tc_type": _SC_DEFAULT_TC_TYPE, "ct_channel_map": [0xFF, 0xFF, 0xFF],
        "calibration_missing": True, "firing_margin_c": 100.0, "overshoot_margin_c": 75.0,
        "overshoot_time_s": 120, "max_rate_c_per_min": 33.3, "rate_window_s": 60,
        "blind_grace_s": 60, "frozen_window_s": 600, "tc_disagreement_c": 200.0,
        "tc_disagreement_time_s": 300, "tc_expected_offset_c": 0.0, "cj_warn_c": 60.0,
        "cj_max_c": 85.0, "cj_time_s": 60, "borrowed_stale_s": 10, "borrowed_stale_trip_s": 60,
        "borrowed_type_expected": _SC_DEFAULT_TC_TYPE, "i_present_a": 2.0,
        "zero_counts": [0, 0, 0], "correlation_window_s": 150, "stuck_on_time_s": 20,
        "trip_verify_s": 10, "k_ct_v_per_a": [0.0, 0.0, 0.0], "gain": [0.715, 0.715, 0.715],
        "mains_voltage_v": 0.0, "power_window_s": 120, "context_max_age_s": 5,
        "link_timeout_s": 10, "link_dead_hard_s": 120, "mainfault_debounce_ms": 200,
        "telemetry_period_ms": 500, "startup_grace_s": 60, "estop_debounce_ms": 50,
        "watchdog_timeout_ms": 1000, "config_check_period_s": 10,
        "ct_cal": [{"calibrated": False, "gain": 0.0, "offset": 0.0} for _ in range(3)],
        "safety_tc_installed": True, "ct_installed": True, "max_expected_power_w": 0.0,
        "i_normal_a": [0.0, 0.0, 0.0], "overcurrent_pct": 0, "overcurrent_time_s": 0,
        "ct_topology": _SC_CT_TOPOLOGY_PER_ZONE, "i_present_a_manual": False,
        "tc_offset_c": 0.0, "estop_active_level": _SC_ESTOP_ACTIVE_HIGH,
        "zone_ct_channel": [0, 1, 2], "reserved_hex": "00" * _SC_RESERVED_LEN,
    }


def _sc_derive_zone_ct_channel(ct_topology: int) -> "list[int]":
    """config_store_derive_zone_ct_channel()."""
    if ct_topology == _SC_CT_TOPOLOGY_PER_ZONE:
        return [0, 1, 2]
    return [2, 2, 2]


def _sc_unpack_v3_fields(buf: bytes) -> dict:
    """unpack_v2_fields() -- named for the wire shape (v3), not the function
    it mirrors; matches firmware's own naming, which kept its original name
    across the v2->v3 field-shift."""
    g_u16 = lambda off: struct.unpack_from("<H", buf, off)[0]
    g_u32 = lambda off: struct.unpack_from("<I", buf, off)[0]
    g_f32 = lambda off: struct.unpack_from("<f", buf, off)[0]

    tc_type_byte = buf[_SC["TC_TYPE"]]
    out = {
        "format_version": g_u16(_SC["FORMAT_VERSION"]),
        "seq": g_u32(_SC["SEQ"]),
        "fields_set": g_u32(_SC["FIELDS_SET"]),
        "tc_source": buf[_SC["TC_SOURCE"]],
        "borrowed_zone_index": buf[_SC["BORROWED_ZONE_INDEX"]],
        "tc_placement_mode": buf[_SC["TC_PLACEMENT_MODE"]],
        "abs_max_temp_c": g_f32(_SC["ABS_MAX_TEMP_C"]),
        "tc_type": tc_type_byte if tc_type_byte <= _SC_TC_TYPE_MAX_REAL else _SC_DEFAULT_TC_TYPE,
        "ct_channel_map": list(buf[_SC["CT_CHANNEL_MAP"]:_SC["CT_CHANNEL_MAP"] + 3]),
        "calibration_missing": buf[_SC["CALIBRATION_MISSING"]] != 0,
        "firing_margin_c": g_f32(_SC["FIRING_MARGIN_C"]),
        "overshoot_margin_c": g_f32(_SC["OVERSHOOT_MARGIN_C"]),
        "overshoot_time_s": g_u32(_SC["OVERSHOOT_TIME_S"]),
        "max_rate_c_per_min": g_f32(_SC["MAX_RATE_C_PER_MIN"]),
        "rate_window_s": g_u32(_SC["RATE_WINDOW_S"]),
        "blind_grace_s": g_u32(_SC["BLIND_GRACE_S"]),
        "frozen_window_s": g_u32(_SC["FROZEN_WINDOW_S"]),
        "tc_disagreement_c": g_f32(_SC["TC_DISAGREEMENT_C"]),
        "tc_disagreement_time_s": g_u32(_SC["TC_DISAGREEMENT_TIME_S"]),
        "tc_expected_offset_c": g_f32(_SC["TC_EXPECTED_OFFSET_C"]),
        "cj_warn_c": g_f32(_SC["CJ_WARN_C"]),
        "cj_max_c": g_f32(_SC["CJ_MAX_C"]),
        "cj_time_s": g_u32(_SC["CJ_TIME_S"]),
        "borrowed_stale_s": g_u32(_SC["BORROWED_STALE_S"]),
        "borrowed_stale_trip_s": g_u32(_SC["BORROWED_STALE_TRIP_S"]),
        "borrowed_type_expected": buf[_SC["BORROWED_TYPE_EXPECTED"]],
        "i_present_a": g_f32(_SC["I_PRESENT_A"]),
        "zero_counts": [g_u16(_SC["ZERO_COUNTS"] + i * 2) for i in range(3)],
        "correlation_window_s": g_u32(_SC["CORRELATION_WINDOW_S"]),
        "stuck_on_time_s": g_u32(_SC["STUCK_ON_TIME_S"]),
        "trip_verify_s": g_u32(_SC["TRIP_VERIFY_S"]),
        "k_ct_v_per_a": [g_f32(_SC["K_CT_V_PER_A"] + i * 4) for i in range(3)],
        "gain": [g_f32(_SC["GAIN"] + i * 4) for i in range(3)],
        "mains_voltage_v": g_f32(_SC["MAINS_VOLTAGE_V"]),
        "power_window_s": g_u32(_SC["POWER_WINDOW_S"]),
        "context_max_age_s": g_u32(_SC["CONTEXT_MAX_AGE_S"]),
        "link_timeout_s": g_u32(_SC["LINK_TIMEOUT_S"]),
        "link_dead_hard_s": g_u32(_SC["LINK_DEAD_HARD_S"]),
        "mainfault_debounce_ms": g_u32(_SC["MAINFAULT_DEBOUNCE_MS"]),
        "telemetry_period_ms": g_u32(_SC["TELEMETRY_PERIOD_MS"]),
        "startup_grace_s": g_u32(_SC["STARTUP_GRACE_S"]),
        "estop_debounce_ms": g_u32(_SC["ESTOP_DEBOUNCE_MS"]),
        "watchdog_timeout_ms": g_u32(_SC["WATCHDOG_TIMEOUT_MS"]),
        "config_check_period_s": g_u32(_SC["CONFIG_CHECK_PERIOD_S"]),
        "ct_cal": _sc_unpack_ct_cal(buf, _SC["CT_CAL"], _SC_CT_CAL_CHANNEL_LEN),
        # Only the explicit sentinel means "not installed" -- every other
        # byte (0x00 legacy, 0xFF erased flash, 0x01 this build's marker)
        # decodes as installed, the safe direction. See config_store.c.
        "safety_tc_installed": buf[_SC["SAFETY_TC_INSTALLED"]] != _SC_SAFETY_TC_INSTALLED_MARKER_NOT_INSTALLED,
        "ct_installed": buf[_SC["CT_INSTALLED"]] != _SC_CT_INSTALLED_MARKER_NOT_INSTALLED,
        "ct_topology": _SC_CT_TOPOLOGY_SUMMED if buf[_SC["CT_TOPOLOGY"]] == 1 else _SC_CT_TOPOLOGY_PER_ZONE,
        "i_present_a_manual": buf[_SC["I_PRESENT_A_MANUAL"]] == 1,
        "tc_offset_c": g_f32(_SC["TC_OFFSET_C"]),
        "estop_active_level": _SC_ESTOP_ACTIVE_LOW if buf[_SC["ESTOP_ACTIVE_LEVEL"]] == 1 else _SC_ESTOP_ACTIVE_HIGH,
        "max_expected_power_w": g_f32(_SC["MAX_EXPECTED_POWER_W"]),
        "i_normal_a": [g_f32(_SC["I_NORMAL_A"] + i * 4) for i in range(3)],
        "reserved_hex": buf[_SC["RESERVED"]:_SC["RESERVED"] + _SC_RESERVED_LEN].hex(),
    }
    oc_pct_raw = g_u16(_SC["OVERCURRENT_PCT"])
    out["overcurrent_pct"] = 0 if oc_pct_raw == 0xFFFF else oc_pct_raw
    oc_time_raw = g_u32(_SC["OVERCURRENT_TIME_S"])
    out["overcurrent_time_s"] = 0 if oc_time_raw == 0xFFFFFFFF else oc_time_raw

    zone_ct_channel = list(buf[_SC["ZONE_CT_CHANNEL"]:_SC["ZONE_CT_CHANNEL"] + 3])
    if any(z > 2 for z in zone_ct_channel):
        zone_ct_channel = _sc_derive_zone_ct_channel(out["ct_topology"])
        # CONFIG_STORE_SET_ZONE_CT_CHANNEL{,_0,_1,_2} = bits 16-19
        # (config_store.h) -- clear all four, same as config_store_unpack_ex()'s
        # own out-of-range fallback.
        out["fields_set"] &= ~0x000F0000
    out["zone_ct_channel"] = zone_ct_channel
    return out


def encode_safety_config_v3(f: dict) -> bytes:
    """config_store_pack() -- always encodes CURRENT-format-version bytes.
    There is no firmware pack path for v1/v2 any more (only the forward
    unpack-side migrations config_store_unpack_ex() implements), so this
    module does not invent one -- see convert_safety_config_blob()."""
    out = bytearray(b"\xff" * _SC_RECORD_LEN)
    struct.pack_into("<I", out, _SC["MAGIC"], _SC_MAGIC)
    struct.pack_into("<H", out, _SC["FORMAT_VERSION"], SAFETY_CONFIG_STORE_FORMAT_VERSION)
    struct.pack_into("<H", out, 6, 0)  # reserved0
    struct.pack_into("<I", out, _SC["SEQ"], f["seq"])
    struct.pack_into("<I", out, _SC["FIELDS_SET"], f["fields_set"])
    out[_SC["TC_SOURCE"]] = f["tc_source"]
    out[_SC["BORROWED_ZONE_INDEX"]] = f["borrowed_zone_index"]
    out[_SC["TC_PLACEMENT_MODE"]] = f["tc_placement_mode"]
    struct.pack_into("<f", out, _SC["ABS_MAX_TEMP_C"], f["abs_max_temp_c"])
    out[_SC["TC_TYPE"]] = f["tc_type"]
    if len(f["ct_channel_map"]) != 3:
        raise ValueError(f"ct_channel_map must have exactly 3 entries, got {len(f['ct_channel_map'])}")
    out[_SC["CT_CHANNEL_MAP"]:_SC["CT_CHANNEL_MAP"] + 3] = bytes(f["ct_channel_map"])
    out[_SC["CALIBRATION_MISSING"]] = 1 if f["calibration_missing"] else 0
    struct.pack_into("<f", out, _SC["FIRING_MARGIN_C"], f["firing_margin_c"])
    struct.pack_into("<f", out, _SC["OVERSHOOT_MARGIN_C"], f["overshoot_margin_c"])
    struct.pack_into("<I", out, _SC["OVERSHOOT_TIME_S"], f["overshoot_time_s"])
    struct.pack_into("<f", out, _SC["MAX_RATE_C_PER_MIN"], f["max_rate_c_per_min"])
    struct.pack_into("<I", out, _SC["RATE_WINDOW_S"], f["rate_window_s"])
    struct.pack_into("<I", out, _SC["BLIND_GRACE_S"], f["blind_grace_s"])
    struct.pack_into("<I", out, _SC["FROZEN_WINDOW_S"], f["frozen_window_s"])
    struct.pack_into("<f", out, _SC["TC_DISAGREEMENT_C"], f["tc_disagreement_c"])
    struct.pack_into("<I", out, _SC["TC_DISAGREEMENT_TIME_S"], f["tc_disagreement_time_s"])
    struct.pack_into("<f", out, _SC["TC_EXPECTED_OFFSET_C"], f["tc_expected_offset_c"])
    struct.pack_into("<f", out, _SC["CJ_WARN_C"], f["cj_warn_c"])
    struct.pack_into("<f", out, _SC["CJ_MAX_C"], f["cj_max_c"])
    struct.pack_into("<I", out, _SC["CJ_TIME_S"], f["cj_time_s"])
    struct.pack_into("<I", out, _SC["BORROWED_STALE_S"], f["borrowed_stale_s"])
    struct.pack_into("<I", out, _SC["BORROWED_STALE_TRIP_S"], f["borrowed_stale_trip_s"])
    out[_SC["BORROWED_TYPE_EXPECTED"]] = f["borrowed_type_expected"]
    struct.pack_into("<f", out, _SC["I_PRESENT_A"], f["i_present_a"])
    for i in range(3):
        struct.pack_into("<H", out, _SC["ZERO_COUNTS"] + i * 2, f["zero_counts"][i])
    struct.pack_into("<I", out, _SC["CORRELATION_WINDOW_S"], f["correlation_window_s"])
    struct.pack_into("<I", out, _SC["STUCK_ON_TIME_S"], f["stuck_on_time_s"])
    struct.pack_into("<I", out, _SC["TRIP_VERIFY_S"], f["trip_verify_s"])
    for i in range(3):
        struct.pack_into("<f", out, _SC["K_CT_V_PER_A"] + i * 4, f["k_ct_v_per_a"][i])
    for i in range(3):
        struct.pack_into("<f", out, _SC["GAIN"] + i * 4, f["gain"][i])
    struct.pack_into("<f", out, _SC["MAINS_VOLTAGE_V"], f["mains_voltage_v"])
    struct.pack_into("<I", out, _SC["POWER_WINDOW_S"], f["power_window_s"])
    struct.pack_into("<I", out, _SC["CONTEXT_MAX_AGE_S"], f["context_max_age_s"])
    struct.pack_into("<I", out, _SC["LINK_TIMEOUT_S"], f["link_timeout_s"])
    struct.pack_into("<I", out, _SC["LINK_DEAD_HARD_S"], f["link_dead_hard_s"])
    struct.pack_into("<I", out, _SC["MAINFAULT_DEBOUNCE_MS"], f["mainfault_debounce_ms"])
    struct.pack_into("<I", out, _SC["TELEMETRY_PERIOD_MS"], f["telemetry_period_ms"])
    struct.pack_into("<I", out, _SC["STARTUP_GRACE_S"], f["startup_grace_s"])
    struct.pack_into("<I", out, _SC["ESTOP_DEBOUNCE_MS"], f["estop_debounce_ms"])
    struct.pack_into("<I", out, _SC["WATCHDOG_TIMEOUT_MS"], f["watchdog_timeout_ms"])
    struct.pack_into("<I", out, _SC["CONFIG_CHECK_PERIOD_S"], f["config_check_period_s"])
    out[_SC["CT_CAL"]:_SC["CT_CAL"] + _SC_CT_CAL_CHANNEL_LEN * 3] = _sc_pack_ct_cal(f["ct_cal"])
    out[_SC["SAFETY_TC_INSTALLED"]] = (
        _SC_SAFETY_TC_INSTALLED_MARKER_INSTALLED if f["safety_tc_installed"]
        else _SC_SAFETY_TC_INSTALLED_MARKER_NOT_INSTALLED
    )
    out[_SC["CT_INSTALLED"]] = (
        _SC_CT_INSTALLED_MARKER_INSTALLED if f["ct_installed"] else _SC_CT_INSTALLED_MARKER_NOT_INSTALLED
    )
    out[_SC["CT_TOPOLOGY"]] = 1 if f["ct_topology"] == _SC_CT_TOPOLOGY_SUMMED else 0
    out[_SC["I_PRESENT_A_MANUAL"]] = 1 if f["i_present_a_manual"] else 0
    struct.pack_into("<f", out, _SC["TC_OFFSET_C"], f["tc_offset_c"])
    out[_SC["ESTOP_ACTIVE_LEVEL"]] = 1 if f["estop_active_level"] == _SC_ESTOP_ACTIVE_LOW else 0
    struct.pack_into("<f", out, _SC["MAX_EXPECTED_POWER_W"], f["max_expected_power_w"])
    for i in range(3):
        struct.pack_into("<f", out, _SC["I_NORMAL_A"] + i * 4, f["i_normal_a"][i])
    struct.pack_into("<H", out, _SC["OVERCURRENT_PCT"], f["overcurrent_pct"])
    struct.pack_into("<I", out, _SC["OVERCURRENT_TIME_S"], f["overcurrent_time_s"])
    if len(f["zone_ct_channel"]) != 3:
        raise ValueError(f"zone_ct_channel must have exactly 3 entries, got {len(f['zone_ct_channel'])}")
    out[_SC["ZONE_CT_CHANNEL"]:_SC["ZONE_CT_CHANNEL"] + 3] = bytes(f["zone_ct_channel"])
    reserved = bytes.fromhex(f.get("reserved_hex", "00" * _SC_RESERVED_LEN))
    if len(reserved) != _SC_RESERVED_LEN:
        raise ValueError(f"reserved_hex must decode to exactly {_SC_RESERVED_LEN} bytes, got {len(reserved)}")
    out[_SC["RESERVED"]:_SC["RESERVED"] + _SC_RESERVED_LEN] = reserved
    crc = _sc_crc32(bytes(out[:_SC["CRC"]]))
    struct.pack_into("<I", out, _SC["CRC"], crc)
    return bytes(out)


def decode_safety_config_blob(blob: bytes) -> "tuple[int, dict]":
    """config_store_unpack_ex() -- decode a raw config_store_record_t
    flash-sector record (any of format versions 1, 2 or current/3) into (version,
    fields_dict). v1 and v2 records are migrated FORWARD into v3 shape
    exactly as firmware does (config_store_default() baseline + overlay for
    v1; a byte-shift then the one v3 decoder for v2), but `version` in the
    return value still names the SOURCE format actually found, the same
    convention decode_profile_blob() uses. Never runs
    config_params_validate_ranges() -- see this section's header comment."""
    if len(blob) != _SC_RECORD_LEN:
        raise ConfigConvertError(
            f"safety_config blob length {len(blob)} does not match CONFIG_STORE_RECORD_LEN "
            f"({_SC_RECORD_LEN}) -- firmware treats anything else as corrupt, not a record to migrate"
        )
    magic = struct.unpack_from("<I", blob, _SC["MAGIC"])[0]
    if magic != _SC_MAGIC:
        raise ConfigConvertError(
            f"safety_config blob has wrong magic (0x{magic:08x}, expected 0x{_SC_MAGIC:08x}) -- "
            "erased flash or not this format at all"
        )
    version = struct.unpack_from("<H", blob, _SC["FORMAT_VERSION"])[0]

    if version == SAFETY_CONFIG_STORE_FORMAT_VERSION:
        stored_crc = struct.unpack_from("<I", blob, _SC["CRC"])[0]
        computed_crc = _sc_crc32(blob[:_SC["CRC"]])
        if stored_crc != computed_crc:
            raise ConfigConvertError(
                f"CRC mismatch for safety_config v{version} blob "
                f"(stored 0x{stored_crc:08x}, computed 0x{computed_crc:08x}) -- treating as corrupt, "
                "same as config_store_unpack_ex()'s own load path"
            )
        return version, _sc_unpack_v3_fields(blob)

    if version == SAFETY_CONFIG_STORE_FORMAT_VERSION_V2:
        stored_crc = struct.unpack_from("<I", blob, _SC_V2_OFF_CRC)[0]
        computed_crc = _sc_crc32(blob[:_SC_V2_OFF_CRC])
        if stored_crc != computed_crc:
            raise ConfigConvertError(
                f"CRC mismatch for safety_config v{version} blob "
                f"(stored 0x{stored_crc:08x}, computed 0x{computed_crc:08x}) -- treating as corrupt"
            )
        v3 = bytearray(b"\xff" * _SC_RECORD_LEN)
        v3[0:_SC_V2_OFF_FIELDS_SET] = blob[0:_SC_V2_OFF_FIELDS_SET]  # magic, format_version, reserved0, seq
        old_fields_set = struct.unpack_from("<H", blob, _SC_V2_OFF_FIELDS_SET)[0]
        struct.pack_into("<I", v3, _SC["FIELDS_SET"], old_fields_set)  # zero-extended; bit 16+ clear
        shift_len = _SC_V2_OFF_RESERVED - _SC_V2_OFF_TC_SOURCE
        v3[_SC["TC_SOURCE"]:_SC["TC_SOURCE"] + shift_len] = blob[_SC_V2_OFF_TC_SOURCE:_SC_V2_OFF_RESERVED]
        v3[_SC["RESERVED"]:_SC["RESERVED"] + _SC_RESERVED_LEN] = (
            blob[_SC_V2_OFF_RESERVED:_SC_V2_OFF_RESERVED + _SC_RESERVED_LEN]
        )
        fields = _sc_unpack_v3_fields(bytes(v3))
        # zone_ct_channel: derived from the migrated ct_topology, same as
        # config_store_derive_zone_ct_channel() in the v2 unpack branch --
        # the raw bytes at that offset are meaningless on a real v2 record
        # (reserved fill), and the gating bit is already clear.
        fields["zone_ct_channel"] = _sc_derive_zone_ct_channel(fields["ct_topology"])
        fields["format_version"] = SAFETY_CONFIG_STORE_FORMAT_VERSION
        # Deliberately NOT forcing calibration_missing -- see
        # config_store_unpack_ex()'s v2 branch comment: a v2 record was
        # commissioned against every field except zone_ct_channel, which has
        # a fully-specified derivation, so nothing is genuinely unknown.
        return version, fields

    if version == SAFETY_CONFIG_STORE_FORMAT_VERSION_V1:
        stored_crc = struct.unpack_from("<I", blob, _SC_V1_OFF_CRC)[0]
        computed_crc = _sc_crc32(blob[:_SC_V1_OFF_CRC])
        if stored_crc != computed_crc:
            raise ConfigConvertError(
                f"CRC mismatch for safety_config v{version} blob "
                f"(stored 0x{stored_crc:08x}, computed 0x{computed_crc:08x}) -- treating as corrupt"
            )
        fields = safety_config_default_fields()
        fields["seq"] = struct.unpack_from("<I", blob, _SC_V1_OFF_SEQ)[0]
        v1_tc_type_byte = blob[_SC_V1_OFF_TC_TYPE]
        fields["tc_type"] = v1_tc_type_byte if v1_tc_type_byte <= _SC_TC_TYPE_MAX_REAL else _SC_DEFAULT_TC_TYPE
        fields["ct_cal"] = _sc_unpack_ct_cal(blob, _SC_V1_OFF_CT_CAL, _SC_V1_CT_CAL_CHANNEL_LEN)
        # Forced true regardless of what the v1 record held -- see
        # config_store_unpack_ex()'s v1 branch comment: a migrated record was
        # never commissioned against everything v3 added.
        fields["calibration_missing"] = True
        fields["format_version"] = SAFETY_CONFIG_STORE_FORMAT_VERSION
        return version, fields

    raise ConfigConvertError(
        f"unknown/unsupported safety_config format_version {version} -- known versions: "
        f"{sorted([SAFETY_CONFIG_STORE_FORMAT_VERSION_V1, SAFETY_CONFIG_STORE_FORMAT_VERSION_V2, SAFETY_CONFIG_STORE_FORMAT_VERSION])}"
    )


def convert_safety_config_blob(blob: bytes, target_version: int) -> "tuple[bytes, ConversionReport]":
    """Best-effort convert a raw config_store_record_t record to
    target_version. Encoding is supported ONLY to
    SAFETY_CONFIG_STORE_FORMAT_VERSION (today 3) -- firmware itself has no
    v1/v2 *pack* path any more, only the forward *unpack*-side migrations
    decode_safety_config_blob() mirrors, so writing a v1/v2-shaped record
    would invent a wire format nothing in firmware produces or reads."""
    if target_version != SAFETY_CONFIG_STORE_FORMAT_VERSION:
        raise ConfigConvertError(
            f"safety_config target version {target_version} is not supported for ENCODING -- firmware has no "
            f"pack path below CONFIG_STORE_FORMAT_VERSION ({SAFETY_CONFIG_STORE_FORMAT_VERSION}), only forward "
            "unpack-side migrations into it. Refusing rather than inventing a wire format firmware never produces."
        )
    source_version, fields = decode_safety_config_blob(blob)
    report = ConversionReport(store="safety_config_blob", source_version=source_version,
                               target_version=target_version)
    if source_version == target_version:
        report.add("document", "version", "kept", "source and target versions are identical")
    elif source_version == SAFETY_CONFIG_STORE_FORMAT_VERSION_V1:
        report.add("document", "most fields", "defaulted",
                    "source predates CONFIG_STORE_FORMAT_VERSION 2/3's whole commissioning surface; every "
                    "field config_store_default() introduced takes its compiled default, "
                    "calibration_missing is forced true, exactly config_store_unpack_ex()'s v1 migration")
    elif source_version == SAFETY_CONFIG_STORE_FORMAT_VERSION_V2:
        report.add("zone_ct_channel", "zone_ct_channel", "defaulted",
                    "source predates zone_ct_channel (v2->v3); derived from ct_topology via "
                    "config_store_derive_zone_ct_channel(), exactly firmware's own migration")
    out = encode_safety_config_v3(fields)
    return out, report


# ---------------------------------------------------------------------------
# zones_blob store -- mirrors firmware/KilnFW/App/drivers/persist/
# zones_config_json.h's CURRENT (ZONES_CFG_VERSION 26) zone_cfg_t/
# zone_timing_profile_t/zones_cfg_t layout byte-for-byte, little-endian
# (Xtensa/ESP32-S3 is little-endian), natural C alignment (no #pragma pack
# anywhere in that header).
#
# STAGE 1 (2026-09-24): decodes/encodes the CURRENT version exactly.
# Every offset below was NOT hand-guessed from field declaration order --
# zone_cfg_t has no historical version's worth of _Static_assert of its own
# (only the FROZEN zone_cfg_vN_t snapshots do, since the current struct's
# offsets shift every time a field is added), but zone_cfg_v25_t is declared
# "field-for-field IDENTICAL to the current zone_cfg_t minus the tail field
# autotune_baseline_k_dc" in that struct's own comment, and IS fully pinned
# by _Static_assert(offsetof(...)) pairs through offset 240 plus
# sizeof(zone_cfg_v25_t) == 244. Every offset up to 240 below was cross-
# checked field-by-field against those real, compiled-checked asserts (and
# transitively against v24/v23/v22/v16's own asserts, which agree at every
# offset they share) before being typed in here; autotune_baseline_k_dc
# itself lands at 244 (no padding needed, 244 is already 4-byte aligned) and
# sizeof(zone_cfg_t) == 248. The zones_cfg_t container's own layout
# (version/thermo_count/.../zones[]/timing_profile_count/timing_profiles[]/
# pc_link_abort_silence_ms/crc32) was computed the same way, by hand, from
# natural alignment rules, 896 bytes total. (zones_config_accessors.h's
# ZONES_CONFIG_BLOB_MAX_SIZE is also 896 today, but firmware only asserts
# sizeof(zones_cfg_t) <= that macro, so the equality alone proves nothing.)
#
# What actually pins this layout is a firmware-generated golden:
# firmware/KilnFW/App/test/test_zones_blob_golden.c fills a real
# zones_cfg_t with a distinct sentinel per field, saves it through the real
# nvs_save() (CRC stamp included), and emits every field's offsetof()/size/
# value plus the raw blob to
# tools/PcTools/tests/fixtures/config_convert/zones_cfg_golden.txt; that C
# test fails when the struct drifts from the committed file.
# tools/PcTools/tests/test_config_convert_zones_golden.py decodes the blob
# with this module and checks every field, the field set, and that encode
# reproduces the firmware bytes exactly. tools/check_config_convert_mirror.py
# separately pins ZONES_CFG_VERSION/ZONE_NAME_MAX_LEN/SRC_GROUP_COUNT/
# TIMING_PROFILE_NAME_MAX_LEN/ZONES_CONFIG_BLOB_MAX_SIZE and the total size.
#
# STAGE 2 (2026-10-05): v21..v25 blobs are decoded too. Each zone_cfg_vN_t
# is a byte-for-byte prefix of the next (zones_config_migrate.c cases 21..25
# are plain memcpys), so _ZONE_TAIL_SEGMENTS lists the tail each bump added
# and the older zone is zero-extended into the current layout. The container
# wrapper is identical, total size is 152 + 3*zone size, and the crc32
# (last field, computed with it zeroed) is verified for every accepted
# version. Firmware's post-load fixups are mirrored: model_fit_* -> UNKNOWN
# for pre-v24, heater floors. Deliberate deviation: real v24/v25
# model_fit_* values are preserved and reported, not wiped as firmware's
# upgrade path does. v20 and older refuse (mid-struct change at v20->v21,
# and v1..v6 predate crc32).
# ---------------------------------------------------------------------------

ZONES_CFG_VERSION = 26  # zones_config_json.h ZONES_CFG_VERSION
ZONE_NAME_MAX_LEN = 15  # zones_config_accessors.h ZONE_NAME_MAX_LEN
TIMING_PROFILE_NAME_MAX_LEN = 7  # zones_config_accessors.h TIMING_PROFILE_NAME_MAX_LEN
SRC_GROUP_COUNT = 5  # zones_config_accessors.h SRC_GROUP_COUNT
THERMO_CHANNEL_COUNT = 3  # uart_task_ids.h THERMO_CHANNEL_COUNT (== MAX31856_CHANNEL_COUNT)
ZONES_CONFIG_BLOB_MAX_SIZE = 896  # zones_config_accessors.h ZONES_CONFIG_BLOB_MAX_SIZE

_ZONE_NAME_LEN = ZONE_NAME_MAX_LEN + 1  # 16
_TIMING_PROFILE_NAME_LEN = TIMING_PROFILE_NAME_MAX_LEN + 1  # 8

# zone_cfg_t, 248 bytes. Field order and every 'x' pad byte below is
# transcribed from the module comment's byte-by-byte derivation above.
_ZONE_V21_FMT = (
    "<"
    f"{_ZONE_NAME_LEN}s"          # name
    "24f"                          # cal_offset_c .. fuzzy_strength_pct (24 floats)
    f"{THERMO_CHANNEL_COUNT}f"     # coupling_coeff[]
    f"{THERMO_CHANNEL_COUNT}f"     # coupling_tau_s[]
    f"{THERMO_CHANNEL_COUNT}f"     # coupling_dead_time_s[]
    "6B"                           # relay_mask, control_mode, tc_type, thermo_mask, ct_mask, timing_profile
    f"{SRC_GROUP_COUNT}B"          # settings_source[]
    "6B"                           # tuning_valid/method/rule/settled/extrapolation_converged/tau_consistent
    "3x"                           # pad to 4-byte-align tuning_baseline_c
    "4f"                           # tuning_baseline_c, tuning_step_ambient_c, tuning_raw_rise_c, tuning_rise_inf_c
    "I"                            # tuning_seq
    "B"                            # adaptive_tune_enabled
    "3x"                           # pad to 4-byte-align coupling_diag_k_dc
    "5f"                           # coupling_diag_k_dc, ease_off_window_mult, approach_rate_cap_c_per_hr,
                                    # error_band_c, rate_band_c_per_s
    "B"                            # relay_type
    "3x"                           # pad to 4-byte-align progress_band_c (v21 ends here, 216 bytes)
)
# Tail appends, one entry per schema bump (stage 2): every hop v21->v26 is a
# pure tail append, so zone_cfg_vN_t is a byte-for-byte prefix of the next.
# (version that ADDED the segment, struct fmt of the segment, fields it adds)
_ZONE_TAIL_SEGMENTS = (
    (22, "f", ("progress_band_c",)),
    (23, "BB2xfHH", ("zone_type", "failsafe_state", "hyst_c", "min_on_s", "min_off_s")),
    (24, "ff", ("model_fit_temp_c", "model_fit_ambient_c")),
    (25, "f", ("coil_power_w",)),
    (26, "f", ("autotune_baseline_k_dc",)),
)
_ZONE_CFG_FMT = _ZONE_V21_FMT + "".join(seg[1] for seg in _ZONE_TAIL_SEGMENTS)
# Oldest zones_cfg_t version this module can decode (stage 2). v20 and older
# differ MID-struct (settings_source grew at v20->v21) or are not transcribed.
ZONES_CFG_MIN_PC_VERSION = 21


def _zone_struct_for_version(version: int) -> "struct.Struct":
    fmt = _ZONE_V21_FMT + "".join(seg[1] for seg in _ZONE_TAIL_SEGMENTS if seg[0] <= version)
    return struct.Struct(fmt)


def _zone_added_fields_after(version: int) -> "list[str]":
    """Zone fields a blob at `version` does not carry (added by a later bump)."""
    out = []
    for added, _fmt, names in _ZONE_TAIL_SEGMENTS:
        if added > version:
            out.extend(names)
    return out
_ZONE_CFG_STRUCT = struct.Struct(_ZONE_CFG_FMT)
assert _ZONE_CFG_STRUCT.size == 248, f"zone_cfg_t layout must be 248 bytes, computed {_ZONE_CFG_STRUCT.size}"
# Frozen historical zone sizes (zones_config_json.h _Static_asserts).
_ZONE_SIZE_BY_VERSION = {21: 216, 22: 220, 23: 232, 24: 240, 25: 244, 26: 248}
for _v, _sz in _ZONE_SIZE_BY_VERSION.items():
    assert _zone_struct_for_version(_v).size == _sz, f"zone_cfg_v{_v}_t must be {_sz} bytes"

_ZONE_FLOAT1_FIELDS = (
    "cal_offset_c", "pid_kp", "pid_ki", "pid_kd", "max_ramp_c_per_hr", "sanity_rate_c_per_min",
    "max_temp_c", "min_temp_c", "heater_window_ms", "heater_min_on_ms", "heater_min_off_ms",
    "guard_wrong_dir_window_s", "guard_wrong_dir_rate_c_per_min", "guard_off_settle_s",
    "guard_runaway_rate_c_per_min", "guard_runaway_margin_c", "guard_drift_period_s",
    "guard_sensor_fault_debounce_ticks", "guard_frozen_window_s", "cross_zone_max_delta_c",
    "model_k_dc", "model_tau_s", "model_dead_time_s", "fuzzy_strength_pct",
)
assert len(_ZONE_FLOAT1_FIELDS) == 24

# zone_timing_profile_t, 44 bytes.
_TIMING_PROFILE_FMT = f"<{_TIMING_PROFILE_NAME_LEN}s9f"
_TIMING_PROFILE_STRUCT = struct.Struct(_TIMING_PROFILE_FMT)
assert _TIMING_PROFILE_STRUCT.size == 44, f"zone_timing_profile_t must be 44 bytes, computed {_TIMING_PROFILE_STRUCT.size}"
_TIMING_PROFILE_FLOAT_FIELDS = (
    "guard_progress_duty_min", "guard_progress_window_s", "guard_drift_hysteresis_c", "guard_frozen_eps_c",
    "guard_cross_zone_period_s", "bangbang_hysteresis_c", "cooling_limited_margin_c", "cooling_limited_hold_s",
    "ramp_lock_band_c",
)
assert len(_TIMING_PROFILE_FLOAT_FIELDS) == 9

# zones_cfg_t header, 6 bytes + 2 pad, before zones[THERMO_CHANNEL_COUNT].
_ZONES_HEADER_FMT = "<6B2x"
_ZONES_HEADER_STRUCT = struct.Struct(_ZONES_HEADER_FMT)
# Tail after zones[]: timing_profile_count (1B) + 3 pad, before
# timing_profiles[THERMO_CHANNEL_COUNT], then pc_link_abort_silence_ms (f) + crc32 (I).
_ZONES_TAIL_FMT = "<B3x"
_ZONES_TAIL_STRUCT = struct.Struct(_ZONES_TAIL_FMT)
_ZONES_FOOTER_FMT = "<fI"
_ZONES_FOOTER_STRUCT = struct.Struct(_ZONES_FOOTER_FMT)

_ZONES_CFG_TOTAL_SIZE = (
    _ZONES_HEADER_STRUCT.size
    + _ZONE_CFG_STRUCT.size * THERMO_CHANNEL_COUNT
    + _ZONES_TAIL_STRUCT.size
    + _TIMING_PROFILE_STRUCT.size * THERMO_CHANNEL_COUNT
    + _ZONES_FOOTER_STRUCT.size
)
assert _ZONES_CFG_TOTAL_SIZE == ZONES_CONFIG_BLOB_MAX_SIZE, (
    f"computed sizeof(zones_cfg_t) {_ZONES_CFG_TOTAL_SIZE} != ZONES_CONFIG_BLOB_MAX_SIZE "
    f"{ZONES_CONFIG_BLOB_MAX_SIZE} -- see this module's zones_blob section comment"
)


def _decode_zone_cfg(raw: bytes) -> dict:
    vals = list(_ZONE_CFG_STRUCT.unpack(raw))
    out = {"name": _decode_name(vals.pop(0))}
    for f in _ZONE_FLOAT1_FIELDS:
        out[f] = vals.pop(0)
    out["coupling_coeff"] = [vals.pop(0) for _ in range(THERMO_CHANNEL_COUNT)]
    out["coupling_tau_s"] = [vals.pop(0) for _ in range(THERMO_CHANNEL_COUNT)]
    out["coupling_dead_time_s"] = [vals.pop(0) for _ in range(THERMO_CHANNEL_COUNT)]
    for f in ("relay_mask", "control_mode", "tc_type", "thermo_mask", "ct_mask", "timing_profile"):
        out[f] = vals.pop(0)
    out["settings_source"] = [vals.pop(0) for _ in range(SRC_GROUP_COUNT)]
    for f in ("tuning_valid", "tuning_method", "tuning_rule", "tuning_settled",
              "tuning_extrapolation_converged", "tuning_tau_consistent"):
        out[f] = vals.pop(0)
    for f in ("tuning_baseline_c", "tuning_step_ambient_c", "tuning_raw_rise_c", "tuning_rise_inf_c"):
        out[f] = vals.pop(0)
    out["tuning_seq"] = vals.pop(0)
    out["adaptive_tune_enabled"] = vals.pop(0)
    for f in ("coupling_diag_k_dc", "ease_off_window_mult", "approach_rate_cap_c_per_hr",
              "error_band_c", "rate_band_c_per_s"):
        out[f] = vals.pop(0)
    out["relay_type"] = vals.pop(0)
    out["progress_band_c"] = vals.pop(0)
    out["zone_type"] = vals.pop(0)
    out["failsafe_state"] = vals.pop(0)
    out["hyst_c"] = vals.pop(0)
    out["min_on_s"] = vals.pop(0)
    out["min_off_s"] = vals.pop(0)
    out["model_fit_temp_c"] = vals.pop(0)
    out["model_fit_ambient_c"] = vals.pop(0)
    out["coil_power_w"] = vals.pop(0)
    out["autotune_baseline_k_dc"] = vals.pop(0)
    assert not vals
    return out


def _encode_zone_cfg(z: dict) -> bytes:
    args = [z["name"].encode("ascii", errors="replace")[:ZONE_NAME_MAX_LEN].ljust(_ZONE_NAME_LEN, b"\x00")]
    args += [z[f] for f in _ZONE_FLOAT1_FIELDS]
    args += list(z["coupling_coeff"])
    args += list(z["coupling_tau_s"])
    args += list(z["coupling_dead_time_s"])
    args += [z[f] for f in ("relay_mask", "control_mode", "tc_type", "thermo_mask", "ct_mask", "timing_profile")]
    args += list(z["settings_source"])
    args += [z[f] for f in ("tuning_valid", "tuning_method", "tuning_rule", "tuning_settled",
                             "tuning_extrapolation_converged", "tuning_tau_consistent")]
    args += [z[f] for f in ("tuning_baseline_c", "tuning_step_ambient_c", "tuning_raw_rise_c",
                             "tuning_rise_inf_c")]
    args += [z["tuning_seq"], z["adaptive_tune_enabled"]]
    args += [z[f] for f in ("coupling_diag_k_dc", "ease_off_window_mult", "approach_rate_cap_c_per_hr",
                             "error_band_c", "rate_band_c_per_s")]
    args += [z["relay_type"], z["progress_band_c"], z["zone_type"], z["failsafe_state"]]
    args += [z["hyst_c"], z["min_on_s"], z["min_off_s"], z["model_fit_temp_c"], z["model_fit_ambient_c"],
             z["coil_power_w"], z["autotune_baseline_k_dc"]]
    return _ZONE_CFG_STRUCT.pack(*args)


def _decode_timing_profile(raw: bytes) -> dict:
    vals = list(_TIMING_PROFILE_STRUCT.unpack(raw))
    out = {"name": _decode_name(vals.pop(0))}
    for f in _TIMING_PROFILE_FLOAT_FIELDS:
        out[f] = vals.pop(0)
    assert not vals
    return out


def _encode_timing_profile(p: dict) -> bytes:
    name = p["name"].encode("ascii", errors="replace")[:TIMING_PROFILE_NAME_MAX_LEN]
    name = name + b"\x00" * (_TIMING_PROFILE_NAME_LEN - len(name))
    args = [name] + [p[f] for f in _TIMING_PROFILE_FLOAT_FIELDS]
    return _TIMING_PROFILE_STRUCT.pack(*args)


_ZONE_HEATER_MIN_ON_MS_FLOOR = 10000.0  # heater_output.h HEATER_MIN_ON_MS_FLOOR
_ZONE_HEATER_WINDOW_MIN_MULTIPLE = 3.0  # heater_output.h HEATER_MIN_WINDOW_MULTIPLE
_MODEL_FIT_UNKNOWN = struct.unpack("<f", struct.pack("<f", -273.15))[0]  # ZONE_MODEL_FIT_TEMP_UNKNOWN


def _zones_total_size_for_version(version: int) -> int:
    return (_ZONES_HEADER_STRUCT.size + _zone_struct_for_version(version).size * THERMO_CHANNEL_COUNT
            + _ZONES_TAIL_STRUCT.size + _TIMING_PROFILE_STRUCT.size * THERMO_CHANNEL_COUNT
            + _ZONES_FOOTER_STRUCT.size)


def _zones_crc32_n(blob: bytes) -> int:
    """esp_crc32_le over the blob with its trailing crc32 field zeroed; works
    for any version (crc32 is the last field of every zones_cfg_vN_t)."""
    return _crc32(blob[:-4] + b"\x00\x00\x00\x00")


def _decode_zones_blob_any(blob: bytes) -> "tuple[int, dict, list]":
    """Decode a zones_cfg_t blob of ZONES_CFG_MIN_PC_VERSION..ZONES_CFG_VERSION
    into (source_version, fields_at_current_version, notes). Older shapes are
    zero-extended with the bytes of the fields they predate (every hop is a
    pure tail append -- zones_config_migrate.c cases 21..25 are plain
    prefix memcpys), then the same fixups firmware applies on load are run.
    notes is a list of (scope, field, action, detail) for the report."""
    if len(blob) < 1:
        raise ConfigConvertError("zones_blob document is empty (need at least a version byte)")
    version = blob[0]
    if version > ZONES_CFG_VERSION:
        raise ConfigConvertError(
            f"zones_blob version {version} is newer than this tool knows (current is {ZONES_CFG_VERSION}) "
            "-- refusing rather than guessing")
    if version < ZONES_CFG_MIN_PC_VERSION:
        raise ConfigConvertError(
            f"zones_blob version {version} is older than the oldest shape this tool decodes "
            f"(v{ZONES_CFG_MIN_PC_VERSION}). v{version} differs from the current layout mid-struct or is "
            "not transcribed; convert it by loading it on a board running firmware that still migrates it "
            "(zones_config_migrate.c) and exporting a current-version backup. Refusing rather than guessing.")
    expected = _zones_total_size_for_version(version)
    if len(blob) != expected:
        raise ConfigConvertError(
            f"zones_blob claims version {version} but is {len(blob)} bytes, not {expected} -- "
            "refusing rather than guessing at a truncated or padded record")
    stored_crc = struct.unpack_from("<I", blob, len(blob) - 4)[0]
    computed_crc = _zones_crc32_n(blob)
    if computed_crc != stored_crc:
        raise ConfigConvertError(
            f"zones_blob crc32 mismatch: stored 0x{stored_crc:08x}, computed 0x{computed_crc:08x} -- "
            "refusing a blob that fails its own integrity check rather than converting corrupt data")

    zst = _zone_struct_for_version(version)
    missing = _zone_added_fields_after(version)
    pad = _ZONE_CFG_STRUCT.size - zst.size
    off = 0
    header = _ZONES_HEADER_STRUCT.unpack_from(blob, off)
    off += _ZONES_HEADER_STRUCT.size
    zones = []
    for _ in range(THERMO_CHANNEL_COUNT):
        zones.append(_decode_zone_cfg(blob[off: off + zst.size] + b"\x00" * pad))
        off += zst.size
    (timing_profile_count,) = _ZONES_TAIL_STRUCT.unpack_from(blob, off)
    off += _ZONES_TAIL_STRUCT.size
    timing_profiles = []
    for _ in range(THERMO_CHANNEL_COUNT):
        timing_profiles.append(_decode_timing_profile(blob[off: off + _TIMING_PROFILE_STRUCT.size]))
        off += _TIMING_PROFILE_STRUCT.size
    pc_link_abort_silence_ms, _crc = _ZONES_FOOTER_STRUCT.unpack_from(blob, off)
    off += _ZONES_FOOTER_STRUCT.size
    assert off == len(blob)

    notes = []
    if version < ZONES_CFG_VERSION:
        for f in missing:
            if f in ("model_fit_temp_c", "model_fit_ambient_c"):
                continue
            notes.append(("zone", f, "defaulted", "field added after this version; 0 is its sentinel/default"))
        if "model_fit_temp_c" in missing:
            for z in zones:
                z["model_fit_temp_c"] = _MODEL_FIT_UNKNOWN
                z["model_fit_ambient_c"] = _MODEL_FIT_UNKNOWN
            notes.append(("zone", "model_fit_temp_c/model_fit_ambient_c", "defaulted",
                          "predate v24; set to the UNKNOWN sentinel -273.15 like firmware"))
        else:
            notes.append(("zone", "model_fit_temp_c/model_fit_ambient_c", "kept",
                          "real fit context preserved; note firmware's own upgrade path "
                          "(zones_config_json_apply_model_fit_defaults) wipes it to UNKNOWN for every "
                          "pre-current version, so the board itself would not keep these"))
        # raise_heater_timing_to_floors(), applied by firmware on every load.
        for zi, z in enumerate(zones):
            v = z["heater_min_on_ms"]
            if v == v and 0.0 < v < _ZONE_HEATER_MIN_ON_MS_FLOOR:
                z["heater_min_on_ms"] = _ZONE_HEATER_MIN_ON_MS_FLOOR
                notes.append((f"zone[{zi}]", "heater_min_on_ms", "defaulted",
                              f"raised from {v:g} to the {_ZONE_HEATER_MIN_ON_MS_FLOOR:g} ms relay floor"))
            need = max(z["heater_min_on_ms"], _ZONE_HEATER_MIN_ON_MS_FLOOR) * _ZONE_HEATER_WINDOW_MIN_MULTIPLE
            w = z["heater_window_ms"]
            if w == w and 0.0 < w < need:
                z["heater_window_ms"] = need
                notes.append((f"zone[{zi}]", "heater_window_ms", "defaulted",
                              f"raised from {w:g} to {need:g} ms (3x min on-time)"))

    fields = {
        "version": ZONES_CFG_VERSION,
        "thermo_count": header[1],
        "relay_count": header[2],
        "max_simultaneous_relays": header[3],
        "continue_on_zone_trip": header[4],
        "safety_tc_type": header[5],
        "zones": zones,
        "timing_profile_count": timing_profile_count,
        "timing_profiles": timing_profiles,
        "pc_link_abort_silence_ms": pc_link_abort_silence_ms,
    }
    return version, fields, notes


def decode_zones_blob(blob: bytes) -> "tuple[int, dict]":
    """Decode a raw zones_cfg_t blob (v21..current, CRC-verified) into
    (source_version, fields_dict). fields_dict is always at the CURRENT
    layout (fields["version"] == ZONES_CFG_VERSION), with fields the source
    predates defaulted as firmware's own migration does. Versions below
    ZONES_CFG_MIN_PC_VERSION refuse -- see _decode_zones_blob_any()."""
    version, fields, _notes = _decode_zones_blob_any(blob)
    return version, fields


def _zones_crc32(blob_with_real_crc: bytes) -> int:
    """zones_config_json_compute_crc(): esp_crc32_le over the whole struct
    with the trailing crc32 field zeroed (never over the body alone)."""
    body = blob_with_real_crc[: _ZONES_CFG_TOTAL_SIZE - 4]
    return _crc32(body + b"\x00\x00\x00\x00")


def encode_zones_blob(fields: dict) -> bytes:
    """Encode fields (as returned by decode_zones_blob(), version must be
    ZONES_CFG_VERSION) back to raw bytes, recomputing crc32 the same way
    firmware's nvs_save() does. See decode_zones_blob().

    A real board-written blob can carry non-zero struct padding bytes (left
    over from whatever was on the stack/heap when the firmware struct was
    filled) that this encoder always writes as zero, so
    encode_zones_blob(decode_zones_blob(x)) may differ from x byte-for-byte
    at those padding offsets while still decoding to the same fields and
    passing the same CRC check -- that is expected, not a round-trip bug."""
    version = fields["version"]
    if version != ZONES_CFG_VERSION:
        raise ConfigConvertError(
            f"cannot encode zones_blob at version {version} -- this module only encodes the CURRENT "
            f"ZONES_CFG_VERSION ({ZONES_CFG_VERSION})"
        )
    header = _ZONES_HEADER_STRUCT.pack(
        fields["version"], fields["thermo_count"], fields["relay_count"],
        fields["max_simultaneous_relays"], fields["continue_on_zone_trip"], fields["safety_tc_type"],
    )
    zones_bytes = b"".join(_encode_zone_cfg(z) for z in fields["zones"])
    if len(fields["zones"]) != THERMO_CHANNEL_COUNT:
        raise ConfigConvertError(f"zones_blob must carry exactly {THERMO_CHANNEL_COUNT} zones, "
                                  f"got {len(fields['zones'])}")
    tail = _ZONES_TAIL_STRUCT.pack(fields["timing_profile_count"])
    if len(fields["timing_profiles"]) != THERMO_CHANNEL_COUNT:
        raise ConfigConvertError(f"zones_blob must carry exactly {THERMO_CHANNEL_COUNT} timing_profiles, "
                                  f"got {len(fields['timing_profiles'])}")
    profiles_bytes = b"".join(_encode_timing_profile(p) for p in fields["timing_profiles"])
    body_no_crc = header + zones_bytes + tail + profiles_bytes + struct.pack("<f", fields["pc_link_abort_silence_ms"])
    crc = _crc32(body_no_crc + b"\x00\x00\x00\x00")
    return body_no_crc + struct.pack("<I", crc)


def convert_zones_blob(blob: bytes, target_version: int) -> "tuple[bytes, ConversionReport]":
    """Convert a zones_cfg_t blob (v21..current) to the CURRENT
    ZONES_CFG_VERSION. Only the current version is an encodable target
    (the historical shapes are never written)."""
    if target_version != ZONES_CFG_VERSION:
        raise ConfigConvertError(
            f"zones_blob target version {target_version} is not supported for ENCODING -- this module "
            f"only encodes the CURRENT ZONES_CFG_VERSION ({ZONES_CFG_VERSION}).")
    source_version, fields, notes = _decode_zones_blob_any(blob)
    report = ConversionReport(store="zones_blob", source_version=source_version, target_version=target_version)
    if source_version == target_version:
        report.add("document", "version", "kept", "source and target versions are identical")
    else:
        report.add("document", "version", "renamed",
                   f"v{source_version} upgraded by tail-append prefix chain to v{target_version}")
        for scope, f, action, detail in notes:
            report.add(scope, f, action, detail)
    out = encode_zones_blob(fields)
    return out, report


# ---------------------------------------------------------------------------
# kiln_package store -- kiln_cfg_store's "kilnpkg.json" envelope (kind
# "kilnctl_kiln_package" -- KILN_PKG_KIND, firmware/KilnFW/App/drivers/
# persist/kiln_package.h/.c). This was the module docstring's "NOT YET
# SUPPORTED" item 2 (STAGE 3): it depended on zones_blob support (item 1,
# STAGE 1/2 above) landing first, since its `esp_blob_hex` field IS a raw
# zones_cfg_t blob and everything else in the envelope (`pico`, a flat array
# of already-decoded {id,type,flags,value_bits} entries with no version of
# its own -- param ids only ever go up, per kiln_package.h's own comment) is
# already plain JSON needing no binary decoding at all.
#
# Converting a kiln_package document to another ZONES_CFG_VERSION therefore
# reduces to: decode `esp_blob_hex` via the zones_blob functions above,
# convert it, re-encode, and recompute `pkg_hash` over the NEW bytes exactly
# the way kiln_package_compute_hash() does (pkg_schema u16 LE, esp_blob_len
# u16 LE, esp_blob bytes, pico->count u16 LE, then each pico entry as
# id(u16 LE)+type(u8)+flags(u8)+value_bits(u32 LE), esp_crc32_le over the
# whole buffer -- the same CRC-32 this module already treats as
# zlib.crc32()-equivalent elsewhere, see module docstring's CRC note).
# `source_board_id` is carried through unchanged (it identifies the
# EXPORTING board, not this document's own version) and is deliberately
# OUTSIDE pkg_hash's input set, matching kiln_package.h's own "RULING"
# comment on why.
#
# zones_blob's range (v21..current) applies transitively here; an older
# esp_blob_hex refuses with convert_zones_blob()'s message.
# ---------------------------------------------------------------------------


KILN_PKG_SCHEMA_VERSION = 1  # kiln_package.h KILN_PKG_SCHEMA_VERSION


def _kiln_pkg_compute_hash(pkg_schema: int, esp_blob: bytes, pico_entries: "list[dict]") -> int:
    try:
        buf = bytearray()
        buf += struct.pack("<HH", pkg_schema, len(esp_blob))
        buf += esp_blob
        buf += struct.pack("<H", len(pico_entries))
        for e in pico_entries:
            if not isinstance(e, dict):
                raise ConfigConvertError("kilnctl_kiln_package document's 'pico' entries must be objects")
            try:
                buf += struct.pack("<HBBI", e["id"], e["type"], e["flags"], e["value_bits"])
            except KeyError as exc:
                raise ConfigConvertError(
                    f"kilnctl_kiln_package document's 'pico' entry is missing required field {exc}") from exc
            except struct.error as exc:
                raise ConfigConvertError(f"kilnctl_kiln_package document's 'pico' entry has an out-of-range "
                                          f"or wrongly-typed field: {exc}") from exc
        return _crc32(bytes(buf))
    except (struct.error, TypeError) as exc:
        # Catches a malformed pkg_schema/esp_blob length that struct.pack
        # itself refuses to encode (e.g. non-int, or > 65535).
        raise ConfigConvertError(f"kilnctl_kiln_package document has a malformed field: {exc}") from exc


def convert_kiln_package(doc: dict, target_version: int) -> "tuple[dict, ConversionReport]":
    """Convert a kilnctl_kiln_package envelope's esp_blob_hex (a raw
    zones_cfg_t blob) to target_version, re-encoding pkg_hash over the
    result. `pico` and `source_board_id` pass through unchanged -- see this
    module's kiln_package section comment.

    Mirrors kiln_package_import_json()'s own validation as closely as a
    file-only tool can: pkg_schema/pkg_hash must both be present (firmware
    refuses a package missing either), pkg_schema must be in 1..
    KILN_PKG_SCHEMA_VERSION (0 and "newer than this tool knows" are both
    refused, never silently accepted), and esp_blob_len (when present) must
    match the decoded blob length. Unlike firmware's own import path -- which
    documents that it decodes but does NOT verify pkg_hash, leaving that to
    its caller -- this function DOES recompute and verify pkg_hash over the
    INPUT before converting anything, since a converter that re-stamps a
    tampered or corrupted document as freshly valid is worse than one that
    refuses it outright."""
    try:
        esp_blob = bytes.fromhex(doc["esp_blob_hex"])
    except (KeyError, ValueError) as exc:
        raise ConfigConvertError(f"kilnctl_kiln_package document must carry a valid hex 'esp_blob_hex': {exc}")
    pico_entries = doc.get("pico", [])
    if not isinstance(pico_entries, list):
        raise ConfigConvertError("kilnctl_kiln_package document's 'pico' field must be a list")

    if "pkg_schema" not in doc:
        raise ConfigConvertError("kilnctl_kiln_package document is missing required field 'pkg_schema'")
    pkg_schema = doc["pkg_schema"]
    if not isinstance(pkg_schema, int) or isinstance(pkg_schema, bool) or not (1 <= pkg_schema <= KILN_PKG_SCHEMA_VERSION):
        raise ConfigConvertError(
            f"kilnctl_kiln_package document's pkg_schema ({pkg_schema!r}) must be an integer in "
            f"1..{KILN_PKG_SCHEMA_VERSION} -- 0 was never emitted and anything higher is newer than this "
            "tool knows, same as kiln_package_import_json()'s own refusal")

    if "pkg_hash" not in doc:
        raise ConfigConvertError("kilnctl_kiln_package document is missing required field 'pkg_hash'")
    try:
        declared_hash = int(str(doc["pkg_hash"]), 16)
    except (TypeError, ValueError) as exc:
        raise ConfigConvertError(f"kilnctl_kiln_package document's 'pkg_hash' is not a valid hex string: {exc}")

    if "esp_blob_len" in doc:
        declared_len = doc["esp_blob_len"]
        if not isinstance(declared_len, int) or isinstance(declared_len, bool) or declared_len != len(esp_blob):
            raise ConfigConvertError(
                f"kilnctl_kiln_package document's esp_blob_len ({declared_len!r}) does not match its decoded "
                f"esp_blob_hex length ({len(esp_blob)}) -- file is truncated or corrupted")

    input_hash = _kiln_pkg_compute_hash(pkg_schema, esp_blob, pico_entries)
    if input_hash != declared_hash:
        raise ConfigConvertError(
            f"kilnctl_kiln_package document's declared pkg_hash (0x{declared_hash:08x}) does not match the "
            f"hash recomputed over its own esp_blob/pico contents (0x{input_hash:08x}) -- refusing to "
            "convert a document that already fails its own integrity check rather than re-stamping it valid")

    out_blob, zones_report = convert_zones_blob(esp_blob, target_version)
    report = ConversionReport(store="kiln_package", source_version=zones_report.source_version,
                               target_version=target_version)
    for o in zones_report.outcomes:
        report.add(o.scope, o.field, o.action, o.detail)
    report.add("pico", "pico", "kept", "Pico param entries carry no version of their own -- passed through unchanged")

    new_hash = _kiln_pkg_compute_hash(pkg_schema, out_blob, pico_entries)
    out_doc = dict(doc)
    out_doc["esp_blob_hex"] = out_blob.hex()
    out_doc["esp_blob_len"] = len(out_blob)
    out_doc["pkg_hash"] = f"0x{new_hash:08x}"
    return out_doc, report


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

    if kind == "safety_config_blob":
        try:
            blob = bytes.fromhex(doc["blob_hex"])
        except (KeyError, ValueError) as exc:
            raise ConfigConvertError(f"kilnctl_safety_config_blob document must carry a valid hex 'blob_hex': {exc}")
        out_blob, report = convert_safety_config_blob(blob, target_version)
        out_doc = {"kind": "kilnctl_safety_config_blob", "version": target_version, "blob_hex": out_blob.hex()}
        return out_doc, report

    if kind == "zones_blob":
        try:
            blob = bytes.fromhex(doc["blob_hex"])
        except (KeyError, ValueError) as exc:
            raise ConfigConvertError(f"kilnctl_zones_blob document must carry a valid hex 'blob_hex': {exc}")
        out_blob, report = convert_zones_blob(blob, target_version)
        out_doc = {"kind": "kilnctl_zones_blob", "version": target_version, "blob_hex": out_blob.hex()}
        return out_doc, report

    if kind == "kiln_package":
        out_doc, report = convert_kiln_package(doc, target_version)
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
