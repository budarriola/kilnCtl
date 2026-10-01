"""Lazy reconnect of the shared serial link (bench 2026-10-01: safety_get_status
failed "no serial port open - connect first" after the MCP server had been up a
while, fixed by a manual connect()).

End-to-end over loopback: a real LinkHub on an ephemeral port with a fake
UartLink behind it, driven through a real RemoteUartLink.
"""
import socket
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

    @property
    def is_connected(self):
        return self.connected

    def connect(self, port=None):
        self.connect_calls.append(port)
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


def test_debug_reset_doc_and_verify_window_match_bench_boot_time():
    from kilnctrl import mcp_server_debug, reset_probe

    doc = mcp_server_debug.debug_reset.__doc__ or ""
    assert "5-10 s" not in doc
    assert "20-25 s" in doc
    # Bench boot-to-answer was 20.0 s (UART) / 23.2 s (HTTP): keep >= ~2x margin.
    assert reset_probe.DEFAULT_WINDOW_S >= 45.0
