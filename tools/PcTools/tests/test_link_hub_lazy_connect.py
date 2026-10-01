"""Lazy reconnect of the shared serial link (bench 2026-10-01: safety_get_status
failed "no serial port open - connect first" after the MCP server had been up a
while, fixed by a manual connect()).

End-to-end over loopback: a real LinkHub on an ephemeral port with a fake
UartLink behind it, driven through a real RemoteUartLink.
"""
import socket
import threading
import time

import pytest

from kilnctrl.link_hub import LinkHub, RemoteUartLink
from kilnctrl.serial_link import SendResult


class FakeLink:
    def __init__(self, fail_connect=None):
        self.connected = False
        self.port = None
        self.connect_calls = []
        self.fail_connect = fail_connect
        self.baudrate = 230400
        self.gate = None  # Event: connect() blocks until set
        self.entered = threading.Event()

    @property
    def is_connected(self):
        return self.connected

    def connect(self, port=None):
        self.connect_calls.append(port)
        if self.gate is not None:
            self.entered.set()
            assert self.gate.wait(5)
        if self.fail_connect:
            raise RuntimeError(self.fail_connect)
        self.connected = True
        self.port = port or "COM14"
        return self.port

    def disconnect(self):
        self.connected = False
        self.port = None

    def send(self, **kw):
        return SendResult.OK if self.connected else SendResult.NOT_CONNECTED

    def status(self):
        return {"connected": self.connected, "port": self.port, "baudrate": self.baudrate}


@pytest.fixture
def rig():
    ls = socket.socket()
    ls.bind(("127.0.0.1", 0))
    ls.listen(4)
    hub = LinkHub(ls)
    hub.link = FakeLink()
    hub.start()
    client = RemoteUartLink(host="127.0.0.1", port=ls.getsockname()[1])
    yield hub, client
    client.close()
    ls.close()


def _send(client):
    return client.send(dst_task=1, src_task=1, payload=b"")


def test_send_on_closed_link_reconnects_lazily(rig):
    hub, client = rig
    assert _send(client) is SendResult.OK
    assert hub.link.connect_calls == [None]  # autodiscover, like connect()
    assert client.is_connected


def test_lazy_reconnect_reuses_last_port(rig):
    hub, client = rig
    client.connect("COM9")
    hub.link.connected = False  # port closed behind everyone's back
    client._is_connected = False
    assert _send(client) is SendResult.OK
    assert hub.link.connect_calls == ["COM9", "COM9"]


def test_explicit_disconnect_is_respected(rig):
    hub, client = rig
    client.connect("COM9")
    client.disconnect()
    time.sleep(0.2)
    assert _send(client) is SendResult.NOT_CONNECTED
    assert hub.link.connect_calls == ["COM9"]  # no lazy reopen
    # an explicit connect re-arms lazy reconnect
    client.connect("COM9")
    hub.link.connected = False
    client._is_connected = False
    assert _send(client) is SendResult.OK


def test_port_held_elsewhere_is_not_stolen(rig):
    hub, client = rig
    hub.link.fail_connect = "could not open port 'COM14': access denied"
    assert _send(client) is SendResult.NOT_CONNECTED
    assert not client.is_connected
    assert hub.link.connect_calls == [None]  # an open WAS attempted, and swallowed
    assert client.last_hub_failure is None  # port-level failure, not an RPC failure


def test_debug_reset_doc_and_verify_window_match_bench_boot_time():
    from kilnctrl import mcp_server_debug, reset_probe

    doc = mcp_server_debug.debug_reset.__doc__ or ""
    assert "5-10 s" not in doc
    assert "20-25 s" in doc
    # Bench boot-to-answer was 20.0 s (UART) / 23.2 s (HTTP): keep >= ~2x margin.
    assert reset_probe.DEFAULT_WINDOW_S >= 45.0


def test_lazy_open_cannot_beat_an_explicit_disconnect(rig):
    """Force the interleaving: a lazy open is mid-connect when an explicit
    disconnect arrives. The disconnect must win (end closed + latched)."""
    hub, _client = rig
    hub.link.gate = threading.Event()
    lazy = threading.Thread(target=hub.ensure_connected)
    lazy.start()
    assert hub.link.entered.wait(5)
    disc = threading.Thread(target=hub.disconnect)
    disc.start()
    time.sleep(0.3)  # without a shared lock the disconnect finishes here
    hub.link.gate.set()
    lazy.join(5)
    disc.join(5)
    assert hub.link.connected is False
    assert hub._explicit_disconnect is True


def test_explicit_connect_not_failed_by_concurrent_lazy_open(rig):
    hub, _client = rig
    hub.link.gate = threading.Event()
    lazy = threading.Thread(target=hub.ensure_connected)
    lazy.start()
    assert hub.link.entered.wait(5)
    results = []
    conn = threading.Thread(target=lambda: results.append(hub.connect("COM9")))
    conn.start()
    time.sleep(0.2)
    hub.link.gate.set()
    lazy.join(5)
    conn.join(5)
    assert results  # the explicit connect completed (FakeLink never raises "already connected")
    assert hub.link.connected


def test_disconnect_on_a_closed_looking_link_still_latches(rig):
    hub, client = rig
    assert not client.is_connected
    client.disconnect()  # client believes closed; the op must still be sent
    time.sleep(0.2)
    assert hub._explicit_disconnect is True
    assert _send(client) is SendResult.NOT_CONNECTED
    assert hub.link.connect_calls == []


def test_mcp_disconnect_tool_sends_op_even_when_link_reads_closed(monkeypatch):
    from kilnctrl import mcp_server_link as msl

    class L:
        is_connected = False
        port = None
        calls = 0

        def disconnect(self):
            L.calls += 1

    monkeypatch.setattr(msl._srv, "_link", L())
    out = msl.disconnect()
    assert L.calls == 1
    assert "latched" in out


def test_failed_lazy_open_backs_off(rig):
    hub, client = rig
    hub.link.fail_connect = "busy"
    assert _send(client) is SendResult.NOT_CONNECTED
    assert _send(client) is SendResult.NOT_CONNECTED
    assert hub.link.connect_calls == [None]  # second send skipped the open
    hub._lazy_retry_after = 0.0  # backoff elapsed
    assert _send(client) is SendResult.NOT_CONNECTED
    assert hub.link.connect_calls == [None, None]
    hub.link.fail_connect = None
    hub._lazy_retry_after = 0.0
    assert _send(client) is SendResult.OK


def test_hub_rpc_failure_is_distinguishable_from_closed_port(rig):
    hub, client = rig
    client.connect("COM9")
    for _ in range(50):
        if client.is_connected:
            break
        time.sleep(0.05)
    assert client.is_connected

    def boom(*a, **k):
        raise TimeoutError("hub did not respond to 'send' within 8.0s")

    client._request = boom
    assert _send(client) is SendResult.NOT_CONNECTED  # return code unchanged
    assert "hub RPC 'send' failed" in client.last_hub_failure
