"""Spare-relay aux outputs (docs/SPARE_RELAY_ONOFF_PLAN.md sec 8, WP-7).

Read-only:  control_get_aux_outputs
Mutating:   control_set_aux_output, control_set_aux_manual, control_convert_onoff_zone_to_aux
Every mutating tool: refuses unless ``confirm is True`` EXACTLY (a dry run
otherwise, no POST), refuses mid-run (precheck plus the firmware's
system_mode_gate 409), and fails loud when the read-back disagrees with what
was written.

Also here: control_convert_onoff_zone_to_aux, the confirm-gated one-shot that moves an
ON_OFF zone's relay into an aux binding and rewrites every stored profile's rules for
that zone to the aux target (plan sec 10). The all-or-nothing work is done by the
firmware (POST /api/zones field ``move_zone_to_aux=Z``); this tool prechecks, calls it
once and verifies the zone, the aux binding and the profiles by read-back.
"""
from __future__ import annotations

import math
from typing import Optional

from . import aux_http_client as ahc
from . import mcp_server_core as _core
from . import zones_http_client
from .io_expander import IoQueryError

_HYST_TOLERANCE = 0.01  # GET prints hyst_c with %.2f


def _resolve_host(host: Optional[str]) -> str:
    from .mcp_server_ota import _ota_resolve_host  # local import: circular, same convention as mcp_server_control
    return _ota_resolve_host(host)


def _running_reason() -> Optional[str]:
    from .mcp_server_control import _profile_or_autotune_running_reason
    return _profile_or_autotune_running_reason()


def _is_int(v) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _fmt_entry(e: dict) -> str:
    tz = e.get("tc_zone")
    return (f"relay {e.get('relay')}: {'ENABLED' if e.get('enabled') else 'disabled'}"
            f"{' CONFLICTED' if e.get('conflicted') else ''}, tc_zone={'none' if tz == -1 else tz}, "
            f"hyst_c={e.get('hyst_c')}, min_on_s={e.get('min_on_s')}, min_off_s={e.get('min_off_s')}")


def _gate_or_error(exc: "ahc.AuxHttpError", what: str, resolved: str) -> str:
    if exc.status == 409 and zones_http_client.is_system_mode_gate_refusal(exc.detail):
        return (f"refused: system_mode_gate refused this write (HTTP 409): {exc.detail} -- a firing or "
                f"autotune run started after this tool's own precheck (host={resolved})")
    if exc.status in (400, 403, 409, 503):
        return f"refused: {what} refused (HTTP {exc.status}): {exc.detail} (host={resolved})"
    return f"error: {what} failed (host={resolved}): {exc}"


@_core._tool()
def control_get_aux_outputs(host: Optional[str] = None) -> str:
    """READ-ONLY. GET /api/aux_outputs: the four spare-relay aux bindings
    (enabled, TC zone, hysteresis, min on/off), the quarantine flag, and the
    zone-claimed relay mask that conflicts with them."""
    resolved = _resolve_host(host)
    try:
        snap = ahc.get_aux_outputs(resolved)
    except ahc.AuxHttpError as exc:
        return f"error: GET /api/aux_outputs failed (host={resolved}): {exc}"
    lines = [f"aux outputs (host={resolved}): quarantined={snap.get('quarantined')}, "
             f"enabled_mask={snap.get('enabled_mask')}, conflict_mask={snap.get('conflict_mask')}, "
             f"zones_relay_mask={snap.get('zones_relay_mask')}"]
    lines += [_fmt_entry(e) for e in snap["relays"]]
    return "\n".join(lines)


