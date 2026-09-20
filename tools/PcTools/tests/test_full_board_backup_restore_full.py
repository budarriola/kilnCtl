#!/usr/bin/env python3
"""Tests for full_board_backup.py's restore_full() -- the round-trip gap
closure task.

Context: full_board_backup.py's GET_ENDPOINTS captures far more than the
old restore path (restore_cfgfs_files() alone) could put back: kiln config
slots, relay cycle counters, ramp-assist, display power, unit preference,
and zones config + profiles all had no orchestrated restore path, and
POST /api/relay_cycles/restore existed in firmware with zero callers. This
adds restore_full(), which restores every domain it can and NAMES every
domain it cannot (IRREDUCIBLE_DOMAINS), rather than silently doing only
some of it.

All HTTP is mocked (urllib.request), same convention as
test_full_board_backup_cfgfs.py. Run with:
    uv run pytest tools/PcTools/tests/test_full_board_backup_restore_full.py
"""
from __future__ import annotations

import base64
import sys
import urllib.parse
import unittest.mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import full_board_backup as fbb  # noqa: E402


class _FakeResponse:
    def __init__(self, body: bytes):
        self._body = body

    def read(self):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def _make_fixture_archive():
    """A representative captured board_backup.json covering every domain
    restore_full() knows how to restore."""
    return {
        "kind": "kilnctl_full_board_backup",
        "host": "10.0.0.5",
        "endpoints": {
            "/api/backup/export": {"zones": [{"id": 0, "pid_kp": 12.5}], "profiles": []},
            "/api/status": {
                "temp_unit": "F",
                "time_tz": "CST6CDT,M3.2.0,M11.1.0",
                "relay_life": [
                    {"relay": 0, "cycles": 111},
                    {"relay": 1, "cycles": 222},
                    {"relay": 2, "cycles": 0},
                    {"relay": 3, "cycles": 0},
                    {"relay": 4, "cycles": 5},
                ],
            },
            "/api/ramp_assist": {"enabled": True},
            "/api/settings/display_power": {
                "brightness_percent": 80,
                "timeout_setting": 2,
                "keep_on_while_firing": True,
                "display_on_error": False,
            },
            "/api/kiln_configs": {"active_id": 3, "configs": [{"id": 3, "name": "cone6", "is_active": True}]},
        },
        "kiln_config_exports": {"3": '{"name":"cone6","kind":"kiln_cfg_package"}'},
        "cfgfs_files": {},
    }


# A LIVE /api/status the board would answer during the pre-restore
# quiescence check: io ready, every ESP relay off, K4 de-energized, and
# relay_life counts BELOW the archive's (so the monotonic guard passes
# through the archived values unchanged in the happy path).
def _quiescent_status(relay_life=None, **overrides):
    status = {
        "io_ready": True,
        "relays": [{"relay": n, "on": False} for n in range(1, 6)],
        "safety_relay_energized": False,
        "safety_heating_enabled": True,  # ARMED latch -- must NOT by itself refuse
        "relay_life": relay_life if relay_life is not None else [
            {"relay": 0, "cycles": 100},
            {"relay": 1, "cycles": 200},
            {"relay": 2, "cycles": 0},
            {"relay": 3, "cycles": 0},
            {"relay": 4, "cycles": 5},
        ],
    }
    status.update(overrides)
    return status


def _make_fake_urlopen(post_log: list, profile_exec_state: str = "idle",
                       autotune_state: str = "idle", live_status=None,
                       live_kiln_configs=None):
    import json as _json

    def fake_urlopen(req_or_url, timeout=None):
        if isinstance(req_or_url, str):
            url, method, body = req_or_url, "GET", None
        else:
            url = req_or_url.full_url
            method = req_or_url.get_method()
            body = req_or_url.data

        if url.endswith("/api/profile_exec"):
            return _FakeResponse(_json.dumps({"state": profile_exec_state}).encode())
        if url.endswith("/api/autotune"):
            return _FakeResponse(_json.dumps({"state": autotune_state}).encode())
        if url.endswith("/api/status"):
            return _FakeResponse(_json.dumps(
                _quiescent_status() if live_status is None else live_status).encode())
        if url.endswith("/api/kiln_configs"):
            return _FakeResponse(_json.dumps(
                {"active_id": 3, "configs": []} if live_kiln_configs is None
                else live_kiln_configs).encode())

        if method == "POST":
            post_log.append((url, body))
            return _FakeResponse(b'{"ok":true}')

        raise AssertionError(f"unexpected GET in restore_full test: {url}")

    return fake_urlopen


