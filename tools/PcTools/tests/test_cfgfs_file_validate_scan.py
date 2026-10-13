"""Source scans for POST /api/cfgfs/file: audit L6 (reply escapes the client-supplied
name) and full_board_backup.py always sending raw=1."""
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


def test_backup_always_sends_raw_flag():
    import inspect
    src = inspect.getsource(fbb.restore_cfgfs_files)
    assert 'url += "&raw=1"' in src
    assert "CFGFS_VALIDATED_FILES" not in src
    assert not hasattr(fbb, "CFGFS_VALIDATED_FILES")
