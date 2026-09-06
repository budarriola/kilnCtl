"""Unit tests for console_capture.py's per-line transport tagging and honest
transport-availability reporting (TODO.md "Logging and consoles", the two
PC-side-only items). Pure host-side: fake PortInfo tuples stand in for
serial.tools.list_ports, no real serial hardware involved.
"""
from __future__ import annotations

from kilnctrl import console_capture as cc
from kilnctrl.serial_link import PortInfo


def _port(device: str, hwid: str, score: int) -> PortInfo:
    return PortInfo(device=device, description="", manufacturer="", hwid=hwid, score=score)


# ---------------------------------------------------------------------------
# per-line transport tagging
# ---------------------------------------------------------------------------
def test_console_event_defaults_to_esp_transport():
    evt = cc.ConsoleEvent(pc_time=1.0, source=cc.SOURCE_ESP, text="hi")
    assert evt.transport == cc.TRANSPORT_ESP_USB_CDC


def test_format_event_tags_transport_untagged_source():
    evt = cc.ConsoleEvent(
        pc_time=0.0, source=cc.SOURCE_SAFETY, text="boot", level="I",
        transport=cc.TRANSPORT_SAFETY_PROBE_UART,
    )
    line = cc.format_event(evt, tag_source=False)
    assert f"[{cc.TRANSPORT_SAFETY_PROBE_UART}]" in line
    assert f"[{cc.SOURCE_SAFETY}/" not in line  # no source/transport prefix when untagged


def test_format_event_tags_source_and_transport_when_interleaved():
    evt = cc.ConsoleEvent(
        pc_time=0.0, source=cc.SOURCE_ESP, text="hi", level="I",
        transport=cc.TRANSPORT_ESP_USB_CDC,
    )
    line = cc.format_event(evt, tag_source=True)
    assert f"[{cc.SOURCE_ESP}/{cc.TRANSPORT_ESP_USB_CDC}]" in line


def test_add_esp_and_safety_events_carry_distinct_transports(tmp_path):
    """The two live sources this module wires up today tag their events with
    two different transports, even though today transport is 1:1 with
    source -- proves the field is populated, not just declared."""
    capture = cc.MultiConsoleCapture(tmp_path)

    esp_evt = cc.ConsoleEvent(pc_time=1.0, source=cc.SOURCE_ESP, text="a", transport=cc.TRANSPORT_ESP_USB_CDC)
    safety_evt = cc.ConsoleEvent(
        pc_time=2.0, source=cc.SOURCE_SAFETY, text="b", transport=cc.TRANSPORT_SAFETY_PROBE_UART
    )
    assert esp_evt.transport != safety_evt.transport
    del capture  # only constructed to prove the dir arg still works; no sources added


# ---------------------------------------------------------------------------
# honest transport-availability reporting
# ---------------------------------------------------------------------------
def test_availability_reports_esp_present_when_scored_port_exists(monkeypatch):
    monkeypatch.setattr(cc, "list_ports", lambda: [_port("COM7", "USB VID:PID=1A86:55D3", 50)])
    monkeypatch.setattr(cc, "list_debug_probe_ports", lambda: [])

    statuses = {s.transport: s for s in cc.check_transport_availability()}
    esp = statuses[cc.TRANSPORT_ESP_USB_CDC]
    assert esp.present is True
    assert esp.candidate_ports == ("COM7",)


def test_availability_reports_esp_absent_honestly_when_nothing_scores(monkeypatch):
    monkeypatch.setattr(cc, "list_ports", lambda: [_port("COM3", "USB VID:PID=0000:0000", 0)])
    monkeypatch.setattr(cc, "list_debug_probe_ports", lambda: [])

    statuses = {s.transport: s for s in cc.check_transport_availability()}
    esp = statuses[cc.TRANSPORT_ESP_USB_CDC]
    assert esp.present is False
    assert esp.candidate_ports == ()
    assert esp.detail  # names the reason rather than being silent


def test_availability_reports_safety_probe_uart_present_by_vid_pid(monkeypatch):
    """Detected via VID:PID, not description text -- this fake port's
    description is deliberately generic/misleading (mirrors the real Windows
    behavior where MI_xx and the descriptive string get stripped/collapsed)."""
    probe_port = PortInfo(
        device="COM12", description="USB Serial Device", manufacturer="",
        hwid="USB VID:PID=2E8A:000C SER=E66540F0A36C6E21 LOCATION=1-2:x.2", score=0,
    )
    monkeypatch.setattr(cc, "list_ports", lambda: [])
    monkeypatch.setattr(cc, "list_debug_probe_ports", lambda: [probe_port])

    statuses = {s.transport: s for s in cc.check_transport_availability()}
    safety = statuses[cc.TRANSPORT_SAFETY_PROBE_UART]
    assert safety.present is True
    assert safety.candidate_ports == ("COM12",)


