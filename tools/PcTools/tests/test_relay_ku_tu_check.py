"""Tests for kilnctrl.relay_ku_tu_check.

Fixtures under tests/fixtures/relay_z0/ are the real zone-0, 45C,
Tyreus-Luyben relay-feedback autotune capture that surfaced the Ku/Tu
discrepancy this module was written to investigate (autotune_get_status +
thermo_read pollers, 20s cadence, 350 lines each).

Every check below has a negative-test companion proving it can actually go
red, per repo policy.
"""
from __future__ import annotations

import math
import os

import pytest

from kilnctrl import relay_ku_tu_check as rk

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures", "relay_z0")
STATUS_PATH = os.path.join(FIXTURES, "relay_z0_status.jsonl")
THERMO_PATH = os.path.join(FIXTURES, "relay_z0_thermo.jsonl")


# ---------------------------------------------------------------------------
# Line parsing
# ---------------------------------------------------------------------------

def test_parse_status_line_extracts_state_and_duty():
    text = ("state=relay_cycling method=1 zone=0 elapsed=63s samples=0 actual=42.51C(valid) "
            "duty=0.85 model_valid=False model_settled=False "
            "proposed_gains=AutotuneGains(kp=0.0, ki=0.0, kd=0.0, rule=0) relay_valid=False")
    parsed = rk.parse_status_line(text)
    assert parsed == ("relay_cycling", pytest.approx(0.85))


def test_parse_status_line_garbage_returns_none():
    """A thermo line fed to the status parser (files cross-wired) must be
    rejected, not silently misread as some bogus state."""
    assert rk.parse_status_line("CH0: 26.70 C (CJ 27.11 C)") is None
    assert rk.parse_status_line("") is None


def test_parse_thermo_line_extracts_all_channels():
    text = "CH0: 26.70 C (CJ 27.11 C)\nCH1: 26.74 C (CJ 27.38 C)\nCH2: 26.70 C (CJ 27.44 C)"
    parsed = rk.parse_thermo_line(text)
    assert parsed == {0: pytest.approx(26.70), 1: pytest.approx(26.74), 2: pytest.approx(26.70)}


def test_parse_thermo_line_missing_channel_is_absent_not_nan():
    parsed = rk.parse_thermo_line("CH0: 26.70 C (CJ 27.11 C)")
    assert set(parsed.keys()) == {0}


# ---------------------------------------------------------------------------
# Raw-trace cycle measurement against the real capture
# ---------------------------------------------------------------------------

def _load():
    return rk.load_relay_capture(STATUS_PATH, THERMO_PATH)


def test_load_relay_capture_finds_cycling_window():
    cap = _load()
    cycling = [r for r in cap.status if r.state == "relay_cycling"]
    assert len(cycling) > 10
    assert len(cap.thermo) > 100


def test_measure_relay_cycles_finds_five_cycles_matching_firmware_report():
    """The firmware reported relay_cycles_seen=5 relay_cycles_used=3 (it
    caps AUTOTUNE_RELAY_FIT_CYCLES); the raw trace itself, walked with the
    same crossing algorithm, yields 4 COMPLETE bounded cycles from 5
    upward crossings -- consistent with "5 cycles seen"."""
    cap = _load()
    meas = rk.measure_relay_cycles(cap, channel=0)
    assert len(meas.upward_crossings_s) == 5
    assert len(meas.cycles) == 4


def test_measure_relay_cycles_period_matches_firmware_tu_within_1_pct():
    """The independent raw-trace period must reproduce the firmware's
    reported relay_tu_s=334.3 to within measurement noise -- this is the
    core check that the firmware's Tu is not a measurement artifact."""
    cap = _load()
    meas = rk.measure_relay_cycles(cap, channel=0)
    assert meas.period_mean_s == pytest.approx(334.3, rel=0.01)
    # tight spread: firmware rejects above 20% CV (RELAY_PERIOD_SPREAD_MAX)
    assert meas.period_stdev_s / meas.period_mean_s < 0.02


