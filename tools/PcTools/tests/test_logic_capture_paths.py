"""Regression test for logic_capture.py's --out default: it used to be the
bare relative literal "logs/saleae", which os.makedirs(..., exist_ok=True)
inside capture() will happily create wherever the process's CWD happens to
be -- no error, just a fresh empty logs/saleae in the wrong place and a
capture that "succeeds" into it. Same bug class as noise_floor.py's
DEFAULT_ARTIFACT_PATH (fixed 9311f3c): DEFAULT_OUT_DIR is now anchored on
__file__, so it must resolve to the same tools/PcTools/logs/saleae
directory regardless of CWD.

Plain unittest (see test_noise_floor.py's docstring for why: pytest is not
installed here and a module-level `import pytest` breaks collection under
`python -m unittest` for every file in the tree).
"""
from __future__ import annotations

import os
import tempfile
import unittest

from kilnctrl import logic_capture as lc


class DefaultOutDirCwdIndependenceTests(unittest.TestCase):
    def setUp(self):
        self._orig_cwd = os.getcwd()
        self.addCleanup(os.chdir, self._orig_cwd)

    def _expected(self):
        # tools/PcTools/src/kilnctrl/logic_capture.py -> tools/PcTools
        return os.path.normpath(
            os.path.join(os.path.dirname(lc.__file__), "..", "..", "logs", "saleae")
        )

    def test_default_is_absolute(self):
        self.assertTrue(os.path.isabs(lc.DEFAULT_OUT_DIR))

    def test_resolves_from_repo_root(self):
        repo_root = os.path.normpath(
            os.path.join(os.path.dirname(lc.__file__), "..", "..", "..", "..")
        )
        os.chdir(repo_root)
        self.assertEqual(
            os.path.normpath(lc.DEFAULT_OUT_DIR), os.path.normpath(self._expected())
        )

    def test_resolves_from_an_unrelated_directory(self):
        os.chdir(tempfile.gettempdir())
        self.assertEqual(
            os.path.normpath(lc.DEFAULT_OUT_DIR), os.path.normpath(self._expected())
        )

    def test_matches_the_directory_that_already_exists_on_disk(self):
        # The real tools/PcTools/logs/saleae directory this project already
        # writes into (console_capture.py / session_log.py /
        # telemetry_capture.py use the identical parents[2]-anchored
        # convention) -- pins DEFAULT_OUT_DIR to that existing location, not
        # just to "some absolute path".
        os.chdir(tempfile.gettempdir())
        self.assertTrue(os.path.isdir(lc.DEFAULT_OUT_DIR))


if __name__ == "__main__":
    unittest.main()
