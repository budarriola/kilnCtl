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
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.dirname(__file__))

from kilnctrl import pid_validation as pv  # noqa: E402
import bench_fixture_session as bfs  # noqa: E402

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
# retry_backoff_s
# ---------------------------------------------------------------------------

def test_retry_backoff_s_grows_exponentially():
    policy = pv.RetryPolicy(backoff_s=2.0, backoff_multiplier=3.0, backoff_max_s=1000.0)
    assert pv.retry_backoff_s(1, policy) == pytest.approx(2.0)
    assert pv.retry_backoff_s(2, policy) == pytest.approx(6.0)
    assert pv.retry_backoff_s(3, policy) == pytest.approx(18.0)


def test_retry_backoff_s_is_capped():
    """NEGATIVE TEST: without a cap this would keep climbing (2 * 2**9 = 1024s
    by attempt 10) -- prove it actually tops out instead."""
    policy = pv.RetryPolicy(backoff_s=2.0, backoff_multiplier=2.0, backoff_max_s=30.0)
    uncapped_attempt10 = 2.0 * (2.0 ** 9)
    assert uncapped_attempt10 > 30.0  # sanity: the cap is actually being exercised
    assert pv.retry_backoff_s(10, policy) == pytest.approx(30.0)


def test_retry_backoff_s_rejects_attempt_below_one():
    with pytest.raises(ValueError):
        pv.retry_backoff_s(0, pv.RetryPolicy())


# ---------------------------------------------------------------------------
# is_transient_http_error
# ---------------------------------------------------------------------------

def test_is_transient_http_error_flags_timeout_and_connection_errors():
    """Linux/glibc-flavoured urllib text -- keep these working too."""
    assert pv.is_transient_http_error(RuntimeError("http://x/api/autotune: <urlopen error timed out>"))
    assert pv.is_transient_http_error(RuntimeError("[Errno 111] Connection refused"))
    assert pv.is_transient_http_error(RuntimeError("Connection reset by peer"))


def test_is_transient_http_error_flags_real_windows_urllib_strings():
    """B2 regression test. These three are VERIFIED EMPIRICALLY as what
    urllib's OSError text actually looks like on this platform (Windows) --
    what a rebooting/refusing ESP32 produces -- not hand-picked to match
    whatever the marker list happens to contain. Before the B2 fix, none of
    TRANSIENT_HTTP_MARKERS matched any of these, so retry was inert against
    the two most likely blips on this platform."""
    assert pv.is_transient_http_error(RuntimeError(
        "http://kilnctl.local/api/status: <urlopen error [WinError 10061] "
        "No connection could be made because the target machine actively "
        "refused it>"))
    assert pv.is_transient_http_error(RuntimeError(
        "http://kilnctl.local/api/status: <urlopen error [WinError 10054] "
        "An existing connection was forcibly closed by the remote host>"))
    assert pv.is_transient_http_error(RuntimeError(
        "http://kilnctl.local/api/status: <urlopen error [WinError 10060] "
        "A connection attempt failed because the connected party did not "
        "properly respond after a period of time, or established connection "
        "failed because connected host has failed to respond>"))


def test_is_transient_http_error_rejects_unrelated_errors():
    """NEGATIVE TEST: an error this module does not recognize must NOT be
    treated as transient -- that is exactly the "silently retry past a dead
    board" failure mode the whole mechanism exists to avoid."""
    assert not pv.is_transient_http_error(ValueError("zone index 9 out of range"))
    assert not pv.is_transient_http_error(RuntimeError("GET /api/status -> 500: internal error"))


def test_is_transient_http_error_rejects_bare_timeout_word_in_body_text():
    """NEGATIVE TEST for the over-broad marker this module used to carry: a
    bare "timeout" substring must NOT be a marker on its own, because
    BenchSessionError embeds up to 200 chars of a non-200 response BODY
    verbatim, and a body that happens to mention "timeout" in prose (e.g. a
    firmware log message) is not a network-transport problem at all -- only
    the specific phrase "timed out" (urllib's own wording) should match."""
    assert not pv.is_transient_http_error(RuntimeError(
        "GET /api/autotune -> 500: profile timeout warning logged, gains unchanged"))


# ---------------------------------------------------------------------------
# call_with_retry
# ---------------------------------------------------------------------------

def test_call_with_retry_succeeds_after_transient_failures():
    calls = {"n": 0}

    def flaky():
        calls["n"] += 1
        if calls["n"] < 3:
            raise RuntimeError("timed out")
        return "ok"

    sleeps = []
    result = pv.call_with_retry(
        flaky, policy=pv.RetryPolicy(max_retries=5, backoff_s=1.0),
        is_transient=pv.is_transient_http_error, sleep_fn=sleeps.append)
    assert result == "ok"
    assert calls["n"] == 3
    assert len(sleeps) == 2  # two retries before the third (successful) attempt


def test_call_with_retry_raises_retry_exhausted_after_max_attempts():
    """NEGATIVE TEST: prove retries actually run out -- an always-failing
    transient error must not retry forever, and must be reported as
    exhausted rather than as the original bare error."""
    def always_fails():
        raise RuntimeError("connection reset")

    with pytest.raises(pv.RetryExhausted) as exc:
        pv.call_with_retry(
            always_fails, policy=pv.RetryPolicy(max_retries=3, backoff_s=0.01),
            is_transient=pv.is_transient_http_error, sleep_fn=lambda s: None)
    assert exc.value.attempts == 3


def test_call_with_retry_does_not_retry_a_non_transient_error():
    """NEGATIVE TEST for the OTHER half: a non-transient error (e.g. a real
    board panic surfacing as a 500, or a programming error) must propagate
    on the FIRST attempt, unretried."""
    calls = {"n": 0}

    def fails_hard():
        calls["n"] += 1
        raise ValueError("board reported a fatal fault")

    with pytest.raises(ValueError):
        pv.call_with_retry(
            fails_hard, policy=pv.RetryPolicy(max_retries=5, backoff_s=0.01),
            is_transient=pv.is_transient_http_error, sleep_fn=lambda s: None)
    assert calls["n"] == 1, "a non-transient error must not be retried at all"


def test_call_with_retry_invokes_on_retry_for_every_attempt():
    calls = {"n": 0}

    def flaky():
        calls["n"] += 1
        if calls["n"] < 2:
            raise RuntimeError("timed out")
        return "ok"

    events = []
    pv.call_with_retry(
        flaky, policy=pv.RetryPolicy(max_retries=3, backoff_s=1.0, backoff_multiplier=2.0),
        is_transient=pv.is_transient_http_error,
        on_retry=lambda attempt, wait_s, exc: events.append((attempt, wait_s)),
        sleep_fn=lambda s: None)
    assert events == [(1, 1.0)]


# ---------------------------------------------------------------------------
# detect_board_restart / BoardHealthTracker
# ---------------------------------------------------------------------------

def test_detect_board_restart_flags_uptime_going_backwards():
    reason = pv.detect_board_restart(500.0, "power-on", {"uptime_s": 12.0, "reset_reason": "power-on"})
    assert reason is not None and "backwards" in reason


def test_detect_board_restart_flags_reset_reason_changing_to_panic():
    reason = pv.detect_board_restart(
        100.0, "power-on", {"uptime_s": 105.0, "reset_reason": "panic/exception"})
    assert reason is not None and "panic/exception" in reason


def test_detect_board_restart_is_none_for_normal_progress():
    """NEGATIVE TEST counterpart: healthy, forward-moving uptime with an
    unchanged benign reset_reason must NOT be flagged."""
    reason = pv.detect_board_restart(
        100.0, "power-on", {"uptime_s": 105.0, "reset_reason": "power-on"})
    assert reason is None


def test_detect_board_restart_does_not_flag_preexisting_panic_history():
    """NEGATIVE TEST: a board whose LAST boot was a panic, but which has not
    rebooted again since the baseline was captured, must not be flagged --
    only a CHANGE mid-run is evidence of a fresh crash."""
    reason = pv.detect_board_restart(
        100.0, "panic/exception", {"uptime_s": 105.0, "reset_reason": "panic/exception"})
    assert reason is None


def test_board_health_tracker_first_observation_never_flags():
    tracker = pv.BoardHealthTracker()
    reason = tracker.observe({"uptime_s": 1.0, "reset_reason": "panic/exception"})
    assert reason is None
    assert tracker.last_uptime_s == 1.0
    assert tracker.last_reset_reason == "panic/exception"


def test_board_health_tracker_flags_a_reboot_after_baseline():
    tracker = pv.BoardHealthTracker()
    tracker.observe({"uptime_s": 500.0, "reset_reason": "power-on"})
    reason = tracker.observe({"uptime_s": 3.0, "reset_reason": "task watchdog"})
    assert reason is not None and "backwards" in reason


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


def test_run_report_records_every_retry_in_the_report():
    """A run that 'passed' after several retries must be visibly different
    from one that passed cleanly."""
    report = pv.RunReport(host="x", started_at="t0", dry_run=True, hard_max_temp_c=90.0)
    report.stages = [pv.StageReport(name="a", passed=True)]
    report.retries = [
        pv.RetryEvent(stage="a", operation="op1", attempt=1, wait_s=3.0, error="timed out"),
        pv.RetryEvent(stage="a", operation="op1", attempt=2, wait_s=6.0, error="timed out"),
    ]
    doc = json.loads(report.to_json())
    assert doc["retry_count"] == 2
    assert doc["retries"][0]["operation"] == "op1"
    assert "RETRIES: 2" in report.summary_text()


def test_run_report_no_retries_omits_the_retries_line():
    report = pv.RunReport(host="x", started_at="t0", dry_run=True, hard_max_temp_c=90.0)
    report.stages = [pv.StageReport(name="a", passed=True)]
    assert "RETRIES" not in report.summary_text()


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


