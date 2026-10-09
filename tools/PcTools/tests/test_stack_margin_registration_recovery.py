"""Recovery-image fixture tests for tools/check_stack_margin_registration.ps1."""
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[3] / "tools" / "check_stack_margin_registration.ps1"


def _run(recovery_dir):
    return subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT),
         "-RecoveryDir", str(recovery_dir)],
        capture_output=True, text=True, timeout=300)


pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="PowerShell check")


def test_unregistered_recovery_task_fails(tmp_path):
    (tmp_path / "x.c").write_text(
        'void f(void){ xTaskCreate(t, "rogue_task", 2048, NULL, 3, NULL); }\n')
    r = _run(tmp_path)
    assert r.returncode != 0
    assert "rogue_task" in r.stdout + r.stderr


def test_reported_recovery_task_passes(tmp_path):
    (tmp_path / "x.c").write_text(
        'void f(void){ xTaskCreate(t, "ok_task", 2048, NULL, 3, &h);\n'
        ' n = uxTaskGetStackHighWaterMark(h); }\n')
    r = _run(tmp_path)
    assert r.returncode == 0, r.stdout + r.stderr
