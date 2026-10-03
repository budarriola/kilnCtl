#!/usr/bin/env python3
"""readiness_ct_channel_map_mirror_drift_check.py -- guards against the exact
defect fixed 2026-09-09: "is ct_channel_map[0..2] required for commissioning"
is decided independently in TWO places, on two different processors, with
nothing forcing them to agree:

  1. firmware/SaftyFW/src/config_params.c's config_params_all_required_set()
     -- feeds the Pico's own `calibration_missing` / commissioned:true wire
     bit.
  2. firmware/KilnFW/App/drivers/http/readiness_http.h's
     readiness_param_required_for_commissioning() -- feeds the ESP's
     /api/readiness item 10a ("Safety processor commissioned").

commit b5cb83a4 fixed #1 so a summed-CT board (ct_topology == SUMMED) no
longer needs ct_channel_map, since summed mode's S14/S15 read
relay_now_mask by zone id and never consult that map. #2 was NOT updated in
that pass, reproducing the identical "permanently uncommissionable" bug one
level up -- the ESP kept counting ct_channel_map[0..2] as unset-forever on a
board the Pico itself already reported commissioned:true for
(docs/audits/commissioning_gap_and_no_heat_2026-09-09.md). This is the
repo's documented "reset one side of a pair" / mirror-drift bug class
(CLAUDE.md): two pieces of state joined by a semantic contract, expressed
nowhere as a single shared definition.

A full textual mirror check (the approach_rate_cap_mirror_drift_check.py
style: normalize two code fragments and diff them line-for-line) is not
honest here -- the two sites have genuinely different shapes (one is an
accumulated required-bitmask over a whole record; the other is a per-param-id
switch keyed off wire ids with no shared struct). What both sides MUST agree
on, structurally, is a much narrower thing: the single boolean "is
ct_channel_map required", as a function of (ct_installed != 0, ct_topology
== SUMMED). This check extracts each side's own boolean expression for that
question by regex, evaluates both across the full 2x2 truth table of
(ct_installed, ct_topology), and fails if they ever disagree -- an
extract-and-evaluate technique rather than extract-and-diff, because the two
expressions are not expected to be byte-identical, only truth-table-
identical.

This is the honest, narrower kind of check CLAUDE.md's mirror-drift-class
writeup calls for: pinned to one concrete, now-stable pair, not a general
syntactic rule (a general rule strict enough to catch this would also flag
the majority of ordinary one-sided code in both files).

Usage: python readiness_ct_channel_map_mirror_drift_check.py [repo_root]
Exit 0: both sides agree on all 4 (ct_installed, ct_topology) combinations.
Exit 1: they disagree on at least one combination, or either expression could
        not be located at all (fail closed -- same contract as this
        directory's other *_mirror_drift_check.py / *_drift_check.py
        scripts: a regex that stops matching its target is a failure, not a
        vacuous pass).
"""
import re
import sys
from pathlib import Path

PICO_REL = "firmware/SaftyFW/src/config_params.c"
ESP_REL = "firmware/KilnFW/App/drivers/http/readiness_http.h"

# config_params_all_required_set()'s ct_channel_map gate:
#   if (rec->ct_installed != 0u && rec->ct_topology != CONFIG_STORE_CT_TOPOLOGY_SUMMED) {
#       required = (uint32_t)(required | CONFIG_STORE_SET_CT_CHANNEL_MAP);
#
# The cast width is matched loosely on purpose: fields_set widened from
# uint16_t to uint32_t when CONFIG_STORE_FORMAT_VERSION went 2 -> 3
# (docs/CT_CHANNEL_MASK.md step 2), and pinning the old width here
# turned a real mirror check into an extraction failure. Accept either.
#   }
PICO_RE = re.compile(
    r"if \((rec->ct_installed[^)]*?)\)\s*\{\s*\n\s*required = \(uint(?:16|32)_t\)\(required \| CONFIG_STORE_SET_CT_CHANNEL_MAP\);",
    re.DOTALL,
)

# readiness_param_required_for_commissioning()'s ct_channel_map case group:
#   case READINESS_PARAM_ID_CT_CHANNEL_MAP_0:
#   case READINESS_PARAM_ID_CT_CHANNEL_MAP_1:
#   case READINESS_PARAM_ID_CT_CHANNEL_MAP_2:
#       return ct_installed_value != 0u && ct_topology_value == 0u;
ESP_RE = re.compile(
    r"case READINESS_PARAM_ID_CT_CHANNEL_MAP_0:\s*\n\s*case READINESS_PARAM_ID_CT_CHANNEL_MAP_1:\s*\n"
    r"\s*case READINESS_PARAM_ID_CT_CHANNEL_MAP_2:\s*\n\s*return (ct_installed_value[^;]*);",
)