def test_dry_run_survives_a_transient_error_and_records_the_retry(harness, tmp_path):
    """A single transient timeout on one HTTP-shaped call must not abort the
    run -- it should retry and the report should show it happened. This is
    the direct regression test for the bug in the brief: one urlopen timeout
    used to blow up the whole run."""

    class FlakyOnceSession(harness.FakeSession):
        def __init__(self):
            super().__init__()
            self._matrix_calls_seen = 0

        def autotune_matrix(self):
            self._matrix_calls_seen += 1
            if self._matrix_calls_seen == 1:
                raise RuntimeError("http://kilnctl.local/api/autotune/matrix: "
                                    "<urlopen error timed out>")
            return super().autotune_matrix()

    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path),
        "--steady-state-err-c", "3.0",  # loosen so ONLY the retry path is under test
        "--retry-backoff-s", "0.001", "--retry-backoff-max-s", "0.001"])
    session = FlakyOnceSession()
    report = harness._run(session, args)
    assert report.aborted is False
    assert report.passed is True
    assert len(report.retries) == 1
    assert report.retries[0].operation == "autotune_matrix"
    assert "timed out" in report.retries[0].error


def test_dry_run_aborts_loudly_when_retries_are_exhausted(harness, tmp_path):
    """NEGATIVE TEST: a persistently-failing transient error must not retry
    forever -- it must abort the run once max_retries is exceeded, and say
    so."""

    class AlwaysFlakySession(harness.FakeSession):
        def autotune_matrix(self):
            raise RuntimeError("Connection refused")

    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path),
        "--max-retries", "2", "--retry-backoff-s", "0.001", "--retry-backoff-max-s", "0.001"])
    session = AlwaysFlakySession()
    report = harness._run(session, args)
    assert report.aborted is True
    assert "gave up after" in report.abort_reason
    assert len(report.retries) == 2  # exactly max_retries retries were recorded, not infinite
    # Teardown must still have run.
    assert any(s.name == "teardown_force_all_stop" for s in report.stages)


def test_dry_run_aborts_on_board_restart_detected_after_a_retry(harness, tmp_path):
    """NEGATIVE TEST for the OTHER half: a request that eventually 'succeeds'
    only because the board rebooted mid-retry (uptime resets, reset_reason
    flips to a panic class) must ABORT loudly, never read as a healthy
    pass."""

    class CrashesThenRebootsSession(harness.FakeSession):
        def __init__(self):
            super().__init__()
            self._matrix_calls_seen = 0
            self._reset_reason = "power-on"

        def autotune_matrix(self):
            self._matrix_calls_seen += 1
            if self._matrix_calls_seen == 1:
                # Simulate the panic happening: the NEXT status read must
                # show a fresh panic reset_reason with a low uptime.
                self._reset_reason = "panic/exception"
                self._uptime = 0.0
                raise RuntimeError("http://kilnctl.local/api/autotune/matrix: "
                                    "<urlopen error timed out>")
            return super().autotune_matrix()

    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path),
        "--retry-backoff-s", "0.001", "--retry-backoff-max-s", "0.001"])
    session = CrashesThenRebootsSession()
    report = harness._run(session, args)
    assert report.aborted is True
    assert "panic/exception" in report.abort_reason
    assert any(s.name == "teardown_force_all_stop" for s in report.stages)


def test_dry_run_aborts_on_reset_reason_change_without_uptime_going_backwards(harness, tmp_path):
    """NEGATIVE TEST isolating detect_board_restart's SECOND signal on its
    own. The test above forces BOTH uptime-backwards AND a reset_reason
    change simultaneously, so it actually proves nothing about the
    reset_reason branch specifically -- detect_board_restart checks uptime
    first and returns on that signal alone, so a test that trips both
    signals could pass even if the reset_reason branch were deleted. Here
    uptime keeps advancing normally (FakeSession.status()'s own monotonic
    increment, untouched) and ONLY reset_reason flips to a panic class, so
    an abort here can only be explained by the reset_reason branch firing."""

    class FlipsResetReasonOnlySession(harness.FakeSession):
        def __init__(self):
            super().__init__()
            self._matrix_calls_seen = 0
            self._reset_reason = "power-on"

        def autotune_matrix(self):
            self._matrix_calls_seen += 1
            if self._matrix_calls_seen == 1:
                # ONLY the reset_reason changes; self._uptime is left alone
                # and keeps climbing via FakeSession.status()'s +1.0 per call.
                self._reset_reason = "panic/exception"
                raise RuntimeError("http://kilnctl.local/api/autotune/matrix: "
                                    "<urlopen error timed out>")
            return super().autotune_matrix()

    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path),
        "--retry-backoff-s", "0.001", "--retry-backoff-max-s", "0.001"])
    session = FlipsResetReasonOnlySession()
    report = harness._run(session, args)
    assert report.aborted is True
    assert "panic/exception" in report.abort_reason
    assert "backwards" not in report.abort_reason, (
        "this must be the reset_reason branch, not the uptime-backwards one")
    assert any(s.name == "teardown_force_all_stop" for s in report.stages)


def test_dry_run_aborts_on_silent_reboot_with_no_http_error(harness, tmp_path):
    """B1 regression test. Demonstrated broken behaviour before the fix: a
    session whose autotune_matrix() reboots (sets uptime_s=0 and
    reset_reason='panic/exception') WITHOUT raising produces zero
    exceptions and zero retries -- the old code only checked board health
    inside ``if events:`` (i.e. only after a retry actually fired), so
    nothing ever compared uptime and the run reported aborted=False,
    passed=True, retries=0. _retrying() now observes health on EVERY call,
    so this must be caught with no retry involved at all."""

    class SilentRebootSession(harness.FakeSession):
        def autotune_matrix(self):
            # The board rebooted, but this call itself returns 200/success --
            # no exception, nothing for call_with_retry to ever see.
            self._uptime = 0.0
            self._reset_reason = "panic/exception"
            return super().autotune_matrix()

    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path)])
    session = SilentRebootSession()
    report = harness._run(session, args)
    assert report.aborted is True, "a mid-run reboot with no HTTP error must still be caught"
    assert len(report.retries) == 0, "this scenario must not have needed any retry to be detected"
    assert any(s.name == "teardown_force_all_stop" for s in report.stages)


def test_tune_zone_retry_does_not_reset_the_wait_deadline(harness, tmp_path):
    """B3 regression test: retrying ``wait_for_autotune_state`` must not
    hand the retried attempt a fresh ``tune_timeout_s`` budget -- the total
    wall time across all attempts for one zone must stay bounded near the
    configured tune_timeout_s, not (max_retries + 1) x tune_timeout_s.
    Proven here by giving the FIRST attempt a slow real sleep before it
    fails transiently, then checking the SECOND attempt was handed a
    shrunk remaining-time budget rather than a fresh full one."""
    import time as time_mod

    class SlowFirstAttemptSession(harness.FakeSession):
        def __init__(self):
            super().__init__()
            self._wait_calls = 0
            self.timeouts_seen: "list[float]" = []

        def wait_for_autotune_state(self, states, timeout_s, poll_s=2.0):
            self._wait_calls += 1
            self.timeouts_seen.append(timeout_s)
            if self._wait_calls == 1:
                # Stand in for a slow first attempt that burned most of its
                # own budget before failing transiently.
                time_mod.sleep(0.1)
                raise RuntimeError("http://kilnctl.local/api/autotune/state: "
                                    "<urlopen error timed out>")
            return super().wait_for_autotune_state(states, timeout_s, poll_s=poll_s)

    session = SlowFirstAttemptSession()
    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path), "--zones", "0",
        "--tune-timeout-s", "0.15",
        "--retry-backoff-s", "0.001", "--retry-backoff-max-s", "0.001"])
    report = harness._run(session, args)
    assert session._wait_calls == 2, "expected exactly one retry to have fired"
    assert session.timeouts_seen[1] <= session.timeouts_seen[0], (
        "the retried attempt's timeout must never exceed the first attempt's")
    # The bug this guards against: the retried attempt used to get a FRESH
    # 0.15s budget regardless of how much of the original deadline the
    # first attempt already spent. Here the first attempt alone burned
    # 0.1s of the 0.15s deadline, so the second attempt must see well under
    # 0.1s remaining -- not another full 0.15s.
    assert session.timeouts_seen[1] < 0.1, (
        f"second attempt was handed {session.timeouts_seen[1]:.3f}s -- "
        "the deadline was not carried across the retry")
    assert report.aborted is False


def test_profile_tracking_sample_response_is_not_retried(harness, tmp_path):
    """B4 regression test: sample_response() must NEVER be wrapped in the
    retry mechanism -- it is not idempotent (the profile keeps executing on
    the board while it polls). Retrying it would throw away every row
    already collected and re-sample a fresh full duration mid-profile,
    producing a trace time-shifted against the profile's own expectations.
    Proof: a transient BenchSessionError from sample_response must end the
    stage as an immediate FAILURE, called exactly once -- with ZERO retry
    events recorded for it. Before the B4 fix this call sat inside
    _retrying() alongside every other stage call."""

    class FlakySampleSession(harness.FakeSession):
        def __init__(self):
            super().__init__()
            self.sample_calls = 0

        def sample_response(self, duration_s, period_s=2.0, zone_index=0, on_sample=None):
            self.sample_calls += 1
            raise harness.BenchSessionError(
                "http://kilnctl.local/api/status: <urlopen error timed out>")

    session = FlakySampleSession()
    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path),
        "--skip-cooldown", "--skip-tune", "--skip-matrix", "--skip-backup",
        "--retry-backoff-s", "0.001", "--retry-backoff-max-s", "0.001"])
    report = harness._run(session, args)
    assert session.sample_calls == 1, "sample_response must be called exactly once -- never retried"
    tracking = [s for s in report.stages if s.name == "profile_tracking"][0]
    assert not tracking.passed
    assert tracking.error and "timed out" in tracking.error
    assert not any(r.operation == "sample_response" for r in report.retries), (
        "sample_response must never appear as a retried operation")
    # Item 3 fix, the OTHER direction: this is a generic transient error, NOT
    # a genuine ceiling breach (no channels_c/hottest_c on the exception) --
    # it must stay a plain stage failure, not get promoted to report.aborted.
    assert report.aborted is False