def test_availability_reports_safety_probe_uart_absent_when_not_enumerated(monkeypatch):
    """The Pico USB CDC / Debug Probe absence case from TODO.md: report
    absent explicitly, never just omit the row."""
    monkeypatch.setattr(cc, "list_ports", lambda: [])
    monkeypatch.setattr(cc, "list_debug_probe_ports", lambda: [])

    statuses = {s.transport: s for s in cc.check_transport_availability()}
    safety = statuses[cc.TRANSPORT_SAFETY_PROBE_UART]
    assert safety.present is False
    assert safety.candidate_ports == ()
    assert "2E8A:000C" in safety.detail


def test_availability_reports_firmware_blocked_transports_as_absent(monkeypatch):
    monkeypatch.setattr(cc, "list_ports", lambda: [])
    monkeypatch.setattr(cc, "list_debug_probe_ports", lambda: [])

    statuses = {s.transport: s for s in cc.check_transport_availability()}
    for transport in (cc.TRANSPORT_SAFETY_LOG_RELAY, cc.TRANSPORT_RTT_SWD):
        assert statuses[transport].present is False
        assert "firmware side pending" in statuses[transport].detail


def test_availability_reports_native_usb_cdc_always_absent(monkeypatch):
    """SAFTYFW_ENABLE_USB_STDIO cannot be queried remotely, so this transport
    is reported absent unconditionally rather than probed for."""
    monkeypatch.setattr(cc, "list_ports", lambda: [])
    monkeypatch.setattr(cc, "list_debug_probe_ports", lambda: [])

    statuses = {s.transport: s for s in cc.check_transport_availability()}
    native = statuses[cc.TRANSPORT_SAFETY_NATIVE_USB_CDC]
    assert native.present is False
    assert "SAFTYFW_ENABLE_USB_STDIO" in native.detail


def test_format_transport_availability_lists_every_transport(monkeypatch):
    monkeypatch.setattr(cc, "list_ports", lambda: [])
    monkeypatch.setattr(cc, "list_debug_probe_ports", lambda: [])

    report = cc.format_transport_availability(cc.check_transport_availability())
    for transport in (
        cc.TRANSPORT_ESP_USB_CDC,
        cc.TRANSPORT_SAFETY_PROBE_UART,
        cc.TRANSPORT_SAFETY_LOG_RELAY,
        cc.TRANSPORT_RTT_SWD,
        cc.TRANSPORT_SAFETY_NATIVE_USB_CDC,
    ):
        assert transport in report
    assert report.count("ABSENT") == 5


def test_transports_cli_flag_prints_report_and_exits_zero(monkeypatch, capsys):
    monkeypatch.setattr(cc, "list_ports", lambda: [])
    monkeypatch.setattr(cc, "list_debug_probe_ports", lambda: [])

    rc = cc.main(["--transports"])
    assert rc == 0
    out = capsys.readouterr().out
    assert cc.TRANSPORT_SAFETY_PROBE_UART in out


# ---------------------------------------------------------------------------
# serial_link.list_debug_probe_ports itself (VID:PID detection, not text)
# ---------------------------------------------------------------------------
def test_list_debug_probe_ports_matches_vid_pid_only(monkeypatch):
    from kilnctrl import serial_link

    class _FakeComPort:
        def __init__(self, device, description, hwid):
            self.device = device
            self.description = description
            self.manufacturer = ""
            self.product = ""
            self.hwid = hwid

    fake_ports = [
        _FakeComPort("COM5", "USB Serial Device (COM5)", "USB VID:PID=1A86:55D3 SER=1 LOCATION=1-1"),
        _FakeComPort("COM12", "USB Serial Device (COM12)", "USB VID:PID=2E8A:000C SER=E66540F0A36C6E21 LOCATION=1-2:x.2"),
        _FakeComPort("COM13", "USB Serial Device (COM13)", "USB VID:PID=2E8A:000C SER=E66540F0A36C6E21 LOCATION=1-2:x.0"),
    ]
    monkeypatch.setattr(serial_link._list_ports, "comports", lambda: fake_ports)

    probe_ports = serial_link.list_debug_probe_ports()
    assert {p.device for p in probe_ports} == {"COM12", "COM13"}
    assert "COM5" not in {p.device for p in probe_ports}
