#!/usr/bin/env python3
"""cfg_convert.py -- host-side, arbitrary-direction converter for the kiln
configuration "backup package" (the JSON document GET /api/backup/export
produces and POST /api/backup/import accepts -- see
firmware/KilnFW/App/drivers/http/backup_export.c/backup_import.c/
backup_http_internal.h).

WHY THIS EXISTS (see docs/CONFIG_MIGRATION_CHAIN_PLAN.md and CLAUDE.md's
pointer to it): the board's own firmware is deliberately kept to one
migration step -- N-1 to N -- so a board more than one release behind cannot
read its own on-flash config and falls back to firmware defaults, silently
losing tuning. This tool moves the *arbitrary* version-to-version conversion
burden onto the PC, where it is cheap, while leaving the board's on-flash
migration code untouched. This module never runs on the board and never
changes firmware migration behaviour.

SCOPE. The document this module converts is the BACKUP document -- the
top-level "version" field is BACKUP_FORMAT_VERSION (currently 4, see
backup_http_internal.h), not ZONES_CFG_VERSION (the on-flash per-zone schema
version, currently 26). The two are different numbers for a reason: most of
the fields ZONES_CFG_VERSION bumps added to the on-flash struct were folded
into the backup document as purely-additive, presence-gated optional keys
with NO BACKUP_FORMAT_VERSION bump at all (backup_import.c's own comments say
so at each one: "No BACKUP_FORMAT_VERSION bump -- these are purely additive
optional keys"). backup_import.c already treats the whole document as a
MERGE onto whatever the target board currently has: a key that is absent
leaves the corresponding on-board value untouched. That means the only
places where BACKUP_FORMAT_VERSION genuinely changes the document's SHAPE
(not just which optional keys might be present) are:

  - v1 -> v2 (2026-08-21): a zone entry grew from {pid_kp/ki/kd, model_*,
    tc_type} to also unconditionally carrying name/relay_mask/thermo_mask/
    ct_mask/cal_offset_c/max_ramp_c_per_hr/sanity_rate_c_per_min/
    control_mode/max_temp_c/min_temp_c/heater_*/guard_*/
    cross_zone_max_delta_c/settings_source.
  - v2 -> v3 (2026-08-30): fuzzy_strength_pct added, and zone-to-zone
    coupling first appears, expressed as at most ONE neighbor per zone via
    the coupling_coeff/coupling_neighbor_zone pair.
  - v3 -> v4 (2026-08-30, same-day follow-up): coupling_coeff/
    coupling_neighbor_zone is replaced by one coupling_c<N> key per neighbor
    channel (a full row, not just one pair) -- this is the one genuinely
    LOSSY shape change in the format's history, because a v3-or-earlier
    document can only ever have expressed one neighbor.

Every other field this module knows about (coupling_tau_c<N>,
coupling_dead_time_c<N>, coupling_diag_k_dc, settings_source_g<N>,
ease_off_window_mult, approach_rate_cap_c_per_hr, error_band_c,
rate_band_c_per_s, relay_type, progress_band_c, zone_type,
model_fit_temp_c/ambient_c, coil_power_w, autotune_baseline_k_dc,
adaptive_tune_enabled, tuning_*, normal_current_a) is additive-only in every
BACKUP_FORMAT_VERSION from 1 through the current one: presence, not the
version number, decides whether it is carried. This module follows that same
presence-first rule -- exactly as backup_import.c does -- rather than
inventing a second, parallel notion of "what version introduced this key"
that the firmware does not itself enforce.

DRIFT CONTROL (see the docstring class comment repeated in every
*_mirror_drift_check.py this repo already has -- CLAUDE.md's "Prioritize
mirror-drift checks" precedent, e.g. approach_rate_cap_mirror_drift_check.py):
this module owns a second copy of firmware's field vocabulary and version
constants, which is exactly the shape that has drifted silently before in
this codebase. Rather than trust hand maintenance,
firmware/KilnFW/App/test/cfg_convert_field_mirror_drift_check.py extracts the
live field list and version constants straight out of backup_export.c/
backup_import.c/backup_http_internal.h by regex and fails the build the
moment this module's KNOWN_ZONE_FIELDS/BACKUP_FORMAT_VERSION*/
LEGACY_COUPLING_KEYS disagree with what firmware actually emits or reads.
Run it directly, or let tools/run_all_checks.ps1 pick it up (it globs
check_*.ps1 recursively; see check_cfg_convert_field_mirror_drift.ps1).

NEVER FABRICATE A CALIBRATION VALUE. normal_current_a (ESP zones) and
i_normal_a (the Pico safety-side name for the same concept, per CLAUDE.md)
are never synthesized, defaulted, derived, or carried forward from a
different zone/channel by this module. If the source document does not have
the key, the output document does not either, and the report says so
explicitly under "dropped (calibration, never fabricated)". This is the
single property covered by its own regression test
(test_cfg_convert.py::test_never_fabricates_calibration).

NEVER EMIT A CREDENTIAL. This module refuses (raises CfgConvertError) if the
input document contains a "kiln_auth" key, or any key matching
FORBIDDEN_KEY_RE (ssid/password/psk/wifi-credential-shaped names) anywhere in
the document -- the real backup format never includes these (see
backup_http.h's header comment, cited above in backup_export.c), so their
presence means this is not a genuine board export and should not be trusted
silently. This module also never writes such a key to any output path.
"""
from __future__ import annotations

