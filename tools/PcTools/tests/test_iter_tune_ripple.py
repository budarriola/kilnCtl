#!/usr/bin/env python3
"""Tests for kilnctrl.iter_tune_ripple (synthetic captures, no board).

Run: python -m pytest tools/PcTools/tests/test_iter_tune_ripple.py -q
"""
from __future__ import annotations

import json
import math
import os
import random
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "src"))
from kilnctrl import iter_tune_ripple as r  # noqa: E402

CLI = os.path.join(HERE, "..", "scripts", "iter_tune_ripple.py")
W, DUTY, GAIN, BASE = 60.0, 0.4, 100.0, 25.0
RIPPLE_GAIN = GAIN * 0.05  # C per unit duty seen as ripple (rest is load-smoothed)


def synth(path, tau, secs=1200, noise_only=False, dwelling=True, seed=1):
    rng = random.Random(seed)
    dt = 0.5
    x = BASE + GAIN * DUTY * 0.95 + RIPPLE_GAIN * DUTY
    with open(path, "w") as f:
        for i in range(int(secs / dt)):
            t = i * dt
            u = 1.0 if (t % W) < DUTY * W else 0.0
            if noise_only:
                temp = BASE + GAIN * DUTY + rng.gauss(0, 0.3)
            else:
                target = BASE + RIPPLE_GAIN * u + GAIN * DUTY * 0.95
                if tau <= 0:
                    x = target
                else:
                    x += (target - x) * (1 - math.exp(-dt / tau))
                temp = x + rng.gauss(0, 0.002)
            row = {"t": 1000.0 + t,
                   "exec": {"dwelling": dwelling, "segment_index": 1,
                            "zones": [{"zone": 0, "actual_c": temp,
                                       "actual_valid": True, "duty": DUTY}]}}
            f.write(json.dumps(row) + "\n")


class T(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()

    def mk(self, name, **kw):
        p = os.path.join(self.d, name + ".jsonl")
        synth(p, **kw)
        return p

    @staticmethod
    def zone(rep):
        return rep["segments"][0]["zones"]["0"]

    def test_tau_recovered(self):
        p = self.mk("a", tau=20.0)
        code, rep = r.analyse(p, window_s=W, gain_c=RIPPLE_GAIN)
        z = self.zone(rep)
        self.assertEqual(code, 0)
        self.assertEqual(z["status"], "OK")
        self.assertAlmostEqual(z["tau_s"], 20.0, delta=3.0)

    def test_tau_zero(self):
        p = self.mk("b", tau=0.0)
        _, rep = r.analyse(p, window_s=W, gain_c=RIPPLE_GAIN)
        z = self.zone(rep)
        self.assertEqual(z["status"], "OK")
        self.assertLess(z["tau_s"], 2.0)

    def test_gate_refused(self):
        p = self.mk("gate1", tau=20.0)
        code, rep = r.analyse(p, gate_ids=["gate1"])
        self.assertEqual(code, r.EXIT_GATE_REFUSED)
        self.assertEqual(rep["status"], "REFUSED_GATE_SET")
        rc = subprocess.run([sys.executable, CLI, p, "--gate-ids", "x", "gate1"],
                            capture_output=True).returncode
        self.assertEqual(rc, r.EXIT_GATE_REFUSED)

    def test_no_dwell(self):
        p = self.mk("nd", tau=20.0, dwelling=False)
        code, rep = r.analyse(p)
        self.assertEqual(code, 0)
        self.assertEqual(rep["status"], "NO_DWELL")

    def test_noise_inconclusive(self):
        p = self.mk("noise", tau=0, noise_only=True)
        _, rep = r.analyse(p, window_s=W, gain_c=RIPPLE_GAIN)
        z = self.zone(rep)
        self.assertEqual(z["status"], "INCONCLUSIVE")
        self.assertIsNone(z["tau_s"])


if __name__ == "__main__":
    unittest.main()
