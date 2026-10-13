#!/usr/bin/env python3
"""Unit tests for kilnctrl.info.InfoClient.wait_for_boot_push().

Task 2 (docs/SYSTEM_MODE_GATE.md): mcp_server_ui_test.py's
factory_default_then_load_preset() used to confirm a factory reset actually
happened by calling get_fw_version() -- a plain QUERY that the device's
always-alive INFO task answers whether or not it ever rebooted. That made it
impossible to tell a real reboot apart from a factory reset silently refused
by the board's system mode gate (mid-firing/mid-autotune): the UART ACK for
the reset request only confirms delivery, not that execute_scope() ever ran,
so a refused reset that still answers a query looked identical to success.

wait_for_boot_push() instead blocks for the device's own once-per-boot,
UNSOLICITED FW-version push (info_boot_push_task) -- the same signal
_handle_reply()'s unsolicited branch already used to fire on_boot_push() for
session-log rollover, now also exposed as something a caller can wait on with
a timeout and treat a miss as "reboot not confirmed" rather than success.

No real UART/serial link is used: InfoClient.__init__ needs a real UartLink
and spawns a background consumer thread, so these tests build a bare instance
(same technique as test_stack_margin_info.py's _bare_info_client()) and drive
_handle_reply() directly with hand-built Frames -- the exact same code path
the real consumer thread calls frame-by-frame.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import struct
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl.info import InfoClient  # noqa: E402
from kilnctrl.protocol import Device, Frame, MsgType  # noqa: E402


def _fw_version_payload(commit: str = "abc1234") -> bytes:
    """A well-formed GET_FW_VERSION reply payload (build_fw_version_reply()'s
    layout): protocol_version u16 LE, dirty u8, commit_len u8 + commit,
    datetime_len u8 + datetime."""
    commit_b = commit.encode("ascii")
    datetime_b = b"2026-09-28 00:00:00Z"
    return (
        struct.pack("<H", devices.UART_PROTOCOL_VERSION)
        + struct.pack("<B", 0)  # dirty
        + struct.pack("<B", len(commit_b))
        + commit_b
        + struct.pack("<B", len(datetime_b))
        + datetime_b
    )


def _fw_version_frame(commit: str = "abc1234") -> Frame:
    return Frame(
        msg_type=MsgType.DATA,
        msg_index=0,
        src_device=Device.ESP,
        src_task=3,
        dst_device=Device.HOST,
        dst_task=3,
        payload=_fw_version_payload(commit),
    )


def _bare_info_client(on_boot_push=None) -> InfoClient:
    """An InfoClient with no real link/consumer thread -- _handle_reply() and
    wait_for_boot_push() only touch the fields set up below (no self.link,
    no self._inbox), so bypass __init__ rather than faking a whole UartLink."""
    client = InfoClient.__new__(InfoClient)
    client.on_boot_push = on_boot_push
    client.last_fw_version = None
    client._boot_push_event = threading.Event()
    client._last_boot_push = None
    client._pending = None
    client._pending_lock = threading.Lock()
    return client


class WaitForBootPushTests(unittest.TestCase):
    def test_times_out_with_none_when_no_push_arrives(self):
        # This is the exact scenario a mode-gate-refused factory reset
        # produces: the request was ACKed, but execute_scope() (and its
        # reboot) never ran, so no unsolicited push ever follows.
        client = _bare_info_client()
        started = time.monotonic()
        result = client.wait_for_boot_push(timeout=0.2)
        elapsed = time.monotonic() - started
        self.assertIsNone(result)
        self.assertGreaterEqual(elapsed, 0.15)

    def test_returns_the_pushed_version_when_an_unsolicited_push_arrives(self):
        client = _bare_info_client()

        def _deliver_after_delay():
            time.sleep(0.05)
            client._handle_reply(_fw_version_frame(commit="deadbee"))

        threading.Thread(target=_deliver_after_delay, daemon=True).start()
        result = client.wait_for_boot_push(timeout=2.0)
        self.assertIsNotNone(result)
        self.assertEqual(result.commit, "deadbee")

    def test_a_solicited_query_reply_does_not_satisfy_wait_for_boot_push(self):
        # The core bug this fix closes: get_fw_version() is a QUERY, answered
        # by an always-alive board whether or not it ever rebooted. Simulate
        # exactly that -- a reply that arrives while a query is outstanding
        # (self._pending set, mirroring _query()'s own bookkeeping) -- and
        # confirm wait_for_boot_push() does NOT treat it as a reboot signal.
        client = _bare_info_client()

        from kilnctrl.protocol import INFO_CMD_GET_FW_VERSION

        class _Pending:
            def __init__(self):
                self.subcommand = INFO_CMD_GET_FW_VERSION
                self.value = None
                self.event = threading.Event()

        client._pending = _Pending()
        client._handle_reply(_fw_version_frame(commit="answered-a-query"))

        # The (solicited) query's own pending event was satisfied...
        self.assertTrue(client._pending.event.is_set())
        self.assertEqual(client._pending.value.commit, "answered-a-query")
        # ...but wait_for_boot_push() must still time out: this was a reply
        # to an outstanding query, never the unsolicited once-per-boot push.
        result = client.wait_for_boot_push(timeout=0.2)
        self.assertIsNone(result)

    def test_clears_a_stale_push_observed_before_the_call(self):
        # A push that already landed before wait_for_boot_push() was called
        # (e.g. left over from setup) must not be reported as satisfying a
        # NEW wait -- only a push that arrives after this call counts, since
        # factory_default_then_load_preset() calls this right after sending
        # the reset request and needs to know about THAT reboot specifically.
        client = _bare_info_client()
        client._handle_reply(_fw_version_frame(commit="stale-push"))
        self.assertTrue(client._boot_push_event.is_set())

        result = client.wait_for_boot_push(timeout=0.2)
        self.assertIsNone(result)

    def test_push_between_arm_and_wait_is_counted_with_arm_false(self):
        # factory_default_then_load_preset() arms before sending the reset,
        # then waits with arm=False: a reboot that pushes before the wait
        # starts must still count, not be cleared away.
        client = _bare_info_client()
        client._handle_reply(_fw_version_frame(commit="stale-push"))
        client.arm_boot_push()
        client._handle_reply(_fw_version_frame(commit="fast-reboot"))
        result = client.wait_for_boot_push(timeout=0.2, arm=False)
        self.assertIsNotNone(result)
        self.assertEqual(result.commit, "fast-reboot")

    def test_arm_discards_a_push_observed_before_it(self):
        client = _bare_info_client()
        client._handle_reply(_fw_version_frame(commit="stale-push"))
        client.arm_boot_push()
        self.assertIsNone(client.wait_for_boot_push(timeout=0.2, arm=False))

    def test_on_boot_push_callback_still_fires_alongside_the_event(self):
        # wait_for_boot_push() is additive -- the pre-existing on_boot_push
        # callback (session-log rollover in mcp_server.py) must be unaffected.
        seen = []
        client = _bare_info_client(on_boot_push=lambda v: seen.append(v.commit))
        client._handle_reply(_fw_version_frame(commit="cb-fires"))
        self.assertEqual(seen, ["cb-fires"])
        self.assertTrue(client._boot_push_event.is_set())


if __name__ == "__main__":
    unittest.main()