import argparse
import copy
import json
import re
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Optional

# ---------------------------------------------------------------------------
# Constants mirrored from backup_http_internal.h. Kept as plain module
# constants (not re-derived at import time) so this module has zero
# dependency on the firmware tree being checked out at all -- the mirror
# drift check (see module docstring) is what keeps them honest, not an
# import-time reach into firmware/.
# ---------------------------------------------------------------------------
BACKUP_FORMAT_VERSION = 4
BACKUP_FORMAT_VERSION_MIN = 1

# Every zone-level key this module knows firmware can emit/read today, by the
# BACKUP_FORMAT_VERSION at which the *document shape* first requires it
# (i.e. becomes unconditionally emitted rather than merely allowed to be
# absent). Keys not listed here at all are additive-only across every known
# version -- see module docstring -- and are simply carried through whenever
# present, at any target version.
V1_ZONE_KEYS = frozenset({"index", "pid_kp", "pid_ki", "pid_kd"})
# have_model / have_tc: optional even at v1.
V1_OPTIONAL_ZONE_KEYS = frozenset({"model_k_dc", "model_tau_s", "model_dead_time_s", "tc_type"})
V2_ZONE_KEYS = frozenset({
    "name", "relay_mask", "thermo_mask", "ct_mask", "cal_offset_c", "max_ramp_c_per_hr",
    "sanity_rate_c_per_min", "control_mode", "max_temp_c", "min_temp_c", "heater_window_ms",
    "heater_min_on_ms", "heater_min_off_ms", "guard_wrong_dir_window_s",
    "guard_wrong_dir_rate_c_per_min", "guard_off_settle_s", "guard_runaway_rate_c_per_min",
    "guard_runaway_margin_c", "guard_drift_period_s", "guard_sensor_fault_debounce_ticks",
    "guard_frozen_window_s", "cross_zone_max_delta_c", "settings_source",
})
V3_ZONE_KEYS = frozenset({"fuzzy_strength_pct"})
# The v3-only single-neighbor coupling representation; superseded (not
# removed -- backup_import.c still accepts it from an old document) at v4.
LEGACY_COUPLING_KEYS = frozenset({"coupling_coeff", "coupling_neighbor_zone"})
# The v4 full-row coupling representation. coupling_c<N> for N in
# range(num_channels) is generated, not listed literally, by
# _coupling_cell_key() below.
V4_COUPLING_KEY_PREFIX = "coupling_c"

# Additive-only keys (any version, any presence) that a future backup could
# carry. Listed so the mirror-drift check can confirm this module has not
# fallen behind a NEW additive key firmware started emitting, even though
# missing one is not a version-shape defect (a converter that has never
# heard of a brand-new additive key still round-trips every OTHER key
# correctly -- it just cannot explain that one key's fate, which is exactly
# what the mirror-drift check is for).
KNOWN_ADDITIVE_ZONE_KEYS = frozenset({
    "coupling_tau_c", "coupling_dead_time_c", "coupling_diag_k_dc", "settings_source_g",
    "ease_off_window_mult", "approach_rate_cap_c_per_hr", "error_band_c", "rate_band_c_per_s",
    "relay_type", "progress_band_c", "zone_type", "model_fit_temp_c", "model_fit_ambient_c",
    "coil_power_w", "autotune_baseline_k_dc", "adaptive_tune_enabled", "tuning_valid",
    "tuning_method", "tuning_rule", "tuning_settled", "tuning_extrapolation_converged",
    "tuning_tau_consistent", "tuning_baseline_c", "tuning_step_ambient_c", "tuning_raw_rise_c",
    "tuning_rise_inf_c", "normal_current_a", "safety_i_normal_a",
})

