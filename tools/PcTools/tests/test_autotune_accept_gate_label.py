"""autotune_accept labels a refusal as the mode gate only for the gate's stable substring."""
from __future__ import annotations

import os
import sys
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_autotune as m  # noqa: E402


def _call(reason):
    res = types.SimpleNamespace(ok=False, reason=reason)
    fake = types.SimpleNamespace(_autotune=types.SimpleNamespace(accept=lambda ack_unsettled=False: res))
    with mock.patch.object(m, "_srv", fake, create=True):
        fn = getattr(m.autotune_accept, "__wrapped__", m.autotune_accept)
        return fn()


class AutotuneAcceptGateLabel(unittest.TestCase):
    def test_gate_reason_labelled_without_409(self):
        out = _call("refused: a firing or autotune run is active")
        self.assertTrue(out.startswith("refused by system mode gate:"))
        self.assertNotIn("409", out)

    def test_other_reason_not_labelled_gate(self):
        out = _call("transport busy, try later")
        self.assertNotIn("mode gate", out)
        self.assertTrue(out.startswith("refused - nothing to accept"))


if __name__ == "__main__":
    unittest.main()
