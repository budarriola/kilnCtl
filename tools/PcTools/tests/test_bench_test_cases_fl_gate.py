#!/usr/bin/env python3
"""FL-10/FL-11 opt-in must be exactly True (review LOW-1): a truthy string such as
"no" must SKIP, never reach the flashing tool."""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_fl as C  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402


class FlashOptInStrictTest(unittest.TestCase):
    def test_truthy_non_true_skips(self):
        for fn in (C._case_fl10, C._case_fl11):
            for bad in ("no", "false", 1, "yes", None):
                r = fn({"allow_flash": bad})
                self.assertEqual(r.verdict, Verdict.SKIP, (fn.__name__, bad))


if __name__ == "__main__":
    unittest.main()
