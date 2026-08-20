"""Scenario-vocabulary <-> wire-numeric mapping for the FAULT group
(PROTOCOL.md sec 5.6) and TC group's INJECT_FAULT (sec 5.2).

``firmware/SimFW/scenarios/*.yaml`` faults use human-readable ``type:``/
``target:`` strings (e.g. ``type: welded_ssr, target: relay:K1``);
``payloads.py``'s wire encoder needs ``fault_sched_fault_type_t``'s numeric
values (``firmware/SimFW/src/tasks/fault_sched.h``) and a numeric ``target``
(a zone index / ``tc_fault_channel_t`` / CT channel / ignored-for-SYSTEM,
per that same header's target-kind table). Nothing before this module did
that translation -- ``scenario.compile_faults()`` only carries the strings
through unchanged, and ``payloads._encode_fault_schedule`` fed a bare string
straight into ``struct.pack("<B", ...)``, which raises. This module is the
missing piece: `fault_type_to_id` / `parse_target` below.

The catalog table's *names* were taken from firmware/SimFW/scenarios/*.yaml's
real `type:` values (grepped 2026-08-20, 14 distinct names) plus every other
PLAN.md sec 7.1 fault fault_sched.h actually implements (so a scenario using
a name none of the 16 shipped files happen to use today still resolves).
Numeric ids mirror fault_sched.h's `fault_sched_fault_type_t` enum order
exactly -- see that header (and its mirror in
firmware/SimFW/tools/virtual_simfw/src/virtual_simfw.c's `fault_type_t`) for
the authoritative numbering; keep the three in sync.
"""

from __future__ import annotations

from typing import Tuple

TC_DISCONNECTED = 0
TC_NOISE = 1
TC_STUCK = 2
TC_DEAD_IC = 3
TC_FLAKY_SPI = 4
TC_SPURIOUS_FAULT_PIN = 5
TC_SHORTED = 6
TC_DRIFT = 7
TC_CJ_FAULT = 8
MAIN_SAFETY_DISAGREE = 9
WELDED_RELAY = 10
STUCK_OPEN_RELAY = 11
BROKEN_ELEMENT = 12
PARTIAL_ELEMENT = 13
HALF_WAVE_SSR = 14
PHASE_LOSS = 15
WELDED_K4_CURRENT_PERSIST = 16
ESTOP = 17
RUNAWAY_ZONE = 18
AMBIENT_SHIFT = 19
THERMAL_MASS_SURPRISE = 20
TC_LAG_STRESS = 21
DUT_POWER_CUT = 22

#: fault_sched.c's target_kind_of() (firmware/SimFW/src/tasks/fault_sched.c) --
#: which flavor of `target` each fault_type expects, so parse_target() knows
#: how to interpret a scenario's `target:` string.
_TARGET_KIND = {
    TC_DISCONNECTED: "tc", TC_NOISE: "tc", TC_STUCK: "tc", TC_DEAD_IC: "tc",
    TC_FLAKY_SPI: "tc", TC_SPURIOUS_FAULT_PIN: "tc", TC_SHORTED: "tc",
    TC_DRIFT: "tc", TC_CJ_FAULT: "tc",
    WELDED_RELAY: "zone", STUCK_OPEN_RELAY: "zone", BROKEN_ELEMENT: "zone",
    PARTIAL_ELEMENT: "zone", RUNAWAY_ZONE: "zone", THERMAL_MASS_SURPRISE: "zone",
    TC_LAG_STRESS: "zone",
    HALF_WAVE_SSR: "ct", PHASE_LOSS: "ct", WELDED_K4_CURRENT_PERSIST: "ct",
    ESTOP: "system", AMBIENT_SHIFT: "system", MAIN_SAFETY_DISAGREE: "system",
    DUT_POWER_CUT: "system",
}

