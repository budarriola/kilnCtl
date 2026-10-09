"""Tests for kilnctrl.coupling_workflow -- the one-command wiring of
run_queue.py + coupled_ident.py + config_presets.py into a repeatable
coupling-matrix re-identification pass.

Every capture here is synthetic (built as ``log_analysis.PollRow`` objects
and serialized into ``run_queue.run_entry``'s own ``{"t","exec","status"}``
capture format -- see ``coupling_workflow``'s module docstring's CAPTURE
FORMAT section for why that, not the plain ``HH:MM:SS {body}`` shape,
matters). No HTTP, no board: ``run_queue.run_entry`` itself is monkeypatched
in every test that exercises firing/resumability, and every scoring/
assembly/preset test drives entirely over files on disk.

Negative-test-every-check discipline: :func:`test_settle_gate_refuses_on_
unsettled_active_zone` and :func:`test_settle_gate_refuses_on_flagged_
unstable` are the refuse-on-unsettled proof this task exists to deliver --
each has a positive sibling (settled/stable) showing the same data passes
once the defect is fixed, so the gate is shown able to go both red and
green, not just asserted never to fire.
"""
from __future__ import annotations

import dataclasses
import json
import os

import numpy as np
import pytest

from kilnctrl import config_presets
from kilnctrl import coupled_ident as ci
from kilnctrl import coupling_workflow as cw
from kilnctrl import http_capture_log as hc
from kilnctrl import log_analysis as la


# ---------------------------------------------------------------------------
# Synthetic capture builders
# ---------------------------------------------------------------------------

def _zs(actual_c, duty):
    return la.ZoneSample(zone=0, actual_c=round(actual_c, 2), duty=round(duty, 3))


