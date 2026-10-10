"""A started job leaves a RUNNING marker; after a 'restart' it reads INTERRUPTED."""
from __future__ import annotations

import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from mcpkit import build_jobs  # noqa: E402


class RunningMarkerTests(unittest.TestCase):
    def test_marker_then_interrupted_after_restart(self):
        gate = threading.Event()
        jid = build_jobs.start_job("bench_test_run", lambda: (gate.wait(5) or "t: OK"),
                                   {"case": "OT-E01"})
        try:
            build_jobs._reset_for_tests()  # simulate restart: memory gone, file stays
            out = build_jobs.job_status(jid)
            self.assertIn("INTERRUPTED", out)
            self.assertIn("OT-E01", out)
        finally:
            gate.set()


if __name__ == "__main__":
    unittest.main()
