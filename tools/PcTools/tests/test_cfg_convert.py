"""test_cfg_convert.py -- tests for kilnctrl.cfg_convert, the host-side
kiln backup-package version converter (see that module's docstring for the
"why"). Fixtures under tests/fixtures/cfg_convert/ are SYNTHESIZED (see that
directory's README.md) -- no real board capture exists in the tree today.

Must be run through the pytest runner, per CLAUDE.md's standing note that
plain unittest spuriously fails four PcTools tests.
"""
import json
from pathlib import Path

import pytest

from kilnctrl import cfg_convert

FIXTURES = Path(__file__).parent / "fixtures" / "cfg_convert"


def _load(name: str) -> dict:
    return cfg_convert.load_package((FIXTURES / name).read_text(encoding="utf-8"))


def test_load_rejects_non_backup_document():
    with pytest.raises(cfg_convert.CfgConvertError):
        cfg_convert.load_package(json.dumps({"kind": "not_a_backup", "version": 1}))


def test_load_rejects_forbidden_credential_key():
    doc = json.loads((FIXTURES / "v1_synthesized.json").read_text(encoding="utf-8"))
    doc["kiln_auth"] = {"ssid": "whatever"}
    with pytest.raises(cfg_convert.CfgConvertError, match="forbidden key"):
        cfg_convert.load_package(json.dumps(doc))


def test_load_rejects_nested_ssid_key():
    doc = json.loads((FIXTURES / "v1_synthesized.json").read_text(encoding="utf-8"))
    doc["zones"][0]["ssid"] = "HomeWifi"
    with pytest.raises(cfg_convert.CfgConvertError, match="forbidden key"):
        cfg_convert.load_package(json.dumps(doc))


def test_same_version_round_trip_is_lossless():
    doc = _load("v4_synthesized.json")
    out, report = cfg_convert.convert(doc, 4)
    assert out == doc
    assert not report.lossy


def test_forward_v1_to_v4_never_fabricates_new_zone2_fields():
    """v1 zones never had a coupling row, guard_* block, etc. Converting
    forward must never invent values for keys that were never in the
    source -- they must simply be absent in the output, not defaulted to
    0/false."""
    doc = _load("v1_synthesized.json")
    out, report = cfg_convert.convert(doc, 4)
    z1 = out["zones"][1]
    for key in ("name", "guard_wrong_dir_window_s", "fuzzy_strength_pct", "coupling_c0"):
        assert key not in z1, f"{key} must not be fabricated for a v1-sourced zone"


def test_never_fabricates_calibration():
    """normal_current_a must never appear on a zone that did not carry it in
    the source, at any target version, in either direction."""
    doc = _load("v4_synthesized.json")
    assert "normal_current_a" not in doc["zones"][1]
    for target in (1, 2, 3, 4):
        out, report = cfg_convert.convert(doc, target)
        assert "normal_current_a" not in out["zones"][1], (
            f"target v{target} fabricated a calibration value for zone 1"
        )
    # And the one zone that DID have it keeps the exact same value, never
    # re-derived.
    out, _ = cfg_convert.convert(doc, 4)
    assert out["zones"][0]["normal_current_a"] == doc["zones"][0]["normal_current_a"]


def test_never_fabricates_safety_i_normal_a():
    """safety_i_normal_a (the Pico's own S14/S15 arming baseline, added to
    the backup document 2026-09-16) is the same never-fabricate hazard as
    normal_current_a under a different key -- a zone lacking it in the
    source must never gain it at any target version."""
    doc = _load("v4_synthesized.json")
    assert "safety_i_normal_a" not in doc["zones"][1]
    for target in (1, 2, 3, 4):
        out, report = cfg_convert.convert(doc, target)
        assert "safety_i_normal_a" not in out["zones"][1], (
            f"target v{target} fabricated a Pico calibration value for zone 1"
        )