def test_profile_tracking_keeps_partial_trace_on_ceiling_breach(harness, tmp_path):
    """NEGATIVE TEST for the actual hardware failure in the brief: a 28.6-
    minute heat run's ``profile_tracking`` stage failed with only the bare
    string "live temperature 80.1 C breached the 80.0 C fixture ceiling
    during sampling" -- checks, detail and history all came back EMPTY, so
    the sampled trace that would have explained the breach was gone.

    Simulates the breach directly: ``sample_response`` here plays two rows
    through ``on_sample`` (as the real one does incrementally as it polls)
    and then raises ``BenchSessionError`` exactly as the real fixture ceiling
    check does, WITHOUT ever returning a row list to its caller -- so any fix
    that only reads the function's return value would still see nothing.

    Before the fix, ``stage_profile_tracking``'s ``except BenchSessionError``
    handler built a bare ``StageReport(error=str(exc))`` with no ``checks=``
    and no ``detail=`` at all -- this assertion block genuinely fails against
    that code (verified by hand: reverting the fix reproduces empty
    ``checks``/``detail`` here, and this test fails on
    ``assert tracking.detail.get("history")`` with detail == {}).
    """

    class BreachingSession(harness.FakeSession):
        def sample_response(self, duration_s, period_s=2.0, zone_index=0, on_sample=None):
            rows_before_breach = [
                {"t_s": 0.0, "channels_c": {0: 40.0, 1: 40.0, 2: 40.0}, "target_c": 50.0,
                 "segment_index": 0, "duty_by_zone": {0: 1.0, 1: 1.0, 2: 1.0}, "hottest_c": 40.0},
                {"t_s": 2.0, "channels_c": {0: 60.0, 1: 60.0, 2: 60.0}, "target_c": 50.0,
                 "segment_index": 0, "duty_by_zone": {0: 1.0, 1: 1.0, 2: 1.0}, "hottest_c": 80.1},
            ]
            for row in rows_before_breach:
                if on_sample is not None:
                    on_sample(row)
            # No `return rows` ever reached -- exactly like the real
            # bench_fixture_session.sample_response() on a ceiling breach.
            # channels_c/hottest_c set on the exception, exactly as the
            # real sample_response() does (see its S1 fix) -- these are
            # what tell stage_profile_tracking this is a genuine breach,
            # not a generic transient error (item 3 fix below).
            exc = harness.BenchSessionError(
                "live temperature 80.1 C breached the 80.0 C fixture ceiling during sampling")
            exc.channels_c = {0: 60.0, 1: 60.0, 2: 60.0}
            exc.hottest_c = 80.1
            raise exc

    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path),
        "--skip-cooldown", "--skip-tune", "--skip-matrix", "--skip-backup"])
    session = BreachingSession()
    report = harness._run(session, args)

    # Item 3 fix: a genuine ceiling breach is exactly as serious as the
    # harness's own hard-max path, which already sets these -- must not be
    # a plain unflagged stage failure.
    assert report.aborted is True
    assert report.abort_reason and "fixture ceiling" in report.abort_reason

    tracking = [s for s in report.stages if s.name == "profile_tracking"][0]
    assert tracking.passed is False
    assert tracking.error and "fixture ceiling" in tracking.error

    # THE FIX under test: the partial trace must have survived the raise.
    assert tracking.detail.get("history"), (
        "the sampled trace must not be empty on a mid-sampling ceiling breach")
    assert len(tracking.detail["history"]) == 2
    assert tracking.detail["history"][0]["hottest_c"] == 40.0
    assert tracking.detail["history"][1]["hottest_c"] == 80.1

    # Requirement 2: the segments actually posted must be recorded, and
    # recorded before this failure -- present even though heating never
    # produced a single passing check. Asserted EQUAL to what was actually
    # computed and posted, not just "truthy".
    expected_segments = harness._profile_segments_c_to_80(
        args.tracking_peak1_c, args.tracking_peak2_c)
    assert tracking.detail.get("posted_segments") == expected_segments

    # Zone-mode checks gathered before sampling began must survive too --
    # they are exactly the "checks" the bug report says came back empty.
    assert any(c.name.endswith(".control_mode_pid") for c in tracking.checks)

    # Requirement 3: breach summary stats computed from the partial trace,
    # never fabricated -- zone 0 saw 40.0 then 60.0, so max_c is 60.0. Row 0
    # (40.0 < target 50.0) then row 1 (60.0 >= target 50.0) is a genuine
    # crossing, with duty 1.0 (fraction_0_1) at that row -- "full_power".
    stats = tracking.detail.get("breach_stats")
    assert stats is not None
    assert stats["0"]["max_c"] == 60.0
    assert stats["0"]["setpoint_crossing"] == "crossed"
    assert stats["0"]["duty_at_clamp"] == "full_power"

    # JSON-serializable end to end (requirement 4), NaN-free (item 6): the
    # dumped/reloaded doc must contain a plain float, never a bare NaN token.
    doc = json.loads(pv.RunReport(
        host="x", started_at="t", dry_run=True, hard_max_temp_c=90.0,
        stages=[tracking]).to_json())
    assert doc["stages"][0]["detail"]["history"][0]["hottest_c"] == 40.0


def test_profile_tracking_records_posted_segments_before_start_profile_fails(harness, tmp_path):
    """A second exit path: the board refuses ``start_profile`` (a bare 400,
    say) before any sampling happens at all. ``posted_segments`` must still
    be in the report -- it was computed and should be recorded before the
    HTTP call that failed, not only on a sampling-time breach."""

    class RefusingStartSession(harness.FakeSession):
        def start_profile(self, slot):
            return 400, "bad request"

    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path),
        "--skip-cooldown", "--skip-tune", "--skip-matrix", "--skip-backup"])
    report = harness._run(RefusingStartSession(), args)
    tracking = [s for s in report.stages if s.name == "profile_tracking"][0]
    assert not tracking.passed
    assert "start refused" in tracking.error
    expected_segments = harness._profile_segments_c_to_80(
        args.tracking_peak1_c, args.tracking_peak2_c)
    assert tracking.detail.get("posted_segments") == expected_segments


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


# ---------------------------------------------------------------------------
# build_tracking_profile_name / validate_profile_payload
#
# This is the fix for the actual hardware failure that prompted this pass: a
# full validation run burned an hour of kiln heating only to discover, at
# stage 5, that the profile name the harness sent ("pid_validation_tracking",
# 23 chars) exceeded the firmware's PROFILE_NAME_MAX_LEN (15 chars --
# firmware/KilnFW/App/drivers/http/profiles_http.h:29, enforced by the "name too
# long" check at profiles_http.c:1346-1349). Every check below is proven able
# to FAIL (break the bound, see the assertion fire, restore) before being
# trusted to pass -- an assertion that cannot be shown failing has no
# business gating an hour of heating.
# ---------------------------------------------------------------------------

_GOOD_SEGMENTS = [{"target_c": 50.0, "ramp_c_per_hr": 120.0, "dwell_min": 8}]


def test_tracking_profile_name_fits_firmware_limit_worst_case():
    """The default prefix ("pv") + an 8-digit MMDDHHMM timestamp must always
    fit PROFILE_NAME_MAX_LEN (15), for every possible timestamp -- not just
    the one 'now' happens to produce when the test runs."""
    import time as _time
    worst_case_when = _time.strptime("2026 12312359", "%Y %m%d%H%M")
    name = pv.build_tracking_profile_name(when=worst_case_when)
    assert len(name) <= pv.PROFILE_FW_NAME_MAX_LEN
    assert name == "pv12312359"  # still human-identifiable, not just short


def test_tracking_profile_name_long_prefix_is_truncated_not_rejected():
    """NEGATIVE-TEST-ADJACENT: a pathological caller-supplied prefix must
    still come back within the firmware bound rather than raising or
    silently exceeding it."""
    name = pv.build_tracking_profile_name(prefix="a_much_too_long_prefix_for_this")
    assert len(name) <= pv.PROFILE_FW_NAME_MAX_LEN


def test_validate_profile_payload_accepts_a_conforming_payload():
    pv.validate_profile_payload(0, "pv12312359", 0b111, _GOOD_SEGMENTS)


def test_validate_profile_payload_rejects_name_over_firmware_limit():
    """NEGATIVE TEST. Reproduces the actual failure: a name one character
    over PROFILE_NAME_MAX_LEN (15) must be caught here, locally, before any
    HTTP call -- not discovered as a 400 after the board has already started
    heating for the stage."""
    long_name = "x" * (pv.PROFILE_FW_NAME_MAX_LEN + 1)
    with pytest.raises(pv.ProfilePayloadError, match="name"):
        pv.validate_profile_payload(0, long_name, 0b111, _GOOD_SEGMENTS)
    # restore: a name exactly at the limit must be accepted
    ok_name = "x" * pv.PROFILE_FW_NAME_MAX_LEN
    pv.validate_profile_payload(0, ok_name, 0b111, _GOOD_SEGMENTS)