def test_restore_full_round_trips_every_restorable_domain():
    """The mandatory genuine round-trip test: capture-shaped fixture in,
    restore_full() out, assert every restorable domain issued the right
    call AND that the result names every domain that did not (irreducible
    list) -- silence about a domain is exactly the bug being fixed."""
    archive = _make_fixture_archive()
    post_log: list = []
    fake_urlopen = _make_fake_urlopen(post_log)

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        result = fbb.restore_full("10.0.0.5", archive, timeout=5.0, dry_run=False)

    assert result["refused"] is None, result["refused"]
    assert result["failed"] == [], result["failed"]

    restored_domains = {e["domain"] for e in result["restored"]}
    expected_domains = {
        "zones_config_and_profiles", "kiln_config_slots", "relay_cycle_counters",
        "ramp_assist", "display_power", "unit_preference", "timezone", "cfg_filesystem_files",
    }
    assert restored_domains == expected_domains, restored_domains

    urls_posted = [u for u, _b in post_log]
    assert any("/api/backup/import" in u for u in urls_posted)
    assert any("/api/kiln_configs/import" in u for u in urls_posted)
    assert any("/api/relay_cycles/restore" in u for u in urls_posted)
    assert any("/api/ramp_assist" in u for u in urls_posted)
    assert any("/api/settings/display_power" in u for u in urls_posted)
    assert any("/api/unit_pref" in u for u in urls_posted)
    assert any("/api/settings/tz" in u for u in urls_posted)

    # relay_cycles/restore body carries every captured count, byte for byte
    # as decimal fields -- this is the "wire up the existing unused route"
    # task item.
    relay_body = next(b for u, b in post_log if "/api/relay_cycles/restore" in u)
    fields = dict(urllib.parse.parse_qsl(relay_body.decode()))
    assert fields == {"c0": "111", "c1": "222", "c2": "0", "c3": "0", "c4": "5"}

    # unit preference round-trips the captured "F".
    unit_body = next(b for u, b in post_log if "/api/unit_pref" in u)
    assert dict(urllib.parse.parse_qsl(unit_body.decode())) == {"unit": "F"}

    # Every domain this tool can never restore is still named, even on a
    # fully successful restore -- never silence.
    not_restorable_items = {e["item"] for e in result["not_restorable"]}
    assert "Wi-Fi credentials" in not_restorable_items
    assert not any("tz" in item.lower() or "timezone" in item.lower()
                   for item in not_restorable_items), \
        "TZ is round-trippable (time_tz in /api/status + POST /api/settings/tz) -- it must not be " \
        "parked as irreducible; a domain listed there when a route exists is a hidden gap"
    assert any("safety commissioning" in item.lower() for item in not_restorable_items)
    assert any("ki-diagnosis baseline" in item.lower() or "ki_baseline" in item.lower()
               for item in not_restorable_items)


def test_restore_refuses_outright_while_a_firing_is_active():
    """NEGATIVE-SAFETY TEST: profile_exec state "running" must refuse the
    WHOLE restore before any POST is issued -- proven by asserting zero
    POSTs, matching the file's existing all-or-nothing convention."""
    archive = _make_fixture_archive()
    post_log: list = []
    fake_urlopen = _make_fake_urlopen(post_log, profile_exec_state="running")

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        result = fbb.restore_full("10.0.0.5", archive, timeout=5.0, dry_run=False)

    assert result["refused"] is not None
    assert "running" in result["refused"]
    assert post_log == [], "no domain may be restored while a firing is active or paused"
    assert result["restored"] == []