def test_measure_relay_cycles_half_amplitude_matches_firmware_report():
    cap = _load()
    meas = rk.measure_relay_cycles(cap, channel=0)
    assert meas.half_amplitude_mean_c == pytest.approx(3.03, abs=0.05)


def test_measure_relay_cycles_rejects_flat_trace():
    """Negative-test proof: an all-flat thermo trace must raise, not
    fabricate zero-length cycles. Proves the tail_pp < 1.0 guard (mirroring
    the firmware's own) actually fires."""
    cap = _load()
    flat_thermo = [
        rk.ThermoRow(t=r.t, channels={0: 30.0} if 0 in r.channels else r.channels)
        for r in cap.thermo
    ]
    flat_cap = rk.RelayCapture(status=cap.status, thermo=flat_thermo)
    with pytest.raises(ValueError, match="never oscillated"):
        rk.measure_relay_cycles(flat_cap, channel=0)


def test_measure_relay_cycles_no_cycling_state_raises():
    cap = _load()
    no_cycling_status = [r for r in cap.status if r.state != "relay_cycling"]
    stripped = rk.RelayCapture(status=no_cycling_status, thermo=cap.thermo)
    with pytest.raises(ValueError, match="relay_cycling"):
        rk.measure_relay_cycles(stripped, channel=0)


# ---------------------------------------------------------------------------
# Firmware Ku formula
# ---------------------------------------------------------------------------

def test_relay_ku_matches_firmware_reported_value():
    """relay_amplitude_c=3.03 IS the half-amplitude already (pid_autotune.c
    computes ``a = 0.5f * pp_mean`` before this formula) -- passing it
    straight in must reproduce the firmware's reported relay_ku=0.19540,
    not some peak-to-peak-confused value."""
    ku = rk.relay_ku(d=0.350, a_half_c=3.03, h_c=2.00)
    assert ku == pytest.approx(0.19540, rel=0.01)


def test_relay_ku_matches_raw_trace_measurement():
    cap = _load()
    meas = rk.measure_relay_cycles(cap, channel=0)
    ku = rk.relay_ku(d=0.350, a_half_c=meas.half_amplitude_mean_c, h_c=2.00)
    assert ku == pytest.approx(0.1954, rel=0.02)


def test_relay_ku_rejects_amplitude_at_or_below_hysteresis():
    """Negative-test proof: a<=h (the case the task's background
    hypothesised might be silently producing a NaN/imaginary Ku) must raise
    a clean ValueError, mirroring the firmware's guard, never return a
    complex/NaN number."""
    with pytest.raises(ValueError, match="hysteresis"):
        rk.relay_ku(d=0.350, a_half_c=1.515, h_c=2.00)


def test_relay_ku_positive_and_finite_above_hysteresis():
    ku = rk.relay_ku(d=0.350, a_half_c=3.03, h_c=2.00)
    assert ku > 0
    assert math.isfinite(ku)


# ---------------------------------------------------------------------------
# Describing-function prediction from a FOPDT model
# ---------------------------------------------------------------------------

def test_predict_relay_oscillation_matches_hand_solved_z0_case():
    """Hand-solved (see relay_ku_tu_check.py module docstring / the
    investigation this module backs) for zone 0's identified plant
    (K=39.25, tau=263.8s, L=52.8s) and this run's relay (d=0.35, h=2.0):
    the hysteresis-correct describing-function solution is Tu ~ 287.7s,
    Ku ~ 0.149 -- much closer to the measured Tu=334.6s/Ku=0.195 than the
    classical h=0 shortcut (Tu ~ 196s), but not identical, which is the
    quantitative basis for "coupling/step-ID bias explains some but not
    all of the gap"."""
    model = rk.FopdtModel(k_dc=39.25, tau_s=263.8, dead_time_s=52.8)
    pred = rk.predict_relay_oscillation(model, relay_d=0.350, hysteresis_c=2.00)
    assert pred.tu_s == pytest.approx(287.7, rel=0.01)
    assert pred.ku == pytest.approx(0.149, rel=0.01)
    assert pred.half_amplitude_c == pytest.approx(3.60, rel=0.01)


