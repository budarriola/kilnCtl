#!/usr/bin/env python3
"""Host tests for the PID-validation harness's DECISION LOGIC and dry run.

No board, no HTTP, no sleeping -- everything gated in ``pid_validation.py``
is pure, and the whole point of that split (see its module docstring) is
that it can be exercised exhaustively here. Every threshold-shaped assertion
below is proven able to FAIL before it is trusted to pass, per this repo's
"negative test every check" convention (see test_cooldown_policy.py for the
established pattern this file follows).

The dry-run test at the bottom drives ``scripts/pid_validation_harness.py``'s
actual ``main()`` end to end against synthetic data -- the same code path a
real run takes, minus the network calls -- and checks it produces a report,
a nonzero exit code when a threshold is (deliberately) violated, and a zero
exit code once it is not.
"""
from __future__ import annotations

import importlib.util
import json
import math
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import pid_validation as pv  # noqa: E402

_SCRIPTS_DIR = os.path.join(os.path.dirname(__file__), "..", "scripts")


def _load_harness_module():
    """Import scripts/pid_validation_harness.py by path -- ``scripts/`` is
    not a package, so this mirrors how ``coordinated_gpio_test.py`` and its
    siblings are exercised: import by file location, not by package name."""
    path = os.path.join(_SCRIPTS_DIR, "pid_validation_harness.py")
    spec = importlib.util.spec_from_file_location("pid_validation_harness", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------
# check_hard_max / HardMaxExceeded
# ---------------------------------------------------------------------------

def test_hard_max_trips_above_limit():
    """NEGATIVE TEST. The whole point of the harness's own ceiling is that it
    fires independently of the firmware's -- prove it actually raises."""
    with pytest.raises(pv.HardMaxExceeded) as exc:
        pv.check_hard_max(91.0, 90.0, "unit test")
    assert exc.value.hottest_c == 91.0
    assert exc.value.limit_c == 90.0


def test_hard_max_passes_at_and_below_limit():
    pv.check_hard_max(90.0, 90.0, "unit test")  # exactly at limit: not a breach
    pv.check_hard_max(10.0, 90.0, "unit test")


def test_hard_max_ignores_nan_reading():
    """A channel that stopped reporting must not be treated as a temperature
    breach -- it is a DIFFERENT failure (an unreadable channel), asserted on
    elsewhere, not silently promoted into a false hard-max trip here."""
    pv.check_hard_max(float("nan"), 90.0, "unit test")  # must not raise


# ---------------------------------------------------------------------------
# compute_spread_c
# ---------------------------------------------------------------------------

def test_spread_is_max_minus_min():
    assert pv.compute_spread_c({0: 50.0, 1: 51.5, 2: 48.0}) == pytest.approx(3.5)


def test_spread_flags_a_wide_gap():
    """NEGATIVE TEST for the threshold this feeds: a genuinely diverged zone
    must produce a spread the caller can fail on, not something rounded away."""
    wide = pv.compute_spread_c({0: 50.0, 1: 62.0, 2: 49.0})
    assert wide > 5.0


def test_spread_single_zone_is_zero_not_error():
    assert pv.compute_spread_c({0: 42.0}) == 0.0


def test_spread_no_readings_is_nan():
    assert math.isnan(pv.compute_spread_c({}))


def test_spread_ignores_nan_channels():
    assert pv.compute_spread_c({0: 50.0, 1: float("nan"), 2: 51.0}) == pytest.approx(1.0)


# ---------------------------------------------------------------------------
# tracking_stats
# ---------------------------------------------------------------------------

def _samples(zone, pairs, hold_flags=None):
    hold_flags = hold_flags or [False] * len(pairs)
    return [pv.TrackingSample(t_s=i, zone=zone, measured_c=m, setpoint_c=sp, in_hold=h)
            for i, ((m, sp), h) in enumerate(zip(pairs, hold_flags))]


def test_tracking_stats_max_and_rms():
    samples = _samples(0, [(50.0, 50.0), (51.0, 50.0), (49.0, 50.0), (54.0, 50.0)])
    stats = pv.tracking_stats(samples, 0)
    assert stats.max_abs_err_c == pytest.approx(4.0)
    expected_rms = math.sqrt((0**2 + 1**2 + 1**2 + 4**2) / 4)
    assert stats.rms_err_c == pytest.approx(expected_rms)
    assert stats.n_samples == 4


def test_tracking_stats_steady_state_is_signed_mean_of_hold_only():
    samples = _samples(
        0,
        [(60.0, 50.0), (48.5, 50.0), (48.5, 50.0)],  # first sample: ramp, not hold
        hold_flags=[False, True, True])
    stats = pv.tracking_stats(samples, 0)
    assert stats.steady_state_err_c == pytest.approx(-1.5)  # consistently 1.5 C low
    assert stats.n_hold_samples == 2


def test_tracking_stats_no_hold_samples_is_nan_not_zero():
    """NEGATIVE TEST: a run that never sampled during a hold must not report
    0.0 steady-state error -- that would read as verified-accurate when
    nothing was actually verified."""
    samples = _samples(0, [(51.0, 50.0), (49.0, 50.0)], hold_flags=[False, False])
    stats = pv.tracking_stats(samples, 0)
    assert math.isnan(stats.steady_state_err_c)


def test_tracking_stats_missing_zone_raises():
    samples = _samples(0, [(50.0, 50.0)])
    with pytest.raises(ValueError):
        pv.tracking_stats(samples, 7)


# ---------------------------------------------------------------------------
# evaluate_zone_tracking / evaluate_spread -- prove FAIL before PASS
# ---------------------------------------------------------------------------

def test_evaluate_zone_tracking_fails_on_bad_data_then_passes_on_good():
    thresholds = pv.TrackingThresholds(max_abs_err_c=2.0, rms_err_c=1.0,
                                       steady_state_err_c=0.5, spread_c=2.0)
    bad = pv.ZoneTrackingStats(zone=1, max_abs_err_c=3.0, rms_err_c=2.0,
                               steady_state_err_c=1.2, n_samples=10, n_hold_samples=4)
    bad_checks = pv.evaluate_zone_tracking(bad, thresholds)
    assert not all(c.passed for c in bad_checks), "bad data must not report all-pass"
    assert any(not c.passed and c.name.endswith("max_abs_err_c") for c in bad_checks)
    assert any(not c.passed and c.name.endswith("rms_err_c") for c in bad_checks)
    assert any(not c.passed and c.name.endswith("steady_state_err_c") for c in bad_checks)

    good = pv.ZoneTrackingStats(zone=1, max_abs_err_c=1.0, rms_err_c=0.5,
                                steady_state_err_c=0.2, n_samples=10, n_hold_samples=4)
    good_checks = pv.evaluate_zone_tracking(good, thresholds)
    assert all(c.passed for c in good_checks)


def test_evaluate_zone_tracking_zero_hold_samples_fails_the_ss_check():
    """NEGATIVE TEST: this is a harness-wiring bug (bad sample period vs.
    hold duration), and it must fail loudly rather than being omitted."""
    thresholds = pv.TrackingThresholds()
    stats = pv.ZoneTrackingStats(zone=0, max_abs_err_c=0.1, rms_err_c=0.1,
                                 steady_state_err_c=float("nan"), n_samples=5, n_hold_samples=0)
    checks = pv.evaluate_zone_tracking(stats, thresholds)
    ss = [c for c in checks if c.name.endswith("steady_state_err_c")][0]
    assert not ss.passed


def test_evaluate_spread_fails_wide_then_passes_tight():
    thresholds = pv.TrackingThresholds(spread_c=2.0)
    wide = pv.evaluate_spread([1.0, 5.5, 2.0], thresholds)
    assert not wide.passed
    assert wide.actual == pytest.approx(5.5)  # worst, not average
    tight = pv.evaluate_spread([1.0, 1.5, 1.8], thresholds)
    assert tight.passed


def test_evaluate_spread_no_valid_samples_fails():
    thresholds = pv.TrackingThresholds(spread_c=2.0)
    result = pv.evaluate_spread([float("nan"), float("nan")], thresholds)
    assert not result.passed


# ---------------------------------------------------------------------------
# autotune_refusal_reason
# ---------------------------------------------------------------------------

def test_autotune_refusal_reason_none_when_clean():
    status = {"state": "done", "model_valid": True, "refusal": "ok", "refusal_reason": ""}
    assert pv.autotune_refusal_reason(status) is None


def test_autotune_refusal_reason_flags_aborted():
    status = {"state": "aborted", "abort_reason": "operator abort"}
    reason = pv.autotune_refusal_reason(status)
    assert reason is not None and "aborted" in reason


def test_autotune_refusal_reason_flags_non_ok_refusal():
    """NEGATIVE TEST directly targeting the "refused tune looked like success"
    bug class (commit 813ad90): a refusal code other than 'ok' must produce
    a non-None reason even when the state made it to 'done'."""
    status = {"state": "done", "model_valid": True,
              "refusal": "nonpositive_gain", "refusal_reason": "k_dc <= 0"}
    reason = pv.autotune_refusal_reason(status)
    assert reason is not None and "nonpositive_gain" in reason


def test_autotune_refusal_reason_flags_invalid_model():
    status = {"state": "done", "model_valid": False, "refusal": "ok"}
    reason = pv.autotune_refusal_reason(status)
    assert reason is not None and "model" in reason


def test_autotune_refusal_reason_flags_wrong_state():
    status = {"state": "stepping"}
    reason = pv.autotune_refusal_reason(status)
    assert reason is not None and "stepping" in reason


# ---------------------------------------------------------------------------
# diff_backup_zones
# ---------------------------------------------------------------------------

def _zone(index, kp=1.0, ki=0.02, kd=3.0, k=40.0, tau=150.0, dt=12.0):
    return {"index": index, "pid_kp": kp, "pid_ki": ki, "pid_kd": kd,
            "model_k_dc": k, "model_tau_s": tau, "model_dead_time_s": dt}


def test_diff_backup_zones_identical_is_no_mismatch():
    doc = {"zones": [_zone(0), _zone(1)]}
    assert pv.diff_backup_zones(doc, doc) == []


def test_diff_backup_zones_detects_changed_field():
    """NEGATIVE TEST. This is stage 4's actual proof-of-life -- an import
    that silently no-ops must be caught here."""
    a = {"zones": [_zone(0, kp=1.0)]}
    b = {"zones": [_zone(0, kp=1.5)]}
    mismatches = pv.diff_backup_zones(a, b)
    assert len(mismatches) == 1
    assert "pid_kp" in mismatches[0]


def test_diff_backup_zones_detects_dropped_zone():
    a = {"zones": [_zone(0), _zone(1)]}
    b = {"zones": [_zone(0)]}
    mismatches = pv.diff_backup_zones(a, b)
    assert any("zone 1" in m for m in mismatches)


def test_diff_backup_zones_tolerates_float_noise_within_tolerance():
    a = {"zones": [_zone(0, kp=1.0)]}
    b = {"zones": [_zone(0, kp=1.0 + 5e-4)]}
    assert pv.diff_backup_zones(a, b) == []


# ---------------------------------------------------------------------------
# RunReport.passed
# ---------------------------------------------------------------------------

def test_run_report_fails_if_any_stage_fails():
    report = pv.RunReport(host="x", started_at="t0", dry_run=True, hard_max_temp_c=90.0)
    report.stages = [pv.StageReport(name="a", passed=True), pv.StageReport(name="b", passed=False)]
    assert report.passed is False


def test_run_report_passes_only_when_every_ran_stage_passes():
    report = pv.RunReport(host="x", started_at="t0", dry_run=True, hard_max_temp_c=90.0)
    report.stages = [pv.StageReport(name="a", passed=True), pv.StageReport(name="b", passed=True)]
    assert report.passed is True


def test_run_report_all_skipped_is_not_a_pass():
    """NEGATIVE TEST: a run where every stage was skipped proved nothing, and
    must not report the same 'passed' as a run that actually exercised the
    board."""
    report = pv.RunReport(host="x", started_at="t0", dry_run=True, hard_max_temp_c=90.0)
    report.stages = [pv.StageReport(name="a", passed=True, skipped=True)]
    assert report.passed is False


def test_run_report_aborted_is_never_a_pass_even_if_stages_all_passed():
    report = pv.RunReport(host="x", started_at="t0", dry_run=True, hard_max_temp_c=90.0)
    report.stages = [pv.StageReport(name="a", passed=True)]
    report.aborted = True
    report.abort_reason = "hard max exceeded"
    assert report.passed is False


def test_run_report_to_json_round_trips():
    report = pv.RunReport(host="x", started_at="t0", dry_run=True, hard_max_temp_c=90.0)
    report.stages = [pv.StageReport(name="a", passed=True,
                                    checks=[pv.CheckResult(name="c", passed=True, actual=1, threshold=2)])]
    doc = json.loads(report.to_json())
    assert doc["host"] == "x"
    assert doc["stages"][0]["checks"][0]["name"] == "c"


# ---------------------------------------------------------------------------
# Dry run: the full flow, no hardware
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def harness():
    return _load_harness_module()


def test_dry_run_exercises_every_stage_and_writes_a_report(harness, tmp_path):
    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path)])
    rc = harness.main(["--dry-run", "--report-dir", str(tmp_path)])
    # The synthetic FakeSession bakes in a zone-2 steady-state miss on
    # purpose (see FakeSession.sample_response's docstring) so the dry run
    # PROVES the tracking stage can fail before anyone trusts it to pass.
    assert rc != 0, "dry run's synthetic data is deliberately bad for zone 2 -- expected a FAIL"

    jsons = list(tmp_path.glob("pid_validation_*.json"))
    txts = list(tmp_path.glob("pid_validation_*.txt"))
    assert len(jsons) == 1 and len(txts) == 1
    doc = json.loads(jsons[0].read_text(encoding="utf-8"))
    assert doc["dry_run"] is True
    assert doc["overall_passed"] is False
    stage_names = {s["name"] for s in doc["stages"]}
    assert {"cooldown", "tune_all_zones", "coupling_matrix", "backup_roundtrip",
            "profile_tracking", "teardown_force_all_stop"} <= stage_names
    tracking = [s for s in doc["stages"] if s["name"] == "profile_tracking"][0]
    assert not tracking["passed"]
    zone2_checks = [c for c in tracking["checks"] if c["name"].startswith("zone2.")]
    assert any(not c["passed"] for c in zone2_checks)


