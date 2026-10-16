"""IRAMGATE LOW-3: check_kilnfw_dram_bss_budget.py fails when .dram0.data is absent and honours an explicit 0 ceiling."""
import importlib.util
import os
import sys
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / "firmware" / "KilnFW" / "App" / "test" / "check_kilnfw_dram_bss_budget.py"


def _load():
    spec = importlib.util.spec_from_file_location("dram_bss_budget_under_test", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run(sizes, *extra):
    m = _load()
    argv = ["x", "--elf", str(SCRIPT), *extra]  # any existing file: the ELF itself is never parsed
    with mock.patch.object(sys, "argv", argv), \
         mock.patch.object(m.base, "find_objdump", return_value="objdump"), \
         mock.patch.object(m, "section_sizes", return_value=sizes):
        return m.main()


def test_passes_when_both_sections_are_under_their_ceilings():
    assert _run({".dram0.bss": 1000, ".dram0.data": 1000}) == 0


def test_missing_data_section_fails_instead_of_reading_zero(capsys):
    assert _run({".dram0.bss": 1000}) == 1
    assert ".dram0.data section not present" in capsys.readouterr().out


def test_explicit_zero_data_ceiling_is_honoured():
    assert _run({".dram0.bss": 1000, ".dram0.data": 1000}, "--data-ceiling-bytes", "0") == 1


def test_explicit_zero_bss_ceiling_is_honoured():
    assert _run({".dram0.bss": 1000, ".dram0.data": 1000}, "--ceiling-bytes", "0") == 1
