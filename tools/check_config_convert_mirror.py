#!/usr/bin/env python3
"""check_config_convert_mirror.py -- mirror-drift check for
tools/PcTools/src/kilnctrl/config_convert.py's profile_blob store.

config_convert.py hand-mirrors profiles_http.c's PROFILE_VERSION and its
struct shapes (profile_persisted_t/profile_t/profile_segment_t/
profile_on_off_rule_t). A hand-maintained mirror with no drift check is
exactly the class of defect this repo's *_mirror_drift_check.py scripts
exist to catch (see cfg_convert_field_mirror_drift_check.py for the same
technique applied to the backup document's BACKUP_FORMAT_VERSION/zone
keys). This check is deliberately narrow: it does not re-verify byte
offsets (that would require a C parser this repo does not have); it
verifies the one thing a version bump can silently break without any other
signal -- that config_convert.py's PROFILE_VERSION constant, and its
knowledge of which version introduced on/off rules and relay/IO segments,
still agrees with firmware.

WHAT IS COMPARED:
  - PROFILE_VERSION: firmware's #define in profiles_http.c against
    config_convert.py's own module constant of the same name.
  - PROFILE_NAME_MAX_LEN / PROFILE_MAX_SEGMENTS / PROFILE_MAX_ON_OFF_RULES:
    firmware's #defines in profiles_types.h against config_convert.py's own
    module constants of the same names.

Fails closed (same convention as every other check in this family): if
either source file's shape no longer matches this check's own extraction
regexes, that is a FAILURE, not a silent pass.

Usage: python tools/check_config_convert_mirror.py [repo_root]
"""
import re
import sys
from pathlib import Path

PROFILES_HTTP_REL = "firmware/KilnFW/App/drivers/http/profiles_http.c"
PROFILES_TYPES_REL = "firmware/KilnFW/App/drivers/persist/profiles_types.h"
CONFIG_CONVERT_REL = "tools/PcTools/src/kilnctrl/config_convert.py"

FW_DEFINE_RE = re.compile(r"#define\s+(PROFILE_VERSION|PROFILE_NAME_MAX_LEN|PROFILE_MAX_SEGMENTS|"
                          r"PROFILE_MAX_ON_OFF_RULES)\s+(\d+)")
TOOL_CONST_RE = re.compile(r"^(PROFILE_VERSION|PROFILE_NAME_MAX_LEN|PROFILE_MAX_SEGMENTS|"
                           r"PROFILE_MAX_ON_OFF_RULES)\s*=\s*(\d+)", re.MULTILINE)

CONSTANTS = ("PROFILE_VERSION", "PROFILE_NAME_MAX_LEN", "PROFILE_MAX_SEGMENTS", "PROFILE_MAX_ON_OFF_RULES")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]

    paths = {
        "profiles_http": repo_root / PROFILES_HTTP_REL,
        "profiles_types": repo_root / PROFILES_TYPES_REL,
        "tool": repo_root / CONFIG_CONVERT_REL,
    }
    for label, path in paths.items():
        if not path.is_file():
            print("CONFIG-CONVERT MIRROR CHECK: FAILED (setup)")
            print(f"  {label} file not found: {path}")
            return 1

    fw_defines = {}
    fw_defines.update(FW_DEFINE_RE.findall(strip_comments(paths["profiles_http"].read_text(encoding="utf-8"))))
    fw_defines.update(FW_DEFINE_RE.findall(strip_comments(paths["profiles_types"].read_text(encoding="utf-8"))))
    fw_defines = {k: int(v) for k, v in fw_defines.items()}

    tool_text = paths["tool"].read_text(encoding="utf-8")
    tool_consts = {k: int(v) for k, v in TOOL_CONST_RE.findall(tool_text)}

    failures = []
    for name in CONSTANTS:
        if name not in fw_defines:
            failures.append(f"could not extract #define {name} from firmware -- update this check's "
                            "FW_DEFINE_RE rather than letting it pass vacuously")
        elif name not in tool_consts:
            failures.append(f"could not extract {name} from {CONFIG_CONVERT_REL} -- update this check's "
                            "TOOL_CONST_RE rather than letting it pass vacuously")
        elif fw_defines[name] != tool_consts[name]:
            failures.append(f"{name}: firmware has {fw_defines[name]}, config_convert.py has "
                            f"{tool_consts[name]} -- update config_convert.py's struct tables (and its "
                            "PROFILE_VERSION-gated conversion logic) to match")

    if failures:
        print("CONFIG-CONVERT MIRROR CHECK: FAILED")
        for f in failures:
            print(f"  - {f}")
        return 1

    print(f"CONFIG-CONVERT MIRROR CHECK: OK ({len(tool_consts)} constants match between firmware and "
         "config_convert.py)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