@_core._tool()
def control_set_aux_output(
    relay: int,
    enabled: bool,
    tc_zone: Optional[int] = None,
    hyst_c: Optional[float] = None,
    min_on_s: Optional[int] = None,
    min_off_s: Optional[int] = None,
    confirm: bool = False,
    host: Optional[str] = None,
) -> str:
    """Bind (enabled=True) or unbind (enabled=False) ONE spare relay as a named
    aux output via POST /api/aux_outputs (GET-merge-POST: the firmware preserves
    every field this call omits, so unspecified tc_zone/hyst_c/min_on_s/min_off_s
    keep their stored values). tc_zone: -1 = none, else a thermocouple zone.

    Refused: bad arguments; unless `confirm is True` exactly (dry run, no POST);
    while a profile or autotune run is active (precheck, plus the firmware's
    system_mode_gate 409); a quarantined aux store; binding a relay that a zone's
    relay_mask uses (a heating or ON_OFF zone owns it). After a confirmed write it
    re-reads GET /api/aux_outputs and FAILS LOUD unless the target entry matches
    and no OTHER relay's entry changed. Never use load_config_preset() for this.
    Uses the http_auth admin-session seam; never prints a credential."""
    if not _is_int(relay) or not 1 <= relay <= ahc.AUX_RELAY_COUNT:
        return f"refused: relay={relay!r} must be an integer 1..{ahc.AUX_RELAY_COUNT}"
    if not isinstance(enabled, bool):
        return f"refused: enabled={enabled!r} must be a bool"
    if tc_zone is not None and (not _is_int(tc_zone) or tc_zone < -1):
        return f"refused: tc_zone={tc_zone!r} must be an integer >= -1 (-1 = none)"
    if hyst_c is not None and (isinstance(hyst_c, bool) or not isinstance(hyst_c, (int, float))
                               or not math.isfinite(hyst_c)):
        return f"refused: hyst_c={hyst_c!r} is not a finite number"
    for name, v in (("min_on_s", min_on_s), ("min_off_s", min_off_s)):
        if v is not None and (not _is_int(v) or v < 0):
            return f"refused: {name}={v!r} must be a non-negative integer"

    resolved = _resolve_host(host)
    running = _running_reason()
    if running is not None:
        return f"refused: {running} -- aux outputs are not reconfigured mid-run (host={resolved})"

    try:
        before = ahc.get_aux_outputs(resolved)
    except ahc.AuxHttpError as exc:
        return f"error: GET /api/aux_outputs failed (host={resolved}): {exc}"
    if before.get("quarantined"):
        return f"refused: the aux store is quarantined (newer-firmware data) -- not overwriting it (host={resolved})"
    cur = ahc.aux_entry(before, relay)
    if cur is None:
        return f"refused: board reports no aux entry for relay {relay} (host={resolved})"
    zmask = before.get("zones_relay_mask")
    if enabled and isinstance(zmask, int) and zmask & (1 << (relay - 1)):
        return (f"refused: relay {relay} is used by a zone's relay_mask (zones_relay_mask={zmask}) -- "
                f"remove it from the zone first (host={resolved})")

    if confirm is not True:
        return (f"DRY RUN (pass confirm=True, exactly, to actually write) -- would set relay {relay} "
                f"enabled={enabled}, tc_zone={tc_zone}, hyst_c={hyst_c}, min_on_s={min_on_s}, "
                f"min_off_s={min_off_s} (None = keep stored). Current: {_fmt_entry(cur)} (host={resolved})")

    try:
        ok = ahc.post_aux_output(resolved, relay, enabled, tc_zone, hyst_c, min_on_s, min_off_s)
    except ahc.AuxHttpError as exc:
        return _gate_or_error(exc, "POST /api/aux_outputs", resolved)
    if not ok:
        return f"refused: POST /api/aux_outputs did not answer ok (host={resolved})"

    try:
        after = ahc.get_aux_outputs(resolved)
    except ahc.AuxHttpError as exc:
        return (f"error: POST returned ok, but the confirming re-fetch of GET /api/aux_outputs failed "
                f"(host={resolved}): {exc} -- state UNKNOWN, re-check before trusting this")
    got = ahc.aux_entry(after, relay)
    if got is None:
        return f"FAILED: relay {relay} missing from the re-fetched aux response (host={resolved})"

    want = {"enabled": enabled}
    if tc_zone is not None:
        want["tc_zone"] = tc_zone
    for k, v in (("min_on_s", min_on_s), ("min_off_s", min_off_s)):
        if v is not None:
            want[k] = v
    bad = [f"{k}: wanted {v!r}, board reports {got.get(k)!r}" for k, v in want.items() if got.get(k) != v]
    if hyst_c is not None:
        g = got.get("hyst_c")
        if not isinstance(g, (int, float)) or abs(float(g) - float(hyst_c)) > _HYST_TOLERANCE:
            bad.append(f"hyst_c: wanted {hyst_c!r}, board reports {g!r}")
    if bad:
        return (f"FAILED: POST returned ok, but read-back does not confirm it landed -- {'; '.join(bad)} "
                f"(host={resolved}). Do not trust this as applied.")
    # Omitted fields must have been preserved, and no other relay may have moved.
    kept = [k for k in ("tc_zone", "hyst_c", "min_on_s", "min_off_s") if k not in want
            and not (k == "hyst_c" and hyst_c is not None)]
    drift = [f"relay {relay} {k}: {cur.get(k)!r} -> {got.get(k)!r}" for k in kept if got.get(k) != cur.get(k)]
    for r in range(1, ahc.AUX_RELAY_COUNT + 1):
        if r != relay and ahc.aux_entry(before, r) != ahc.aux_entry(after, r):
            drift.append(f"relay {r} entry changed")
    if drift:
        return (f"FAILED: relay {relay} landed correctly, but other field(s) changed unexpectedly -- "
                f"{'; '.join(drift)} (host={resolved}). Investigate before trusting this board's config.")
    return f"ok - {_fmt_entry(got)} (confirmed by read-back; host={resolved})"


