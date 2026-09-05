"""gate_fields.py -- the checked-in inventory of GATE FIELDS: config values
that make a feature REACHABLE (the code path runs at all), as distinct from
fields that merely TUNE a feature that is already reachable.

WHY THIS EXISTS, stated with both incidents that paid for it.

  (1) An ad hoc analysis pooled 28 HTTP captures taken at ``control_mode: 2``
      (plain PID -- ``pid_fuzzy_adjust()`` is never even called, see
      ``profile_executor.c``'s mode dispatch) with a single capture taken at
      ``control_mode: 3`` (PID_FUZZY), and reported a 37,008-sample
      conclusion about the fuzzy layer's membership bands whose REAL n
      (mode-3 samples) was 2178. See ``fuzzy_band_probe.py``'s docstring and
      MEMORY's "Fuzzy bands never leave the centre cell" note. The captures
      recorded ``control_mode`` in every sample; nothing pooling them
      checked it.

  (2) Separately, two campaigns ran for hours at ``control_mode: 2`` while
      the operator believed the fuzzy layer was active, because the
      preset/readback verification that ran checked ``fuzzy_strength_pct``
      (a TUNING field -- it only has any effect once the gate is open) and
      never checked ``control_mode`` (the GATE field that makes it
      reachable at all). ``zones_http_client.apply_zone_preset()`` already
      reads back and asserts every field a preset names
      (``_verify_against_preset``) -- so if a preset had pinned
      ``control_mode: 3`` explicitly, that machinery would have caught a
      failed write. The gap was authoring-time: a preset (or a hand-edited
      one) can set ``fuzzy_strength_pct`` without ever pinning
      ``control_mode``, and nothing said "this preset tunes a gated feature
      without opening its gate".

Both incidents share one shape: the information needed (the gate field's
value) was present the whole time, in the preset or in the capture, and
simply never checked because nothing said it needed to be. This module is
the single place that inventory now lives, so:

  * a preset author (or ``run_queue.py``'s preflight, see
    :func:`check_preset_gate_consistency`) can check BEFORE any kiln hour is
    spent whether a preset tunes a gated feature without opening its gate;
  * an analysis author pooling N captures (see
    ``capture_pool_provenance.py``) can check, generically, whether every
    capture in the pool agrees on a gate field's reachability, without
    hand-rolling a ``control_mode``-shaped check for every future gate.

WHAT COUNTS AS A GATE FIELD (and what does not). A gate field is a config
value that appears in an early-return, a dispatch, or a guard-enable check
that skips a WHOLE feature/subsystem for one of its values, as opposed to a
field that scales/tunes behaviour that is already running either way (e.g.
``pid_kp`` is never a gate -- there is no value of ``pid_kp`` that skips the
PID loop entirely). This inventory is deliberately conservative: it lists
fields confirmed by reading the dispatch/early-return in the firmware
source, not every ``*_enabled``-shaped name.

THIS LIST IS NOT EXHAUSTIVE. A 2026-09-04 pass through
``firmware/KilnFW/App/drivers`` found the four entries below with high
confidence (dispatch or early-return read directly). Likely candidates NOT
yet added because they were not confirmed by reading the gating code in
this pass: autotune enable/skip conditions, OTA/backup feature flags,
wifi_prov gating, adaptive-tune fields. Add an entry here, with a
file:line citation to the actual gating code, when one of those (or
anything else) is confirmed -- do not guess a condition from a field's name
alone (``fuzzy_strength_pct`` LOOKS like a tuning-only field and mostly is,
but see its own entry below for the one place it also acts as a gate)."""
from __future__ import annotations

import dataclasses
from typing import Any, Callable, Optional, Sequence