# Calibration fields this module will NEVER fabricate, default, or derive.
# normal_current_a is the ESP-side name from backup_export.c. i_normal_a is
# the Pico/SaftyFW-side name for the analogous CT-normal concept (CLAUDE.md)
# -- not present under that bare name in today's ESP backup document, but
# guarded here too in case a future document folds safety-side config into
# the same package under that name. safety_i_normal_a is the actual key the
# backup document uses today (added 2026-09-16, "close the Pico's own
# i_normal_a gap") for the Pico's own S14/S15 arming baseline
# (zones_get_safety_pico_i_normal_a() in backup_export.c) -- it is the same
# never-fabricate hazard under a different document key, so it gets the same
# treatment as normal_current_a.
CALIBRATION_KEYS = frozenset({"normal_current_a", "i_normal_a", "safety_i_normal_a"})

# A document containing any of these has no business being processed by this
# tool at all -- see backup_http.h's own header comment: Wi-Fi credentials
# are never in the real export. kiln_auth is the NVS namespace CLAUDE.md
# names as never to be read.
FORBIDDEN_KEY_RE = re.compile(r"(?i)^(kiln_auth|ssid|password|psk|wifi_ssid|wifi_password)$")

DEFAULT_HTTP_TIMEOUT_S = 5.0


class CfgConvertError(RuntimeError):
    """Raised for a malformed/unsupported document, a forbidden key, or a
    board-communication failure when --to-board is used."""


@dataclass
class FieldOutcome:
    """One line of the conversion report: what happened to one field of one
    zone (or the top-level document)."""
    scope: str          # e.g. "zone 2" or "profile 0" or "document"
    field: str
    outcome: str        # "kept", "dropped", "derived", "defaulted"
    detail: str = ""

    def line(self) -> str:
        return f"{self.scope}: {self.field}: {self.outcome}" + (f" ({self.detail})" if self.detail else "")


@dataclass
class ConversionReport:
    source_version: int
    target_version: int
    outcomes: list = field(default_factory=list)

    def add(self, scope: str, field_name: str, outcome: str, detail: str = "") -> None:
        self.outcomes.append(FieldOutcome(scope, field_name, outcome, detail))

    @property
    def lossy(self) -> bool:
        return any(o.outcome == "dropped" for o in self.outcomes)

    def text(self) -> str:
        lines = [
            f"cfg_convert report: v{self.source_version} -> v{self.target_version}"
            + (" (LOSSY)" if self.lossy else " (lossless)"),
        ]
        for o in self.outcomes:
            lines.append("  " + o.line())
        if not self.outcomes:
            lines.append("  (no field required special handling; every present key carried through unchanged)")
        return "\n".join(lines)

    def to_dict(self) -> dict:
        return {
            "source_version": self.source_version,
            "target_version": self.target_version,
            "lossy": self.lossy,
            "outcomes": [
                {"scope": o.scope, "field": o.field, "outcome": o.outcome, "detail": o.detail}
                for o in self.outcomes
            ],
        }


def _coupling_cell_key(n: int) -> str:
    return f"{V4_COUPLING_KEY_PREFIX}{n}"


def _is_coupling_cell_key(k: str) -> bool:
    """True only for coupling_c<digits> (coupling_c0, coupling_c12, ...) --
    NOT coupling_coeff, which shares the same string prefix but is the
    legacy single-neighbor key, not a per-cell one. A plain startswith()
    check here would delete coupling_coeff right after this module had just
    written it (found by test_downgrade_v4_to_v3_collapses_coupling_row_
    to_single_neighbor_and_reports_loss)."""
    return k.startswith(V4_COUPLING_KEY_PREFIX) and k[len(V4_COUPLING_KEY_PREFIX):].isdigit()


def _check_forbidden(obj: Any, path: str = "$") -> None:
    if isinstance(obj, dict):
        for k, v in obj.items():
            if FORBIDDEN_KEY_RE.match(str(k)):
                raise CfgConvertError(
                    f"refusing to process document: forbidden key '{k}' at {path} -- a genuine "
                    "board backup never contains credentials; this tool never reads or emits them"
                )
            _check_forbidden(v, f"{path}.{k}")
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            _check_forbidden(v, f"{path}[{i}]")


def load_package(raw: str) -> dict:
    try:
        doc = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise CfgConvertError(f"not valid JSON: {exc}") from exc
    if not isinstance(doc, dict) or doc.get("kind") != "kilnctl_backup":
        raise CfgConvertError("not a kilnctl_backup package (missing/wrong 'kind')")
    if "version" not in doc:
        raise CfgConvertError("package has no 'version' field")
    _check_forbidden(doc)
    return doc


