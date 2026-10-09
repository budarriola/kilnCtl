"""Zone current-sweep MCP tools -- HTTP, not the UART link.

Exposes the three current-sweep endpoints in
firmware/KilnFW/App/drivers/http/zones_http.c
(POST .../current_sweep/start, GET .../current_sweep/status,
POST .../current_sweep/abort) through the sanctioned tool facade. Before
this module, these three routes had NO MCP tool at all -- CLAUDE.md's
standing rule is that anything touching the boards goes through the MCP
facade, not a raw HTTP snippet an operator or an agent pastes by hand, and
this sweep was the one config-area write path that had slipped through
that rule. Same "own module, plumbed through mcp_server.py's star-import
list" pattern mcp_server_ramp_assist.py/mcp_server_adaptive_tune.py use.
Host resolution mirrors those modules' own resolver: `_ota_resolve_host()`
from mcp_server_ota.py (imported locally, same as
mcp_server_config_presets.py's load_config_preset() does, to avoid a
circular import).

WHAT THE SWEEP PHYSICALLY DOES (see zones_current_sweep_http_client.py's
own module docstring for the wire-level detail, and
zones_current_sweep_task.c/zones_current_sweep_engine.c for the
implementation): for each configured zone in turn, it forces every OTHER
zone's relay(s) off, turns THIS zone's own relay(s) ON for
ZONE_SWEEP_SETTLE_MS (10000 ms), then samples the shared CT for
ZONE_SWEEP_SAMPLE_MS (4000 ms, about 8 safety-link polls averaged) before
moving on -- roughly 14 seconds per zone, one zone energized at a time,
never two at once. In summed-CT topology it also samples a fresh idle
baseline and subtracts it. On a successful measurement it calls
zone_normals_set(zone, normal_a), WRITING i_normal_a[zone] (wire ids
0x031A-0x031C) to the safety processor's persisted commissioning data, and
separately derives k_ct_v_per_a (the CT's volts-per-amp scale).

THE NOISE FLOOR, AND WHY "UNMEASURED" ON THIS BENCH IS CORRECT, NOT A
FAILURE: any zone whose measured current comes in below
ZONE_SWEEP_NORMAL_NOISE_FLOOR_A (0.045 A) is refused and reported as
unmeasured (summed_unmeasured_mask) rather than recorded -- the firmware
cannot distinguish a value that small from CT/ADC noise. This project's own
bench fixture (project_bench_is_a_4w_test_fixture) draws roughly 70 mA with
all three heaters on and roughly 23 mA per zone measured alone -- BOTH
numbers are below the 45 mA floor. Running this sweep against the current
bench hardware is therefore STRUCTURALLY GUARANTEED to report every zone
unmeasured: that is the correct, expected outcome on this fixture, not a
bug and not something to retry with different timing. It will behave
differently -- and actually record i_normal_a -- against a kiln with real
heating-element current draw.

WHY START IS CONFIRM-GATED: this call, if accepted, ENERGIZES HEATER
RELAYS -- the same class of live-board action debug_program()/
ramp_assist_set_enabled()/adaptive_tune_set_enabled() gate on `confirm=True`
with no default that becomes a write. status()/abort() are read-only or
unconditionally safe (abort always de-energizes) and carry no such gate.

WHY START ALSO RUNS A PREFLIGHT BEFORE EVER POSTING: the firmware's own
zone_sweep_check_refusal() already atomically refuses (already running, no
hardware, invalid config, no zones, a firing profile or autotune running,
safety link down, a trip latched, a relay already on, CT topology unknown)
-- this tool does not duplicate any of that logic (see capability_preflight.py's
own docstring on why a second copy of a firmware rule is a drift hazard to
avoid). What the firmware's gate does NOT cover is a board carrying an
UNACKNOWLEDGED CRASH REPORT, or the other three items the readiness firing
interlock blocks on (recovery_mode/safety_trip/estop_verified) -- those are
reasons to refuse ANY unattended hardware action regardless of what it
needs, the same standard capability_preflight.PreflightReport.ok already
enforces for a config-preset run. zone_current_sweep_start() calls
capability_preflight.get_board_info() and refuses on the same conditions,
rather than writing a third copy of that check."""
from __future__ import annotations