def test_validate_profile_payload_rejects_slot_out_of_range():
    """NEGATIVE TEST. Reproduces a second real bug found in this pass: the
    harness's own --tracking-slot default (15) was out of the firmware's
    0-7 range (PROFILES_MAX_COUNT=8, profiles_http.h:28) and the firmware
    silently reinterpreted an out-of-range id as "first free slot" instead
    of rejecting it -- so this must catch it locally instead of relying on
    that fallback."""
    with pytest.raises(pv.ProfilePayloadError, match="slot"):
        pv.validate_profile_payload(15, "ok", 0b111, _GOOD_SEGMENTS)
    with pytest.raises(pv.ProfilePayloadError, match="slot"):
        pv.validate_profile_payload(-1, "ok", 0b111, _GOOD_SEGMENTS)
    # restore: every in-range slot (0..7) is accepted
    pv.validate_profile_payload(7, "ok", 0b111, _GOOD_SEGMENTS)
    pv.validate_profile_payload(0, "ok", 0b111, _GOOD_SEGMENTS)


def test_validate_profile_payload_rejects_zero_or_oversize_zone_mask():
    """NEGATIVE TEST. zone_mask must select at least one zone
    (profiles_http.c:1367) and fit a uint8_t."""
    with pytest.raises(pv.ProfilePayloadError, match="zone_mask"):
        pv.validate_profile_payload(0, "ok", 0, _GOOD_SEGMENTS)
    with pytest.raises(pv.ProfilePayloadError, match="zone_mask"):
        pv.validate_profile_payload(0, "ok", 0x100, _GOOD_SEGMENTS)
    # restore
    pv.validate_profile_payload(0, "ok", 0xFF, _GOOD_SEGMENTS)


def test_validate_profile_payload_rejects_segment_count_out_of_range():
    """NEGATIVE TEST. seg_count must be 1-12 (PROFILE_MAX_SEGMENTS,
    profiles_http.h:30 / profiles_http.c:1382)."""
    with pytest.raises(pv.ProfilePayloadError, match="segment count"):
        pv.validate_profile_payload(0, "ok", 0b111, [])
    too_many = [dict(_GOOD_SEGMENTS[0]) for _ in range(pv.PROFILE_FW_MAX_SEGMENTS + 1)]
    with pytest.raises(pv.ProfilePayloadError, match="segment count"):
        pv.validate_profile_payload(0, "ok", 0b111, too_many)
    # restore: exactly the max is fine
    at_max = [dict(_GOOD_SEGMENTS[0]) for _ in range(pv.PROFILE_FW_MAX_SEGMENTS)]
    pv.validate_profile_payload(0, "ok", 0b111, at_max)


def test_validate_profile_payload_rejects_target_c_out_of_range():
    """NEGATIVE TEST. target_c must be 0-1400 (profiles_http.c:102-103)."""
    bad = [{"target_c": 1400.1, "ramp_c_per_hr": 100.0, "dwell_min": 5}]
    with pytest.raises(pv.ProfilePayloadError, match="target_c"):
        pv.validate_profile_payload(0, "ok", 0b111, bad)
    # restore: exactly at the ceiling is fine
    ok = [{"target_c": 1400.0, "ramp_c_per_hr": 100.0, "dwell_min": 5}]
    pv.validate_profile_payload(0, "ok", 0b111, ok)


def test_validate_profile_payload_rejects_ramp_out_of_range():
    """NEGATIVE TEST. ramp_c_per_hr must be 0-1000 (profiles_http.c:104-105)."""
    bad = [{"target_c": 50.0, "ramp_c_per_hr": 1000.1, "dwell_min": 5}]
    with pytest.raises(pv.ProfilePayloadError, match="ramp_c_per_hr"):
        pv.validate_profile_payload(0, "ok", 0b111, bad)
    ok = [{"target_c": 50.0, "ramp_c_per_hr": 1000.0, "dwell_min": 5}]
    pv.validate_profile_payload(0, "ok", 0b111, ok)


def test_validate_profile_payload_rejects_dwell_out_of_range():
    """NEGATIVE TEST. dwell_min must be 0-1440 (profiles_http.c:106, 24h)."""
    bad = [{"target_c": 50.0, "ramp_c_per_hr": 100.0, "dwell_min": 1441}]
    with pytest.raises(pv.ProfilePayloadError, match="dwell_min"):
        pv.validate_profile_payload(0, "ok", 0b111, bad)
    ok = [{"target_c": 50.0, "ramp_c_per_hr": 100.0, "dwell_min": 1440}]
    pv.validate_profile_payload(0, "ok", 0b111, ok)


# ---------------------------------------------------------------------------
# Zone commissioning pre-flight (check_zones_commissioned et al.)
# ---------------------------------------------------------------------------

#: A profile matching the real bug report: 120 C/hr ramps up to 70 C.
_TRACKING_SEGMENTS = [
    {"target_c": 50.0, "ramp_c_per_hr": 120.0, "dwell_min": 8},
    {"target_c": 70.0, "ramp_c_per_hr": 120.0, "dwell_min": 8},
]

#: Zone 0 from the bug report: fully commissioned.
_ZONE0_COMMISSIONED = {"index": 0, "max_ramp_c_per_hr": 900.0, "max_temp_c": 80.0,
                        "cross_zone_max_delta_c": 50.0}
#: Zones 1 and 2 from the bug report: autotune wrote gains but the guard-limit
#: fields were left at their post-flash 0.0.
_ZONE_UNCOMMISSIONED = {"index": 1, "max_ramp_c_per_hr": 0.0, "max_temp_c": 0.0,
                         "cross_zone_max_delta_c": 0.0}


def test_profile_commissioning_requirements_takes_steepest_ramp_and_highest_target():
    reqs = pv.profile_commissioning_requirements([0, 1], _TRACKING_SEGMENTS)
    assert reqs[0].steepest_ramp_c_per_hr == 120.0
    assert reqs[0].highest_target_c == 70.0
    # identical requirement handed to every zone in the profile
    assert reqs[1].steepest_ramp_c_per_hr == 120.0
    assert reqs[1].highest_target_c == 70.0


def test_profile_commissioning_requirements_ignores_dwell_only_segments():
    """NEGATIVE-TEST-ADJACENT: a segment with ramp_c_per_hr 0/absent (a pure
    dwell) must not count toward the ramp requirement, or a profile that is
    ENTIRELY dwells would wrongly demand a nonzero ceiling."""
    dwell_only = [{"target_c": 40.0, "ramp_c_per_hr": 0.0, "dwell_min": 5}]
    reqs = pv.profile_commissioning_requirements([0], dwell_only)
    assert reqs[0].steepest_ramp_c_per_hr == 0.0
    assert reqs[0].highest_target_c == 40.0


def test_check_zone_commissioned_accepts_a_properly_commissioned_zone():
    req = pv.profile_commissioning_requirements([0], _TRACKING_SEGMENTS)[0]
    assert pv.check_zone_commissioned(req, _ZONE0_COMMISSIONED) is None


def test_check_zone_commissioned_rejects_zero_ceiling_zone():
    """NEGATIVE TEST. Reproduces the actual bug: a zone whose
    max_ramp_c_per_hr/max_temp_c/cross_zone_max_delta_c are all still 0.0
    (never commissioned) must fail, and the message must name the zone and
    the field."""
    req = pv.profile_commissioning_requirements([1], _TRACKING_SEGMENTS)[1]
    reason = pv.check_zone_commissioned(req, _ZONE_UNCOMMISSIONED)
    assert reason is not None
    assert "zone 1" in reason
    assert "max_ramp_c_per_hr" in reason
    # restore: the same zone commissioned like zone 0 passes
    fixed = dict(_ZONE_UNCOMMISSIONED, max_ramp_c_per_hr=900.0, max_temp_c=80.0,
                 cross_zone_max_delta_c=50.0)
    assert pv.check_zone_commissioned(req, fixed) is None


def test_check_zone_commissioned_rejects_ceiling_too_low_for_this_profile_distinctly():
    """NEGATIVE TEST. A zone that IS commissioned (nonzero fields) but whose
    ramp ceiling is merely too low for THIS profile's steepest segment must
    fail with a message distinct from the "never configured" (0.0) case --
    the operator needs to know whether to commission the zone at all, or
    just raise an existing number."""
    req = pv.profile_commissioning_requirements([1], _TRACKING_SEGMENTS)[1]
    too_low = {"index": 1, "max_ramp_c_per_hr": 60.0, "max_temp_c": 80.0,
               "cross_zone_max_delta_c": 50.0}
    reason = pv.check_zone_commissioned(req, too_low)
    assert reason is not None
    assert "below this profile" in reason
    assert "never" not in reason  # not the uncommissioned-zone message
    # restore: raising the ceiling to cover the profile passes
    fixed = dict(too_low, max_ramp_c_per_hr=120.0)
    assert pv.check_zone_commissioned(req, fixed) is None


def test_check_zone_commissioned_rejects_zero_max_temp_c_even_with_ramp_ok():
    """NEGATIVE TEST. max_temp_c==0.0 disables guard 5 entirely
    (thermal_guard.c:106) -- a zone with a fine ramp ceiling but max_temp_c
    still at 0 must still fail, distinctly, naming max_temp_c."""
    req = pv.profile_commissioning_requirements([1], _TRACKING_SEGMENTS)[1]
    zero_temp = {"index": 1, "max_ramp_c_per_hr": 900.0, "max_temp_c": 0.0,
                 "cross_zone_max_delta_c": 50.0}
    reason = pv.check_zone_commissioned(req, zero_temp)
    assert reason is not None
    assert "max_temp_c" in reason
    fixed = dict(zero_temp, max_temp_c=80.0)
    assert pv.check_zone_commissioned(req, fixed) is None