def _num_channels(doc: dict) -> int:
    """Widest channel index actually referenced by any coupling_c<N> key in
    the document, +1; falls back to len(zones) if no coupling key is present
    at all, since MAX31856_CHANNEL_COUNT itself is not carried in the
    document and this module has no firmware tree to read it from."""
    max_idx = -1
    for z in doc.get("zones", []):
        for k in z:
            if k.startswith(V4_COUPLING_KEY_PREFIX) and k[len(V4_COUPLING_KEY_PREFIX):].isdigit():
                max_idx = max(max_idx, int(k[len(V4_COUPLING_KEY_PREFIX):]))
    if max_idx >= 0:
        return max_idx + 1
    return max(len(doc.get("zones", [])), 1)


def _convert_zone(zone: dict, source_version: int, target_version: int,
                   num_channels: int, report: ConversionReport, scope: str) -> dict:
    out = copy.deepcopy(zone)

    # --- v2 shape: nothing to do going forward (additive, unconditionally
    # emitted once present) other than reporting what a v1 target must drop.
    if target_version < 2:
        for k in sorted(V2_ZONE_KEYS):
            if k in out:
                del out[k]
                report.add(scope, k, "dropped", "not representable at BACKUP_FORMAT_VERSION 1 (added in v2)")

    # --- v3 shape (fuzzy_strength_pct): additive; drop below v3.
    if target_version < 3:
        for k in sorted(V3_ZONE_KEYS):
            if k in out:
                del out[k]
                report.add(scope, k, "dropped", "not representable below BACKUP_FORMAT_VERSION 3")

    # --- Coupling representation: the one genuinely lossy shape change.
    coupling_cells = {}
    for k in list(out.keys()):
        if _is_coupling_cell_key(k):
            coupling_cells[int(k[len(V4_COUPLING_KEY_PREFIX):])] = out[k]

    if target_version >= 4:
        # Expand a legacy single-neighbor pair into the full row, if present
        # and the row isn't already there.
        if not coupling_cells and "coupling_coeff" in zone and "coupling_neighbor_zone" in zone:
            neighbor = int(zone["coupling_neighbor_zone"])
            coeff = zone["coupling_coeff"]
            for n in range(num_channels):
                out[_coupling_cell_key(n)] = coeff if n == neighbor else 0.0
            del out["coupling_coeff"]
            del out["coupling_neighbor_zone"]
            report.add(scope, "coupling_coeff/coupling_neighbor_zone", "derived",
                       f"expanded into coupling_c0..coupling_c{num_channels - 1} "
                       f"(only channel {neighbor} was nonzero; the rest are derived zeros, "
                       "not measurements)")
    else:
        # Collapsing v4's full row into v3-or-earlier's single-pair format.
        # Best effort: v3 can express exactly one neighbor. Keep the
        # strongest-magnitude nonzero off-diagonal cell; report every other
        # nonzero cell as dropped.
        nonzero = {n: v for n, v in coupling_cells.items() if v}
        if nonzero:
            chosen_n = max(nonzero, key=lambda n: abs(nonzero[n]))
            if target_version >= 3:
                out["coupling_coeff"] = nonzero[chosen_n]
                out["coupling_neighbor_zone"] = chosen_n
                report.add(scope, f"coupling_c{chosen_n}", "kept",
                           "collapsed into coupling_coeff/coupling_neighbor_zone")
            for n in nonzero:
                if n != chosen_n:
                    report.add(scope, f"coupling_c{n}", "dropped",
                               f"BACKUP_FORMAT_VERSION {target_version} can only express one "
                               f"coupling neighbor per zone; channel {chosen_n} (larger magnitude) "
                               "was kept instead")
        for k in list(out.keys()):
            if _is_coupling_cell_key(k):
                del out[k]
        if target_version < 3:
            for k in ("coupling_coeff", "coupling_neighbor_zone"):
                if k in out:
                    del out[k]
                    report.add(scope, k, "dropped", "coupling was not representable at all below v3")

    # --- Calibration: never fabricate, never drop-and-silently-forget.
    for k in sorted(CALIBRATION_KEYS):
        if k not in zone:
            # Nothing to report: an absent calibration value is not this
            # conversion's doing, and reporting "not present" for a field
            # that never existed in the source would just be noise on every
            # zone of every conversion. Presence is checked explicitly by
            # test_never_fabricates_calibration instead.
            continue
        # Present in source: always carried through untouched, regardless of
        # target version -- there is no version at which this key is
        # unrepresentable, and this module never derives a different value
        # for it.
        report.add(scope, k, "kept", "calibration value carried through unchanged; never re-derived")

    return out


