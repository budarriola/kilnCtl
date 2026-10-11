#!/usr/bin/env python3
"""cfg_convert_field_mirror_drift_check.py -- tools/PcTools/src/kilnctrl/
cfg_convert.py hand-maintains a second copy of backup_export.c/
backup_import.c/backup_http_internal.h's field vocabulary and version
constants (see that module's own docstring for why: the PC-side converter
must know the same shape firmware's backup format uses, but must not
duplicate the ON-BOARD migration semantics CLAUDE.md/
docs/CONFIG_MIGRATION_CHAIN.md keep to one step). A hand-maintained
second copy with no drift check is exactly the class of defect this repo's
existing *_mirror_drift_check.py scripts (approach_rate_cap_mirror_drift_
check.py, power_diag_flag_mirror_drift_check.py, ...) exist to catch --
same extract-normalize-diff technique, applied here to a field-name
vocabulary and two integer constants instead of a C arithmetic fragment.

WHAT IS COMPARED:
  - BACKUP_FORMAT_VERSION / BACKUP_FORMAT_VERSION_MIN, extracted from
    backup_http_internal.h's #define lines, against cfg_convert.py's own
    module constants of the same name.
  - Every literal JSON key backup_export.c writes via
    backup_stream_printf(&s, "...\"KEY\":...", ...) for a zone entry (found
    by regex over quoted-key format-string fragments), against the union of
    cfg_convert.py's V1_ZONE_KEYS | V1_OPTIONAL_ZONE_KEYS | V2_ZONE_KEYS |
    V3_ZONE_KEYS | LEGACY_COUPLING_KEYS | KNOWN_ADDITIVE_ZONE_KEYS (with the
    coupling_c<N>/coupling_tau_c<N>/coupling_dead_time_c<N>/
    settings_source_g<N> per-cell families collapsed to their common prefix
    before comparing, since cfg_convert.py generates those programmatically
    rather than listing every index literally).

This is a vocabulary check, not a semantics check: it fails the moment
firmware starts emitting/reading a zone key cfg_convert.py has never heard
of (or cfg_convert.py claims a key firmware no longer has), or the version
constants disagree. It does NOT verify that cfg_convert.py's conversion
logic for any given key is correct -- test_cfg_convert.py covers that with
real conversion assertions. Fails closed if either source file's shape no
longer matches this check's own extraction regexes, per this repo's
standing rule for this class of check (see approach_rate_cap_mirror_drift_
check.py's docstring).

Usage: python cfg_convert_field_mirror_drift_check.py [repo_root]
"""
import re
import sys
from pathlib import Path

BACKUP_EXPORT_REL = "firmware/KilnFW/App/drivers/http/backup_export.c"
BACKUP_IMPORT_REL = "firmware/KilnFW/App/drivers/http/backup_import.c"
BACKUP_HEADER_REL = "firmware/KilnFW/App/drivers/http/backup_http_internal.h"
CFG_CONVERT_REL = "tools/PcTools/src/kilnctrl/cfg_convert.py"

# Quoted JSON keys inside a backup_stream_printf(...) call's format string,
# e.g. "\"pid_kp\":%.9g" -> pid_kp (the value's own format specifier is
# irrelevant to this regex -- it only matches up to the colon). Deliberately
# narrow (only inside this one
# call) so it does not pick up unrelated quoted strings (log messages, the
# Content-Disposition header, etc.) elsewhere in the file.
STREAM_PRINTF_CALL_RE = re.compile(r'backup_stream_printf\((.*?)\);', re.DOTALL)
# A JSON key inside the format string, key characters optionally followed by
# a literal "%u" (the coupling_c%u/coupling_tau_c%u/.../settings_source_g%u
# per-index key families embed the format specifier IN the key name itself,
# since the index is substituted at runtime), then the closing escaped quote
# and colon.
JSON_KEY_RE = re.compile(r'\\"([a-zA-Z_][a-zA-Z0-9_]*?)(?:%u)?\\"\s*:')

# Import-side field reads: backup_json_field_num(ze, "key", ...) /
# backup_json_field_opt_num(ze, "key", ...) / snprintf(ckey, ..., "key%u", j)
IMPORT_FIELD_RE = re.compile(r'backup_json_field_(?:opt_)?num\(\s*ze,\s*"([a-zA-Z0-9_]+)"')
IMPORT_INDEXED_FIELD_RE = re.compile(r'snprintf\(\s*ckey,\s*sizeof\(ckey\),\s*"([a-zA-Z0-9_]+?)%u"')