from typing import Optional

from . import capability_preflight
from . import zones_current_sweep_http_client as sweep_http

from . import mcp_server_core as _core


def _zone_sweep_resolve_host(host: Optional[str]) -> str:
    """Same resolution order as mcp_server_ramp_assist.py's
    ``_ramp_assist_resolve_host()``/mcp_server_ota.py's
    ``_ota_resolve_host()``: explicit ``host`` always wins; otherwise prefer
    the board's current Wi-Fi station IP, else the fallback-AP address."""
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py
    return _ota_resolve_host(host)


@_core._tool()
def zone_current_sweep_start(confirm: bool = False, host: Optional[str] = None) -> str:
    """Start the per-zone current-measurement sweep. THIS ENERGIZES HEATER
    RELAYS, one zone at a time, for roughly 14 seconds per zone (10 s settle
    + 4 s sample -- see this module's own docstring for the full sequence).
    On success it WRITES i_normal_a[zone] to the safety processor for every
    zone it could measure, and derives the CT's volts-per-amp scale.

    REFUSED UNLESS `confirm=True` IS PASSED EXPLICITLY -- same idiom
    debug_program()/ramp_assist_set_enabled()/adaptive_tune_set_enabled()
    use for a live-board action of this consequence; there is no default
    that becomes a write.

    BEFORE ENERGIZING ANYTHING, this tool runs a capability_preflight
    (capability_preflight.get_board_info()) and refuses if the board is
    unreachable, carries an UNACKNOWLEDGED CRASH REPORT, or has any of the
    readiness firing interlock's four blocking items red (recovery_mode,
    safety_trip, estop_verified -- safety_trip here is belt-and-suspenders,
    since the firmware's own gate below also refuses on a latched trip).
    This mirrors the exact standard a config-preset run is already held to
    (capability_preflight_check) rather than inventing a second version of
    that check.

    THE FIRMWARE ALSO REFUSES ON ITS OWN (zone_sweep_check_refusal()),
    atomically, on top of the above: a sweep already running, board I/O
    unavailable this boot, zone config that did not load cleanly, no zones
    configured, A FIRING PROFILE OR AUTOTUNE RUNNING, the safety link down,
    a trip latched, a relay already on, or CT topology unknown. This tool
    does not re-implement any of those checks -- it surfaces the firmware's
    own refusal reason string verbatim if one of them fires. **A firing in
    progress is refused by this firmware-side gate**, not by this tool.

    NOISE FLOOR / "UNMEASURED" IS EXPECTED ON THIS BENCH: any zone whose
    measured current is below 0.045 A is refused and reported unmeasured,
    never recorded, because the firmware cannot distinguish it from CT/ADC
    noise. This project's ~4 W bench fixture draws roughly 70 mA with all
    three heaters on and roughly 23 mA per zone alone -- BOTH below that
    floor -- so running this sweep on the current bench hardware is
    STRUCTURALLY GUARANTEED to report every zone unmeasured. That is the
    correct, expected result here, not a failure to retry with different
    timing or a different host. Call zone_current_sweep_status() to see the
    per-zone outcome and `summed_unmeasured_mask` once it settles into
    "done".

    Returns immediately once the board accepts or refuses the start; the
    sweep itself (if accepted) keeps running in the background on the
    board -- poll `zone_current_sweep_status()` for progress, or call
    `zone_current_sweep_abort()` to stop it early.

    `host`: board IP or hostname. Defaults to the board's current station IP
    if connected, else the fallback-AP address 192.168.4.1.
    """
    if confirm is not True:
        return (
            "error: current sweep refused without confirm=True -- this ENERGIZES HEATER "
            "RELAYS one zone at a time (see this tool's own docstring for the sequence, "
            "timing, and what it writes)"
        )
    resolved = _zone_sweep_resolve_host(host)
    board = capability_preflight.get_board_info(resolved)
    if not board.reachable:
        return f"error: board unreachable, refusing to start a sweep (host={resolved}): {board.error}"
    if board.crash_unacknowledged:
        return (
            f"error: board carries an UNACKNOWLEDGED CRASH REPORT ({board.crash_summary}) -- "
            "refusing to start a sweep. Review and acknowledge via GET/POST /api/crash_report "
            f"before energizing relays (host={resolved})."
        )
    if board.readiness_blocked:
        names = ", ".join(f"{label} ({key}): {detail}" for key, label, detail in board.readiness_blocked)
        return f"error: readiness firing interlock blocks on {names} -- refusing to start a sweep (host={resolved})"
    try:
        result = sweep_http.start(resolved)
    except sweep_http.ZoneSweepHttpError as exc:
        return f"error: {exc} (host={resolved})"
    if not result.get("ok"):
        return f"refused by firmware: {result.get('reason', '(no reason given)')} (host={resolved})"
    return (
        f"ok - current sweep started (host={resolved}); energizing relays one zone at a time, "
        "roughly 5s/zone -- poll zone_current_sweep_status() for progress"
    )