def _convert_profile(profile: dict, source_version: int, target_version: int,
                      report: ConversionReport, scope: str) -> dict:
    # No profile-level field has ever been gated on BACKUP_FORMAT_VERSION;
    # profiles carry through unchanged at every version.
    return copy.deepcopy(profile)


def convert(doc: dict, target_version: int) -> "tuple[dict, ConversionReport]":
    """Convert a loaded (and forbidden-key-checked) package dict to
    target_version. Returns (new_doc, report). Never mutates doc."""
    source_version = int(doc["version"])
    if target_version < 1:
        raise CfgConvertError(f"target version {target_version} is not a valid BACKUP_FORMAT_VERSION (>= 1)")

    report = ConversionReport(source_version=source_version, target_version=target_version)
    num_channels = _num_channels(doc)

    out = {
        "kind": "kilnctl_backup",
        "version": target_version,
    }
    out["profiles"] = [
        _convert_profile(p, source_version, target_version, report, f"profile {p.get('id', i)}")
        for i, p in enumerate(doc.get("profiles", []))
    ]
    out["zones"] = [
        _convert_zone(z, source_version, target_version, num_channels, report,
                     f"zone {z.get('index', i)}")
        for i, z in enumerate(doc.get("zones", []))
    ]
    if "safety_tc_type" in doc:
        out["safety_tc_type"] = doc["safety_tc_type"]

    if source_version == target_version:
        report.add("document", "version", "kept", "source and target versions are identical; document unchanged")

    return out, report


# ---------------------------------------------------------------------------
# "Convert to whatever version a live board currently understands" --
# closes the "restore never hands a board a package it cannot read" half of
# the requirement. Reuses the board's own GET /api/backup/export purely to
# read its "version" field; the (possibly large) rest of that response body
# is discarded unread. No new firmware endpoint required.
# ---------------------------------------------------------------------------

def fetch_board_backup_version(host: str, timeout_s: float = DEFAULT_HTTP_TIMEOUT_S) -> int:
    url = f"http://{host}/api/backup/export"
    try:
        with urllib.request.urlopen(url, timeout=timeout_s) as resp:
            body = resp.read()
    except urllib.error.HTTPError as exc:
        raise CfgConvertError(f"board at {host} returned HTTP {exc.code} for {url}") from exc
    except urllib.error.URLError as exc:
        raise CfgConvertError(f"could not reach board at {host} ({url}): {exc.reason}") from exc
    try:
        doc = json.loads(body)
    except json.JSONDecodeError as exc:
        raise CfgConvertError(f"board at {host} returned non-JSON body from {url}") from exc
    if "version" not in doc:
        raise CfgConvertError(f"board at {host}'s own export has no 'version' field")
    return int(doc["version"])


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="kilnctrl-cfg-convert",
        description="Convert a kilnCtl backup package (GET /api/backup/export's JSON) between "
                    "BACKUP_FORMAT_VERSION values, in either direction, best effort. Forward "
                    "conversion follows firmware's own additive-field semantics; backward "
                    "conversion drops what the target version cannot express. Never fabricates "
                    "a calibration value, never reads or writes credentials.",
    )
    p.add_argument("input", help="path to the source backup JSON, or '-' for stdin")
    target = p.add_mutually_exclusive_group(required=True)
    target.add_argument("--to-version", type=int, help="target BACKUP_FORMAT_VERSION, e.g. 2")
    target.add_argument("--to-board", metavar="HOST",
                        help="convert to whatever BACKUP_FORMAT_VERSION this board's own "
                             "GET /api/backup/export currently reports, so a restore never "
                             "hands the board a package it cannot read (host:port or bare host, "
                             "e.g. 192.168.1.42 or 192.168.4.1)")
    p.add_argument("-o", "--output", help="path to write the converted package (default: stdout)")
    p.add_argument("--report", help="path to write the JSON conversion report (default: printed to stderr)")
    p.add_argument("--quiet", action="store_true", help="suppress the human-readable report on stderr")
    return p


def main(argv: Optional[list] = None) -> int:
    args = _build_arg_parser().parse_args(argv)

    raw = sys.stdin.read() if args.input == "-" else open(args.input, "r", encoding="utf-8").read()
    try:
        doc = load_package(raw)
        target_version = args.to_version if args.to_version is not None else fetch_board_backup_version(args.to_board)
        out_doc, report = convert(doc, target_version)
    except CfgConvertError as exc:
        print(f"cfg_convert: error: {exc}", file=sys.stderr)
        return 1

    out_text = json.dumps(out_doc, indent=2) + "\n"
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(out_text)
    else:
        sys.stdout.write(out_text)

    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            json.dump(report.to_dict(), f, indent=2)
            f.write("\n")
    if not args.quiet:
        print(report.text(), file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