DEFINE_RE = re.compile(r'#define\s+(BACKUP_FORMAT_VERSION(?:_MIN)?)\s+(\d+)')

# Families cfg_convert.py generates programmatically (prefix, not a literal
# key) rather than listing every channel index.
INDEXED_PREFIXES = ("coupling_c", "coupling_tau_c", "coupling_dead_time_c", "settings_source_g")

# Keys that are structural (not zone tuning data) or already covered by a
# name collision with an indexed family and handled specially below.
NON_ZONE_STRUCTURAL_KEYS = {"kind", "version", "profiles", "zones", "id", "segments", "zone_mask",
                            "target_c", "ramp_c_per_hr", "dwell_min", "safety_tc_type", "update_repo",
                            # Spare-relay WP-7: the top-level aux_outputs[] array and the per-entry keys
                            # that are not also zone keys (hyst_c/min_on_s/min_off_s are). Not zone
                            # data: cfg_convert.py carries the whole array verbatim, see its convert().
                            "aux_outputs", "relay", "enabled", "tc_zone",
                            # Top-level additive blocks (relay_cycles, backup_export_prefs()) and
                            # their nested sub-keys. Not zone data: cfg_convert.py carries the
                            # top-level ones verbatim via ADDITIVE_TOP_LEVEL_KEYS.
                            "relay_cycles", "stale_or_unknown_stores", "hw_relays", "ramp_assist", "display_power",
                            "brightness_percent", "timeout_setting", "keep_on_while_firing",
                            "display_on_error", "hidden_builtin_profiles", "tz", "relay_names"}

# Generic key names that are legitimate ONLY inside one export function (sweep INFO-3). Exempting them
# globally would silently skip a future zone field of the same name; scoped, a zone key "type"/"unit"/"c"
# emitted from any other function is still compared against cfg_convert.py. Key -> enclosing function(s).
SCOPED_NON_ZONE_KEYS = {
    "c": ("backup_export_relay_cycles",),
    "unit": ("backup_export_prefs",),
    "type": ("backup_export_prefs",),
}
# Top-level definitions only (column 0, not indented): calls inside `if (...) {` must not match.
FUNC_DEF_RE = re.compile(r'^(?!\s|#)[^\n;{}]*?\b(\w+)\(\s*[^;{}]*\)\s*\{', re.MULTILINE)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def collapse_indexed(key: str) -> str:
    for prefix in INDEXED_PREFIXES:
        if key.startswith(prefix) and key[len(prefix):].isdigit():
            return prefix
    return key


def _enclosing_function(text: str, pos: int) -> str:
    name = ""
    for m in FUNC_DEF_RE.finditer(text, 0, pos):
        name = m.group(1)
    return name


def extract_export_zone_keys(text: str) -> set:
    keys = set()
    for m_call in STREAM_PRINTF_CALL_RE.finditer(text):
        func = _enclosing_function(text, m_call.start())
        for m in JSON_KEY_RE.finditer(m_call.group(1)):
            k = m.group(1)
            if func in SCOPED_NON_ZONE_KEYS.get(k, ()):
                continue
            keys.add(k)
    keys -= NON_ZONE_STRUCTURAL_KEYS
    return {collapse_indexed(k) for k in keys}


def extract_import_zone_keys(text: str) -> set:
    keys = set(IMPORT_FIELD_RE.findall(text))
    keys |= {p + "" for p in IMPORT_INDEXED_FIELD_RE.findall(text)}
    keys -= NON_ZONE_STRUCTURAL_KEYS
    return {collapse_indexed(k) for k in keys}


def extract_defines(text: str) -> dict:
    return {name: int(val) for name, val in DEFINE_RE.findall(text)}


def extract_tool_constants(text: str) -> dict:
    out = {}
    for name in ("BACKUP_FORMAT_VERSION", "BACKUP_FORMAT_VERSION_MIN"):
        m = re.search(rf'^{name}\s*=\s*(\d+)', text, re.MULTILINE)
        if m:
            out[name] = int(m.group(1))
    return out