@_core._tool()
def zone_current_sweep_status(host: Optional[str] = None) -> str:
    """Read the current-measurement sweep's live status. Read-only, safe to
    call at any time including while a sweep is running -- does not touch
    relays or config.

    Reports state (idle/running/done/aborted/failed), which zone is/was
    being energized and how many of the total are done, the derived
    CT-channel-map/k_ct/nameplate-mismatch advisories (each with its own
    independent reason string), `i_normal_pushed_mask` (zones whose
    measured baseline current was actually written to the safety
    processor), and `summed_unmeasured_mask` (zones that could not be
    measured -- see zone_current_sweep_start()'s own docstring for why this
    is EXPECTED, not a failure, on this project's ~4 W bench fixture: both
    the all-heaters-on (~70 mA) and per-zone-alone (~23 mA) currents
    measured on it sit below the firmware's 0.045 A noise floor).

    `host`: board IP or hostname, same resolution as
    zone_current_sweep_start().
    """
    resolved = _zone_sweep_resolve_host(host)
    try:
        st = sweep_http.status(resolved)
    except sweep_http.ZoneSweepHttpError as exc:
        return f"error: {exc} (host={resolved})"
    lines = [
        f"state={st.get('state')} zone_index={st.get('zone_index')} "
        f"zones_done={st.get('zones_done')}/{st.get('zones_total')} reason={st.get('reason')!r}",
        f"i_normal_pushed_mask={st.get('i_normal_pushed_mask')} "
        f"summed_unmeasured_mask={st.get('summed_unmeasured_mask')}",
        f"ct_map_derived_mask={st.get('ct_map_derived_mask')} ct_map_reason={st.get('ct_map_reason')!r}",
        f"k_ct_derived_mask={st.get('k_ct_derived_mask')} k_ct_reason={st.get('k_ct_reason')!r}",
        f"nameplate_mismatch_mask={st.get('nameplate_mismatch_mask')} "
        f"nameplate_reason={st.get('nameplate_reason')!r}",
    ]
    if st.get("summed_unmeasured_mask"):
        lines.append(
            "note: unmeasured zones are EXPECTED on this project's ~4 W bench fixture -- "
            "its currents (~70 mA all zones, ~23 mA/zone alone) sit below the firmware's "
            "0.045 A noise floor; this is not a failure to retry."
        )
    lines.append(f"(host={resolved})")
    return "\n".join(lines)


@_core._tool()
def zone_current_sweep_abort(host: Optional[str] = None) -> str:
    """Abort the current-measurement sweep, immediately de-energizing
    whatever zone it currently has on. Safe to call at any time, including
    when no sweep is running -- the firmware's abort handler is
    unconditional and idempotent, not an error in either case.

    `host`: board IP or hostname, same resolution as
    zone_current_sweep_start().
    """
    resolved = _zone_sweep_resolve_host(host)
    try:
        result = sweep_http.abort(resolved)
    except sweep_http.ZoneSweepHttpError as exc:
        return f"error: {exc} (host={resolved})"
    if not result.get("ok"):
        return f"error: board reported failure: {result} (host={resolved})"
    return f"ok - current sweep aborted, relays de-energized (host={resolved})"

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