def test_restore_refuses_outright_while_paused():
    archive = _make_fixture_archive()
    post_log: list = []
    fake_urlopen = _make_fake_urlopen(post_log, profile_exec_state="paused")

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        result = fbb.restore_full("10.0.0.5", archive, timeout=5.0, dry_run=False)

    assert result["refused"] is not None
    assert post_log == []


def test_restore_refuses_when_board_state_cannot_be_confirmed():
    """An unreachable /api/profile_exec is a hard refusal, never assumed idle."""
    archive = _make_fixture_archive()

    def fake_urlopen(req_or_url, timeout=None):
        import urllib.error
        raise urllib.error.URLError("connection refused")

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        result = fbb.restore_full("10.0.0.5", archive, timeout=5.0, dry_run=False)

    assert result["refused"] is not None
    assert "could not confirm" in result["refused"]


def test_restore_reports_missing_domains_without_crashing():
    """An archive missing a section (e.g. captured by an older script
    version, or a required endpoint failed at backup time) must be reported
    as a named FAILURE for that domain, never a silent skip and never a
    crash that hides every other domain's result."""
    archive = {
        "endpoints": {},  # nothing captured at all
        "kiln_config_exports": {},
        "cfgfs_files": {},
    }
    post_log: list = []
    fake_urlopen = _make_fake_urlopen(post_log)

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        result = fbb.restore_full("10.0.0.5", archive, timeout=5.0, dry_run=False)

    assert result["refused"] is None
    failed_domains = {e["domain"] for e in result["failed"]}
    assert "zones_config_and_profiles" in failed_domains
    assert "relay_cycle_counters" in failed_domains
    assert "ramp_assist" in failed_domains
    assert "display_power" in failed_domains
    assert "unit_preference" in failed_domains
    assert "timezone" in failed_domains
    # kiln_config_slots and cfg_filesystem_files legitimately have nothing
    # to restore when the archive's section is empty (not every board has
    # saved kiln config slots) -- that is success, not failure.
    restored_domains = {e["domain"] for e in result["restored"]}
    assert "kiln_config_slots" in restored_domains
    assert "cfg_filesystem_files" in restored_domains


def test_dry_run_writes_nothing():
    archive = _make_fixture_archive()
    post_log: list = []
    fake_urlopen = _make_fake_urlopen(post_log)

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        result = fbb.restore_full("10.0.0.5", archive, timeout=5.0, dry_run=True)

    assert result["refused"] is None
    assert result["failed"] == []
    assert post_log == [], "dry run must never POST"


def test_print_restore_report_names_every_bucket(capsys):
    archive = _make_fixture_archive()
    post_log: list = []
    fake_urlopen = _make_fake_urlopen(post_log)
    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        result = fbb.restore_full("10.0.0.5", archive, timeout=5.0, dry_run=False)
    code = fbb.print_restore_report(result)
    out = capsys.readouterr().out
    assert code == 0
    assert "Restored domains:" in out
    assert "Never restorable by this tool" in out
    assert "Wi-Fi credentials" in out



# ---------------------------------------------------------------------------
# 2026-09-20 landing review: regression tests for the defects found in the
# version above. Each names the defect it pins.
# ---------------------------------------------------------------------------

