#!/usr/bin/env python3
"""Unit tests for kilnctrl.kiln_configs_apply_http_client -- POST
/api/kiln_configs/apply and GET /api/kiln_configs/apply_status, all against
MOCKED urllib responses. No real socket and no live board.

Run with: python -m pytest tools/PcTools/tests/test_kiln_configs_apply_http_client.py -q
"""
from __future__ import annotations

import io
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import kiln_configs_apply_http_client as ac  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


class PostApplyRequestShapeTest(unittest.TestCase):
    def test_default_sends_no_ack_header(self):
        """The whole point of this client: the ack-hardware-differs header
        must be sent ONLY when the caller explicitly opts in. Before this
        module, no PcTools client sent this header at all."""
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true,"state":"running"}', status=202)

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            status, body = ac.post_apply("host", 3)
        self.assertEqual(status, 202)
        self.assertEqual(sent[0].get_method(), "POST")
        self.assertEqual(sent[0].data, b"id=3")
        header_names = {k.lower() for k in sent[0].headers}
        self.assertNotIn(ac.ACK_HARDWARE_DIFFERS_HEADER.lower(), header_names)

    def test_ack_true_sends_header_value_1(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true,"state":"running"}', status=202)

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            ac.post_apply("host", 3, ack_hardware_differs=True)
        header_key = ac.ACK_HARDWARE_DIFFERS_HEADER
        # urllib normalizes header keys to Capitalized-Dashes internally.
        got = None
        for k, v in sent[0].headers.items():
            if k.lower() == header_key.lower():
                got = v
        self.assertEqual(got, "1")

    def test_428_hardware_differs_surfaces_body_not_raise(self):
        err = urllib.error.HTTPError(
            "u", 428, "Precondition Required", {}, io.BytesIO(b"hardware shape differs: ct_topology"))

        def fake_urlopen(req, timeout=None):
            raise err

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            status, body = ac.post_apply("host", 3)
        self.assertEqual(status, 428)
        self.assertIn("hardware shape differs", body)

    def test_404_no_such_id_surfaces_body_not_raise(self):
        err = urllib.error.HTTPError("u", 404, "Not Found", {}, io.BytesIO(b"no such kiln config"))

        def fake_urlopen(req, timeout=None):
            raise err

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            status, body = ac.post_apply("host", 999)
        self.assertEqual(status, 404)

    def test_unreachable_host_raises(self):
        def fake_urlopen(req, timeout=None):
            raise urllib.error.URLError("no route to host")

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            with self.assertRaises(ac.KilnConfigsApplyHttpError):
                ac.post_apply("host", 3)

    def test_bare_socket_timeout_is_wrapped_not_escaped(self):
        """http_auth.urlopen() calls urllib.request.urlopen() directly, which
        can raise a bare socket.timeout/OSError that is NOT a URLError
        subclass in every Python build path exercised here -- only
        HTTPError/URLError were caught before this test, so this exact class
        of failure escaped past post_apply()'s except clauses and past the
        MCP tool's own `except ac.KilnConfigsApplyHttpError` around it."""
        def fake_urlopen(req, timeout=None):
            raise TimeoutError("timed out")

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            with self.assertRaises(ac.KilnConfigsApplyHttpError):
                ac.post_apply("host", 3)

    def test_http_auth_error_is_wrapped_not_escaped(self):
        """A 401 with no credential in the environment makes http_auth.urlopen()
        raise HttpAuthError, a RuntimeError subclass -- not an HTTPError/URLError
        -- so it also escaped uncaught before this test."""
        from kilnctrl import http_auth

        def fake_urlopen(req, timeout=None):
            raise http_auth.HttpAuthError("no credential available")

        with unittest.mock.patch.object(ac.http_auth, "urlopen", fake_urlopen):
            with self.assertRaises(ac.KilnConfigsApplyHttpError):
                ac.post_apply("host", 3)


