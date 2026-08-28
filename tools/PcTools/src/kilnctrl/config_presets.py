"""Known-good config presets: a consistent starting point for tests.

The owner's request: "we should have a tool that loads some known default
configs that we can use as an initial basis for test so that the ai can
always factory default then load the config file to provide a consistent
test." This module is the data-driven half of that -- ``mcp_server.py``
wires its three functions up as MCP tools (``list_config_presets``,
``load_config_preset``, ``factory_default_then_load_preset``).

Presets are DATA under ``tools/PcTools/config_presets/*.json``, never
compiled into any firmware image. That is deliberate, not incidental: a
value baked into firmware ships to every board that runs it, including a
real kiln; a value read from a JSON file on the PC running the test tooling
cannot reach a board unless something on the PC side chooses to send it.
See ``config_presets/bench_fixture.json`` for why that distinction matters
here specifically (its 80C ceiling is a fixture limit, never a kiln one).

SCOPE, and why: this only writes what the firmware's UART CONTROL task
(task 8) actually exposes a setter for -- SET_ZONE_PID and SET_ZONE_MODEL
(``control.py``). ``zones_cfg_t`` carries a great deal more (max_temp_c,
relay_mask, control_mode, thermo_count/relay_count, timing profiles, ...),
but the only write path for those is the ESP's HTTP form handler
(``POST /api/zones`` in ``zones_http.c``), which requires a dense repost of
the *entire* config (every zone, every timing profile, by form field name)
and has no PC-side client in this repo yet. Building one untested against
real hardware risked silently mismatching that ~4000-line handler's
required-field contract and corrupting a board's NVS zones config -- worse
than the fixture-consistency problem this tool exists to solve. So those
fields are captured in the preset (for a human/future tool to read and
compare against ``get_board_state``) and reported as "not written back:
read-only over this link" rather than attempted.

THE HOOK for finishing this: a ``zones_http_client.py`` alongside
``ota_http_client.py`` (same mocked-``urllib`` test pattern -- see
``tests/test_ota_http_client.py``) that GETs the live ``zones_cfg_t`` JSON
from ``GET /api/zones``, applies the preset's fields onto it (preserving
every field the preset doesn't mention, especially the timing-profile
block), and POSTs the merged form body back. That client would let
``load_config_preset`` write the full preset, not just PID/model. It is not
built here.

Safety-processor parameters (``safety_cfg_http.c`` / SaftyFW) are out of
scope for a second, independent reason: those config writes are currently
broken (report success without landing, refused while ARMED) and are being
fixed by another agent. Once that path is trustworthy, its hook is a second
preset section (e.g. ``"safety"``) alongside ``"zones"`` in the JSON, applied
by a ``load_config_preset`` step added next to the zones one here -- not
built now.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from typing import TYPE_CHECKING, Optional

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
_REQUIRED_TOP_FIELDS = ("name", "zones")


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
    #: Fields present in the preset this module has no write path for yet --
    #: see the module docstring's SCOPE section. Always non-empty today.
    not_written: "list[str]"

    def describe(self) -> str:
        lines = [f"preset {self.preset_name!r}:"]
        for z in self.zones:
            pid = "ok" if z.pid_ok else f"FAILED ({z.pid_detail})"
            lines.append(f"  zone {z.zone}: PID {pid}")
            if z.model_ok is not None:
                model = "ok" if z.model_ok else f"FAILED ({z.model_detail})"
                lines.append(f"    model {model}")
        if self.not_written:
            lines.append(
                "  NOT written back (read-only over this link; preset value is "
                "reference/expected state only): " + ", ".join(self.not_written)
            )
        return "\n".join(lines)

    @property
    def all_ok(self) -> bool:
        return all(z.pid_ok and (z.model_ok in (None, True)) for z in self.zones)


def apply_preset(control: "ControlClient", preset: dict) -> PresetApplyResult:
    """Write everything this preset can be written through over the UART
    CONTROL task (PID gains, and the thermal model when the preset carries
    one) and report the rest as not-written. Never touches relays, never
    resets, never enables anything -- see the module docstring."""
    results = []
    for zone in preset["zones"]:
        pid: "OkReason" = control.set_zone_pid(
            zone["index"], zone["pid_kp"], zone["pid_ki"], zone["pid_kd"]
        )
        model_ok = None
        model_detail = ""
        if all(k in zone for k in ("k_dc", "tau_s", "dead_time_s")):
            model = control.set_zone_model(
                zone["index"], zone["k_dc"], zone["tau_s"], zone["dead_time_s"]
            )
            model_ok = bool(model)
            model_detail = getattr(model, "reason", "") or ""
        results.append(
            ZoneApplyResult(
                zone=zone["index"],
                pid_ok=bool(pid),
                pid_detail=getattr(pid, "reason", "") or "",
                model_ok=model_ok,
                model_detail=model_detail,
            )
        )
    not_written = sorted(
        {
            field
            for zone in preset["zones"]
            for field in ("relay_mask", "control_mode", "max_temp_c", "min_temp_c", "max_ramp_c_per_hr")
            if field in zone
        }
        | ({"thermo_count", "relay_count"} & preset.keys())
    )
    return PresetApplyResult(preset_name=preset["name"], zones=results, not_written=not_written)