def _single_zone_run(active_zone, ambient, active_settle_c, active_duty,
                      passive_rise_c=None, target_c=55.0, dwell_s=2100.0, dt=30.0,
                      final_state="done", segment_index=0, drift_active=False):
    """One synthetic single-zone-excitation run: a ramp-in row (dwelling
    False, seeds ambient), then a dwell window holding ``active_zone`` at
    ``active_settle_c``/``active_duty`` and the other two zones at
    ``ambient[z] + passive_rise_c[z]``/~0 duty, everything held flat (no
    slope) so both the active settle test (min 180s) and the passive settle
    test (min 1800s) fire comfortably inside a 2100s (35 min) dwell -- the
    real profile #4/#5/#6 dwell length this task specifies.

    ``drift_active=True`` keeps the active zone's actual_c climbing for the
    whole window instead of holding flat -- the "never settled" case, used
    by the refusal-gate negative test.
    """
    passive_rise_c = passive_rise_c if passive_rise_c is not None else {}
    zones0 = {z: la.ZoneSample(zone=z, actual_c=ambient[z], duty=0.0) for z in ci.ZONES}
    rows = [la.PollRow(wall_time="00:00:00", elapsed_s=0.0, segment_index=segment_index,
                        segment_count=1, dwelling=False, target_c=target_c, state="running",
                        zones=zones0)]
    n = int(dwell_s // dt)
    for i in range(1, n + 1):
        e = float(i * dt)
        zones = {}
        for z in ci.ZONES:
            if z == active_zone:
                c = active_settle_c + (0.02 * e if drift_active else 0.0)
                d = active_duty
            else:
                c = ambient[z] + passive_rise_c.get(z, 0.0)
                d = 0.0
            zones[z] = la.ZoneSample(zone=z, actual_c=round(c, 2), duty=round(d, 3))
        state = final_state if i == n else "running"
        rows.append(la.PollRow(
            wall_time=f"00:{int(e // 60):02d}:{int(e % 60):02d}", elapsed_s=e,
            segment_index=segment_index, segment_count=1, dwelling=True,
            target_c=target_c, state=state, zones=zones))
    return rows


def _poll_row_to_exec_body(row: la.PollRow) -> dict:
    return {
        "elapsed_s": row.elapsed_s, "segment_index": row.segment_index,
        "segment_count": row.segment_count, "dwelling": row.dwelling,
        "target_c": row.target_c, "state": row.state,
        "zones": [{"zone": z, "actual_c": s.actual_c, "duty": s.duty}
                  for z, s in row.zones.items()],
    }


def _write_capture(path, *row_groups, t0=1_700_000_000.0):
    """Write one or more ``rows`` sequences (each its own ``run_entry``-style
    run, elapsed_s restarting at 0) into a single ``http_capture_log`` file
    -- exactly what a poller left running across two firings would produce,
    and exactly the shape ``log_analysis.split_runs`` detects."""
    with open(path, "w", encoding="utf-8") as fh:
        t = t0
        for rows in row_groups:
            for row in rows:
                body = _poll_row_to_exec_body(row)
                fh.write(json.dumps({"t": t, "exec": body, "status": {}}) + "\n")
                t += 1.0


AMBIENT = {0: 25.0, 1: 25.2, 2: 24.8}


def _good_run(active_zone):
    """A well-settled, plausibly-coupled single-zone run: the driven zone
    rises 50C at duty 0.5 (k_diag=100), the other two rise 5C at that same
    duty (k_offdiag=10) -- diagonal-dominant, all non-negative."""
    passive = {z: 5.0 for z in ci.ZONES if z != active_zone}
    return _single_zone_run(active_zone, AMBIENT, AMBIENT[active_zone] + 50.0, 0.5,
                             passive_rise_c=passive)


# ---------------------------------------------------------------------------
# capture_is_complete / resumability
# ---------------------------------------------------------------------------

class TestCaptureIsComplete:
    def test_missing_file_is_not_complete(self, tmp_path):
        assert cw.capture_is_complete(str(tmp_path / "nope.jsonl")) is False

    def test_terminal_state_is_complete(self, tmp_path):
        path = tmp_path / "z0.jsonl"
        _write_capture(str(path), _good_run(0))
        assert cw.capture_is_complete(str(path)) is True

    def test_non_terminal_last_state_is_not_complete(self, tmp_path):
        """A capture cut off mid-run (last row still 'running') must be
        refired, not silently accepted -- prove this can go both ways: flip
        the last row's state to 'done' and it flips to complete."""
        rows = _good_run(0)
        rows[-1] = dataclasses.replace(rows[-1], state="running")
        path = tmp_path / "z0.jsonl"
        _write_capture(str(path), rows)
        assert cw.capture_is_complete(str(path)) is False  # RED: interrupted capture

        rows[-1] = dataclasses.replace(rows[-1], state="done")
        _write_capture(str(path), rows)
        assert cw.capture_is_complete(str(path)) is True  # GREEN: same data, terminal now

    def test_empty_file_is_not_complete(self, tmp_path):
        path = tmp_path / "z0.jsonl"
        path.write_text("")
        assert cw.capture_is_complete(str(path)) is False


class TestEnsureSingleZoneCaptures:
    def test_skips_already_complete_captures(self, tmp_path, monkeypatch):
        """zone0 already has a complete capture on disk; zone1/zone2 do not.
        run_entry must be called for zone1/zone2 only -- the resumability
        contract."""
        out_dir = str(tmp_path)
        _write_capture(cw.capture_log_path(out_dir, 0), _good_run(0))

        called = []

        def fake_run_entry(entry, cfg, control=None):
            called.append(entry.label)
            _write_capture(entry.log_path, _good_run(entry.profile_id - 4))

        monkeypatch.setattr(cw.rq, "run_entry", fake_run_entry)

        results = cw.ensure_single_zone_captures("192.0.2.1", "some_preset", out_dir)

        assert sorted(called) == ["zone1-excitation", "zone2-excitation"]
        by_zone = {r.zone: r for r in results}
        assert by_zone[0].skipped is True
        assert by_zone[1].skipped is False
        assert by_zone[2].skipped is False

    def test_interrupted_capture_is_refired_not_silently_skipped(self, tmp_path, monkeypatch):
        """A capture with no terminal row (queue killed mid-run) must NOT be
        treated as complete -- prove it triggers a refire."""
        out_dir = str(tmp_path)
        rows = _good_run(0)
        rows[-1] = dataclasses.replace(rows[-1], state="running")
        _write_capture(cw.capture_log_path(out_dir, 0), rows)

        called = []
        monkeypatch.setattr(cw.rq, "run_entry", lambda entry, cfg, control=None: called.append(entry.label))

        cw.ensure_single_zone_captures("192.0.2.1", "some_preset", out_dir)
        assert "zone0-excitation" in called


# ---------------------------------------------------------------------------
# Settle gate -- the refuse-on-unsettled proof.
# ---------------------------------------------------------------------------

class TestSettleGate:
    def test_passes_when_every_active_zone_settled(self, tmp_path):
        captures = []
        for z in ci.ZONES:
            path = tmp_path / f"z{z}.jsonl"
            _write_capture(str(path), _good_run(z))
            captures.append(cw.CaptureResult(zone=z, log_path=str(path), skipped=False))
        cw.settle_gate(captures)  # must not raise

    def test_settle_gate_refuses_on_unsettled_active_zone(self, tmp_path):
        """RED: zone0's own dwell never settles (kept drifting the whole
        window) -- settle_gate must refuse and name zone0."""
        path = tmp_path / "z0.jsonl"
        _write_capture(str(path), _single_zone_run(0, AMBIENT, AMBIENT[0] + 50.0, 0.5,
                                                     drift_active=True))
        captures = [cw.CaptureResult(zone=0, log_path=str(path), skipped=False)]
        with pytest.raises(cw.CouplingWorkflowError, match="zone0"):
            cw.settle_gate(captures)

    def test_settle_gate_passes_once_settled(self, tmp_path):
        """GREEN: the same capture, with the drift fixed (held flat) --
        passes. Proves the refusal above is about the data, not a bug that
        always fires."""
        path = tmp_path / "z0.jsonl"
        _write_capture(str(path), _single_zone_run(0, AMBIENT, AMBIENT[0] + 50.0, 0.5,
                                                     drift_active=False))
        captures = [cw.CaptureResult(zone=0, log_path=str(path), skipped=False)]
        cw.settle_gate(captures)  # must not raise

    def test_settle_gate_refuses_on_flagged_unstable(self, tmp_path):
        """RED: the active zone settles (slope test fires) but duty keeps
        swinging afterward -- the tightened sec 3.2 check must still
        refuse, even though the plain firmware settle test would have
        accepted this reading."""
        rows = _single_zone_run(0, AMBIENT, AMBIENT[0] + 50.0, 0.5)
        # Oscillate duty on the second half of the dwell, well past the
        # settle instant (which fires at ~180s into the dwell window).
        fixed = []
        for r in rows:
            if r.dwelling and r.elapsed_s > 300.0:
                z0 = r.zones[0]
                swung_duty = 0.5 + (0.3 if int(r.elapsed_s // 30) % 2 == 0 else -0.3)
                r = dataclasses.replace(r, zones={
                    **r.zones, 0: dataclasses.replace(z0, duty=round(swung_duty, 3))})
            fixed.append(r)
        path = tmp_path / "z0.jsonl"
        _write_capture(str(path), fixed)
        captures = [cw.CaptureResult(zone=0, log_path=str(path), skipped=False)]
        with pytest.raises(cw.CouplingWorkflowError, match="FLAGGED unstable"):
            cw.settle_gate(captures)

    def test_passive_zone_never_settling_does_not_gate(self, tmp_path):
        """A passive zone's own 'settled=False' entry under the plain audit
        (its duty never crosses MIN_DUTY_FOR_OBSERVATION, by design) must
        NEVER trigger the refusal -- only the capture's OWN driven zone
        does. This is the negative-test companion proving the zone==
        active_zone filter in active_zone_settle_failures actually matters:
        without it, every single-zone capture would always refuse."""
        path = tmp_path / "z0.jsonl"
        _write_capture(str(path), _good_run(0))
        captures = [cw.CaptureResult(zone=0, log_path=str(path), skipped=False)]
        # Sanity: the plain audit DOES report passive zones as unsettled --
        # if this assertion ever failed, the "does not gate" assertion below
        # would be vacuous (nothing to filter out in the first place).
        entries = ci.settle_criterion_audit(la.split_runs(hc.poll_rows(str(path)))[0])
        passive_unsettled = [e for e in entries if e.zone != 0 and not e.settled]
        assert passive_unsettled, "fixture no longer exercises the passive-zone unsettled case"

        cw.settle_gate(captures)  # must not raise despite passive zones


# ---------------------------------------------------------------------------
# Multi-run files -- must not silently take the last run.
# ---------------------------------------------------------------------------

class TestMultiRunFiles:
    def test_load_capture_runs_returns_every_run(self, tmp_path):
        run1 = _good_run(0)
        run2 = _good_run(0)
        path = tmp_path / "z0_two_runs.jsonl"
        _write_capture(str(path), run1, run2)
        runs = cw._load_capture_runs(str(path))
        assert len(runs) == 2

    def test_settle_gate_checks_every_run_not_just_the_last(self, tmp_path):
        """A capture holding an unsettled FIRST run followed by a settled
        SECOND run must still refuse -- picking only the last run would
        silently hide the first run's bad dwell."""
        bad_run = _single_zone_run(0, AMBIENT, AMBIENT[0] + 50.0, 0.5, drift_active=True)
        good_run = _good_run(0)
        path = tmp_path / "z0_two_runs.jsonl"
        _write_capture(str(path), bad_run, good_run)
        captures = [cw.CaptureResult(zone=0, log_path=str(path), skipped=False)]
        with pytest.raises(cw.CouplingWorkflowError, match="zone0"):
            cw.settle_gate(captures)

    def test_column_observations_pool_across_both_runs(self, tmp_path):
        """Both runs in a two-run file must contribute observations -- not
        just the second/last one."""
        run1 = _good_run(0)
        run2 = _good_run(0)
        path = tmp_path / "z0_two_runs.jsonl"
        _write_capture(str(path), run1, run2)
        obs = []
        for run_idx, rows in enumerate(cw._load_capture_runs(str(path))):
            obs.extend(ci.single_zone_column_observations(rows, 0, source=f"run{run_idx}"))
        sources = {o.source for o in obs}
        assert sources == {"run0", "run1"}


# ---------------------------------------------------------------------------
# Assembly + scoring
# ---------------------------------------------------------------------------

class TestAssembleAndScore:
    def _captures(self, tmp_path):
        captures = []
        for z in ci.ZONES:
            path = tmp_path / f"z{z}.jsonl"
            _write_capture(str(path), _good_run(z))
            captures.append(cw.CaptureResult(zone=z, log_path=str(path), skipped=False))
        return captures

    def test_full_matrix_assembled_and_plausible(self, tmp_path):
        result = cw.assemble_and_score(self._captures(tmp_path), sec32_paths=[])
        matrix = np.array(result["matrix"])
        assert matrix.shape == (3, 3)
        for z in ci.ZONES:
            assert matrix[z, z] == pytest.approx(100.0, abs=1.0)  # 50C / 0.5 duty
            for j in ci.ZONES:
                if j != z:
                    assert matrix[z, j] == pytest.approx(10.0, abs=1.0)  # 5C / 0.5 duty
        assert result["plausible"] is True
        assert np.isfinite(result["condition_number"])
        assert result["sec32_scores"] is None  # sec32_paths=[] -> not scored
        assert set(result["transient_scores"]) == {"zone0", "zone1", "zone2"}

    def test_incomplete_matrix_refuses(self, tmp_path):
        """Only two of three zones captured -- assembling anyway (silently
        leaving a cell at some fallback) is exactly the failure mode this
        must refuse against."""
        captures = self._captures(tmp_path)[:2]
        with pytest.raises(cw.CouplingWorkflowError, match="INCOMPLETE"):
            cw.assemble_and_score(captures, sec32_paths=[])

    def test_unsettled_capture_refuses_before_assembly(self, tmp_path):
        captures = self._captures(tmp_path)
        bad_path = tmp_path / "z0_bad.jsonl"
        _write_capture(str(bad_path), _single_zone_run(0, AMBIENT, AMBIENT[0] + 50.0, 0.5,
                                                          drift_active=True))
        captures[0] = cw.CaptureResult(zone=0, log_path=str(bad_path), skipped=False)
        with pytest.raises(cw.CouplingWorkflowError, match="settle criterion"):
            cw.assemble_and_score(captures, sec32_paths=[])


# ---------------------------------------------------------------------------
# Preset emission
# ---------------------------------------------------------------------------

class TestBuildPreset:
    def test_overlays_matrix_onto_base_preset_verbatim(self):
        base = config_presets.load_preset_data("coupling_matrix_20260831")
        matrix = np.array([[0.0, 11.0, 22.0], [33.0, 0.0, 44.0], [55.0, 66.0, 0.0]])
        preset = cw.build_preset(matrix, "coupling_matrix_20260831", "test_reidentified")

        assert preset["name"] == "test_reidentified"
        assert len(preset["zones"]) == len(base["zones"])
        for base_zone, new_zone in zip(base["zones"], preset["zones"]):
            idx = base_zone["index"]
            assert new_zone["coupling_coeff"] == list(matrix[idx])
            assert new_zone["coupling_coeff"][idx] == 0.0
            # every other field copied verbatim
            for k in ("pid_kp", "pid_ki", "pid_kd", "relay_mask", "control_mode",
                      "max_temp_c", "min_temp_c", "max_ramp_c_per_hr", "cal_offset_c"):
                assert new_zone[k] == base_zone[k]

    def test_diagonal_forced_zero_even_if_fit_disagrees(self):
        """A fit's own diagonal entry (which single_zone excitation never
        actually measures as anything but the k_dc-like DC gain) must never
        leak into coupling_coeff's diagonal -- the firmware ignores it
        anyway (force-ranged to [0,0] on POST), so writing anything else is
        a number that would silently be discarded, or worse, misread by a
        human inspecting the preset file."""
        matrix = np.array([[999.0, 1.0, 2.0], [3.0, 999.0, 4.0], [5.0, 6.0, 999.0]])
        preset = cw.build_preset(matrix, "coupling_matrix_20260831", "test_reidentified")
        for z in preset["zones"]:
            idx = z["index"]
            assert z["coupling_coeff"][idx] == 0.0

    def test_validates_against_config_presets_schema(self):
        matrix = np.zeros((3, 3))
        preset = cw.build_preset(matrix, "coupling_matrix_20260831", "test_reidentified")
        config_presets._validate("test_reidentified", preset)  # must not raise

    def test_write_preset_round_trips(self, tmp_path):
        matrix = np.array([[0.0, 1.0, 2.0], [3.0, 0.0, 4.0], [5.0, 6.0, 0.0]])
        preset = cw.build_preset(matrix, "coupling_matrix_20260831", "test_reidentified")
        path = cw.write_preset(preset, str(tmp_path))
        assert os.path.isfile(path)
        with open(path, "r", encoding="utf-8") as fh:
            loaded = json.load(fh)
        assert loaded["zones"][1]["coupling_coeff"] == [3.0, 0.0, 4.0]


# ---------------------------------------------------------------------------
# End-to-end orchestration, dry-run only (this suite never touches a board).
# ---------------------------------------------------------------------------

class TestRunIdentificationDryRun:
    def _write_zone_captures(self, tmp_path):
        paths = {}
        for z in ci.ZONES:
            path = tmp_path / f"cap_z{z}.jsonl"
            _write_capture(str(path), _good_run(z))
            paths[z] = str(path)
        return paths

    def test_dry_run_end_to_end(self, tmp_path):
        zone_captures = self._write_zone_captures(tmp_path)
        out_dir = str(tmp_path / "out")
        result = cw.run_identification(
            preset_name="coupling_matrix_20260831", out_dir=out_dir, dry_run=True,
            zone_captures=zone_captures, sec32_paths=[])
        assert os.path.isfile(result["preset_path"])
        assert result["preset_name"] == "coupling_matrix_20260831_reidentified"
        assert all(c["skipped"] for c in result["captures"])
        # format_result_text must not raise on a real result
        text = cw.format_result_text(result)
        assert "assembled matrix" in text

    def test_dry_run_missing_zone_refuses(self, tmp_path):
        zone_captures = self._write_zone_captures(tmp_path)
        del zone_captures[2]
        with pytest.raises(cw.CouplingWorkflowError, match=r"missing"):
            cw.run_identification(preset_name="coupling_matrix_20260831", out_dir=str(tmp_path),
                                   dry_run=True, zone_captures=zone_captures, sec32_paths=[])

    def test_dry_run_nonexistent_capture_path_refuses(self, tmp_path):
        zone_captures = self._write_zone_captures(tmp_path)
        zone_captures[1] = str(tmp_path / "does_not_exist.jsonl")
        with pytest.raises(cw.CouplingWorkflowError, match="not found"):
            cw.run_identification(preset_name="coupling_matrix_20260831", out_dir=str(tmp_path),
                                   dry_run=True, zone_captures=zone_captures, sec32_paths=[])

    def test_dry_run_never_calls_run_entry(self, tmp_path, monkeypatch):
        called = []
        monkeypatch.setattr(cw.rq, "run_entry", lambda *a, **k: called.append(1))
        zone_captures = self._write_zone_captures(tmp_path)
        cw.run_identification(preset_name="coupling_matrix_20260831", out_dir=str(tmp_path / "out"),
                               dry_run=True, zone_captures=zone_captures, sec32_paths=[])
        assert called == []

    def test_cli_dry_run(self, tmp_path, capsys):
        zone_captures = self._write_zone_captures(tmp_path)
        out_dir = str(tmp_path / "out")
        rc = cw.main([
            "--preset", "coupling_matrix_20260831", "--out-dir", out_dir, "--dry-run",
            "--zone0-capture", zone_captures[0], "--zone1-capture", zone_captures[1],
            "--zone2-capture", zone_captures[2],
        ])
        assert rc == 0
        out = capsys.readouterr().out
        assert "assembled matrix" in out
        assert os.path.isfile(os.path.join(out_dir, "coupling_matrix_20260831_reidentified.json"))

    def test_cli_dry_run_missing_capture_returns_1(self, tmp_path, capsys):
        zone_captures = self._write_zone_captures(tmp_path)
        rc = cw.main([
            "--preset", "coupling_matrix_20260831", "--out-dir", str(tmp_path / "out"), "--dry-run",
            "--zone0-capture", zone_captures[0], "--zone1-capture", zone_captures[1],
            # zone2 omitted
        ])
        assert rc == 1
        assert "REFUSED" in capsys.readouterr().out
