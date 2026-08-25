#!/usr/bin/env python3
"""kilnsim's virtual_simfw port must never be kilnctrl's link-hub port.

Both defaulted to 8765 until 2026-08-24. They are different protocols --
benchproto framing over a raw TCP stream vs. the hub's own RPC -- living in
one repo, one venv, and typically one bench session.

The failure was near-silent. With a kilnctrl hub running (the normal state
whenever anyone is working on the ESP), `kilnsim --virtual` connected to the
HUB, sent benchproto at it, had the connection dropped, and reported

    SimFW fixture not present: SimFW fixture not reachable: write failed:
    [WinError 10053] An established connection was aborted...

which reads as "no fixture is attached". Every scenario came back
NOT_RUNNABLE and `testmgr` exited 1. Nothing pointed at the port. The whole
hardware-free CI path was unavailable, on a machine where it was supposedly
the cheap always-runnable layer, and the reason looked like absent hardware.

`virtual_simfw` itself gets the clearer symptom of the same collision --
`bind() failed: 10013` -- but only if you happen to run it by hand.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.link_hub import HUB_PORT  # noqa: E402
from kilnsim.link import DEFAULT_VIRTUAL_SIMFW_PORT  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[3]
VIRTUAL_SIMFW_C = (REPO_ROOT / "firmware" / "SimFW" / "tools" / "virtual_simfw"
                   / "src" / "virtual_simfw.c")


class PortCollisionTests(unittest.TestCase):
    def test_kilnsim_and_kilnctrl_do_not_share_a_default_port(self):
        self.assertNotEqual(
            DEFAULT_VIRTUAL_SIMFW_PORT, HUB_PORT,
            "kilnsim's virtual_simfw port and kilnctrl's link-hub port are the same. "
            "Whichever tool starts second cannot bind, and kilnsim's client side reports "
            "it as an absent fixture rather than as a port conflict.",
        )

    def test_the_c_default_matches_the_python_default(self):
        """The two halves have to agree or `kilnsim --virtual` with no
        explicit address dials a port nothing is listening on -- which
        produces the same misleading "fixture not reachable" as the
        collision did."""
        self.assertTrue(VIRTUAL_SIMFW_C.is_file(), f"missing {VIRTUAL_SIMFW_C}")
        text = VIRTUAL_SIMFW_C.read_text(encoding="utf-8")
        # Strip comments first: this file's own comment explains the old 8765
        # collision by name, and a scan that matched it would read the
        # history as the current value.
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//[^\n]*", " ", text)
        m = re.search(r"uint16_t\s+port\s*=\s*(\d+)\s*;", text)
        self.assertIsNotNone(m, "could not find virtual_simfw's default port in its main()")
        self.assertEqual(
            int(m.group(1)), DEFAULT_VIRTUAL_SIMFW_PORT,
            "virtual_simfw.c's default listen port and kilnsim.link's "
            "DEFAULT_VIRTUAL_SIMFW_PORT have drifted apart",
        )

    def test_the_c_default_also_avoids_the_hub(self):
        text = VIRTUAL_SIMFW_C.read_text(encoding="utf-8")
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//[^\n]*", " ", text)
        m = re.search(r"uint16_t\s+port\s*=\s*(\d+)\s*;", text)
        self.assertNotEqual(int(m.group(1)), HUB_PORT,
                            "virtual_simfw would fail to bind whenever a kilnctrl hub is running")


if __name__ == "__main__":
    unittest.main()