@dataclasses.dataclass(frozen=True)
class GateField:
    """One config value confirmed to gate a whole feature on/off.

    ``name``: the field itself, as it appears in a preset/config dict (a
        per-zone field unless ``top_level`` is True).
    ``feature``: operator-facing description of what it gates.
    ``reachable_when``: value -> True iff the feature is reachable at that
        value.
    ``reachable_value_desc``: human text for the condition (report/log use).
    ``tuning_fields``: fields that only have any effect once this gate is
        open -- setting one of these without opening the gate is the
        authoring mistake incident (2) above.
    ``capture_json_path``: dotted-ish description of where this field shows
        up per-sample in an HTTP-capture JSONL line's ``exec`` body, or
        ``None`` when it is config/commissioning-only and never appears in
        per-sample telemetry (so a pool-provenance check can only compare
        preset/config snapshots for it, not per-tick capture data).
    ``top_level``: True when the field lives at the top of a preset/config
        dict rather than per-zone.
    ``source``: file:line citation for the actual gating code.
    """

    name: str
    feature: str
    reachable_when: Callable[[Any], bool]
    reachable_value_desc: str
    tuning_fields: Sequence[str]
    capture_json_path: Optional[str]
    source: str
    top_level: bool = False


GATE_FIELDS: "tuple[GateField, ...]" = (
    GateField(
        name="control_mode",
        feature="fuzzy PID gain-scheduling layer (pid_fuzzy_adjust(), pid_fuzzy.c)",
        reachable_when=lambda v: v == 3,
        reachable_value_desc="== 3 (ZONE_CONTROL_MODE_PID_FUZZY)",
        tuning_fields=("fuzzy_strength_pct",),
        capture_json_path="exec.zones[].control_mode (also zones.zones[].control_mode "
                           "over GET /api/zones)",
        source="firmware/KilnFW/App/drivers/profile_executor.c:894-912 (mode dispatch); "
               "profile_executor_pid_tick.c:311-364 (pid_fuzzy_prepare_gains, only called "
               "for mode 3)",
    ),
    GateField(
        name="fuzzy_strength_pct",
        feature="fuzzy gain adjustment magnitude, once control_mode==3 has already opened "
                "the fuzzy layer",
        # A SECOND gate, nested inside the first: even under control_mode==3,
        # pid_fuzzy_adjust() degrades to the unmodified base gains when
        # strength is 0 -- so a capture/preset at mode 3, strength 0 is
        # "fuzzy code path entered" but "fuzzy math had zero effect", a
        # different (weaker) inertness than control_mode!=3 entirely. Not
        # currently surfaced per-sample independent of bd_kp_effective et
        # al., so capture_json_path is None -- the effect is only visible
        # by comparing bd_*_effective against the base gains
        # (bd_reachability_check.py's method), not by reading this field
        # back from a capture.
        reachable_when=lambda v: v not in (None, 0, 0.0),
        reachable_value_desc="> 0 (0 degrades pid_fuzzy_adjust() back to unmodified base "
                              "gains -- see profile_executor.c:904-912)",
        tuning_fields=(),
        capture_json_path=None,
        source="firmware/KilnFW/App/drivers/profile_executor.c:904-912",
    ),
    GateField(
        name="ct_installed",
        feature="current-transformer-dependent safety guards S3/S4/S9/S11/S14, and the "
                "commissioning requirement for ct_channel_map[0..2] + the three "
                "overcurrent_* params",
        reachable_when=lambda v: bool(v),
        reachable_value_desc="!= 0",
        tuning_fields=("ct_channel_map", "overcurrent_warn_a", "overcurrent_trip_a"),
        # Commissioning/config-only: not a per-sample telemetry field on
        # exec.zones[]. A pool-provenance check for this gate has to compare
        # config/readiness snapshots, not capture ticks.
        capture_json_path=None,
        source="firmware/SaftyFW/src/config_params.c:855; safety_core.c:1021 "
               "(cfg_rec.ct_installed == 0u); KilnFW mirror: "
               "firmware/KilnFW/App/drivers/readiness_http.h:100-109, "
               "readiness_http.c:516-539",
        top_level=True,
    ),
    GateField(
        name="ramp_assist_enabled",
        feature="ramp-stretch (slows a ramp so a lagging zone can keep up) and dwell-credit "
                "spend (early dwell entry)",
        reachable_when=lambda v: bool(v),
        reachable_value_desc="== true",
        tuning_fields=(),
        capture_json_path=None,
        source="firmware/KilnFW/App/drivers/profile_executor_ramp_assist.c:90,140-142,274 "
               "(both gated functions return immediately when disabled); called from "
               "profile_executor.c:540,656,821. Already tracked separately as an HTTP "
               "CAPABILITY (endpoint existence) by capability_preflight.py -- this entry "
               "is about the on/off VALUE, a different question from whether the endpoint "
               "exists at all.",
        top_level=True,
    ),
)