def test_check_zone_commissioned_rejects_zero_cross_zone_delta_even_with_others_ok():
    """NEGATIVE TEST. cross_zone_max_delta_c==0.0 disables guard 8 entirely
    (thermal_guard.c:331) -- the opposite convention from max_ramp_c_per_hr.
    A zone with ramp+temp both fine but cross_zone_max_delta_c still at 0
    must fail, naming that field specifically, proving the check does not
    mistakenly treat 0 there as "maximally strict and therefore fine"."""
    req = pv.profile_commissioning_requirements([1], _TRACKING_SEGMENTS)[1]
    zero_xzone = {"index": 1, "max_ramp_c_per_hr": 900.0, "max_temp_c": 80.0,
                  "cross_zone_max_delta_c": 0.0}
    reason = pv.check_zone_commissioned(req, zero_xzone)
    assert reason is not None
    assert "cross_zone_max_delta_c" in reason
    fixed = dict(zero_xzone, cross_zone_max_delta_c=50.0)
    assert pv.check_zone_commissioned(req, fixed) is None


def test_check_zones_commissioned_raises_naming_every_failing_zone():
    """NEGATIVE TEST. zone_configs matching the actual bug report (zone 0
    fine, zones 1 and 2 uncommissioned) must raise, and the exception's
    ``failures`` must name both zone 1 and zone 2."""
    zone_configs = {
        0: _ZONE0_COMMISSIONED,
        1: dict(_ZONE_UNCOMMISSIONED, index=1),
        2: dict(_ZONE_UNCOMMISSIONED, index=2),
    }
    with pytest.raises(pv.ZoneNotCommissionedError) as excinfo:
        pv.check_zones_commissioned([0, 1, 2], zone_configs, _TRACKING_SEGMENTS)
    failures = excinfo.value.failures
    assert any("zone 1" in f for f in failures)
    assert any("zone 2" in f for f in failures)
    assert not any("zone 0" in f for f in failures)
    # restore: all three commissioned like zone 0 passes cleanly
    all_ok = {i: dict(_ZONE0_COMMISSIONED, index=i) for i in (0, 1, 2)}
    pv.check_zones_commissioned([0, 1, 2], all_ok, _TRACKING_SEGMENTS)


def test_check_zones_commissioned_reports_missing_zone_config():
    """NEGATIVE TEST. A zone the harness intends to drive but that GET
    /api/zones never reported at all must fail loudly (not KeyError)."""
    with pytest.raises(pv.ZoneNotCommissionedError, match="zone 2"):
        pv.check_zones_commissioned([0, 2], {0: _ZONE0_COMMISSIONED}, _TRACKING_SEGMENTS)


# ---------------------------------------------------------------------------
# stage_profile_tracking wiring: pre-flight rejection + PID-mode enforcement
# ---------------------------------------------------------------------------

def test_dry_run_profile_tracking_pid_mode_is_verified_by_readback(harness, tmp_path):
    """The FakeSession reports every zone already in PID mode; confirm the
    stage actually records that check rather than skipping it."""
    rc = harness.main([
        "--dry-run", "--report-dir", str(tmp_path), "--steady-state-err-c", "5.0"])
    assert rc == 0
    jsons = sorted(tmp_path.glob("pid_validation_*.json"), key=lambda p: p.stat().st_mtime)
    doc = json.loads(jsons[-1].read_text(encoding="utf-8"))
    tracking = next(s for s in doc["stages"] if s["name"] == "profile_tracking")
    mode_checks = [c for c in tracking["checks"] if c["name"].endswith("control_mode_pid")]
    assert len(mode_checks) == len(harness.build_arg_parser().parse_args(["--dry-run"]).zones)
    assert all(c["passed"] for c in mode_checks)


def test_dry_run_profile_tracking_aborts_locally_when_zone_not_in_pid_mode(harness, tmp_path, monkeypatch):
    """NEGATIVE TEST for the accept-writes-gains-but-not-mode gap: force the
    FakeSession to report a zone stuck in OFF after ensure_pid_mode() and
    confirm the stage fails locally (never reaches put_profile/start_profile)
    instead of silently running a profile against a zone that cannot heat."""
    put_profile_called = []

    def _fake_ensure_pid_mode(self, zones):
        # zone 1 refuses to take PID mode -- readback still shows OFF (0).
        return {z: (0 if z == 1 else 2) for z in zones}

    def _fake_put_profile(self, slot, name, zone_mask, segments):
        put_profile_called.append(True)

    monkeypatch.setattr(harness.FakeSession, "ensure_pid_mode", _fake_ensure_pid_mode)
    monkeypatch.setattr(harness.FakeSession, "put_profile", _fake_put_profile)

    rc = harness.main([
        "--dry-run", "--report-dir", str(tmp_path), "--skip-cooldown", "--skip-tune",
        "--skip-matrix", "--skip-backup"])
    assert rc != 0
    assert not put_profile_called, "must not start the profile when a zone's PID mode is unconfirmed"
    jsons = sorted(tmp_path.glob("pid_validation_*.json"), key=lambda p: p.stat().st_mtime)
    doc = json.loads(jsons[-1].read_text(encoding="utf-8"))
    tracking = next(s for s in doc["stages"] if s["name"] == "profile_tracking")
    assert tracking["passed"] is False
    zone1_check = next(c for c in tracking["checks"] if c["name"] == "zone1.control_mode_pid")
    assert zone1_check["passed"] is False
    assert zone1_check["actual"] == 0


def test_dry_run_profile_tracking_aborts_locally_when_zone_not_commissioned(harness, tmp_path, monkeypatch):
    """NEGATIVE TEST reproducing the actual bug report: zone 1's guard-limit
    fields are still at their post-flash 0.0. Confirm the stage refuses to
    start (never reaches put_profile) and reports the requirement -- not a
    bare 400 discovered after the cooldown gate already ran."""
    put_profile_called = []

    def _fake_get_zones(self):
        return {"zones": [
            {"index": 0, "max_ramp_c_per_hr": 900.0, "max_temp_c": 80.0, "cross_zone_max_delta_c": 50.0},
            {"index": 1, "max_ramp_c_per_hr": 0.0, "max_temp_c": 0.0, "cross_zone_max_delta_c": 0.0},
            {"index": 2, "max_ramp_c_per_hr": 0.0, "max_temp_c": 0.0, "cross_zone_max_delta_c": 0.0},
        ]}

    def _fake_put_profile(self, slot, name, zone_mask, segments):
        put_profile_called.append(True)

    monkeypatch.setattr(harness.FakeSession, "get_zones", _fake_get_zones)
    monkeypatch.setattr(harness.FakeSession, "put_profile", _fake_put_profile)

    rc = harness.main([
        "--dry-run", "--report-dir", str(tmp_path), "--skip-cooldown", "--skip-tune",
        "--skip-matrix", "--skip-backup"])
    assert rc != 0
    assert not put_profile_called, "must not start the profile against uncommissioned zones"
    jsons = sorted(tmp_path.glob("pid_validation_*.json"), key=lambda p: p.stat().st_mtime)
    doc = json.loads(jsons[-1].read_text(encoding="utf-8"))
    tracking = next(s for s in doc["stages"] if s["name"] == "profile_tracking")
    assert tracking["passed"] is False
    assert "zone 1" in tracking["error"] and "zone 2" in tracking["error"]
    assert "max_ramp_c_per_hr" in tracking["error"]
    # Item 5 regression: this return used to build its OWN
    # ``detail={"commissioning_failures": ...}``, silently overwriting the
    # ``posted_segments`` already recorded into the shared detail dict.
    assert tracking["detail"].get("commissioning_failures")
    assert tracking["detail"].get("posted_segments") == \
        harness._profile_segments_c_to_80(
            harness.build_arg_parser().parse_args(["--dry-run"]).tracking_peak1_c,
            harness.build_arg_parser().parse_args(["--dry-run"]).tracking_peak2_c)


def test_dry_run_profile_tracking_commission_zones_flag_writes_only_deficient_fields(
        harness, tmp_path, monkeypatch):
    """--commission-zones is opt-in: with it passed, a zone missing ramp/temp
    coverage gets exactly those fields written (never cross_zone_max_delta_c,
    which has no safe default) and the run proceeds."""
    written = []

    def _fake_get_zones(self):
        zones = getattr(self, "_zones", None)
        if zones is None:
            zones = {
                0: {"index": 0, "max_ramp_c_per_hr": 900.0, "max_temp_c": 80.0,
                    "cross_zone_max_delta_c": 50.0},
                1: {"index": 1, "max_ramp_c_per_hr": 0.0, "max_temp_c": 0.0,
                    "cross_zone_max_delta_c": 50.0},
            }
            self._zones = zones
        return {"zones": list(zones.values())}

    def _fake_commission_zones(self, current, preset):
        for entry in preset["zones"]:
            written.append(dict(entry))
            self._zones[entry["index"]].update(entry)

    monkeypatch.setattr(harness.FakeSession, "get_zones", _fake_get_zones)
    monkeypatch.setattr(harness.FakeSession, "commission_zones", _fake_commission_zones)

    rc = harness.main([
        "--dry-run", "--report-dir", str(tmp_path), "--skip-cooldown", "--skip-tune",
        "--skip-matrix", "--skip-backup", "--zones", "0,1", "--commission-zones"])
    assert rc == 0
    assert len(written) == 1
    assert written[0]["index"] == 1
    assert "cross_zone_max_delta_c" not in written[0], \
        "must never auto-write cross_zone_max_delta_c, even under --commission-zones"
    assert written[0]["max_ramp_c_per_hr"] == 120.0
    assert written[0]["max_temp_c"] == 70.0


