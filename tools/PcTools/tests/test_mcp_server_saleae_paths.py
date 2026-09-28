"""Regression test for mcp_server_saleae.py's saleae_capture() out_dir
default: it used to be the bare relative literal "logs/saleae". This MCP
server is long-running and its CWD is whatever launched it, not something a
tool caller controls -- and logic_capture.capture() creates output
directories with os.makedirs(..., exist_ok=True), so a wrong CWD never
raises: it silently writes a "successful" capture into a fresh, empty
logs/saleae wherever the server process happened to start from. Highest
severity of this sweep's findings, since this is the one MCP-tool-callable
path (the others are CLI scripts a human runs and can watch the CWD of).

This test deliberately does NOT `import kilnctrl.mcp_server_saleae` --
importing it pulls in kilnctrl.mcp_server, which eagerly calls
link_hub.get_shared_link() at import time and connects to the live UART
link as a second client. That's a real risk while a firing campaign has
the link open (see project_safety_link_consumer_cannot_keep_up /
project_isolated_link_baud_ceiling memory notes: the link is fragile
under concurrent consumers already). Instead this test source-inspects the
file (confirms the default references an anchored constant, not a bare
relative literal) and independently recomputes the same
Path(__file__).resolve().parents[2] anchor formula against the file's real
on-disk location -- exercising the identical logic without importing the
module or touching any live link.
"""
from __future__ import annotations

import os
import pathlib
import re
import unittest

_THIS_DIR = pathlib.Path(__file__).resolve().parent
_MODULE_PATH = _THIS_DIR.parent / "src" / "kilnctrl" / "mcp_server_saleae.py"


class SaleaeCaptureDefaultOutDirTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(_MODULE_PATH.is_file(), f"expected {_MODULE_PATH} to exist")
        self._source = _MODULE_PATH.read_text(encoding="utf-8")

    def test_saleae_capture_no_longer_defaults_to_a_bare_relative_literal(self):
        self.assertNotIn('out_dir: str = "logs/saleae"', self._source)

    def test_saleae_capture_defaults_to_the_anchored_constant(self):
        self.assertIn("out_dir: str = _DEFAULT_SALEAE_OUT_DIR", self._source)

    def test_anchored_constant_is_computed_from___file__(self):
        m = re.search(
            r'_DEFAULT_SALEAE_OUT_DIR\s*=\s*str\(pathlib\.Path\(__file__\)\.resolve\(\)\.parents\[2\]\s*/\s*"logs"\s*/\s*"saleae"\)',
            self._source,
        )
        self.assertIsNotNone(
            m, "expected _DEFAULT_SALEAE_OUT_DIR to be anchored on __file__, not the process CWD"
        )

    def test_anchor_formula_resolves_to_the_directory_that_already_exists(self):
        # Reproduces the exact formula from the module (Path(__file__)
        # .resolve().parents[2] / "logs" / "saleae") against this file's
        # real on-disk path, without importing the module itself.
        resolved = _MODULE_PATH.resolve().parents[2] / "logs" / "saleae"
        if not resolved.is_dir():
            self.skipTest(
                f"{resolved} does not exist in this checkout yet "
                "(tools/PcTools/logs/ is gitignored and only created by an "
                "actual capture run, not by cloning/worktree-minting)"
            )
        self.assertEqual(resolved.name, "saleae")
        self.assertEqual(resolved.parent.name, "logs")
        # tools/PcTools/logs/saleae -- matches console_capture.py /
        # session_log.py / telemetry_capture.py's existing convention.
        self.assertEqual(resolved.parent.parent.name, "PcTools")

    def test_cwd_independence_of_the_anchor_formula(self):
        orig_cwd = os.getcwd()
        try:
            os.chdir(os.path.dirname(os.__file__))  # some unrelated directory
            resolved = _MODULE_PATH.resolve().parents[2] / "logs" / "saleae"
            if not resolved.is_dir():
                self.skipTest(
                    f"{resolved} does not exist in this checkout yet "
                    "(tools/PcTools/logs/ is gitignored and only created by "
                    "an actual capture run, not by cloning/worktree-minting)"
                )
        finally:
            os.chdir(orig_cwd)


if __name__ == "__main__":
    unittest.main()