def find_gate(name: str) -> Optional[GateField]:
    for g in GATE_FIELDS:
        if g.name == name:
            return g
    return None


def _iter_preset_field_values(preset: dict, field: str, top_level: bool):
    """Yield every value ``preset`` gives ``field``: the single top-level
    value for a top-level field, or one value per zone (tagged with its
    zone index) for a per-zone field. Yields nothing when the field is
    absent everywhere it could appear."""
    if top_level:
        if field in preset:
            yield None, preset[field]
        return
    for zone in preset.get("zones", []):
        if not isinstance(zone, dict):
            continue
        if field in zone:
            yield zone.get("index"), zone[field]


def check_preset_gate_consistency(preset: dict) -> "list[str]":
    """The authoring-time check for incident (2) above: for every
    :data:`GATE_FIELDS` entry, if this preset sets one of that gate's
    ``tuning_fields`` (per zone) but never pins the gate field itself to a
    reachable value IN THAT SAME ZONE, return a human-readable problem
    string naming the zone. Returns ``[]`` when every gated tuning field
    this preset sets is backed by a reachable gate value, or when the
    preset sets no tuning fields at all (nothing to check).

    Deliberately does not flag a preset that sets neither the gate nor any
    tuning field for a feature -- that preset simply doesn't touch the
    feature, which is not this incident's shape."""
    problems: "list[str]" = []
    for gate in GATE_FIELDS:
        if not gate.tuning_fields:
            continue
        for zone in preset.get("zones", []):
            if not isinstance(zone, dict):
                continue
            zone_idx = zone.get("index")
            tuning_present = [f for f in gate.tuning_fields if f in zone]
            if not tuning_present:
                continue
            gate_value = zone.get(gate.name)
            if gate_value is None:
                problems.append(
                    f"zone {zone_idx}: preset sets {tuning_present} (tunes "
                    f"{gate.feature!r}) but never pins {gate.name!r} in this zone at all -- "
                    f"the feature is reachable only when {gate.name} {gate.reachable_value_desc}. "
                    f"Add {gate.name!r} to zone {zone_idx} explicitly."
                )
            elif not gate.reachable_when(gate_value):
                problems.append(
                    f"zone {zone_idx}: preset sets {tuning_present} (tunes "
                    f"{gate.feature!r}) but pins {gate.name}={gate_value!r} in this zone, "
                    f"which does NOT satisfy {gate.reachable_value_desc} -- {gate.feature} "
                    f"will be UNREACHABLE in this zone regardless of {tuning_present}. This is "
                    "the exact shape of the incident where two campaigns ran for hours "
                    "believing the fuzzy layer was active because fuzzy_strength_pct was "
                    "checked instead of control_mode."
                )
    return problems


def summarize_preset_gates(preset: dict) -> "dict[str, dict]":
    """Machine-readable snapshot of every :data:`GATE_FIELDS` value this
    preset pins, per zone (or once, for a top-level gate) -- meant to be
    embedded verbatim into a capture's header (see ``run_queue.py``'s meta
    line) so a capture answers "which features were REACHABLE during this
    run" from its own header, per this task's item 1, without a reader
    having to re-derive it from the raw per-tick telemetry the way
    ``fuzzy_band_probe.py``/``capture_pool_provenance.py`` do after the
    fact. Only records gates the preset actually sets; a gate this preset
    is silent on is omitted (silence is not a value).

    Shape: ``{gate_name: {"reachable_value_desc": ..., "top_level": bool,
    "values": {zone_index_or_null: {"value": ..., "reachable": bool}}}}``.
    """
    out: "dict[str, dict]" = {}
    for gate in GATE_FIELDS:
        values = dict(_iter_preset_field_values(preset, gate.name, gate.top_level))
        if not values:
            continue
        out[gate.name] = {
            "reachable_value_desc": gate.reachable_value_desc,
            "top_level": gate.top_level,
            "values": {
                ("top" if idx is None else idx): {
                    "value": val,
                    "reachable": bool(gate.reachable_when(val)),
                }
                for idx, val in values.items()
            },
        }
    return out