@_core._tool()
def control_set_aux_manual(relay: int, on: bool, confirm: bool = False, host: Optional[str] = None) -> str:
    """Manually switch an ENABLED aux relay on/off while IDLE via POST
    /api/aux_outputs/manual (admin only). Refused: bad arguments; unless
    `confirm is True` exactly (dry run); while a profile/autotune run is active
    (precheck, plus the firmware's 409, reported as a refusal); a relay that is not
    an enabled, unconflicted aux output. At firing start the profile rule takes
    over and at run end aux goes OFF (plan sec 14 item 12), so this is never a
    hold. After the write it reads the relay shadow (io_read) and FAILS LOUD unless
    it equals `on`. Never prints a credential."""
    if not _is_int(relay) or not 1 <= relay <= ahc.AUX_RELAY_COUNT:
        return f"refused: relay={relay!r} must be an integer 1..{ahc.AUX_RELAY_COUNT}"
    if not isinstance(on, bool):
        return f"refused: on={on!r} must be a bool"

    resolved = _resolve_host(host)
    running = _running_reason()
    if running is not None:
        return f"refused: {running} -- aux manual control is idle-only (host={resolved})"

    try:
        snap = ahc.get_aux_outputs(resolved)
    except ahc.AuxHttpError as exc:
        return f"error: GET /api/aux_outputs failed (host={resolved}): {exc}"
    ent = ahc.aux_entry(snap, relay)
    if ent is None or not ent.get("enabled"):
        return f"refused: relay {relay} is not an enabled aux output (host={resolved})"
    if ent.get("conflicted"):
        return f"refused: relay {relay}'s aux binding is conflicted with a zone relay_mask (host={resolved})"

    if confirm is not True:
        return (f"DRY RUN (pass confirm=True, exactly, to actually write) -- would switch aux relay "
                f"{relay} {'ON' if on else 'OFF'} (host={resolved})")

    try:
        ok = ahc.post_aux_manual(resolved, relay, on)
    except ahc.AuxHttpError as exc:
        return _gate_or_error(exc, "POST /api/aux_outputs/manual", resolved)
    if not ok:
        return f"refused: POST /api/aux_outputs/manual did not answer ok (host={resolved})"

    try:
        state = _srv._io.read()
    except IoQueryError as exc:
        return (f"error: POST returned ok, but the confirming io read failed (host={resolved}): {exc} "
                f"-- state UNKNOWN, re-check before trusting this")
    if state.relay(relay) != on:
        return (f"FAILED: POST returned ok, but the relay shadow reads {'ON' if state.relay(relay) else 'OFF'} "
                f"for relay {relay}, wanted {'ON' if on else 'OFF'} (host={resolved}). Do not trust this.")
    return f"ok - aux relay {relay} {'ON' if on else 'OFF'} (confirmed by relay shadow read-back; host={resolved})"


