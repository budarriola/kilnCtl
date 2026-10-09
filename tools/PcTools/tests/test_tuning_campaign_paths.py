"""Regression test for tuning_campaign.py's output-artifact path: it used
to be the bare relative literal
"tools/PcTools/config_presets/tuning_recommendations.json", written with
plain open(out_path, "w") in main(). From any CWD other than the repo root
this raises FileNotFoundError (the parent directory doesn't exist relative
to that CWD) -- less dangerous than a fully silent miss, but still a path
that only worked by accident of where the process happened to be launched
from. DEFAULT_OUT_PATH is now anchored on __file__, matching the pattern
noise_floor.py's DEFAULT_ARTIFACT_PATH established (fixed 9311f3c).

Plain unittest (see test_noise_floor.py's docstring for why).
"""
from __future__ import annotations

import os
import tempfile
import unittest

from kilnctrl import tuning_campaign as tcamp


class DefaultOutPathCwdIndependenceTests(unittest.TestCase):
    def setUp(self):
        self._orig_cwd = os.getcwd()
        self.addCleanup(os.chdir, self._orig_cwd)

    def _expected(self):
        # tools/PcTools/src/kilnctrl/tuning_campaign.py -> repo root is
        # four levels up, then back down to tools/PcTools/config_presets.
        # Mirror production's Path(__file__).resolve(): plain __file__ can
        # carry whatever drive-letter case the interpreter/collector used to
        # import the module (e.g. a lowercase-drive checkout), while
        # DEFAULT_OUT_PATH is built from the resolved (OS-canonical-case)
        # path -- comparing unresolved to resolved makes this test flaky by
        # drive-letter case alone, not by the CWD independence it means to
        # check.
        return os.path.normpath(os.path.join(
            os.path.dirname(os.path.realpath(tcamp.__file__)), "..", "..", "..", "..",
            "tools", "PcTools", "config_presets", "tuning_recommendations.json",
        ))

    def test_default_is_absolute(self):
        self.assertTrue(os.path.isabs(tcamp.DEFAULT_OUT_PATH))

    def test_resolves_from_repo_root(self):
        repo_root = os.path.normpath(
            os.path.join(os.path.dirname(tcamp.__file__), "..", "..", "..", "..")
        )
        os.chdir(repo_root)
        self.assertEqual(
            os.path.normpath(tcamp.DEFAULT_OUT_PATH), self._expected()
        )

    def test_resolves_from_an_unrelated_directory(self):
        os.chdir(tempfile.gettempdir())
        self.assertEqual(
            os.path.normpath(tcamp.DEFAULT_OUT_PATH), self._expected()
        )

    def test_points_at_the_config_presets_dir_that_already_exists(self):
        os.chdir(tempfile.gettempdir())
        self.assertTrue(
            os.path.isdir(os.path.dirname(tcamp.DEFAULT_OUT_PATH))
        )


if __name__ == "__main__":
    unittest.main()
