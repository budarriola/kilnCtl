#!/usr/bin/env python3
"""ui_test_click_result_mirror_drift_check.py -- ties the firmware's
CLICK_BY_NAME result-code wire enum to its two independent PC-side mirrors,
with nothing today forcing all three to agree:

  1. firmware/KilnFW/App/drivers/common/uart_task_ids.h's
     `#define UI_TEST_CLICK_*` constants -- the actual wire values
     kiln_ui_click_result_t is cast into on the byte1 result field of a
     CLICK_BY_NAME response (see that header's own comment just above them:
     "kept as its own wire enum ... so a future reorder of the C enum can't
     silently renumber the wire value underneath it").
  2. tools/PcTools/src/kilnctrl/protocol.py's `UI_TEST_CLICK_*` Python
     constants -- the PC side's copy of the same wire values, maintained by
     hand rather than generated from the header.
  3. tools/PcTools/src/kilnctrl/ui_test_client.py's `_CLICK_RESULT_NAMES`
     dict -- the decoder table turning a received result byte into a human
     string; every constant protocol.py defines must have an entry here, or
     a genuinely new wire value falls into whatever the decoder's own
     "unknown result" fallback does, silently.

f3f2f4cd added UI_TEST_CLICK_INJECT_FAILED (0x06) to all three sites by
hand in the same commit; nothing catches a future change that only touches
one or two of them. This is the repo's documented "reset one side of a
pair" / mirror-drift bug class (CLAUDE.md) applied to a three-way constant
table rather than a two-way boolean or code fragment -- closer in shape to
safety_cfg_param_table_mirror_drift_check.py than to
approach_rate_cap_mirror_drift_check.py's line-for-line body diff, since
what must agree here is a NAME -> VALUE table, not a chunk of arithmetic.

WHAT IS COMPARED:
  - The set of (name, numeric value) pairs extracted from uart_task_ids.h's
    `#define UI_TEST_CLICK_<NAME> 0x<HEX>u` lines.
  - The set of (name, numeric value) pairs extracted from protocol.py's
    `UI_TEST_CLICK_<NAME> = <DEC>` lines.
  - The set of NAMEs referenced as dict keys (`UI_TEST_CLICK_<NAME>:`)
    inside ui_test_client.py's `_CLICK_RESULT_NAMES = { ... }` block.

A name present on only one side, or present on both with a different
numeric value, is a FAIL. A name protocol.py defines but the decoder table
never references is also a FAIL (an undecoded result code). Extra decoder
entries are impossible by construction (the decoder can only reference
names protocol.py actually exports, since ui_test_client.py imports them by
name) but a missing extraction match on any side still fails closed rather
than passing vacuously, per this repo's standing rule for this check class.

Usage: python ui_test_click_result_mirror_drift_check.py [repo_root]
Exit 0: all three sides agree.
Exit 1: they disagree, or any of the three fragments could not be located
        at all (fail closed).
"""
import re
import sys
from pathlib import Path

FW_REL = "firmware/KilnFW/App/drivers/common/uart_task_ids.h"
PROTOCOL_REL = "tools/PcTools/src/kilnctrl/protocol.py"
CLIENT_REL = "tools/PcTools/src/kilnctrl/ui_test_client.py"

FW_DEFINE_RE = re.compile(
    r"#define\s+UI_TEST_CLICK_(?P<name>[A-Z_]+)\s+0x(?P<hex>[0-9a-fA-F]+)u\b"
)
PROTOCOL_ASSIGN_RE = re.compile(
    r"^UI_TEST_CLICK_(?P<name>[A-Z_]+)\s*=\s*(?P<dec>\d+)\s*$", re.MULTILINE
)
DECODER_BLOCK_RE = re.compile(
    r"_CLICK_RESULT_NAMES\s*=\s*\{(?P<body>.*?)\n\}", re.DOTALL
)
DECODER_KEY_RE = re.compile(r"UI_TEST_CLICK_(?P<name>[A-Z_]+)\s*:")


