#!/usr/bin/env python3
"""Unit tests for kilnctrl.ui_test_client.UiTestClient, against a fake link
(register_task/send/unregister_task only -- no real serial port), same
in-process approach as test_fake_peer.py.

Run with: python -m pytest tools/PcTools/tests/test_ui_test_client.py -q
"""
from __future__ import annotations

import os
import queue
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.protocol import (  # noqa: E402
    UART_TASK_ID_UI_TEST,
    UI_TEST_CLICK_AMBIGUOUS,
    UI_TEST_CLICK_HIDDEN,
    UI_TEST_CLICK_NOT_FOUND,
    UI_TEST_CLICK_OK,
    UI_TEST_CMD_CLICK_BY_NAME,
    UI_TEST_CMD_GET_CURRENT_PAGE,
    UI_TEST_CMD_LIST_TAP_TARGETS,
    Device,
    Frame,
    MsgType,
)
from kilnctrl.serial_link import SendResult  # noqa: E402
from kilnctrl.ui_test_client import (  # noqa: E402
    UiTestClient,
    UiTestQueryError,
    UiTestResponseError,
)


class FakeLink:
    """Just enough of UartLink's surface for UiTestClient: register_task()
    hands back an inbox the test pushes replies into, send() records what
    was sent and returns a canned SendResult."""

    def __init__(self, send_result: SendResult = SendResult.OK) -> None:
        self.send_result = send_result
        self.sent: "list[bytes]" = []
        self._inboxes: "dict[int, queue.Queue]" = {}

    def register_task(self, task_id: int, inbox_len: int = 8):
        inbox = queue.Queue(maxsize=inbox_len)
        self._inboxes[task_id] = inbox
        return inbox

    def unregister_task(self, task_id: int) -> None:
        self._inboxes.pop(task_id, None)

    def send(self, dst_task, src_task, payload=b"", dst_device=Device.ESP, timeout=None):
        self.sent.append(payload)
        return self.send_result

    def push_reply(self, task_id: int, payload: bytes) -> None:
        frame = Frame(
            msg_type=MsgType.DATA, msg_index=1, src_device=Device.ESP, src_task=task_id,
            dst_device=Device.HOST, dst_task=task_id, payload=payload,
        )
        self._inboxes[task_id].put(frame)


class GetCurrentPageTest(unittest.TestCase):
    def setUp(self):
        self.link = FakeLink()
        self.client = UiTestClient(self.link)

    def tearDown(self):
        self.client.close()

    def test_decodes_page_name(self):
        payload = struct.pack("<B", UI_TEST_CMD_GET_CURRENT_PAGE) + struct.pack("<B", 4) + b"home"
        self.link.push_reply(UART_TASK_ID_UI_TEST, payload)
        self.assertEqual(self.client.get_current_page(timeout=1.0), "home")

    def test_send_not_acked_raises(self):
        self.link.send_result = SendResult.TIMEOUT
        with self.assertRaises(UiTestQueryError):
            self.client.get_current_page(timeout=0.2)

    def test_no_reply_within_timeout_raises(self):
        with self.assertRaises(UiTestQueryError):
            self.client.get_current_page(timeout=0.2)


class ListTapTargetsTest(unittest.TestCase):
    def setUp(self):
        self.link = FakeLink()
        self.client = UiTestClient(self.link)

    def tearDown(self):
        self.client.close()

    def test_decodes_targets_and_truncated_flag(self):
        body = bytearray()
        body += struct.pack("<B", UI_TEST_CMD_LIST_TAP_TARGETS)
        body += struct.pack("<B", 2)  # count
        body += struct.pack("<B", 1)  # truncated=True
        body += struct.pack("<B", 5) + b"Start" + struct.pack("<hhB", 100, 200, 0)
        body += struct.pack("<B", 4) + b"Stop" + struct.pack("<hhB", 100, 260, 1)
        self.link.push_reply(UART_TASK_ID_UI_TEST, bytes(body))

        result = self.client.list_tap_targets(timeout=1.0)
        self.assertTrue(result["truncated"])
        self.assertEqual(len(result["targets"]), 2)
        self.assertEqual(result["targets"][0], {"name": "Start", "cx": 100, "cy": 200, "hidden": False})
        self.assertEqual(result["targets"][1], {"name": "Stop", "cx": 100, "cy": 260, "hidden": True})

    def test_empty_list_not_truncated(self):
        body = struct.pack("<B", UI_TEST_CMD_LIST_TAP_TARGETS) + struct.pack("<B", 0) + struct.pack("<B", 0)
        self.link.push_reply(UART_TASK_ID_UI_TEST, body)
        result = self.client.list_tap_targets(timeout=1.0)
        self.assertEqual(result["targets"], [])
        self.assertFalse(result["truncated"])

    def test_truncated_entry_raises(self):
        body = (struct.pack("<B", UI_TEST_CMD_LIST_TAP_TARGETS) + struct.pack("<B", 1)
                + struct.pack("<B", 0) + struct.pack("<B", 5) + b"Sta")
        self.link.push_reply(UART_TASK_ID_UI_TEST, body)
        with self.assertRaises(UiTestResponseError):
            self.client.list_tap_targets(timeout=1.0)


