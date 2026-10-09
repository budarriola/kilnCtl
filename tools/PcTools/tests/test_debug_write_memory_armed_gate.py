#!/usr/bin/env python3
"""Unit tests for mcp_server_debug.debug_write_memory()'s Pico-ARMED
write-refusal gate -- see tools/PcTools/TODO.md, "Debug and programming --
OpenOCD wrapper" section.

Prior coverage (test_debug_probe_armed.py) only exercised
debug_probe.pico_armed_state() itself -- the SWD read/decode of
relay_owner_state_t. It never called the actual gate this exists to back,
mcp_server_debug.debug_write_memory(), so the one behaviour that matters --
"a write to the Pico while it reads ARMED is refused, and a write while it
reads not-armed proceeds" -- had zero coverage of its own. TODO.md's own
note is that the one live SWD smoke test done so far read an invalid
relay_owner_state_t byte, which correctly fails closed but never actually
proved the true-ARMED refusal path.

This file calls the REAL production function (mcp_server_debug.debug_write_memory),
not a reimplementation, and mocks only two seams below it:
debug_probe.pico_armed_state() (the SWD read) and debug_probe.write_memory()
(the actual OpenOCD write) -- no real OpenOCD session, no live board.

Both halves are pinned, per this repo's standing "one-directional test is a
vacuity failure" rule:
  - refusal path: armed True, and armed unknown (None) -- both must refuse,
    and write_memory() must never even be called.
  - permitted path: armed False -- the write must actually go through, and
    write_memory() must be called with the caller's real arguments.
  - the peer="esp" path is untouched by this additive guard (it has no
    relay_owner_state_t of its own) -- write_memory() is called directly,
    pico_armed_state() is never even consulted.
  - the confirm=False gate (pre-existing, separate from the ARMED gate)
    still refuses before this file's ARMED check ever runs.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import debug_probe  # noqa: E402
from kilnctrl import mcp_server_debug as dbg  # noqa: E402


class DebugWriteMemoryArmedGateTests(unittest.TestCase):
    def _patch_armed(self, armed, detail="detail"):
        return unittest.mock.patch.object(debug_probe, "pico_armed_state", return_value=(armed, detail))

    def _patch_write(self, ok=True, output="ok"):
        return unittest.mock.patch.object(debug_probe, "write_memory", return_value=(ok, output))

    # --- Refusal path ----------------------------------------------------

    def test_true_armed_refuses_and_never_writes(self):
        with self._patch_armed(True, "s_state=0x2 (ARMED)") as _armed, self._patch_write() as write:
            result = dbg.debug_write_memory(peer="pico", address=0x2000BA33, value=0, confirm=True)
        self.assertIn("error", result)
        self.assertIn("ARMED", result)
        write.assert_not_called()

    def test_unknown_armed_state_fails_closed_and_never_writes(self):
        # An unreadable/ambiguous state must be refused, never treated as
        # "not armed" -- see debug_probe.py's own fail-closed doc comment.
        with self._patch_armed(None, "could not resolve symbol") as _armed, self._patch_write() as write:
            result = dbg.debug_write_memory(peer="pico", address=0x2000BA33, value=0, confirm=True)
        self.assertIn("error", result)
        self.assertIn("could not confidently determine", result)
        write.assert_not_called()

    # --- Permitted path ----------------------------------------------------

    def test_false_armed_state_permits_the_write(self):
        with self._patch_armed(False, "s_state=0x1 (not armed)") as _armed, self._patch_write(
            ok=True, output="wrote"
        ) as write:
            result = dbg.debug_write_memory(peer="pico", address=0x2000BA33, value=0x7, width=32, confirm=True)
        self.assertNotIn("error", result)
        self.assertIn("0x7", result)
        write.assert_called_once_with("pico", 0x2000BA33, 0x7, 32)

    # --- peer="esp" is untouched by this additive guard -------------------

    def test_esp_peer_never_consults_pico_armed_state(self):
        with unittest.mock.patch.object(debug_probe, "pico_armed_state") as armed_mock, self._patch_write(
            ok=True, output="wrote"
        ) as write:
            result = dbg.debug_write_memory(peer="esp", address=0x3FC00000, value=1, confirm=True)
        armed_mock.assert_not_called()
        write.assert_called_once_with("esp", 0x3FC00000, 1, 32)
        self.assertNotIn("error", result)

    # --- confirm=False still refuses first, before the ARMED gate --------

    def test_confirm_false_refuses_before_armed_check(self):
        with unittest.mock.patch.object(debug_probe, "pico_armed_state") as armed_mock, self._patch_write() as write:
            result = dbg.debug_write_memory(peer="pico", address=0x2000BA33, value=0)
        self.assertIn("error", result)
        self.assertIn("confirm=True", result)
        armed_mock.assert_not_called()
        write.assert_not_called()


if __name__ == "__main__":
    unittest.main()