def test_dry_run_passes_when_thresholds_are_loosened_to_cover_the_synthetic_miss(harness, tmp_path):
    """The counterpart to the FAIL above: the same synthetic data, with the
    steady-state threshold loosened past the deliberate 2.0 C zone-2 miss,
    reports PASS -- proving the gate is a real comparison, not a stub that
    always fails or always passes regardless of the numbers."""
    rc = harness.main([
        "--dry-run", "--report-dir", str(tmp_path), "--steady-state-err-c", "3.0"])
    assert rc == 0


def test_dry_run_skip_flags_are_recorded_as_skipped(harness, tmp_path):
    rc = harness.main([
        "--dry-run", "--report-dir", str(tmp_path),
        "--skip-cooldown", "--skip-matrix", "--skip-backup", "--skip-tune",
        "--steady-state-err-c", "3.0"])
    assert rc == 0
    jsons = sorted(tmp_path.glob("pid_validation_*.json"), key=lambda p: p.stat().st_mtime)
    doc = json.loads(jsons[-1].read_text(encoding="utf-8"))
    skipped = {s["name"] for s in doc["stages"] if s["skipped"]}
    assert skipped == {"cooldown", "tune", "matrix", "backup"}


def test_hard_max_abort_stops_the_dry_run_and_still_tears_down(harness, tmp_path):
    """NEGATIVE TEST for the safety requirement itself: force the harness's
    own ceiling below what the synthetic run reaches and confirm it aborts
    (rather than completing) and still runs teardown."""
    rc = harness.main([
        "--dry-run", "--report-dir", str(tmp_path), "--max-temp-c", "10.0"])
    assert rc != 0
    jsons = sorted(tmp_path.glob("pid_validation_*.json"), key=lambda p: p.stat().st_mtime)
    doc = json.loads(jsons[-1].read_text(encoding="utf-8"))
    assert doc["aborted"] is True
    stage_names = [s["name"] for s in doc["stages"]]
    assert "teardown_force_all_stop" in stage_names, "teardown must still run after an abort"