class GetApplyStatusTest(unittest.TestCase):
    def test_parses_json(self):
        def fake_urlopen(req, timeout=None):
            return _fake_response(b'{"state":"done_ok","id":3,"diverged":false,"reason":""}')

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            status = ac.get_apply_status("host")
        self.assertEqual(status["state"], "done_ok")
        self.assertFalse(status["diverged"])

    def test_non_json_body_raises(self):
        def fake_urlopen(req, timeout=None):
            return _fake_response(b"not json")

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            with self.assertRaises(ac.KilnConfigsApplyHttpError):
                ac.get_apply_status("host")

    def test_http_error_raises_with_status(self):
        err = urllib.error.HTTPError("u", 500, "Internal", {}, io.BytesIO(b"boom"))

        def fake_urlopen(req, timeout=None):
            raise err

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            with self.assertRaises(ac.KilnConfigsApplyHttpError) as ctx:
                ac.get_apply_status("host")
        self.assertEqual(ctx.exception.status, 500)

    def test_bare_socket_timeout_is_wrapped_not_escaped(self):
        def fake_urlopen(req, timeout=None):
            raise TimeoutError("timed out")

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            with self.assertRaises(ac.KilnConfigsApplyHttpError):
                ac.get_apply_status("host")

    def test_http_auth_error_is_wrapped_not_escaped(self):
        from kilnctrl import http_auth

        def fake_urlopen(req, timeout=None):
            raise http_auth.HttpAuthError("no credential available")

        with unittest.mock.patch.object(ac.http_auth, "urlopen", fake_urlopen):
            with self.assertRaises(ac.KilnConfigsApplyHttpError):
                ac.get_apply_status("host")


class PollApplyStatusTest(unittest.TestCase):
    def test_polls_to_terminal_state(self):
        seq = iter([{"state": "running"}, {"state": "done_ok", "diverged": False}])
        clock = {"t": 0.0}

        def now():
            return clock["t"]

        def sleep(s):
            clock["t"] += s

        with unittest.mock.patch.object(ac, "get_apply_status", side_effect=lambda host, timeout=8.0: next(seq)):
            result = ac.poll_apply_status("host", deadline_s=30.0, now=now, sleep=sleep)
        self.assertEqual(result["state"], "done_ok")

    def test_deadline_hit_returns_last_seen_non_terminal(self):
        clock = {"t": 0.0}

        def now():
            return clock["t"]

        def sleep(s):
            clock["t"] += s

        with unittest.mock.patch.object(ac, "get_apply_status", return_value={"state": "running"}):
            result = ac.poll_apply_status("host", deadline_s=1.0, now=now, sleep=sleep)
        self.assertEqual(result["state"], "running")

    def test_transient_read_failure_does_not_abort_poll(self):
        seq = iter([
            ac.KilnConfigsApplyHttpError("transient"),
            {"state": "done_failed", "reason": "interlock"},
        ])
        clock = {"t": 0.0}

        def now():
            return clock["t"]

        def sleep(s):
            clock["t"] += s

        def fake_get(host, timeout=8.0):
            item = next(seq)
            if isinstance(item, Exception):
                raise item
            return item

        with unittest.mock.patch.object(ac, "get_apply_status", side_effect=fake_get):
            result = ac.poll_apply_status("host", deadline_s=30.0, now=now, sleep=sleep)
        self.assertEqual(result["state"], "done_failed")

    def test_every_poll_failing_raises_named_error_instead_of_unknown(self):
        """If GET /api/kiln_configs/apply_status never once succeeds in the
        whole deadline, the caller must be told the status was unreadable --
        not handed back an empty/default dict that looks exactly like "polled
        fine, board just never finished" (state=None)."""
        clock = {"t": 0.0}

        def now():
            return clock["t"]

        def sleep(s):
            clock["t"] += s

        def fake_get(host, timeout=8.0):
            raise ac.KilnConfigsApplyHttpError("board unreachable mid-poll")

        with unittest.mock.patch.object(ac, "get_apply_status", side_effect=fake_get):
            with self.assertRaises(ac.KilnConfigsApplyHttpError) as ctx:
                ac.poll_apply_status("host", deadline_s=5.0, now=now, sleep=sleep)
        self.assertIn("board unreachable mid-poll", str(ctx.exception))


class IsHardwareDiffersBodyTest(unittest.TestCase):
    """The discriminator between the route's two different 428s."""

    def test_real_firmware_hardware_differs_body_is_recognised(self):
        body = ("this saved config's 'ct_installed' differs from what this controller currently "
                "reports -- confirm the hardware shape matches before applying. Resend with "
                "X-Kiln-Ack-Hardware-Differs: 1.")
        self.assertTrue(ac.is_hardware_differs_body(body))

    def test_interlock_reason_is_not_mistaken_for_it(self):
        self.assertFalse(ac.is_hardware_differs_body("no safety processor is connected"))
        self.assertFalse(ac.is_hardware_differs_body("a firing is running"))

    def test_empty_and_none_are_not_hardware_differs(self):
        self.assertFalse(ac.is_hardware_differs_body(""))
        self.assertFalse(ac.is_hardware_differs_body(None))


if __name__ == "__main__":
    unittest.main()