def load(path: Path, label: str, errors: list) -> str:
    if not path.is_file():
        errors.append(f"{label} file not found: {path}")
        return ""
    return path.read_text(encoding="utf-8")


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    setup_errors: list = []
    fw_text = load(repo_root / FW_REL, "firmware", setup_errors)
    protocol_text = load(repo_root / PROTOCOL_REL, "protocol.py", setup_errors)
    client_text = load(repo_root / CLIENT_REL, "ui_test_client.py", setup_errors)

    if setup_errors:
        print("UI_TEST_CLICK RESULT-CODE MIRROR DRIFT CHECK: FAILED (setup)")
        for err in setup_errors:
            print(f"  {err}")
        return 1

    fw_table = {m.group("name"): int(m.group("hex"), 16) for m in FW_DEFINE_RE.finditer(fw_text)}
    protocol_table = {
        m.group("name"): int(m.group("dec")) for m in PROTOCOL_ASSIGN_RE.finditer(protocol_text)
    }

    if not fw_table:
        print("UI_TEST_CLICK RESULT-CODE MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  No '#define UI_TEST_CLICK_<NAME> 0x<HEX>u' lines found in {FW_REL} --")
        print("  update this check's FW_DEFINE_RE rather than letting it pass vacuously.")
        return 1

    if not protocol_table:
        print("UI_TEST_CLICK RESULT-CODE MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  No 'UI_TEST_CLICK_<NAME> = <DEC>' lines found in {PROTOCOL_REL} --")
        print("  update this check's PROTOCOL_ASSIGN_RE rather than letting it pass vacuously.")
        return 1

    decoder_match = DECODER_BLOCK_RE.search(client_text)
    if not decoder_match:
        print("UI_TEST_CLICK RESULT-CODE MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate the _CLICK_RESULT_NAMES = {{ ... }} block in {CLIENT_REL} --")
        print("  update this check's DECODER_BLOCK_RE rather than letting it pass vacuously.")
        return 1
    decoder_names = {m.group("name") for m in DECODER_KEY_RE.finditer(decoder_match.group("body"))}
    if not decoder_names:
        print("UI_TEST_CLICK RESULT-CODE MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  No 'UI_TEST_CLICK_<NAME>:' decoder keys found inside _CLICK_RESULT_NAMES in {CLIENT_REL} --")
        print("  update this check's DECODER_KEY_RE rather than letting it pass vacuously.")
        return 1

    fw_names = set(fw_table)
    protocol_names = set(protocol_table)

    problems = []

    only_fw = sorted(fw_names - protocol_names)
    if only_fw:
        problems.append(
            f"present in {FW_REL} but not {PROTOCOL_REL}: "
            + ", ".join(f"UI_TEST_CLICK_{n}=0x{fw_table[n]:02x}" for n in only_fw)
        )
    only_protocol = sorted(protocol_names - fw_names)
    if only_protocol:
        problems.append(
            f"present in {PROTOCOL_REL} but not {FW_REL}: "
            + ", ".join(f"UI_TEST_CLICK_{n}={protocol_table[n]}" for n in only_protocol)
        )

    value_mismatches = []
    for name in sorted(fw_names & protocol_names):
        if fw_table[name] != protocol_table[name]:
            value_mismatches.append(
                f"UI_TEST_CLICK_{name}: firmware=0x{fw_table[name]:02x} ({fw_table[name]}) "
                f"protocol.py={protocol_table[name]}"
            )
    if value_mismatches:
        problems.append("numeric value mismatches: " + "; ".join(value_mismatches))

    undecoded = sorted(protocol_names - decoder_names)
    if undecoded:
        problems.append(
            f"defined in {PROTOCOL_REL} but never decoded in {CLIENT_REL}'s "
            "_CLICK_RESULT_NAMES: " + ", ".join(f"UI_TEST_CLICK_{n}" for n in undecoded)
        )

    stale_decoder = sorted(decoder_names - protocol_names)
    if stale_decoder:
        problems.append(
            f"decoded in {CLIENT_REL}'s _CLICK_RESULT_NAMES but not defined in "
            f"{PROTOCOL_REL}: " + ", ".join(f"UI_TEST_CLICK_{n}" for n in stale_decoder)
        )

    if problems:
        print("UI_TEST_CLICK RESULT-CODE MIRROR DRIFT CHECK: FAILED")
        print(f"  {FW_REL}, {PROTOCOL_REL}, and {CLIENT_REL} disagree on the")
        print("  UI_TEST_CLICK_* result-code table:")
        for p in problems:
            print(f"    - {p}")
        return 1

    print(
        "UI_TEST_CLICK RESULT-CODE MIRROR DRIFT CHECK: OK "
        f"({len(fw_names)} codes agree by name and value across firmware, protocol.py, "
        "and the ui_test_client.py decoder)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