def extract_tool_known_keys(text: str) -> set:
    """Collects every literal key string inside the KNOWN key frozenset/
    set-literal assignments this module defines. Deliberately structural
    (regex over the source, not an import+introspect of cfg_convert.py) so
    this check has no runtime dependency on the module it is checking, same
    as every other mirror-drift check in this tree reading its target as
    plain text."""
    keys = set()
    for m in re.finditer(
        r'(?:V1_ZONE_KEYS|V1_OPTIONAL_ZONE_KEYS|V2_ZONE_KEYS|V3_ZONE_KEYS|'
        r'LEGACY_COUPLING_KEYS|KNOWN_ADDITIVE_ZONE_KEYS)\s*=\s*frozenset\(\{([^}]*)\}\)',
        text, re.DOTALL,
    ):
        for lit in re.findall(r'"([a-zA-Z0-9_]+)"', m.group(1)):
            keys.add(lit)
    # The v4 coupling family is generated from V4_COUPLING_KEY_PREFIX, not
    # listed as a literal in any of the key sets above.
    m = re.search(r'V4_COUPLING_KEY_PREFIX\s*=\s*"([a-zA-Z0-9_]+)"', text)
    if m:
        keys.add(m.group(1))
    return {collapse_indexed(k) for k in keys}


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    paths = {
        "export": repo_root / BACKUP_EXPORT_REL,
        "import": repo_root / BACKUP_IMPORT_REL,
        "header": repo_root / BACKUP_HEADER_REL,
        "tool": repo_root / CFG_CONVERT_REL,
    }
    for label, path in paths.items():
        if not path.is_file():
            print("CFG-CONVERT FIELD MIRROR DRIFT CHECK: FAILED (setup)")
            print(f"  {label} file not found: {path}")
            return 1

    export_text = strip_comments(paths["export"].read_text(encoding="utf-8"))
    import_text = strip_comments(paths["import"].read_text(encoding="utf-8"))
    header_text = strip_comments(paths["header"].read_text(encoding="utf-8"))
    tool_text = paths["tool"].read_text(encoding="utf-8")

    fw_defines = extract_defines(header_text)
    tool_consts = extract_tool_constants(tool_text)
    failures = []

    for name in ("BACKUP_FORMAT_VERSION", "BACKUP_FORMAT_VERSION_MIN"):
        if name not in fw_defines:
            failures.append(f"could not extract #define {name} from {BACKUP_HEADER_REL} -- "
                            "update this check's DEFINE_RE rather than letting it pass vacuously")
        elif name not in tool_consts:
            failures.append(f"could not extract {name} from {CFG_CONVERT_REL} -- "
                            "update this check's extract_tool_constants() rather than letting it "
                            "pass vacuously")
        elif fw_defines[name] != tool_consts[name]:
            failures.append(f"{name}: firmware has {fw_defines[name]}, cfg_convert.py has "
                            f"{tool_consts[name]} -- update cfg_convert.py to match")

    export_keys = extract_export_zone_keys(export_text)
    import_keys = extract_import_zone_keys(import_text)
    fw_keys = export_keys | import_keys
    if not fw_keys:
        failures.append(f"extracted zero zone keys from {BACKUP_EXPORT_REL}/{BACKUP_IMPORT_REL} -- "
                        "this check's extraction regexes no longer match; update them rather than "
                        "letting this pass vacuously")

    tool_keys = extract_tool_known_keys(tool_text)
    if not tool_keys:
        failures.append(f"extracted zero known keys from {CFG_CONVERT_REL} -- this check's "
                        "extract_tool_known_keys() no longer matches; update it rather than letting "
                        "this pass vacuously")

    missing_in_tool = sorted(fw_keys - tool_keys)
    extra_in_tool = sorted(tool_keys - fw_keys)
    if missing_in_tool:
        failures.append("firmware emits/reads zone key(s) cfg_convert.py has never heard of: "
                        f"{missing_in_tool} -- add them to cfg_convert.py's KNOWN_ADDITIVE_ZONE_KEYS "
                        "(or the appropriate version-gated set) and to its conversion logic")
    if extra_in_tool:
        failures.append(f"cfg_convert.py claims zone key(s) firmware no longer emits/reads: "
                        f"{extra_in_tool} -- update cfg_convert.py, it is testing a shape firmware "
                        "does not ship")

    if failures:
        print("CFG-CONVERT FIELD MIRROR DRIFT CHECK: FAILED")
        for f in failures:
            print(f"  - {f}")
        return 1

    print(f"CFG-CONVERT FIELD MIRROR DRIFT CHECK: OK ({len(fw_keys)} zone keys, "
         f"{len(tool_consts)} version constants match between firmware and cfg_convert.py)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
