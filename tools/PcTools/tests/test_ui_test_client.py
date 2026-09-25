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
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.protocol import (  # noqa: E402
    UART_TASK_ID_UI_TEST,
    UI_TEST_CLICK_AMBIGUOUS,
    UI_TEST_CLICK_HIDDEN,
    UI_TEST_CLICK_INJECT_FAILED,
    UI_TEST_CLICK_NOT_FOUND,
    UI_TEST_CLICK_OK,
    UI_TEST_CLICK_SWALLOWED,
    UI_TEST_CLICK_VERDICT_UNKNOWN,
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

    def test_swallowed_result(self):
        # 2026-09-24: the press was delivered but screen_idle_touch_swallow()
        # ate it (wake/ERROR_HOLD dismissal) -- distinct from "ok" (nothing
        # under the tap target ran) and distinct from every prior refusal
        # code, which all mean the target itself was not clickable.
        self._reply(UI_TEST_CLICK_SWALLOWED)
        self.assertEqual(self.client.click_by_name("Start", timeout=1.0)["result"], "swallowed")

    def test_verdict_unknown_result(self):
        # 2026-09-24 follow-up: the press was injected (target found,
        # visible) but kiln_ui_click_by_name()'s own bounded wait for the
        # swallow verdict timed out before it could be read -- neither a
        # confirmed "ok" nor a confirmed "swallowed".
        self._reply(UI_TEST_CLICK_VERDICT_UNKNOWN)
        self.assertEqual(self.client.click_by_name("Start", timeout=1.0)["result"], "verdict_unknown")

    def test_inject_failed_result(self):
        # 2026-09-24 follow-up: lvgl_port_inject_touch() itself returned 0
        # (never queued) before kiln_ui_click_by_name() started any wait --
        # no press was ever sent, distinct from "verdict_unknown" (a press
        # WAS sent, only its verdict is unconfirmed).
        self._reply(UI_TEST_CLICK_INJECT_FAILED)
        self.assertEqual(self.client.click_by_name("Start", timeout=1.0)["result"], "inject_failed")

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

    def test_non_ascii_name_raises_without_sending(self):
        # Firmware matches raw bytes with strcmp; an errors="replace" '?'
        # substitute could never match the real target and would surface as
        # a misleading not_found instead of a loud, immediate refusal.
        with self.assertRaises(ValueError):
            self.client.click_by_name("Café", timeout=1.0)
        self.assertEqual(self.link.sent, [])

    def test_empty_name_raises_without_sending(self):
        # Firmware would see a 1-byte frame (subcommand only) and reject it
        # as truncated with no indication of the real cause.
        with self.assertRaises(ValueError):
            self.client.click_by_name("", timeout=1.0)
        self.assertEqual(self.link.sent, [])


class _ReplyPerSendLink(FakeLink):
    """Delivers one queued reply per send(), i.e. only once the client has a
    query pending. Pre-pushing several replies races UiTestClient's consumer
    thread, which drops any reply that arrives with nothing outstanding."""

    def __init__(self) -> None:
        super().__init__()
        self.replies: "list[bytes]" = []

    def send(self, dst_task, src_task, payload=b"", dst_device=Device.ESP, timeout=None):
        result = super().send(dst_task, src_task, payload, dst_device, timeout)
        if self.replies:
            self.push_reply(UART_TASK_ID_UI_TEST, self.replies.pop(0))
        return result


class EnterPinTest(unittest.TestCase):
    def setUp(self):
        self.link = _ReplyPerSendLink()
        self.client = UiTestClient(self.link)
        # Never actually sleep for the first-digit retry poll in tests.
        self._retry_poll_patcher = mock.patch(
            "kilnctrl.ui_test_client._ENTER_PIN_FIRST_DIGIT_RETRY_POLL_S", 0.0
        )
        self._retry_poll_patcher.start()

    def tearDown(self):
        self._retry_poll_patcher.stop()
        self.client.close()

    def _reply(self, code: int, cx: int = 1, cy: int = 1) -> None:
        payload = struct.pack("<B", UI_TEST_CMD_CLICK_BY_NAME) + struct.pack("<Bhh", code, cx, cy)
        self.link.replies.append(payload)

    def test_clicks_each_digit_then_ok_in_order(self):
        for _ in range(4):
            self._reply(UI_TEST_CLICK_OK)
        self._reply(UI_TEST_CLICK_OK)
        self.client.enter_pin("1234", timeout=1.0)
        self.assertEqual(
            self.link.sent,
            [struct.pack("<B", UI_TEST_CMD_CLICK_BY_NAME) + digit.encode("ascii")
             for digit in ["1", "2", "3", "4", "OK"]],
        )

    def test_returns_per_digit_and_ok_results(self):
        for _ in range(4):
            self._reply(UI_TEST_CLICK_OK)
        self._reply(UI_TEST_CLICK_OK)
        result = self.client.enter_pin("1234", timeout=1.0)
        self.assertEqual(len(result["digit_results"]), 4)
        self.assertTrue(all(r["result"] == "ok" for r in result["digit_results"]))
        self.assertEqual(result["ok_result"]["result"], "ok")

    def test_a_not_found_digit_click_does_not_raise_and_still_presses_ok(self):
        # A stale/closed keypad or a mistyped digit name must surface as
        # data (not_found), not an exception -- callers detect wrong-PIN vs.
        # no-keypad by polling tap-target names afterward, not by exception.
        # The FIRST digit's not_found is retried once (2026-09-25 fix, see
        # below); a not_found on a LATER digit is not, so this uses digit
        # index 1 (the "2") to keep exercising the plain not-retried path.
        self._reply(UI_TEST_CLICK_OK)  # digit "1"
        self._reply(UI_TEST_CLICK_NOT_FOUND)  # digit "2"
        for _ in range(2):
            self._reply(UI_TEST_CLICK_OK)  # digits "3", "4"
        self._reply(UI_TEST_CLICK_OK)  # OK
        result = self.client.enter_pin("1234", timeout=1.0)
        self.assertEqual(result["digit_results"][1]["result"], "not_found")
        self.assertEqual(len(self.link.sent), 5)  # all 4 digits + OK still sent, no retry

    def test_first_digit_not_found_retries_once_after_a_short_poll(self):
        # 2026-09-25 (LCD-19 bench root cause, 20260925T170357Z_full/
        # summary.json): the keypad overlay was confirmed raised, yet the
        # very first digit click still came back not_found -- a startup
        # race, not a genuinely absent target. One retry, after a short
        # poll, recovers it.
        self._reply(UI_TEST_CLICK_NOT_FOUND)  # digit "1", first attempt
        self._reply(UI_TEST_CLICK_OK)  # digit "1", retry
        for _ in range(3):
            self._reply(UI_TEST_CLICK_OK)  # digits "2", "3", "4"
        self._reply(UI_TEST_CLICK_OK)  # OK
        result = self.client.enter_pin("1234", timeout=1.0)
        self.assertEqual(result["digit_results"][0]["result"], "ok")
        self.assertEqual(len(result["digit_results"]), 4)
        # 5 clicks (2 for digit "1" + 3 for "2"/"3"/"4") + 1 for OK.
        self.assertEqual(len(self.link.sent), 6)
        self.assertEqual(
            self.link.sent[:2],
            [struct.pack("<B", UI_TEST_CMD_CLICK_BY_NAME) + b"1"] * 2,
        )

    def test_first_digit_not_found_twice_is_not_retried_again(self):
        # Never more than one retry: a persistently not_found first digit
        # (a genuinely closed/absent keypad, not a startup race) still
        # surfaces as not_found rather than looping.
        self._reply(UI_TEST_CLICK_NOT_FOUND)  # digit "1", first attempt
        self._reply(UI_TEST_CLICK_NOT_FOUND)  # digit "1", retry -- still not_found
        for _ in range(3):
            self._reply(UI_TEST_CLICK_OK)
        self._reply(UI_TEST_CLICK_OK)
        result = self.client.enter_pin("1234", timeout=1.0)
        self.assertEqual(result["digit_results"][0]["result"], "not_found")
        self.assertEqual(len(self.link.sent), 6)  # 2 for digit "1", not 3+

    def test_first_digit_ok_is_never_retried(self):
        # Never re-click a digit that was already reported clicked -- a
        # blind retry on an "ok" first digit would type it twice and
        # corrupt the PIN.
        self._reply(UI_TEST_CLICK_OK)
        for _ in range(3):
            self._reply(UI_TEST_CLICK_OK)
        self._reply(UI_TEST_CLICK_OK)
        result = self.client.enter_pin("1234", timeout=1.0)
        self.assertEqual(result["digit_results"][0]["result"], "ok")
        self.assertEqual(len(self.link.sent), 5)  # exactly one click per digit + OK

    def test_empty_pin_still_presses_ok_only(self):
        self._reply(UI_TEST_CLICK_OK)
        result = self.client.enter_pin("", timeout=1.0)
        self.assertEqual(result["digit_results"], [])
        self.assertEqual(self.link.sent, [struct.pack("<B", UI_TEST_CMD_CLICK_BY_NAME) + b"OK"])

    def test_trailing_ok_not_found_retries_once_after_a_short_poll(self):
        # 2026-09-25 (LCD-19 bench root cause, 20260925T191709Z_lcd/
        # summary.json): all 6 wrong-PIN digit clicks reported "ok", yet the
        # trailing "OK" click reported "not_found" -- same click-then-read
        # race class as the first digit, on the closing click instead of the
        # opening one. One retry, after a short poll, recovers it.
        for _ in range(4):
            self._reply(UI_TEST_CLICK_OK)  # digits "1".."4"
        self._reply(UI_TEST_CLICK_NOT_FOUND)  # OK, first attempt
        self._reply(UI_TEST_CLICK_OK)  # OK, retry
        result = self.client.enter_pin("1234", timeout=1.0)
        self.assertEqual(result["ok_result"]["result"], "ok")
        # 4 digits + 2 for OK (first attempt + retry).
        self.assertEqual(len(self.link.sent), 6)
        self.assertEqual(
            self.link.sent[-2:],
            [struct.pack("<B", UI_TEST_CMD_CLICK_BY_NAME) + b"OK"] * 2,
        )

    def test_trailing_ok_not_found_twice_is_not_retried_again(self):
        # Never more than one retry: a persistently not_found OK (a genuinely
        # closed/absent keypad) still surfaces as not_found rather than
        # looping.
        for _ in range(4):
            self._reply(UI_TEST_CLICK_OK)
        self._reply(UI_TEST_CLICK_NOT_FOUND)  # OK, first attempt
        self._reply(UI_TEST_CLICK_NOT_FOUND)  # OK, retry -- still not_found
        result = self.client.enter_pin("1234", timeout=1.0)
        self.assertEqual(result["ok_result"]["result"], "not_found")
        self.assertEqual(len(self.link.sent), 6)  # 4 digits + 2 for OK, not 3+

    def test_trailing_ok_ok_is_never_retried(self):
        # Never re-click an "OK" that already landed -- a blind retry would
        # resubmit the PIN a second time (double-submit a granted PIN, or
        # re-trigger the wrong-PIN reset path).
        for _ in range(4):
            self._reply(UI_TEST_CLICK_OK)
        self._reply(UI_TEST_CLICK_OK)  # OK
        result = self.client.enter_pin("1234", timeout=1.0)
        self.assertEqual(result["ok_result"]["result"], "ok")
        self.assertEqual(len(self.link.sent), 5)  # exactly one click per digit + one OK


if __name__ == "__main__":
    unittest.main()