def test_downgrade_v4_to_v1_drops_everything_v1_cannot_express_and_says_so():
    doc = _load("v4_synthesized.json")
    out, report = cfg_convert.convert(doc, 1)
    z0 = out["zones"][0]
    # v1's document SHAPE only requires index/pid_*/model_*/tc_type -- these
    # must be dropped going to v1.
    for key in ("name", "guard_wrong_dir_window_s", "fuzzy_strength_pct", "coupling_c0", "coupling_c1"):
        assert key not in z0
    # normal_current_a and the settings_source_g<N> family are additive-only
    # at every BACKUP_FORMAT_VERSION (never version-gated by firmware, see
    # backup_import.c's own "no BACKUP_FORMAT_VERSION bump" comments) so
    # they are NOT stripped just because the target is v1 -- only the
    # coupling-representation change and the v1/v2/v3 shape fields are.
    assert z0["normal_current_a"] == doc["zones"][0]["normal_current_a"]
    assert z0["settings_source_g0"] == doc["zones"][0]["settings_source_g0"]
    assert report.lossy
    dropped_fields = {o.field for o in report.outcomes if o.outcome == "dropped"}
    assert "name" in dropped_fields
    assert any(f.startswith("coupling_c") for f in dropped_fields)


def test_downgrade_v4_to_v3_collapses_coupling_row_to_single_neighbor_and_reports_loss():
    doc = _load("v4_synthesized.json")
    out, report = cfg_convert.convert(doc, 3)
    z0 = out["zones"][0]
    # zone 0's row was coupling_c1=0.12, coupling_c2=0.05 -- 0.12 has the
    # larger magnitude and must be the one kept.
    assert z0["coupling_neighbor_zone"] == 1
    assert z0["coupling_coeff"] == pytest.approx(0.12)
    assert "coupling_c1" not in z0 and "coupling_c2" not in z0
    dropped = [o for o in report.outcomes if o.outcome == "dropped" and o.field == "coupling_c2"]
    assert dropped, "the smaller-magnitude neighbor coefficient must be reported as dropped"


def test_upgrade_v3_to_v4_expands_single_neighbor_to_full_row_and_reports_derivation():
    doc = _load("v3_synthesized.json")
    out, report = cfg_convert.convert(doc, 4)
    z0 = out["zones"][0]
    assert z0["coupling_c1"] == pytest.approx(0.12)
    assert z0["coupling_c0"] == 0.0
    assert "coupling_coeff" not in z0 and "coupling_neighbor_zone" not in z0
    derived = [o for o in report.outcomes if o.outcome == "derived" and "coupling" in o.field]
    assert derived, "expanding a legacy single-neighbor pair into a full row must be reported as derived"


def test_round_trip_a_to_b_to_a_is_not_reported_as_lossless():
    """A round trip through an intermediate version is lossy in general --
    converting v4 -> v1 -> v4 must not silently claim the second hop
    restored everything the first hop dropped."""
    v4 = _load("v4_synthesized.json")
    v1, report_down = cfg_convert.convert(v4, 1)
    back_to_v4, report_up = cfg_convert.convert(v1, 4)
    assert report_down.lossy
    # The round trip did not restore what was dropped -- the two zone
    # objects must differ from the original.
    assert back_to_v4["zones"][0] != v4["zones"][0]
    assert "coupling_c1" not in back_to_v4["zones"][0]
    assert "name" not in back_to_v4["zones"][0]


def test_fetch_board_backup_version_uses_export_endpoint(monkeypatch):
    calls = []

    class _FakeResp:
        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def read(self):
            return json.dumps({"kind": "kilnctl_backup", "version": 2, "zones": [], "profiles": []}).encode()

    def _fake_urlopen(url, timeout=None):
        calls.append(url)
        return _FakeResp()

    monkeypatch.setattr(cfg_convert.urllib.request, "urlopen", _fake_urlopen)
    version = cfg_convert.fetch_board_backup_version("192.168.1.42")
    assert version == 2
    assert calls == ["http://192.168.1.42/api/backup/export"]


def test_cli_to_board_end_to_end(tmp_path, monkeypatch, capsys):
    def _fake_fetch(host, timeout_s=5.0):
        assert host == "192.168.1.42"
        return 1

    monkeypatch.setattr(cfg_convert, "fetch_board_backup_version", _fake_fetch)
    out_path = tmp_path / "out.json"
    rc = cfg_convert.main([
        str(FIXTURES / "v4_synthesized.json"),
        "--to-board", "192.168.1.42",
        "-o", str(out_path),
        "--quiet",
    ])
    assert rc == 0
    out_doc = json.loads(out_path.read_text(encoding="utf-8"))
    assert out_doc["version"] == 1
    assert "name" not in out_doc["zones"][0]


def test_cli_rejects_forbidden_document(tmp_path):
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({
        "kind": "kilnctl_backup", "version": 4, "zones": [], "profiles": [],
        "kiln_auth": {"ssid": "x", "password": "y"},
    }))
    rc = cfg_convert.main([str(bad), "--to-version", "1", "--quiet"])
    assert rc == 1
