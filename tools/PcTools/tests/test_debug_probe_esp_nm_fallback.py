#!/usr/bin/env python3
"""Tests for PEER_ESP's nm-tool resolution in debug_probe.py, added
2026-09-14 alongside docs/audits/esp_coredump_extraction_2026-09-14.md.

Before this fix, PEER_ESP had no ``nm_tool`` configured at all, so
``debug_read_symbol(peer="esp")``/``symbol_table("esp", ...)`` failed
unconditionally with "has no nm tool configured for symbol lookup" even on a
machine where the Xtensa toolchain's nm genuinely exists (just not on PATH --
the ESP-IDF tools installer puts it under
``~/.espressif/tools/xtensa-esp-elf/<version>/xtensa-esp-elf/bin/``, not PATH).

These tests exercise ``symbol_table()``'s fallback path with a fake ``nm`` a
few lines long (no real toolchain, no real ELF, no live board) and prove the
negative case still fails loudly: with the fallback finder returning nothing
AND nothing on PATH, symbol_table("esp", ...) must still raise
FileNotFoundError, not silently succeed or return something bogus.
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import debug_probe  # noqa: E402


class SymbolTableEspNmFallbackTest(unittest.TestCase):
    def setUp(self):
        debug_probe._SYMBOL_CACHE.clear()
        self._tmp = tempfile.NamedTemporaryFile(suffix=".elf", delete=False)
        self._tmp.write(b"not a real elf, just needs to exist")
        self._tmp.close()

    def tearDown(self):
        os.unlink(self._tmp.name)

    def test_falls_back_to_finder_when_not_on_path(self):
        """shutil.which() finds nothing (nothing on PATH) but the ESP-specific
        fallback finder resolves a path -- symbol_table() must use it rather
        than raising, and must actually invoke that resolved executable."""
        fake_nm = os.path.join(os.path.dirname(self._tmp.name), "fake-xtensa-nm.exe")

        def fake_run(cmd, **kwargs):
            self.assertEqual(cmd[0], fake_nm)
            return subprocess.CompletedProcess(cmd, 0, stdout="1000 4 D my_symbol\n", stderr="")

        with unittest.mock.patch.object(debug_probe.shutil, "which", return_value=None), \
             unittest.mock.patch.dict(
                 debug_probe._NM_FALLBACK_FINDERS, {debug_probe.PEER_ESP: lambda: fake_nm}
             ), \
             unittest.mock.patch.object(debug_probe.subprocess, "run", side_effect=fake_run):
            table = debug_probe.symbol_table(debug_probe.PEER_ESP, self._tmp.name)

        self.assertIn("my_symbol", table)
        self.assertEqual(table["my_symbol"], (0x1000, 4))

    def test_raises_loudly_when_neither_path_nor_fallback_find_it(self):
        """Negative test: nothing on PATH AND the fallback finder also comes
        up empty -- must fail closed (FileNotFoundError naming the tool),
        never silently return an empty/bogus symbol table. Proves the
        fallback wiring can actually fail, not just always succeed."""
        with unittest.mock.patch.object(debug_probe.shutil, "which", return_value=None), \
             unittest.mock.patch.dict(
                 debug_probe._NM_FALLBACK_FINDERS, {debug_probe.PEER_ESP: lambda: None}
             ):
            with self.assertRaises(FileNotFoundError) as ctx:
                debug_probe.symbol_table(debug_probe.PEER_ESP, self._tmp.name)
        self.assertIn("xtensa-esp-elf-nm", str(ctx.exception))

    def test_pico_peer_unaffected_by_esp_fallback_absence(self):
        """The fallback dict only has an entry for PEER_ESP -- PEER_PICO must
        still raise its own plain FileNotFoundError (no fallback exists for
        it in this dict; its own _find_arm_nm_exe() logic lives in
        resolve_symbol(), a separate function) rather than picking up the
        ESP's fallback by accident."""
        self.assertNotIn(debug_probe.PEER_PICO, debug_probe._NM_FALLBACK_FINDERS)
        with unittest.mock.patch.object(debug_probe.shutil, "which", return_value=None):
            with self.assertRaises(FileNotFoundError):
                debug_probe.symbol_table(debug_probe.PEER_PICO, self._tmp.name)


if __name__ == "__main__":
    unittest.main()
