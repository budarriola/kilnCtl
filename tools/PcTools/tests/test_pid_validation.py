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

        def sample_response(self, duration_s, period_s=2.0, zone_index=0):
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
# firmware/KilnFW/App/drivers/profiles_http.h:29, enforced by the "name too
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