def test_accepted_profile_exec_state_is_real_firmware_vocabulary():
    """ANTI-FABRICATION (the 24954063 / 661d229f class): the state string
    this tool gates on must come from the vocabulary a device actually
    emits, not one invented here and fed back to itself by the fixtures.
    Sourced from kilnctrl.devices_profiles.ProfileExecStatus.STATE_NAMES --
    the same mirror of firmware's exec_state_name() the bench suite uses."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
    from kilnctrl.devices_profiles import ProfileExecStatus

    real = set(ProfileExecStatus.STATE_NAMES.values())
    assert fbb.PROFILE_EXEC_QUIESCENT_STATES <= real, \
        "this tool accepts a profile_exec state no device emits"
    assert fbb.PROFILE_EXEC_QUIESCENT_STATES == {"idle"}
    # Every OTHER real state must actually refuse -- driven from the device
    # module's own dict, so a firmware state added there without being
    # considered here fails this test rather than passing silently.
    for state in sorted(real - fbb.PROFILE_EXEC_QUIESCENT_STATES):
        post_log: list = []
        with unittest.mock.patch("urllib.request.urlopen",
                                 side_effect=_make_fake_urlopen(post_log, profile_exec_state=state)):
            result = fbb.restore_full("10.0.0.5", _make_fixture_archive(), timeout=5.0)
        assert result["refused"] is not None, f"state {state!r} was let through"
        assert post_log == []


def test_restore_refuses_while_autotune_is_running_with_exec_idle():
    """DEFECT 1: autotune drives real heat while /api/profile_exec still
    reports "idle" -- it does not run through the profile executor. A
    profile_exec-only gate would have let a restore overwrite zones/PID
    config under a live heat output."""
    for at_state in ("settling", "stepping", "relay_approach", "relay_cycling"):
        post_log: list = []
        fake = _make_fake_urlopen(post_log, profile_exec_state="idle", autotune_state=at_state)
        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake):
            result = fbb.restore_full("10.0.0.5", _make_fixture_archive(), timeout=5.0)
        assert result["refused"] is not None, f"autotune {at_state!r} was let through"
        assert "autotune" in result["refused"]
        assert post_log == [], "no domain may be written while autotune is heating"


def test_restore_refuses_while_a_relay_is_forced_on():
    """DEFECT 1, second path: POST /api/diagnostics/danger/relay can force a
    relay on with neither the executor nor autotune running. That is visible
    only in /api/status's relays[]."""
    post_log: list = []
    status = _quiescent_status()
    status["relays"][2]["on"] = True
    fake = _make_fake_urlopen(post_log, live_status=status)
    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake):
        result = fbb.restore_full("10.0.0.5", _make_fixture_archive(), timeout=5.0)
    assert result["refused"] is not None
    assert "energized" in result["refused"]
    assert post_log == []


def test_restore_refuses_when_k4_is_energized_or_unknown():
    """DEFECT 1, third path: K4 is the relay that actually gates heat. null
    means the safety link never answered -- unknown, which is refused, not
    assumed safe."""
    for k4 in (True, None):
        post_log: list = []
        status = _quiescent_status(safety_relay_energized=k4)
        fake = _make_fake_urlopen(post_log, live_status=status)
        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake):
            result = fbb.restore_full("10.0.0.5", _make_fixture_archive(), timeout=5.0)
        assert result["refused"] is not None, f"safety_relay_energized={k4!r} was let through"
        assert post_log == []


def test_armed_latch_alone_does_not_refuse():
    """The converse, so the gate above cannot be "widened" into uselessness:
    safety_heating_enabled is true on ANY healthy past-grace Pico regardless
    of whether heat was ever requested (dashboard_http.h), so gating on it
    would refuse every restore forever. A quiescent board with the ARMED
    latch set must still restore."""
    post_log: list = []
    status = _quiescent_status(safety_heating_enabled=True, safety_relay_energized=False)
    fake = _make_fake_urlopen(post_log, live_status=status)
    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake):
        result = fbb.restore_full("10.0.0.5", _make_fixture_archive(), timeout=5.0)
    assert result["refused"] is None, result["refused"]
    assert post_log != []


def test_restore_refuses_when_relay_state_cannot_be_read():
    """io_ready:false / io_read_failed means UNKNOWN relay state, and
    unknown is not quiescent."""
    for overrides in ({"io_ready": False}, {"io_read_failed": True}):
        post_log: list = []
        fake = _make_fake_urlopen(post_log, live_status=_quiescent_status(**overrides))
        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake):
            result = fbb.restore_full("10.0.0.5", _make_fixture_archive(), timeout=5.0)
        assert result["refused"] is not None, f"{overrides} was let through"
        assert post_log == []