class ClickByNameTest(unittest.TestCase):
    def setUp(self):
        self.link = FakeLink()
        self.client = UiTestClient(self.link)

    def tearDown(self):
        self.client.close()

    def _reply(self, code: int, cx: int = 12, cy: int = 34) -> None:
        payload = struct.pack("<B", UI_TEST_CMD_CLICK_BY_NAME) + struct.pack("<Bhh", code, cx, cy)
        self.link.push_reply(UART_TASK_ID_UI_TEST, payload)

    def test_ok_result(self):
        self._reply(UI_TEST_CLICK_OK, 12, 34)
        result = self.client.click_by_name("Start", timeout=1.0)
        self.assertEqual(result, {"result": "ok", "cx": 12, "cy": 34})

    def test_not_found_result_does_not_raise(self):
        self._reply(UI_TEST_CLICK_NOT_FOUND)
        result = self.client.click_by_name("Nope", timeout=1.0)
        self.assertEqual(result["result"], "not_found")

    def test_ambiguous_result(self):
        self._reply(UI_TEST_CLICK_AMBIGUOUS)
        self.assertEqual(self.client.click_by_name("Start", timeout=1.0)["result"], "ambiguous")

    def test_hidden_result(self):
        self._reply(UI_TEST_CLICK_HIDDEN)
        self.assertEqual(self.client.click_by_name("Start", timeout=1.0)["result"], "hidden")

    def test_request_carries_name(self):
        # uart_task_ids.h's CLICK_BY_NAME request is raw ASCII with NO
        # length-prefix byte (bytes1..(length-1) = name, "NOT
        # null-terminated") -- unlike every reply on this task. The frame's
        # own `length` field is the only length; a length-prefix byte here
        # would be read by uart_bridge_ui_test.c as the first character of
        # the name, shifting everything by one.
        self._reply(UI_TEST_CLICK_OK)
        self.client.click_by_name("Start", timeout=1.0)
        sent = self.link.sent[-1]
        self.assertEqual(sent, struct.pack("<B", UI_TEST_CMD_CLICK_BY_NAME) + b"Start")

    def test_unknown_result_code_raises(self):
        payload = struct.pack("<B", UI_TEST_CMD_CLICK_BY_NAME) + struct.pack("<Bhh", 99, 0, 0)
        self.link.push_reply(UART_TASK_ID_UI_TEST, payload)
        with self.assertRaises(UiTestResponseError):
            self.client.click_by_name("Start", timeout=1.0)

    def test_name_at_max_length_is_sent_unprefixed(self):
        # uart_bridge_ui_test.c copies into `char name[32]` and NUL-terminates,
        # so 31 ASCII bytes is the largest name that survives intact.
        self._reply(UI_TEST_CLICK_OK)
        name = "x" * 31
        self.client.click_by_name(name, timeout=1.0)
        sent = self.link.sent[-1]
        self.assertEqual(sent, struct.pack("<B", UI_TEST_CMD_CLICK_BY_NAME) + name.encode("ascii"))

    def test_name_over_max_length_raises_without_sending(self):
        with self.assertRaises(ValueError):
            self.client.click_by_name("x" * 32, timeout=1.0)
        # Refused before ever touching the link -- never let the firmware
        # silently truncate a name that doesn't fit its buffer.
        self.assertEqual(self.link.sent, [])


if __name__ == "__main__":
    unittest.main()