def test_predict_relay_oscillation_hysteresis_free_relay_matches_classical_4l_regime():
    """Sanity check on the solver itself: with h=0 (ideal relay, no
    hysteresis), the phase condition reduces to the classical
    atan(w*tau) + w*L = pi/2... no: atan(w*tau)+w*L = pi (phase=-180deg)
    exactly, independent of amplitude. Confirms the solver's phase
    equation is really the h=0 shortcut when h=0, which is the basis for
    every ratio/comparison the investigation draws against it."""
    model = rk.FopdtModel(k_dc=39.25, tau_s=263.8, dead_time_s=52.8)
    pred = rk.predict_relay_oscillation(model, relay_d=0.350, hysteresis_c=0.0)
    w = pred.omega_rad_s
    phase_condition = math.atan(w * model.tau_s) + w * model.dead_time_s
    assert phase_condition == pytest.approx(math.pi, abs=1e-6)


def test_predict_relay_oscillation_zero_hysteresis_gives_shorter_tu_than_with_hysteresis():
    """Negative-test proof that the hysteresis phase term in the solver
    actually does something: turning it off (h=0) must yield the shorter,
    classical-shortcut-like Tu, not the same answer as h=2.0 -- if this
    assertion is flipped or removed the solver's hysteresis handling could
    silently regress to a no-op and no other test here would catch it."""
    model = rk.FopdtModel(k_dc=39.25, tau_s=263.8, dead_time_s=52.8)
    pred_h0 = rk.predict_relay_oscillation(model, relay_d=0.350, hysteresis_c=0.0)
    pred_h2 = rk.predict_relay_oscillation(model, relay_d=0.350, hysteresis_c=2.00)
    assert pred_h0.tu_s < pred_h2.tu_s


# ---------------------------------------------------------------------------
# Tyreus-Luyben conversion -- mirrors pid_autotune_tune_from_relay()'s
# AUTOTUNE_RULE_TYREUS_LUYBEN branch exactly.
# ---------------------------------------------------------------------------

def test_tyreus_luyben_gains_reproduce_firmware_reported_gains():
    """Firmware reported (from relay_ku=0.19540, relay_tu_s=334.3):
    kp=0.06106 ki=0.00008303 kd=3.24009. Feeding the firmware's own
    reported Ku/Tu through the same TL formula must reproduce those
    numbers, confirming the module's TL formula matches
    pid_autotune_tune_from_relay()'s kc=ku/3.2, ti=2.2*tu, td=tu/6.3."""
    gains = rk.tyreus_luyben_gains(ku=0.19540, tu_s=334.3)
    assert gains.kp == pytest.approx(0.06106, rel=1e-3)
    assert gains.ki == pytest.approx(8.303e-05, rel=1e-2)
    assert gains.kd == pytest.approx(3.24009, rel=1e-3)


def test_tyreus_luyben_gains_scale_with_ku_and_tu():
    """Negative-test proof: doubling Ku must double Kp (kc=ku/3.2 is
    linear in ku), and doubling Tu must roughly halve Ki (ti=2.2*tu is
    linear in tu, ki=kc/ti)."""
    base = rk.tyreus_luyben_gains(ku=0.2, tu_s=300.0)
    double_ku = rk.tyreus_luyben_gains(ku=0.4, tu_s=300.0)
    double_tu = rk.tyreus_luyben_gains(ku=0.2, tu_s=600.0)
    assert double_ku.kp == pytest.approx(2 * base.kp)
    assert double_tu.ki == pytest.approx(base.ki / 2, rel=1e-6)
