"""Known-good config presets: a consistent starting point for tests.

The owner's request: "we should have a tool that loads some known default
configs that we can use as an initial basis for test so that the ai can
always factory default then load the config file to provide a consistent
test." This module is the data-driven half of that -- ``mcp_server.py``
wires its three functions up as MCP tools (``list_config_presets``,
``load_config_preset``, ``factory_default_then_load_preset``).

SCOPE, and why: presets are applied over three write paths, each with its
own reason for existing.

  * PID gains and the thermal model go over the firmware's UART CONTROL task
    (task 8) -- SET_ZONE_PID / SET_ZONE_MODEL (``control.py``). Always
    attempted.
  * The rest of ``zones_cfg_t`` (max_temp_c, relay_mask, control_mode,
    thermo_count/relay_count, ...) has no UART setter at all; its only write
    path is ``POST /api/zones`` in ``zones_http.c``, a whole-page-submit
    handler that would ZERO every field a naive partial POST omitted.
    ``zones_http_client.py`` is the GET-merge-POST-verify client that makes
    that safe, and ``apply_preset(zones_host=...)`` is what reaches it.
  * SAFETY-PROCESSOR parameters (SaftyFW, ``safety_cfg_http.c``) go over
    ``POST /api/safety/commissioning`` via ``safety_cfg_http_client.py``,
    reached with ``apply_preset(safety_host=...)``. THIS WAS PREVIOUSLY OUT
    OF SCOPE and this docstring said so: those writes reported success
    without landing, and were refused while ARMED. That is fixed --
    SET_PARAM/COMMIT_CONFIG now confirm by a live read-back on the firmware
    side (``confirm_commit_landed()``), and the client re-reads and compares
    again on top of that. A preset's ``"safety"`` section is written and
    verified like any other.

ONE SAFETY FIELD IS STILL GATED ON HARDWARE, and not by a software gap:
``ct_channel_map[0..2]`` records which relay each current transformer is
physically clamped around. Its only honest producer is the zone
current-sweep (``zone_sweep_push_ct_channel_map()`` in ``zones_http.c``),
which needs a CT to exist. ``ct_channel_map`` is NOT unconditionally required
for ``calibration_missing`` to clear: since the 2026-09-09 CT-optional fix,
``config_params_all_required_set()`` (``firmware/SaftyFW/src/config_params.c``)
only adds ``CONFIG_STORE_SET_CT_CHANNEL_MAP`` to its required-bits mask when
``ct_installed != 0`` (and the board is not in summed CT topology); a preset
that honestly commits ``ct_installed = 0`` on a CT-less bench exempts the map
from the requirement entirely, rather than needing to supply one. Committing
a GUESSED map instead -- on a board that does have a CT, or by leaving
``ct_installed`` at a stale/wrong value -- would clear ``calibration_missing``
on the strength of a mapping nobody measured, which is what this module
guards against: it keeps any assumed map in a SEPARATE
``"safety_ct_channel_map_backup"`` section that is applied only when a caller
passes ``use_ct_map_backup=True`` knowingly. See ``config_presets/
bench_fixture.json`` for the worked example -- as of the CT-optional fix, that
file's own ``"safety"`` section commits ``ct_installed = 0`` and carries no
``ct_channel_map`` at all (see its ``_ct_channel_map_retired_comment``); the
backup-section mechanism remains for a board that does have CTs the
current-sweep cannot resolve on its own.

THIS PC-SIDE PRESET IS NOT THE ONLY THING CALLED A "BENCH PRESET" IN THIS
CODEBASE. This module's ``load_config_preset("bench_fixture")`` writes over
``POST /api/safety/commissioning`` and is what the note above describes. A
separate, unrelated ``POST /api/safety/commissioning/bench_preset`` HTTP
route also exists on the board itself (``bench_preset_post_handler()`` in
``firmware/KilnFW/App/drivers/http/safety_cfg_http.c``, dev-tools-only,
gated out of release builds) -- it stages a fixed set of non-commissioning
test values (debounce/watchdog/etc.) over the safety link and deliberately
does NOT send the sec-1 commissioning fields, so it leaves
``calibration_missing`` SET. Do not conflate the two: this file's preset
clears the flag (via ``ct_installed = 0``); the ESP route of the same name
does not touch it at all.

Presets are DATA under ``tools/PcTools/config_presets/*.json``, never
compiled into any firmware image. That is deliberate, not incidental: a
value baked into firmware ships to every board that runs it, including a
real kiln; a value read from a JSON file on the PC running the test tooling
cannot reach a board unless something on the PC side chooses to send it.
See ``config_presets/bench_fixture.json`` for why that distinction matters
here specifically (its 80C ceiling is a fixture limit, never a kiln one).
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from typing import TYPE_CHECKING, Optional

from . import ramp_assist_http_client, safety_cfg_http_client, zones_http_client

if TYPE_CHECKING:  # pragma: no cover - typing only
    from .control import ControlClient
    from .devices import OkReason
    from .serial_link import UartLink


class ConfigPresetError(ValueError):
    """Raised for a missing preset, or one that fails schema validation."""


#: Fields every zone entry must carry. ``control_mode``/``max_temp_c``/
#: ``min_temp_c``/``relay_mask`` are validated (so a malformed preset is
#: caught early) even though this module cannot write them back yet -- see
#: the module docstring's SCOPE section.
_REQUIRED_ZONE_FIELDS = (
    "index", "relay_mask", "control_mode", "cal_offset_c",
    "pid_kp", "pid_ki", "pid_kd", "max_ramp_c_per_hr",
    "max_temp_c", "min_temp_c",
)
#: "ramp_assist_enabled" is REQUIRED, not optional, and deliberately has no
#: default this module supplies -- ramp_assist_cfg.h's forthcoming feature
#: (kiln-wide, not per-zone: warn/stretch-ramp/credit-dwell when the kiln
#: cannot keep up with a commanded rate) can silently change the ramp/dwell
#: shape a firing actually runs. A PID tuning run or A/B controller
#: comparison's tracking-error numbers are only comparable if every preset
#: PINS this explicitly rather than inheriting whatever the board happens to
#: have left over from a previous session -- see apply_preset()'s own
#: docstring and mcp_server_ramp_assist.py's module docstring for the full
#: hazard. A preset missing this field fails validation outright (the same
#: "malformed preset caught early" discipline _REQUIRED_ZONE_FIELDS already
#: applies), rather than silently defaulting to either value.
_REQUIRED_TOP_FIELDS = ("name", "zones", "ramp_assist_enabled")


def presets_dir() -> str:
    """``tools/PcTools/config_presets`` -- found relative to this file, not
    the caller's cwd, so it resolves the same from an MCP server, a test
    runner, or an interactive shell."""
    here = os.path.abspath(os.path.dirname(__file__))
    # here = .../tools/PcTools/src/kilnctrl
    return os.path.abspath(os.path.join(here, "..", "..", "config_presets"))


def _preset_path(name: str) -> str:
    if not name or any(c in name for c in ("/", "\\", "..")):
        raise ConfigPresetError(f"invalid preset name: {name!r}")
    return os.path.join(presets_dir(), f"{name}.json")


def list_presets() -> "list[dict]":
    """Every preset file in :func:`presets_dir`, as ``{name, description}``.

    A preset that fails to parse or validate is still listed, with its
    ``description`` replaced by the error -- so a broken file is visible
    from the listing tool instead of silently vanishing from it.
    """
    directory = presets_dir()
    if not os.path.isdir(directory):
        return []
    out = []
    for fname in sorted(os.listdir(directory)):
        if not fname.endswith(".json"):
            continue
        name = fname[: -len(".json")]
        try:
            preset = load_preset_data(name)
            out.append({"name": name, "description": preset.get("description", "")})
        except (ConfigPresetError, OSError) as exc:
            out.append({"name": name, "description": f"error: {exc}"})
    return out


def load_preset_data(name: str) -> dict:
    """Read and validate one preset file. Raises :class:`ConfigPresetError`
    on a missing file, bad JSON, or a schema violation -- never returns a
    partially-valid preset."""
    path = _preset_path(name)
    if not os.path.isfile(path):
        raise ConfigPresetError(f"no preset named {name!r} (looked for {path})")
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        raise ConfigPresetError(f"could not read preset {name!r}: {exc}") from exc
    _validate(name, data)
    return data


def _validate(name: str, data: object) -> None:
    if not isinstance(data, dict):
        raise ConfigPresetError(f"preset {name!r}: top level must be a JSON object")
    for field in _REQUIRED_TOP_FIELDS:
        if field not in data:
            raise ConfigPresetError(f"preset {name!r}: missing required field {field!r}")
    zones = data["zones"]
    if not isinstance(zones, list) or not zones:
        raise ConfigPresetError(f"preset {name!r}: 'zones' must be a non-empty list")
    seen_index = set()
    for i, zone in enumerate(zones):
        if not isinstance(zone, dict):
            raise ConfigPresetError(f"preset {name!r}: zones[{i}] must be an object")
        for field in _REQUIRED_ZONE_FIELDS:
            if field not in zone:
                raise ConfigPresetError(
                    f"preset {name!r}: zones[{i}] missing required field {field!r}"
                )
        idx = zone["index"]
        if not isinstance(idx, int) or idx < 0:
            raise ConfigPresetError(f"preset {name!r}: zones[{i}].index must be a non-negative int")
        if idx in seen_index:
            raise ConfigPresetError(f"preset {name!r}: duplicate zone index {idx}")
        seen_index.add(idx)
    if not isinstance(data["ramp_assist_enabled"], bool):
        raise ConfigPresetError(f"preset {name!r}: 'ramp_assist_enabled' must be a bool (true/false), "
                                "not inherited or inferred -- see this field's own comment above "
                                "_REQUIRED_TOP_FIELDS for why it must be pinned explicitly")
    _validate_safety_sections(name, data)


#: Safety param names a preset's "safety" section may NEVER carry. These are
#: the three ct_channel_map ids. Committing all three sets the
#: CONFIG_STORE_SET_CT_CHANNEL_MAP fields_set bit, which is one of the bits
#: config_params_all_required_set() (firmware/SaftyFW/src/config_params.c)
#: checks before calibration_missing clears -- but that bit is only ever
#: REQUIRED when ct_installed != 0 (CT-optional fix, 2026-09-09); a preset
#: that honestly declares ct_installed = 0 does not need this field at all
#: (see this module's docstring). So the risk this guards against is a
#: preset committing a GUESSED map -- on a board that does have a CT, or
#: alongside a wrong/stale ct_installed -- which would help clear
#: calibration_missing on the strength of a mapping nobody measured, letting
#: commissioning_gate.c grant heat on it. ct_channel_map fields belong in the
#: opt-in-only backup section (or nowhere), so that a routine "load the bench
#: preset" can never silently declare a board commissioned this way.
_CT_MAP_FIELDS = ("ct_channel_map[0]", "ct_channel_map[1]", "ct_channel_map[2]")


def _validate_safety_sections(name: str, data: dict) -> None:
    """Both safety sections must be flat ``{param_name: number}`` maps, and
    the ct_channel_map fields must appear ONLY in the backup section. The
    param NAMES themselves are deliberately not validated against a list
    here -- the live board's own parameter table is the authority, and
    safety_cfg_http_client.build_post_body() refuses an unknown name against
    that table rather than against a copy of it that could go stale."""
    for section in (safety_cfg_http_client.SAFETY_SECTION,
                    safety_cfg_http_client.SAFETY_CT_MAP_BACKUP_SECTION):
        if section not in data:
            continue
        values = data[section]
        if not isinstance(values, dict):
            raise ConfigPresetError(f"preset {name!r}: {section!r} must be a JSON object")
        for key, value in values.items():
            if not isinstance(key, str):
                raise ConfigPresetError(f"preset {name!r}: {section!r} has a non-string key {key!r}")
            if isinstance(value, bool):
                continue
            if not isinstance(value, (int, float)):
                raise ConfigPresetError(
                    f"preset {name!r}: {section}[{key!r}] must be a number or bool, got {value!r}")
    for field_name in _CT_MAP_FIELDS:
        if field_name in (data.get(safety_cfg_http_client.SAFETY_SECTION) or {}):
            raise ConfigPresetError(
                f"preset {name!r}: {field_name!r} may not appear in the "
                f"{safety_cfg_http_client.SAFETY_SECTION!r} section -- an unmeasured CT map that "
                "applies by default would let a board report itself commissioned on a mapping "
                "nobody verified; put it in "
                f"{safety_cfg_http_client.SAFETY_CT_MAP_BACKUP_SECTION!r} instead")


def _verify_pid_readback(control, preset, results):
    """Replace each ZoneApplyResult whose PID did not read back over GET_ZONES
    with a pid_ok=False copy naming what the board reports."""
    from .control import ControlQueryError  # deferred: control.py imports a lot
    try:
        _, _, zones = control.get_zones()
    except (ControlQueryError, AttributeError) as exc:
        return [ZoneApplyResult(r.zone, False, f"UNVERIFIED: PID read-back failed ({exc})", r.model_ok, r.model_detail)
                for r in results]
    by_index = {z.index: z for z in zones}
    wanted = {z["index"]: z for z in preset["zones"]}
    out = []
    for r in results:
        got = by_index.get(r.zone)
        w = wanted[r.zone]
        if not r.pid_ok:
            out.append(r)
        elif got is None:
            out.append(ZoneApplyResult(r.zone, False, "UNVERIFIED: zone missing from GET_ZONES read-back", r.model_ok, r.model_detail))
        elif not (_pid_matches(w["pid_kp"], got.pid_kp) and _pid_matches(w["pid_ki"], got.pid_ki)
                  and _pid_matches(w["pid_kd"], got.pid_kd)):
            out.append(ZoneApplyResult(
                r.zone, False,
                f"read-back mismatch: wanted Kp/Ki/Kd={w['pid_kp']}/{w['pid_ki']}/{w['pid_kd']}, "
                f"board reports {got.pid_kp}/{got.pid_ki}/{got.pid_kd}", r.model_ok, r.model_detail))
        else:
            out.append(r)
    return out


@dataclass(frozen=True)
class ZoneApplyResult:
    zone: int
    pid_ok: bool
    pid_detail: str
    model_ok: Optional[bool]  # None when the preset carries no model fields for this zone
    model_detail: str


@dataclass(frozen=True)
class PresetApplyResult:
    preset_name: str
    zones: "list[ZoneApplyResult]"
    #: Fields present in the preset with no write path attempted this call --
    #: either because no ``zones_host`` was given (the whole zones_cfg_t
    #: group, same as before zones_http_client.py existed) or because a field
    #: config_presets.py's schema does not carry at all (there are none of
    #: those today). Empty when ``zones_host`` was given and the write
    #: succeeded -- see ``zones_result`` for that path's own pass/fail.
    not_written: "list[str]"
    #: Set only when ``apply_preset()`` was called with ``zones_host`` --
    #: the GET/merge/POST/read-back-verify result from zones_http_client.py
    #: for max_temp_c/relay_mask/control_mode/thermo_count/relay_count/etc.
    #: None means "not attempted this call", not "attempted and unknown" --
    #: check ``not_written`` to tell the two apart.
    zones_result: "Optional[zones_http_client.ZonesApplyResult]" = None
    #: Set only when ``apply_preset()`` was called with ``safety_host`` --
    #: the POST/read-back-verify result for the preset's "safety" section
    #: (safety_cfg_http_client.py). None means "not attempted this call".
    safety_result: "Optional[safety_cfg_http_client.SafetyApplyResult]" = None
    #: Set only when ``apply_preset()`` was called with ``zones_host`` --
    #: the board's own POST /api/ramp_assist response
    #: (``{"ok":true,"enabled":<bool>}`` or ``{"ok":false,"error":...}``) for
    #: pinning ``ramp_assist_enabled``. None means "not attempted this call",
    #: not "attempted and unknown" -- check ``not_written`` to tell the two
    #: apart (same convention as ``zones_result``/``safety_result``).
    ramp_assist_result: "Optional[dict]" = None

    def describe(self) -> str:
        lines = [f"preset {self.preset_name!r}:"]
        for z in self.zones:
            pid = "ok" if z.pid_ok else f"FAILED ({z.pid_detail})"
            lines.append(f"  zone {z.zone}: PID {pid}")
            if z.model_ok is not None:
                model = "ok" if z.model_ok else f"FAILED ({z.model_detail})"
                lines.append(f"    model {model}")
        if self.zones_result is not None:
            status = "ok" if self.zones_result.ok else "FAILED"
            lines.append(f"  zones config (relay_mask/max_temp_c/control_mode/...) over HTTP: {status}")
            if not self.zones_result.ok:
                for m in self.zones_result.mismatches:
                    lines.append(f"    {m}")
        if self.safety_result is not None:
            for line in self.safety_result.describe().splitlines():
                lines.append("  " + line)
        if self.ramp_assist_result is not None:
            status = "ok" if self.ramp_assist_result.get("ok") else "FAILED"
            lines.append(f"  ramp_assist_enabled pinned over HTTP: {status} "
                        f"({self.ramp_assist_result})")
        if self.not_written:
            lines.append(
                "  NOT written back (no zones_host given this call; preset value is "
                "reference/expected state only): " + ", ".join(self.not_written)
            )
        return "\n".join(lines)

    @property
    def all_ok(self) -> bool:
        pid_model_ok = all(z.pid_ok and (z.model_ok in (None, True)) for z in self.zones)
        zones_ok = self.zones_result is None or self.zones_result.ok
        safety_ok = self.safety_result is None or self.safety_result.ok
        ramp_assist_ok = self.ramp_assist_result is None or bool(self.ramp_assist_result.get("ok"))
        return pid_model_ok and zones_ok and safety_ok and ramp_assist_ok


#: Zone-level fields this preset schema carries that only zones_http_client's
#: GET/POST /api/zones path can write -- see that module's docstring for why
#: the UART CONTROL task has no setter for any of these. Mirrors
#: zones_http_client._PRESET_ZONE_OVERRIDE_FIELDS; kept as a separate literal
#: here (rather than importing that private name) so this module's own
#: not_written accounting doesn't reach into zones_http_client's internals.
_ZONES_HTTP_ONLY_ZONE_FIELDS = (
    "relay_mask", "control_mode", "max_temp_c", "min_temp_c", "max_ramp_c_per_hr",
    # 2026-08-29: the guard-1 minimum rise rate. HTTP-only, like the rest --
    # the UART CONTROL task has no setter for it either.
    "sanity_rate_c_per_min",
    # 2026-08-29: the relay timing pair. Also HTTP-only.
    "heater_window_ms",
    "heater_min_on_ms",
)


def apply_preset(control: "ControlClient", preset: dict,
                  zones_host: "Optional[str]" = None,
                  zones_timeout: float = zones_http_client.ZONES_HTTP_TIMEOUT_S,
                  verify_zones: bool = True,
                  safety_host: "Optional[str]" = None,
                  safety_timeout: float = safety_cfg_http_client.SAFETY_CFG_HTTP_TIMEOUT_S,
                  verify_safety: bool = True,
                  use_ct_map_backup: bool = False,
                  verify_pid: bool = True) -> PresetApplyResult:
    """Write everything this preset can be written through over the UART
    CONTROL task (PID gains, and the thermal model when the preset carries
    one).

    ``zones_host`` (new): when given, ALSO writes relay_mask/control_mode/
    max_temp_c/min_temp_c/max_ramp_c_per_hr/thermo_count/relay_count over
    GET/POST /api/zones via zones_http_client.py -- the fields the UART link
    has no setter for. That client GETs the board's live config first and
    merges the preset onto it before POSTing (POST /api/zones is a
    whole-page-submit endpoint; a naive partial POST would zero every field
    it doesn't mention, including max_temp_c -- see zones_http_client.py's
    module docstring), then (unless ``verify_zones=False``) re-reads the
    config and confirms it actually landed -- see ``zones_result`` on the
    returned ``PresetApplyResult``. A ZonesHttpError from that path
    propagates, same as a ControlQueryError from the PID/model calls above.

    When ``zones_host`` is omitted (the default, preserving this function's
    original behavior), those fields are reported in ``not_written`` as
    reference/expected data only, exactly as before this parameter existed.

    ``safety_host`` (new): when given, ALSO writes the preset's ``"safety"``
    section -- the SaftyFW commissioning parameters -- over
    POST /api/safety/commissioning via safety_cfg_http_client.py, then
    re-reads and confirms each field actually landed (see ``safety_result``
    on the returned ``PresetApplyResult``, and that module's docstring for
    why an ACK is not proof on this link). Almost always the SAME host as
    ``zones_host``: both endpoints are served by the same ESP32, which is
    the only thing that can talk to the Pico at all. A SafetyCfgHttpError
    propagates, same as the other two paths' errors.

    ``use_ct_map_backup`` (default False): additionally writes the preset's
    ``"safety_ct_channel_map_backup"`` section -- an ASSUMED, UNMEASURED
    CT-to-zone map. Committing it clears ``calibration_missing`` and so lets
    the safety processor grant heat; do not pass it to get a green
    commissioning check on a bench whose CTs are not fitted. See this
    module's docstring.

    ``ramp_assist_enabled`` (REQUIRED on every preset, see
    ``_REQUIRED_TOP_FIELDS``'s own comment): when ``zones_host`` is given,
    ALSO PINS the kiln-wide ramp-assist flag (ramp_assist_cfg.h) over
    POST /api/ramp_assist to exactly the value the preset names -- never
    left to inherit whatever the board happens to already have. This is the
    load-bearing reason the field is required rather than optional: ramp
    assist can silently stretch a ramp or shorten a dwell, which would
    invalidate a PID tuning run or A/B comparison's tracking-error numbers
    if it were left to chance. See ``ramp_assist_result`` on the returned
    ``PresetApplyResult``. When ``zones_host`` is omitted, the field is
    reported in ``not_written`` instead, same as the rest of the
    zones-http-only fields.

    PARTIAL FAILURE (2026-10-09): a stage that raises (ControlQueryError,
    ZonesHttpError, SafetyCfgHttpError, RampAssistHttpError) is re-raised
    unchanged, but with ``exc.preset_partial`` set to a text summary of the
    stages that had ALREADY landed, so a caller never reports a failure as if
    nothing was written. PID gains are read back over GET_ZONES
    (``verify_pid``, default True): an ACK is not proof on this link, and a
    gain that does not read back (or cannot be read) is ``pid_ok=False``.
    The zones POST strips the firmware's omit-preserved fields
    (zones_http_client.strip_omit_preserved) so they stay bit-exact.

    Never touches relays, never resets, never enables anything."""
    done: "list[str]" = []
    try:
        return _apply_preset_stages(control, preset, zones_host, zones_timeout, verify_zones,
                                     safety_host, safety_timeout, verify_safety, use_ct_map_backup,
                                     verify_pid, done)
    except (Exception,) as exc:
        if getattr(exc, "preset_partial", None) is None:
            try:
                exc.preset_partial = "\n".join(done) if done else "(nothing had landed yet)"
            except Exception:  # noqa: BLE001 -- exotic exception without __dict__
                pass
        raise


def _pid_matches(want: float, got: float) -> bool:
    return abs(want - got) <= 1e-4 + 1e-3 * abs(want)


def _apply_preset_stages(control, preset, zones_host, zones_timeout, verify_zones,
                         safety_host, safety_timeout, verify_safety, use_ct_map_backup,
                         verify_pid, done) -> PresetApplyResult:
    results = []
    try:
        for zone in preset["zones"]:
            pid: "OkReason" = control.set_zone_pid(
                zone["index"], zone["pid_kp"], zone["pid_ki"], zone["pid_kd"]
            )
            # Record the PID write the moment it lands, so a raise in this
            # zone's model write (or in a later zone) still reports it.
            results.append(
                ZoneApplyResult(
                    zone=zone["index"],
                    pid_ok=bool(pid),
                    pid_detail=getattr(pid, "reason", "") or "",
                    model_ok=None,
                    model_detail="",
                )
            )
            if all(k in zone for k in ("k_dc", "tau_s", "dead_time_s")):
                model = control.set_zone_model(
                    zone["index"], zone["k_dc"], zone["tau_s"], zone["dead_time_s"]
                )
                results[-1] = ZoneApplyResult(
                    zone=zone["index"], pid_ok=results[-1].pid_ok,
                    pid_detail=results[-1].pid_detail, model_ok=bool(model),
                    model_detail=getattr(model, "reason", "") or "")
    except Exception:
        # A raise on zone N: report which zones already landed, and still
        # read the PID back on them (best effort -- never mask the original).
        if results:
            landed = results
            if verify_pid:
                try:
                    landed = _verify_pid_readback(control, preset, results)
                except Exception:  # noqa: BLE001
                    pass
            done.append("PID written over UART before the failure: " + ", ".join(
                f"zone {r.zone} pid={'ok' if r.pid_ok else 'FAILED'}"
                f"{'' if r.model_ok is None else (' model=ok' if r.model_ok else ' model=FAILED')}"
                for r in landed))
        raise

    if verify_pid and results:
        results = _verify_pid_readback(control, preset, results)
    done.append("PID/model written over UART: " + ", ".join(
        f"zone {r.zone} pid={'ok' if r.pid_ok else 'FAILED'}" for r in results))

    zones_result = None
    ramp_assist_result = None
    if zones_host:
        zones_result = zones_http_client.apply_zone_preset(
            zones_host, preset, timeout=zones_timeout, verify=verify_zones)
        done.append(f"zones config POSTed over HTTP ({'read back ok' if zones_result.ok else 'READ-BACK MISMATCH'})")
        ramp_assist_result = ramp_assist_http_client.set_enabled(
            zones_host, bool(preset["ramp_assist_enabled"]))
        done.append(f"ramp_assist pinned: {ramp_assist_result}")
        not_written = []
    else:
        not_written = sorted(
            {
                field
                for zone in preset["zones"]
                for field in _ZONES_HTTP_ONLY_ZONE_FIELDS
                if field in zone
            }
            | ({"thermo_count", "relay_count"} & preset.keys())
            | {"ramp_assist_enabled"}
        )
    safety_result = None
    if safety_host:
        safety_result = safety_cfg_http_client.apply_safety_preset(
            safety_host, preset, timeout=safety_timeout, verify=verify_safety,
            use_ct_map_backup=use_ct_map_backup)
    elif preset.get(safety_cfg_http_client.SAFETY_SECTION):
        not_written = sorted(set(not_written) | {
            f"safety.{k}" for k in preset[safety_cfg_http_client.SAFETY_SECTION]})

    return PresetApplyResult(preset_name=preset["name"], zones=results, not_written=not_written,
                              zones_result=zones_result, safety_result=safety_result,
                              ramp_assist_result=ramp_assist_result)