def _zone_entry(zones_json: dict, zone: int) -> Optional[dict]:
    zs = zones_json.get("zones") if isinstance(zones_json, dict) else None
    if isinstance(zs, list) and 0 <= zone < len(zs) and isinstance(zs[zone], dict):
        return zs[zone]
    return None


def _count_rules(rules_by_profile: dict, target: int) -> int:
    return sum(1 for rules in rules_by_profile.values() for r in rules if r.get("zone") == target)


@_core._tool()
def control_convert_onoff_zone_to_aux(zone: int, confirm: bool = False, host: Optional[str] = None) -> str:
    """ONE-WAY, confirm-gated. Convert an ON_OFF zone into a spare-relay aux output:
    the zone becomes a heater zone with no relay, its single relay becomes an aux binding
    (tc_zone = the zone if it has a thermocouple; hysteresis and min on/off copied), and
    EVERY stored profile's rules for the zone are rewritten to the aux target (zone byte
    8 + relay - 1). One POST /api/zones with only move_zone_to_aux=Z&confirm=1; the
    firmware does it all-or-nothing and rolls back on any failure.

    Refused: bad argument; unless `confirm is True` exactly (dry run, no POST, reports what
    would change); while a profile or autotune run is active (precheck, plus the firmware's
    409); a zone that is not ON_OFF, has failsafe_state ON, or owns anything but exactly one
    aux-capable relay (1..4); a relay already bound as an aux output; a quarantined aux
    store; a profile already holding a rule at the aux target; a running/paused profile or
    the live working copy using the zone; a rule an aux cannot represent (temp_source 2, or a
    temperature compare with no thermocouple). After the POST it re-reads zones, aux outputs
    and every profile and FAILS LOUD unless the zone is a relay-less heater, the aux entry is
    enabled with the right tc_zone, no zone/relay other than the two touched changed, no rule
    for the zone remains and exactly the planned number of rules now target the aux. Take a
    backup_export() first: a failure after commit is not reversed by this tool. Uses the
    http_auth admin-session seam; never prints a credential."""
    if not _is_int(zone) or not 0 <= zone <= 2:
        return f"refused: zone={zone!r} must be an integer 0..2"

    resolved = _resolve_host(host)
    running = _running_reason()
    if running is not None:
        return f"refused: {running} -- zones are not converted mid-run (host={resolved})"

    try:
        zones_before = zones_http_client.get_zones(resolved)
        aux_before = ahc.get_aux_outputs(resolved)
        profiles_before = ahc.get_stored_profile_rules(resolved)
    except (ahc.AuxHttpError, zones_http_client.ZonesHttpError) as exc:
        return f"error: precheck read failed (host={resolved}): {exc}"

    z = _zone_entry(zones_before, zone)
    if z is None:
        return f"refused: board reports no zone {zone} (host={resolved})"
    if z.get("zone_type") != 1:
        return f"refused: zone {zone} is not an ON_OFF zone (zone_type={z.get('zone_type')!r}) (host={resolved})"
    if z.get("failsafe_state"):
        return (f"refused: zone {zone} has failsafe_state ON, which an aux output does not support -- "
                f"set it to OFF first (host={resolved})")
    mask = z.get("relay_mask")
    if not _is_int(mask) or mask == 0 or mask & (mask - 1):
        return f"refused: zone {zone} relay_mask={mask!r} must name exactly one relay (host={resolved})"
    relay = mask.bit_length()
    if relay > ahc.AUX_RELAY_COUNT:
        return (f"refused: zone {zone}'s relay {relay} is outside the aux relays 1..{ahc.AUX_RELAY_COUNT} "
                f"(host={resolved})")
    if aux_before.get("quarantined"):
        return f"refused: the aux store is quarantined -- not touching it (host={resolved})"
    aux_cur = ahc.aux_entry(aux_before, relay)
    if aux_cur is None or aux_cur.get("enabled") or aux_cur.get("conflicted"):
        return f"refused: relay {relay} already has an aux binding or conflict (host={resolved})"
    dest = 8 + relay - 1
    if _count_rules(profiles_before, dest):
        return f"refused: a stored profile already has a rule at aux relay {relay} (target {dest}) (host={resolved})"
    has_tc = bool(z.get("thermo_mask"))
    hits = {pid: [r for r in rules if r.get("zone") == zone] for pid, rules in profiles_before.items()}
    hits = {pid: rs for pid, rs in hits.items() if rs}
    n_rules = sum(len(rs) for rs in hits.values())
    for pid, rs in hits.items():
        for r in rs:
            if r.get("temp_source", 0) > 1:
                return (f"refused: profile {pid} has a rule for zone {zone} with temp_source "
                        f"{r.get('temp_source')}, which an aux output cannot represent (host={resolved})")
            if r.get("temp_cmp", 0) and (r.get("temp_source") != 1 or not has_tc):
                return (f"refused: profile {pid} has a temperature-compare rule for zone {zone} but the aux "
                        f"output would have no thermocouple zone (host={resolved})")

    want_tc = zone if has_tc else -1
    plan = (f"zone {zone} (relay {relay}) -> aux relay {relay}, tc_zone={want_tc}; "
            f"{n_rules} rule(s) in {len(hits)} of {len(profiles_before)} stored profile(s) retargeted to {dest}")
    if confirm is not True:
        return (f"DRY RUN (pass confirm=True, exactly, to actually write; take a backup_export first) -- "
                f"would convert {plan} (host={resolved})")

    try:
        ack = ahc.post_move_zone_to_aux(resolved, zone)
    except ahc.AuxHttpError as exc:
        return _gate_or_error(exc, "POST /api/zones move_zone_to_aux", resolved)

    try:
        zones_after = zones_http_client.get_zones(resolved)
        aux_after = ahc.get_aux_outputs(resolved)
        profiles_after = ahc.get_stored_profile_rules(resolved)
    except (ahc.AuxHttpError, zones_http_client.ZonesHttpError) as exc:
        return (f"error: POST answered ok, but the confirming re-read failed (host={resolved}): {exc} "
                f"-- state UNKNOWN, re-check zones, aux outputs and profiles before trusting this")

    bad = []
    za = _zone_entry(zones_after, zone)
    if za is None or za.get("zone_type") != 0 or za.get("relay_mask") != 0:
        bad.append(f"zone {zone} is not a relay-less heater (zone_type={za and za.get('zone_type')!r}, "
                   f"relay_mask={za and za.get('relay_mask')!r})")
    for i in range(3):
        zb, zn = _zone_entry(zones_before, i), _zone_entry(zones_after, i)
        if i != zone and zb is not None:
            for k in ("zone_type", "relay_mask", "thermo_mask"):
                if (zb or {}).get(k) != (zn or {}).get(k):
                    bad.append(f"zone {i} {k} changed")
    ea = ahc.aux_entry(aux_after, relay)
    if ea is None or not ea.get("enabled") or ea.get("conflicted") or ea.get("tc_zone") != want_tc:
        bad.append(f"aux relay {relay} does not read enabled/unconflicted with tc_zone={want_tc}: {ea!r}")
    for r in range(1, ahc.AUX_RELAY_COUNT + 1):
        if r != relay and ahc.aux_entry(aux_before, r) != ahc.aux_entry(aux_after, r):
            bad.append(f"aux relay {r} entry changed")
    if _count_rules(profiles_after, zone):
        bad.append(f"{_count_rules(profiles_after, zone)} rule(s) still target zone {zone}")
    if _count_rules(profiles_after, dest) != n_rules:
        bad.append(f"{_count_rules(profiles_after, dest)} rule(s) now target the aux, expected {n_rules}")
    if set(profiles_after) != set(profiles_before):
        bad.append("the set of stored profiles changed")
    else:
        for pid, rules in profiles_before.items():
            expect = [dict(r, zone=dest) if r.get("zone") == zone else r for r in rules]
            if profiles_after[pid] != expect:
                bad.append(f"profile {pid} differs from the expected rewrite")
    if bad:
        return (f"FAILED: POST answered ok but the read-back does not confirm it -- {'; '.join(bad)} "
                f"(host={resolved}). Do not trust this; compare with the backup_export.")
    return (f"ok - converted {plan} (confirmed by read-back of zones, aux outputs and every profile; "
            f"firmware ack: {ack}; host={resolved})")


# Bound last, on purpose: see mcp_server_control.py's trailing comment.
from . import mcp_server as _srv  # noqa: E402