def test_dry_run_profile_tracking_commission_zones_flag_still_refuses_missing_cross_zone(
        harness, tmp_path, monkeypatch):
    """NEGATIVE TEST: even with --commission-zones, a zone whose ONLY
    deficiency is cross_zone_max_delta_c==0 must still abort the stage --
    that field is never auto-written, so re-checking after the (partial)
    commission must still fail."""
    def _fake_get_zones(self):
        return {"zones": [
            {"index": 0, "max_ramp_c_per_hr": 900.0, "max_temp_c": 80.0, "cross_zone_max_delta_c": 0.0},
        ]}

    def _fake_commission_zones(self, current, preset):
        pytest.fail("must not be called when there is nothing safe to auto-write")

    monkeypatch.setattr(harness.FakeSession, "get_zones", _fake_get_zones)
    monkeypatch.setattr(harness.FakeSession, "commission_zones", _fake_commission_zones)

    rc = harness.main([
        "--dry-run", "--report-dir", str(tmp_path), "--skip-cooldown", "--skip-tune",
        "--skip-matrix", "--skip-backup", "--zones", "0", "--commission-zones"])
    assert rc != 0
    jsons = sorted(tmp_path.glob("pid_validation_*.json"), key=lambda p: p.stat().st_mtime)
    doc = json.loads(jsons[-1].read_text(encoding="utf-8"))
    tracking = next(s for s in doc["stages"] if s["name"] == "profile_tracking")
    assert tracking["passed"] is False
    assert "cross_zone_max_delta_c" in tracking["error"]
    # Item 5 regression, same as the sibling test above: this is the SECOND
    # commissioning-failure return (post --commission-zones re-check) and it
    # used to overwrite posted_segments the same way the first one did.
    assert tracking["detail"].get("commissioning_failures")
    assert tracking["detail"].get("posted_segments")


# ---------------------------------------------------------------------------
# S1/S2: sample_response()'s emergency ordering, against the REAL
# bench_fixture_session.BenchSession -- not FakeSession's own override.
#
# A BreachingSession subclass that supplies its own sample_response() (as
# the tests above do) cannot exercise what bench_fixture_session.
# sample_response() itself does internally: an implementation that calls
# on_sample AFTER the ceiling raise, or that fetches exec_status() before
# checking the ceiling, would pass every test above while still being the
# safety regression the review found. These construct a real BenchSession
# and monkeypatch its status()/exec_status() methods directly (no urllib
# involved) to prove the ordering.
# ---------------------------------------------------------------------------

def _breach_status(temp_c: float = 80.1, channel: int = 0) -> dict:
    return {
        "channels": [{"channel": channel, "temp_c": temp_c, "valid": True}],
        "relays": [], "safety_heating_enabled": True, "safety_relay_energized": True,
        "heat_block_sources_words": [0, 0],
    }


def test_real_sample_response_raises_on_breach_without_calling_exec_status():
    """S1 NEGATIVE TEST. The emergency raise must not depend on
    exec_status() at all -- prove it by making exec_status() itself explode
    if it is ever called, and confirm sample_response() still raises the
    expected BenchSessionError (not exec_status()'s exception) with
    exec_status() never invoked."""
    session = bfs.BenchSession(host="unit-test-host")
    session.status = lambda: _breach_status(80.1)

    def _exec_status_must_not_run():
        raise RuntimeError("exec_status() must never be reached on a breaching sample")

    session.exec_status = _exec_status_must_not_run

    with pytest.raises(bfs.BenchSessionError) as exc_info:
        session.sample_response(duration_s=10.0, period_s=1.0)
    assert "fixture ceiling" in str(exc_info.value)
    # The exception carries the channel/temperature that tripped it -- item
    # 4's "record which channel actually breached" -- computed from data
    # already in hand (chan_c came off the same status() call as hottest),
    # never a second network round trip.
    assert exc_info.value.channels_c == {0: 80.1}
    assert exc_info.value.hottest_c == 80.1


def test_real_sample_response_breach_is_not_delayed_by_a_slow_exec_status():
    """S1 NEGATIVE TEST, the latency half: even if exec_status() would be
    slow to fail (an 8s HTTP timeout is exactly what a hung board looks
    like), the breach raise must not wait on it. exec_status() sleeps
    briefly here and then raises; the test asserts sample_response() returns
    in well under that sleep, proving exec_status() is never reached at
    all, not merely reached-and-caught."""
    session = bfs.BenchSession(host="unit-test-host")
    session.status = lambda: _breach_status(90.0)
    calls = {"n": 0}

    def _slow_exec_status():
        calls["n"] += 1
        time.sleep(0.3)
        raise RuntimeError("must never be reached")

    session.exec_status = _slow_exec_status

    t0 = time.monotonic()
    with pytest.raises(bfs.BenchSessionError):
        session.sample_response(duration_s=10.0, period_s=1.0)
    elapsed = time.monotonic() - t0
    assert calls["n"] == 0, "exec_status() must never be called on a breaching sample"
    assert elapsed < 0.15, (
        f"breach raise took {elapsed:.3f}s -- must not be delayed by exec_status() at all")


def test_real_sample_response_on_sample_exception_does_not_mask_the_breach():
    """S2 NEGATIVE TEST. An exception thrown from a caller's on_sample
    callback must never propagate in place of the real ceiling breach --
    that would silently convert "the kiln is over the ceiling" into
    whatever unrelated bug the callback happened to have, and drop the
    force_all_stop()-triggering exception entirely."""
    session = bfs.BenchSession(host="unit-test-host")
    # First poll: safe temperature (on_sample fires and explodes). Second
    # poll: breaches. If the explosion masked/aborted sampling, no second
    # poll would happen and BenchSessionError would never be raised.
    statuses = [_breach_status(40.0), _breach_status(80.1)]
    call_index = {"n": 0}

    def _status():
        s = statuses[min(call_index["n"], len(statuses) - 1)]
        call_index["n"] += 1
        return s

    session.status = _status
    session.exec_status = lambda: {"state": "running", "target_c": 50.0,
                                   "segment_index": 0, "zones": []}

    def _bad_on_sample(row):
        raise ValueError("boom -- a bug in the caller's callback, not a real breach")

    with pytest.raises(bfs.BenchSessionError) as exc_info:
        session.sample_response(duration_s=10.0, period_s=0.0, on_sample=_bad_on_sample)
    assert "fixture ceiling" in str(exc_info.value)


def test_on_sample_exception_is_logged_once_not_per_sample(caplog):
    """Minor fix NEGATIVE TEST: a persistently-throwing on_sample callback
    must not write one full traceback to the log per poll for the whole
    run -- that floods the log for a multi-hour run with a bug that only
    needed reporting once. Confirms exactly ONE exception-level record is
    emitted across 250+ samples, with a rate-limited summary line covering
    the rest."""
    import logging

    class _StopLoop(Exception):
        """Deterministically ends the sampling loop after N calls, instead
        of depending on wall-clock duration_s in a unit test."""

    session = bfs.BenchSession(host="unit-test-host")
    call_count = {"n": 0}

    def _status():
        call_count["n"] += 1
        if call_count["n"] > 250:
            raise _StopLoop()
        return {"channels": [{"channel": 0, "temp_c": 40.0, "valid": True}], "relays": [],
                "safety_heating_enabled": True, "safety_relay_energized": True,
                "heat_block_sources_words": [0, 0]}

    session.status = _status
    session.exec_status = lambda: {"state": "running", "target_c": 50.0,
                                   "segment_index": 0, "zones": []}

    def _bad_on_sample(row):
        raise ValueError("boom -- a persistently broken callback")

    with caplog.at_level(logging.WARNING, logger="bench_fixture_session"):
        with pytest.raises(_StopLoop):
            session.sample_response(duration_s=1e9, period_s=0.0, on_sample=_bad_on_sample)

    exception_records = [r for r in caplog.records if r.exc_info]
    assert len(exception_records) == 1, (
        "on_sample's traceback must be logged exactly once across the whole run, not per sample")
    summary_records = [r for r in caplog.records if "on_sample callback has now raised" in r.message]
    assert len(summary_records) >= 1, "a rate-limited summary must still make a persistent failure visible"


def test_real_sample_response_duty_by_zone_uses_the_firmware_zone_key():
    """DEFECT NEGATIVE TEST (consumer without producer): the firmware's
    /api/profile_exec zone objects use the key "zone", NOT "index" --
    firmware/KilnFW/App/drivers/http/dashboard_http.c:1220-1258's
    append_zone_status_json() (shared by /api/profile_exec and
    /api/control) emits ``{"zone":%u,...,"duty":%.3f,...}``. "index" is a
    DIFFERENT endpoint's config key (/api/zones, zones_http.c:4184/4210)
    and is never present here. Getting this wrong left duty_by_zone == {}
    on every real-hardware row -- this test feeds a response shaped
    field-for-field like the real firmware's (key names and value ranges
    copied from that snprintf format string, not invented) through the
    REAL BenchSession.sample_response and confirms duty_by_zone comes out
    populated with the right 0-1 fraction, not empty.
    """
    session = bfs.BenchSession(host="unit-test-host")
    session.status = lambda: {
        "channels": [{"channel": 0, "temp_c": 45.2, "valid": True}],
        "relays": [{"relay": 0, "on": True}],
        "safety_heating_enabled": True, "safety_relay_energized": True,
        "heat_block_sources_words": [0, 0],
    }
    # Field-for-field from dashboard_http.c:1238-1250's control_fields
    # snprintf: real key names, real value ranges. "duty" is z->duty, a
    # float already in [0,1] on the wire -- profile_executor.c:130 only
    # multiplies by 100 for ITS OWN separate percent-display conversion,
    # not this JSON field.
    session.exec_status = lambda: {
        "state": "running", "target_c": 50.0, "segment_index": 0,
        "zones": [
            {"zone": 0, "control_mode": 2, "actual_c": 45.23, "actual_valid": True,
             "duty": 0.842, "relay_on": True, "pid_p": 0.1, "pid_i": 0.05, "pid_d": 0.02,
             "pid_ff": 0.3, "cooling_limited": False, "faulted": False, "fault_guard": 0,
             "heat_blocked": False, "heat_blocked_sources": 0},
        ],
    }
    rows = session.sample_response(duration_s=0.0, period_s=0.0)
    assert rows[0]["duty_by_zone"] == {0: {"value": 0.842, "unit": "fraction_0_1"}}, (
        "duty_by_zone must be populated from the firmware's real \"zone\"/\"duty\" keys, "
        "not empty")