def test_relay_cycle_counters_are_never_lowered_by_a_stale_archive():
    """DEFECT 2: firmware's relay_cycles_restore_all() enforces only an
    UPPER sanity ceiling -- it accepts a value lower than the board's
    current count, which understates wear. The clamp lives here and is
    reported, never silent."""
    post_log: list = []
    live = _quiescent_status(relay_life=[
        {"relay": 0, "cycles": 999},   # board is AHEAD of the archive's 111
        {"relay": 1, "cycles": 200},
        {"relay": 2, "cycles": 0},
        {"relay": 3, "cycles": 0},
        {"relay": 4, "cycles": 5},
    ])
    fake = _make_fake_urlopen(post_log, live_status=live)
    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake):
        result = fbb.restore_full("10.0.0.5", _make_fixture_archive(), timeout=5.0)

    body = next(b for u, b in post_log if "/api/relay_cycles/restore" in u)
    fields = dict(urllib.parse.parse_qsl(body.decode()))
    assert fields["c0"] == "999", "a wear counter was moved DOWNWARD by a stale archive"
    assert fields["c1"] == "222", "an archived count higher than live must still be restored"
    entry = next(e for e in result["restored"] if e["domain"] == "relay_cycle_counters")
    assert "STALE ARCHIVE" in entry["detail"], "the clamp must be reported, not silent"
    assert "relay 0" in entry["detail"]


def test_kiln_config_import_cannot_silently_exhaust_the_slot_store():
    """DEFECT 3: kiln_cfg_http.c's import ALWAYS allocates a new slot. With
    the board already near KILN_CFG_MAX_COUNT the old code would import
    until firmware started answering "store is full", leaving a
    half-restored store. Refuse up front, naming the numbers."""
    archive = _make_fixture_archive()
    archive["kiln_config_exports"] = {str(i): '{"kind":"kiln_cfg_package"}' for i in range(3)}
    full = {"active_id": 1, "configs": [{"id": i, "name": f"c{i}", "is_active": False} for i in range(9)]}
    post_log: list = []
    fake = _make_fake_urlopen(post_log, live_kiln_configs=full)
    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake):
        result = fbb.restore_full("10.0.0.5", archive, timeout=5.0)

    entry = next(e for e in result["failed"] if e["domain"] == "kiln_config_slots")
    assert "9" in entry["detail"] and str(fbb.KILN_CFG_MAX_COUNT) in entry["detail"]
    assert not any("/api/kiln_configs/import" in u for u, _b in post_log), \
        "nothing may be imported when the store cannot hold all of it"


def test_http_error_body_is_surfaced_not_swallowed():
    """DEFECT 4: firmware puts the only actionable reason in the 400 body
    ("kiln config store is full"); str(HTTPError) is just "HTTP Error 400:
    Bad Request"."""
    import io
    import urllib.error as _uerr

    post_log: list = []
    base = _make_fake_urlopen(post_log)

    def fake(req_or_url, timeout=None):
        url = req_or_url if isinstance(req_or_url, str) else req_or_url.full_url
        if url.endswith("/api/kiln_configs/import"):
            raise _uerr.HTTPError(url, 400, "Bad Request", {},
                                  io.BytesIO(b'{"error":"kiln config store is full"}'))
        return base(req_or_url, timeout)

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake):
        result = fbb.restore_full("10.0.0.5", _make_fixture_archive(), timeout=5.0)

    entry = next(e for e in result["failed"] if e["domain"] == "kiln_config_slots")
    assert "kiln config store is full" in entry["detail"]


def test_timezone_round_trips_from_status_time_tz():
    """DEFECT 5 / stale claim: TZ was parked as irreducible on the grounds
    that /api/settings/tz has no GET. The VALUE is carried as time_tz in
    /api/status, and the POST takes it straight back."""
    post_log: list = []
    with unittest.mock.patch("urllib.request.urlopen", side_effect=_make_fake_urlopen(post_log)):
        result = fbb.restore_full("10.0.0.5", _make_fixture_archive(), timeout=5.0)
    assert result["refused"] is None
    body = next(b for u, b in post_log if "/api/settings/tz" in u)
    assert dict(urllib.parse.parse_qsl(body.decode())) == {"tz": "CST6CDT,M3.2.0,M11.1.0"}
    assert any(e["domain"] == "timezone" for e in result["restored"])


if __name__ == "__main__":
    import pytest

    raise SystemExit(pytest.main([__file__, "-v"]))