def to_python_expr(c_expr: str) -> str:
    """Translate the narrow vocabulary these two expressions use into a
    Python-evaluable boolean expression over `installed` (0/1) and
    `topology` (0=PER_ZONE, 1=SUMMED). Fails loudly (raises) on any token it
    does not recognize, rather than silently evaluating something else."""
    e = c_expr.strip()
    e = e.replace("rec->ct_installed", "installed")
    e = e.replace("ct_installed_value", "installed")
    e = e.replace("rec->ct_topology", "topology")
    e = e.replace("ct_topology_value", "topology")
    e = e.replace("CONFIG_STORE_CT_TOPOLOGY_SUMMED", "1")
    e = e.replace("0u", "0")
    e = e.replace("&&", "and")
    e = e.strip().rstrip(";")
    allowed = set("installed topoogy!=and() 0123456789")  # noqa: loose charset check below is the real gate
    # Real safety gate: only known identifiers/operators may remain.
    stripped = e
    for tok in ("installed", "topology", "!=", "==", "and", "(", ")", "0", "1", " "):
        stripped = stripped.replace(tok, "")
    if stripped != "":
        raise ValueError(f"unrecognized token(s) {stripped!r} in expression {c_expr!r}")
    return e


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    pico_path = repo_root / PICO_REL
    esp_path = repo_root / ESP_REL
    for label, path in (("Pico", pico_path), ("ESP", esp_path)):
        if not path.is_file():
            print("READINESS CT_CHANNEL_MAP MIRROR DRIFT CHECK: FAILED (setup)")
            print(f"  {label} file not found: {path}")
            return 1

    pico_text = pico_path.read_text(encoding="utf-8")
    esp_text = esp_path.read_text(encoding="utf-8")

    pico_match = PICO_RE.search(pico_text)
    if not pico_match:
        print("READINESS CT_CHANNEL_MAP MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate the ct_channel_map required-mask gate in {PICO_REL} --")
        print("  update this check's PICO_RE rather than letting it pass vacuously.")
        return 1

    esp_match = ESP_RE.search(esp_text)
    if not esp_match:
        print("READINESS CT_CHANNEL_MAP MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate the ct_channel_map case group in {ESP_REL} --")
        print("  update this check's ESP_RE rather than letting it pass vacuously.")
        return 1

    try:
        pico_expr = to_python_expr(pico_match.group(1))
        esp_expr = to_python_expr(esp_match.group(1))
    except ValueError as exc:
        print("READINESS CT_CHANNEL_MAP MIRROR DRIFT CHECK: FAILED (translation)")
        print(f"  {exc}")
        return 1

    mismatches = []
    for installed in (0, 1):
        for topology in (0, 1):
            env = {"installed": installed, "topology": topology}
            pico_result = eval(pico_expr, {"__builtins__": {}}, env)  # noqa: S307 -- vetted charset above
            esp_result = eval(esp_expr, {"__builtins__": {}}, env)  # noqa: S307
            if bool(pico_result) != bool(esp_result):
                mismatches.append((installed, topology, pico_result, esp_result))

    if mismatches:
        print("READINESS CT_CHANNEL_MAP MIRROR DRIFT CHECK: FAILED")
        print(f"  {PICO_REL}'s config_params_all_required_set() and")
        print(f"  {ESP_REL}'s readiness_param_required_for_commissioning()")
        print("  disagree on whether ct_channel_map is required for at least one")
        print("  (ct_installed, ct_topology) combination:")
        for installed, topology, pico_result, esp_result in mismatches:
            print(
                f"    ct_installed={installed} ct_topology={topology}: "
                f"Pico says required={bool(pico_result)}, ESP says required={bool(esp_result)}"
            )
        print(f"  Pico expression: {pico_expr}")
        print(f"  ESP  expression: {esp_expr}")
        return 1

    print(
        "READINESS CT_CHANNEL_MAP MIRROR DRIFT CHECK: OK "
        f"(Pico `{pico_expr}` and ESP `{esp_expr}` agree on all 4 (ct_installed, ct_topology) combinations)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