# ---------------------------------------------------------------------------
# Item 3: the trace cap keeps the NEWEST rows (deque), not the oldest.
# ---------------------------------------------------------------------------

def _many_rows_session_class(harness, n):
    """Shared by the cap tests below: a FakeSession subclass whose
    sample_response() produces ``n`` rows with distinct, ordered
    ``t_s``/``channels_c`` values (cheap, no sleeping) so a test can tell
    exactly which rows a cap kept. Everything else (ensure_pid_mode,
    get_zones, put_profile, start_profile, force_all_stop, status, ...) is
    inherited unchanged from the real FakeSession."""

    class ManyRowsSession(harness.FakeSession):
        def sample_response(self, duration_s, period_s=2.0, zone_index=0, on_sample=None):
            rows = []
            for i in range(n):
                row = {
                    "t_s": float(i), "channels_c": {0: float(i)}, "target_c": 50.0,
                    "segment_index": 0,
                    "duty_by_zone": {0: {"value": 1.0, "unit": "fraction_0_1"}},
                    "hottest_c": float(i),
                }
                rows.append(row)
                if on_sample is not None:
                    on_sample(row)
            return rows

    return ManyRowsSession()


def _run_tracking_only(harness, tmp_path, session):
    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path),
        "--skip-cooldown", "--skip-tune", "--skip-matrix", "--skip-backup"])
    report = harness._run(session, args)
    return [s for s in report.stages if s.name == "profile_tracking"][0]


def test_profile_tracking_trace_cap_evicts_oldest_and_flags_truncation(harness, tmp_path, monkeypatch):
    """Item 2 NEGATIVE TEST, driven through the REAL stage function (not a
    hand-built deque): with the module-level ``MAX_TRACE_ROWS`` shrunk to 3
    and a session producing 10 rows, the surviving ``history`` must be
    EXACTLY the newest 3 (t_s 7, 8, 9) -- not the oldest 3, and not all 10
    -- and ``history_truncated`` must be set. A regression where the cap
    stops appending instead of evicting (the bug this replaced) would keep
    [0, 1, 2] here instead; a regression where ``history_truncated`` is
    simply never set would still keep the right rows but fail the flag
    assertion below."""
    monkeypatch.setattr(harness, "MAX_TRACE_ROWS", 3)
    session = _many_rows_session_class(harness, n=10)
    tracking = _run_tracking_only(harness, tmp_path, session)
    history = tracking.detail["history"]
    assert [row["t_s"] for row in history] == [7.0, 8.0, 9.0], (
        "the cap must keep the NEWEST rows, not the oldest")
    assert tracking.detail.get("history_truncated") is True


def test_profile_tracking_trace_under_cap_is_not_flagged_truncated(harness, tmp_path, monkeypatch):
    """The counterpart, same mechanism: with the cap shrunk to 10 rows and a
    session producing only 5, nothing is evicted and ``history_truncated``
    must be ABSENT -- proving the flag is a real signal, not set
    unconditionally (which would make the assertion above vacuous the other
    way)."""
    monkeypatch.setattr(harness, "MAX_TRACE_ROWS", 10)
    session = _many_rows_session_class(harness, n=5)
    tracking = _run_tracking_only(harness, tmp_path, session)
    history = tracking.detail["history"]
    assert [row["t_s"] for row in history] == [0.0, 1.0, 2.0, 3.0, 4.0]
    assert "history_truncated" not in tracking.detail, (
        "a run under the cap must not be flagged truncated")


def test_profile_tracking_history_truncated_flag_absent_on_a_short_run(harness, tmp_path):
    """Same as above, against the REAL 200k cap (no monkeypatch) with
    FakeSession's ordinary synthetic run -- the everyday case."""
    args = harness.build_arg_parser().parse_args([
        "--dry-run", "--report-dir", str(tmp_path),
        "--skip-cooldown", "--skip-tune", "--skip-matrix", "--skip-backup"])
    report = harness._run(harness.FakeSession(), args)
    tracking = [s for s in report.stages if s.name == "profile_tracking"][0]
    assert "history_truncated" not in tracking.detail, (
        "a run far under the cap must not be flagged truncated")
    assert tracking.detail.get("history")


# ---------------------------------------------------------------------------
# pv.summarize_breach -- crossing detection, duty units, "starts above
# setpoint", multiple crossings, breached-channel-outside-zones, and the
# never-fabricate contract.
# ---------------------------------------------------------------------------

def _row(t_s, channels_c, target_c, duty_by_zone=None):
    return {"t_s": t_s, "channels_c": channels_c, "target_c": target_c,
            "duty_by_zone": duty_by_zone or {}, "segment_index": 0,
            "hottest_c": max(channels_c.values())}


def test_summarize_breach_requires_a_genuine_crossing():
    """NEGATIVE TEST for the bug the review found: a row where the FIRST
    sample is already at/above target must not be reported as a crossing
    just because ``v >= target`` is true on it -- there is no PREVIOUS
    below-target sample, so there is no crossing to attribute a duty
    reading to."""
    trace = [
        _row(0.0, {0: 55.0}, 50.0, {0: {"value": 0.5, "unit": "fraction_0_1"}}),
    ]
    stats = pv.summarize_breach(trace, [0])
    assert stats["0"]["setpoint_crossing"] == "started_above_setpoint"
    assert stats["0"]["duty_at_clamp"] is None, "must not fabricate a duty reading with no crossing"


def test_summarize_breach_detects_a_real_crossing_and_reads_duty_there():
    trace = [
        _row(0.0, {0: 40.0}, 50.0, {0: {"value": 1.0, "unit": "fraction_0_1"}}),
        _row(2.0, {0: 60.0}, 50.0, {0: {"value": 1.0, "unit": "fraction_0_1"}}),
    ]
    stats = pv.summarize_breach(trace, [0])
    assert stats["0"]["setpoint_crossing"] == "crossed"
    assert stats["0"]["duty_at_clamp"] == "full_power"


def test_summarize_breach_multiple_crossings_prefers_the_one_nearest_the_breach():
    """NEGATIVE TEST: an early crossing (duty pinned full) followed by a
    LATER crossing (duty already backed off) must report the LATER one --
    the one nearest whatever eventually breached -- not the first."""
    trace = [
        _row(0.0, {0: 40.0}, 50.0, {0: {"value": 1.0, "unit": "fraction_0_1"}}),
        _row(1.0, {0: 55.0}, 50.0, {0: {"value": 1.0, "unit": "fraction_0_1"}}),  # crossing #1
        _row(2.0, {0: 48.0}, 50.0, {0: {"value": 0.4, "unit": "fraction_0_1"}}),  # dips back below
        _row(3.0, {0: 60.0}, 50.0, {0: {"value": 0.4, "unit": "fraction_0_1"}}),  # crossing #2
    ]
    stats = pv.summarize_breach(trace, [0])
    assert stats["0"]["setpoint_crossing"] == "crossed"
    assert stats["0"]["duty_at_clamp"] == "not_at_clamp", (
        "must report the SECOND crossing's duty (0.4), not the first crossing's (1.0)")


def test_summarize_breach_duty_at_clamp_distinguishes_full_power_from_commanded_off():
    """NEGATIVE TEST for the conflation the review flagged: "duty pinned
    full and still climbing" and "duty commanded off and still climbing"
    are opposite diagnoses and must not collapse to the same value."""
    full_power_trace = [
        _row(0.0, {0: 40.0}, 50.0, {0: {"value": 1.0, "unit": "fraction_0_1"}}),
        _row(1.0, {0: 55.0}, 50.0, {0: {"value": 1.0, "unit": "fraction_0_1"}}),
    ]
    commanded_off_trace = [
        _row(0.0, {0: 40.0}, 50.0, {0: {"value": 0.0, "unit": "fraction_0_1"}}),
        _row(1.0, {0: 55.0}, 50.0, {0: {"value": 0.0, "unit": "fraction_0_1"}}),
    ]
    not_clamped_trace = [
        _row(0.0, {0: 40.0}, 50.0, {0: {"value": 0.5, "unit": "fraction_0_1"}}),
        _row(1.0, {0: 55.0}, 50.0, {0: {"value": 0.5, "unit": "fraction_0_1"}}),
    ]
    assert pv.summarize_breach(full_power_trace, [0])["0"]["duty_at_clamp"] == "full_power"
    assert pv.summarize_breach(commanded_off_trace, [0])["0"]["duty_at_clamp"] == "commanded_off"
    assert pv.summarize_breach(not_clamped_trace, [0])["0"]["duty_at_clamp"] == "not_at_clamp"
    assert (pv.summarize_breach(full_power_trace, [0])["0"]["duty_at_clamp"]
            != pv.summarize_breach(commanded_off_trace, [0])["0"]["duty_at_clamp"])


def test_summarize_breach_normalizes_percent_and_fraction_duty_the_same_way():
    """NEGATIVE TEST for the unit-ambiguity bug: 100.0 tagged
    percent_0_100 and 1.0 tagged fraction_0_1 are the SAME duty and must
    both report full_power -- comparing a percent value against the
    fraction thresholds (>=0.999) without normalizing would instead read
    100.0 as nowhere near clamped."""
    pct_trace = [
        _row(0.0, {0: 40.0}, 50.0, {0: {"value": 0.0, "unit": "percent_0_100"}}),
        _row(1.0, {0: 55.0}, 50.0, {0: {"value": 100.0, "unit": "percent_0_100"}}),
    ]
    frac_trace = [
        _row(0.0, {0: 40.0}, 50.0, {0: {"value": 0.0, "unit": "fraction_0_1"}}),
        _row(1.0, {0: 55.0}, 50.0, {0: {"value": 1.0, "unit": "fraction_0_1"}}),
    ]
    assert pv.summarize_breach(pct_trace, [0])["0"]["duty_at_clamp"] == "full_power"
    assert pv.summarize_breach(frac_trace, [0])["0"]["duty_at_clamp"] == "full_power"


