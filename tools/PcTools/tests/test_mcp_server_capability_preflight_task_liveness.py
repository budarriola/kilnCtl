#!/usr/bin/env python3
"""Unit tests for mcp_server_capability_preflight._preflight_task_liveness() --
specifically that it treats InfoResponseError the same as InfoQueryError:
both mean "could not check" (return None), not "confirmed absent". No real
socket, no live board.

Run with:
  python -m pytest tools/PcTools/tests/test_mcp_server_capability_preflight_task_liveness.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server as _srv  # noqa: E402
from kilnctrl import mcp_server_capability_preflight as mscp  # noqa: E402
from kilnctrl.devices_info import InfoResponseError  # noqa: E402
from kilnctrl.info import InfoQueryError  # noqa: E402


class PreflightTaskLivenessErrorHandlingTest(unittest.TestCase):
    def test_info_query_error_returns_none(self):
        """The pre-existing failure path: no link/UART reachable at all."""
        with unittest.mock.patch.object(
            _srv._info, "get_stack_margin", side_effect=InfoQueryError("no reply")
        ):
            result = mscp._preflight_task_liveness()
        self.assertIsNone(result)

    def test_info_response_error_returns_none(self):
        """InfoResponseError (devices_info.py) is a ValueError, NOT a
        subclass of InfoQueryError -- a reply that arrives but parses to a
        malformed shape must be treated the same as "could not check", not
        surfaced as an uncaught exception out of a READ-ONLY preflight
        helper."""
        with unittest.mock.patch.object(
            _srv._info, "get_stack_margin",
            side_effect=InfoResponseError("stack margin reply truncated"),
        ):
            result = mscp._preflight_task_liveness()
        self.assertIsNone(result)


if __name__ == "__main__":
    unittest.main()