#: scenario YAML `type:` name -> numeric fault_sched_fault_type_t. Includes
#: every name actually used in firmware/SimFW/scenarios/*.yaml (grepped
#: 2026-08-20) plus the PLAN.md sec 7.1 catalog's other documented names, so
#: a name absent from the 16 shipped scenarios still resolves if a new
#: scenario uses it.
FAULT_TYPE_NAMES = {
    "disconnected_tc": TC_DISCONNECTED,
    "tc_disconnected": TC_DISCONNECTED,
    "broken_intermittent_tc": TC_DISCONNECTED,  # PLAN.md 7.1: no separate type --
                                                 # "intermittent" comes from the
                                                 # scenario's own repeat/duration spec
    "flaky_noise_tc": TC_NOISE,
    "tc_noise": TC_NOISE,
    "stuck_tc": TC_STUCK,
    "tc_stuck": TC_STUCK,
    "tc_dead_ic": TC_DEAD_IC,
    "dead_ic": TC_DEAD_IC,
    "flaky_spi_tc_ic": TC_FLAKY_SPI,
    "tc_flaky_spi": TC_FLAKY_SPI,
    "spurious_fault_pin": TC_SPURIOUS_FAULT_PIN,
    "tc_spurious_fault_pin": TC_SPURIOUS_FAULT_PIN,
    "shorted_tc": TC_SHORTED,
    "tc_shorted": TC_SHORTED,
    "drifting_tc": TC_DRIFT,
    "tc_drift": TC_DRIFT,
    "cj_fault": TC_CJ_FAULT,
    "main_safety_disagree": MAIN_SAFETY_DISAGREE,
    "main_safety_skew": MAIN_SAFETY_DISAGREE,
    "welded_ssr": WELDED_RELAY,
    "welded_relay": WELDED_RELAY,
    "stuck_open_relay": STUCK_OPEN_RELAY,
    "broken_heater_coil": BROKEN_ELEMENT,
    "broken_element": BROKEN_ELEMENT,
    "partial_element_health": PARTIAL_ELEMENT,
    "partial_element": PARTIAL_ELEMENT,
    "half_wave_ssr": HALF_WAVE_SSR,
    "phase_loss": PHASE_LOSS,
    "welded_k4_current_persist": WELDED_K4_CURRENT_PERSIST,
    "estop_trip": ESTOP,
    "estop": ESTOP,
    "runaway_zone": RUNAWAY_ZONE,
    "ambient_shift": AMBIENT_SHIFT,
    "thermal_mass_surprise": THERMAL_MASS_SURPRISE,
    "tc_lag_stress": TC_LAG_STRESS,
    "dut_power_cut": DUT_POWER_CUT,
}

_RELAY_TO_ZONE = {"K1": 0, "K2": 1, "K3": 2}


class FaultCatalogError(ValueError):
    pass


def fault_type_to_id(name: str) -> int:
    """Scenario ``type:`` string -> numeric fault_sched_fault_type_t.
    Case-insensitive; raises :class:`FaultCatalogError` for an unknown name
    rather than silently defaulting to 0 (TC_DISCONNECTED), which would
    otherwise be a very easy typo to ship unnoticed."""
    if isinstance(name, int):
        return name
    key = str(name).strip().lower()
    if key not in FAULT_TYPE_NAMES:
        raise FaultCatalogError(
            f"unknown fault type {name!r} (known: {sorted(FAULT_TYPE_NAMES)})"
        )
    return FAULT_TYPE_NAMES[key]


def parse_target(fault_type_id: int, target: str) -> int:
    """Scenario ``target:`` string -> numeric wire `target`, validated
    against `fault_type_id`'s expected target kind (fault_sched.c's
    ``target_kind_of()``). Accepted forms: ``relay:K1``/``K2``/``K3``
    (-> zone 0/1/2, the fixed relay<->zone mapping sim_engine.c documents),
    ``zone:N``/``element:N`` (-> zone N), ``tc:N`` (-> TC channel N),
    ``tc:safety`` (-> TC channel 3), ``ct:N`` (-> CT channel N), and
    ``io:...``/anything else for a SYSTEM-kind fault (target is ignored on
    the wire for those types, per PROTOCOL.md sec 5.6, so any string is
    accepted and mapped to 0)."""
    kind = _TARGET_KIND.get(fault_type_id)
    if kind is None:
        raise FaultCatalogError(f"unknown fault_type id {fault_type_id}")

    target = str(target).strip()
    prefix, _, rest = target.partition(":")
    prefix = prefix.lower()

    if kind == "system":
        return 0  # ignored on the wire regardless of the string (PROTOCOL.md 5.6)

    if kind == "tc":
        if prefix != "tc":
            raise FaultCatalogError(f"fault_type {fault_type_id} needs a tc:N/tc:safety target, got {target!r}")
        if rest.strip().lower() == "safety":
            return 3
        return int(rest)

    if kind == "ct":
        if prefix != "ct":
            raise FaultCatalogError(f"fault_type {fault_type_id} needs a ct:N target, got {target!r}")
        return int(rest)

    if kind == "zone":
        if prefix == "relay":
            if rest not in _RELAY_TO_ZONE:
                raise FaultCatalogError(f"unknown relay {rest!r} in target {target!r} (expected K1/K2/K3)")
            return _RELAY_TO_ZONE[rest]
        if prefix in ("zone", "element"):
            return int(rest)
        raise FaultCatalogError(
            f"fault_type {fault_type_id} needs a relay:K1/zone:N/element:N target, got {target!r}"
        )

    raise FaultCatalogError(f"unhandled target kind {kind!r}")  # pragma: no cover


def resolve(fault_type: str, target: str) -> Tuple[int, int]:
    """Convenience: ``(fault_type_name, target_string) -> (type_id, target_id)``."""
    type_id = fault_type_to_id(fault_type)
    return type_id, parse_target(type_id, target)