def test_summarize_breach_duty_present_but_value_none_falls_back_honestly():
    """The review's "make z.get('duty', ...) fall back when the 'duty' key
    exists with a None value" -- exercised here one layer up: an entry
    whose ``value`` is ``None`` must normalize to ``None``, not to 0.0 or
    crash."""
    trace = [
        _row(0.0, {0: 40.0}, 50.0, {0: {"value": None, "unit": "fraction_0_1"}}),
        _row(1.0, {0: 55.0}, 50.0, {0: {"value": None, "unit": "fraction_0_1"}}),
    ]
    stats = pv.summarize_breach(trace, [0])
    assert stats["0"]["setpoint_crossing"] == "crossed"
    assert stats["0"]["duty_at_clamp"] is None, "a missing duty value must never be fabricated"


def test_summarize_breach_never_crossed_is_distinct_from_started_above():
    """A zone that stayed below its (rising) target the whole trace is a
    third, distinct state from both "crossed" and "started above" -- must
    not be silently folded into either."""
    trace = [
        _row(0.0, {0: 10.0}, 50.0, {0: {"value": 1.0, "unit": "fraction_0_1"}}),
        _row(1.0, {0: 20.0}, 50.0, {0: {"value": 1.0, "unit": "fraction_0_1"}}),
    ]
    stats = pv.summarize_breach(trace, [0])
    assert stats["0"]["setpoint_crossing"] == "never_crossed"
    assert stats["0"]["duty_at_clamp"] is None


def test_summarize_breach_too_short_trace_reports_none_not_fabricated():
    """NEGATIVE TEST: an empty trace, or one with no target_c at all, must
    report None throughout -- never a fabricated 0.0/False that would read
    as a real (and reassuring) measurement."""
    stats = pv.summarize_breach([], [0])
    assert stats["0"] == {
        "max_c": None, "peak_overshoot_c": None,
        "setpoint_crossing": None, "duty_at_clamp": None,
    }
    no_target_trace = [{"t_s": 0.0, "channels_c": {0: 40.0}, "target_c": None,
                        "duty_by_zone": {}, "hottest_c": 40.0}]
    stats2 = pv.summarize_breach(no_target_trace, [0])
    assert stats2["0"]["max_c"] == 40.0  # max_c does not need target_c
    assert stats2["0"]["setpoint_crossing"] is None
    assert stats2["0"]["peak_overshoot_c"] is None


def test_summarize_breach_includes_the_breached_channel_even_outside_zones():
    """NEGATIVE TEST for item 4's last bullet: the ceiling trips on the
    hottest of ALL valid channels, not just --zones. A breach on channel 4
    while tracking only zones [0, 1] must still show up in breach_stats."""
    trace = [
        _row(0.0, {0: 40.0, 1: 41.0, 4: 79.0}, 50.0,
             {0: {"value": 0.5, "unit": "fraction_0_1"}}),
        _row(1.0, {0: 45.0, 1: 42.0, 4: 80.1}, 50.0,
             {0: {"value": 0.5, "unit": "fraction_0_1"}}),
    ]
    stats = pv.summarize_breach(trace, [0, 1], breach_channel=4, breach_hottest_c=80.1)
    assert "4" in stats, "the breached channel must be present even though it is outside --zones"
    assert stats["4"]["max_c"] == 80.1
    assert stats["breach"] == {"channel": 4, "hottest_c": 80.1}


def test_summarize_breach_channel_already_in_zones_is_not_duplicated():
    trace = [_row(0.0, {0: 79.0}, 50.0)]
    stats = pv.summarize_breach(trace, [0], breach_channel=0, breach_hottest_c=79.0)
    assert set(stats.keys()) == {"0", "breach"}


# ---------------------------------------------------------------------------
# Aborted.partial_report / _run()'s fallback.
# ---------------------------------------------------------------------------

def test_aborted_partial_report_is_used_by_run_instead_of_an_empty_stub(harness, tmp_path):
    """NEGATIVE TEST directly against the Aborted/_run() mechanism (not
    routed through stage_profile_tracking): a stage that raises
    ``Aborted(msg, partial_report=...)`` must have THAT report land in
    ``RunReport.stages``, not a freshly built empty one."""
    carried = pv.StageReport(
        name="tune", passed=False, error="synthetic abort",
        checks=[pv.CheckResult(name="probe", passed=False, actual=1, threshold=0)],
        detail={"marker": "carried-through"})

    def _stage_that_aborts_with_a_report(session, args):
        raise harness.Aborted("synthetic abort", partial_report=carried)

    real_stages = harness.STAGES
    harness.STAGES = [("tune", _stage_that_aborts_with_a_report)]
    try:
        args = harness.build_arg_parser().parse_args(["--dry-run", "--report-dir", str(tmp_path)])
        report = harness._run(harness.FakeSession(), args)
    finally:
        harness.STAGES = real_stages

    assert report.aborted is True
    tune_stage = [s for s in report.stages if s.name == "tune"][0]
    assert tune_stage.detail == {"marker": "carried-through"}
    assert tune_stage.checks and tune_stage.checks[0].name == "probe"


def test_aborted_without_partial_report_still_falls_back_cleanly(harness, tmp_path):
    """The counterpart: a plain ``Aborted("msg")`` with no partial_report
    (every other stage's ``_guard``/``_retrying`` raises this way) must
    still produce a usable, empty-but-present StageReport -- confirms the
    fallback branch itself was not broken by adding the other one."""
    def _stage_that_aborts_plain(session, args):
        raise harness.Aborted("plain abort, no report")

    real_stages = harness.STAGES
    harness.STAGES = [("tune", _stage_that_aborts_plain)]
    try:
        args = harness.build_arg_parser().parse_args(["--dry-run", "--report-dir", str(tmp_path)])
        report = harness._run(harness.FakeSession(), args)
    finally:
        harness.STAGES = real_stages

    assert report.aborted is True
    tune_stage = [s for s in report.stages if s.name == "tune"][0]
    assert tune_stage.passed is False
    assert tune_stage.detail == {}


def test_ceiling_breach_stops_a_later_stage_regardless_of_stage_ordering(harness, tmp_path):
    """Item 3 NEGATIVE TEST for the ordering-dependence half of the review
    finding: before the fix, a real ceiling breach RETURNED a failed
    StageReport from stage_profile_tracking instead of raising Aborted, so
    stopping the run depended entirely on 'tracking' happening to be the
    LAST entry in STAGES (control then fell through to the top-level
    ``for``/``if not stage_report.passed and args.stop_on_first_failure``
    -- and even that only stops the run if --stop-on-first-failure is set;
    otherwise a later stage runs on an over-ceiling kiln).

    Puts a dummy stage AFTER tracking in STAGES and confirms it never runs,
    with the DEFAULT (not --stop-on-first-failure) args -- i.e. this must
    hold unconditionally, the same way an Aborted-raising path already
    does for every other stage."""
    class BreachingSession(harness.FakeSession):
        def sample_response(self, duration_s, period_s=2.0, zone_index=0, on_sample=None):
            exc = harness.BenchSessionError(
                "live temperature 90.0 C breached the 80.0 C fixture ceiling during sampling")
            exc.channels_c = {0: 90.0}
            exc.hottest_c = 90.0
            raise exc

    later_stage_ran = []

    def _later_stage(session, args):
        later_stage_ran.append(True)
        return pv.StageReport(name="backup", passed=True)

    real_stages = harness.STAGES
    # Reuses the "backup" name (a real skip-dict key) for the dummy stage so
    # _run()'s ``skip[name]`` lookup does not need touching -- the point is
    # ORDER, not which stage.
    harness.STAGES = [("tracking", harness.stage_profile_tracking), ("backup", _later_stage)]
    try:
        args = harness.build_arg_parser().parse_args([
            "--dry-run", "--report-dir", str(tmp_path),
            "--skip-cooldown", "--skip-tune", "--skip-matrix"])
        report = harness._run(BreachingSession(), args)
    finally:
        harness.STAGES = real_stages

    assert report.aborted is True
    assert not later_stage_ran, "a later stage must not run after a genuine ceiling breach"
    assert not any(s.name == "backup" and s.passed for s in report.stages)


# ---------------------------------------------------------------------------
# Item 6: NaN must never reach the JSON output as a bare token.
# ---------------------------------------------------------------------------

def test_json_safe_replaces_nan_and_inf_with_none():
    doc = {"a": float("nan"), "b": [1.0, float("inf"), float("-inf")], "c": {"d": float("nan")}}
    safe = pv._json_safe(doc)
    assert safe == {"a": None, "b": [1.0, None, None], "c": {"d": None}}


def test_run_report_to_json_never_emits_a_bare_nan_token():
    """NEGATIVE TEST: a report whose trace contains ``float("nan")`` (a
    thermocouple fault mid-run, i.e. no valid channel that poll) must
    produce output with no bare ``NaN``/``Infinity`` token -- proving the
    output is strict JSON, not merely "readable by Python's own permissive
    json module"."""
    report = pv.RunReport(host="x", started_at="t0", dry_run=True, hard_max_temp_c=90.0)
    report.stages = [pv.StageReport(
        name="profile_tracking", passed=False,
        detail={"history": [{"t_s": 0.0, "hottest_c": float("nan")}]})]
    text = report.to_json()
    assert "NaN" not in text and "Infinity" not in text
    doc = json.loads(text)
    assert doc["stages"][0]["detail"]["history"][0]["hottest_c"] is None
