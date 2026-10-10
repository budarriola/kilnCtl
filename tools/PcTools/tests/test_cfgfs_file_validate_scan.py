"""Source scans for POST /api/cfgfs/file: audit L6 (reply escapes the client-supplied
name) and parity between the firmware validator table and full_board_backup.py."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DRV = ROOT / "firmware/KilnFW/App/drivers"
sys.path.insert(0, str(ROOT / "tools/PcTools/scripts"))
import full_board_backup as fbb  # noqa: E402


def test_cfgfs_post_reply_escapes_name():
    src = (DRV / "http/diagnostics_http.c").read_text(encoding="utf-8", errors="replace")
    m = re.search(r'"\{\\"ok\\":true,\\"name\\":\\"%s\\",\\"size_bytes\\":%u\}",\s*([A-Za-z_]+),', src)
    assert m, "cfgfs POST success reply not found"
    assert m.group(1) == "name_esc", "reply must print the escaped name"
    assert re.search(r"kiln_json_escape_ctl\(\s*name,\s*name_esc", src)


def test_backup_validated_set_matches_firmware_table():
    src = (DRV / "persist/cfgfs_file_validate.c").read_text(encoding="utf-8", errors="replace")
    body = src[src.index("PREF_FILE_RULES[] = {"):]
    body = body[: body.index("};")]
    consts = dict(re.findall(r'#define\s+(\w+)\s+"([^"]+)"', "".join(
        p.read_text(encoding="utf-8", errors="replace") for p in DRV.rglob("*.h"))))
    names = {"zones.json"}
    for tok in re.findall(r"\{\s*([A-Z0-9_]+|\"[^\"]+\")\s*,", body):
        names.add(tok.strip('"') if tok.startswith('"') else consts[tok])
    assert names == set(fbb.CFGFS_VALIDATED_FILES)
