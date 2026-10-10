"""LCD+web UI regression-test scripts: load/validate JSON scripts under
tools/PcTools/ui_scripts/, dispatch each step to the right backend client,
and report a compact pass/fail result -- same "presets are DATA, never
compiled into firmware" reasoning as config_presets.py, and the same
path-safety validation style for a script name.

A script is one JSON file: ``{"name","description","backend","preset"?,"steps"}``.
``backend`` picks which client :func:`run_ui_script` dispatches every step
to -- "lcd" -> UiTestClient, "web" -> WebUiClient. Each step is
``{"action","target"?, ...}``; see :func:`run_ui_step` for the action set.
"""

from __future__ import annotations

import json
import os
import time
from typing import Optional

from . import config_presets

_VALID_BACKENDS = ("lcd", "web")
_VALID_ACTIONS = ("click", "wait_for", "assert_text", "fill", "goto", "sleep_ms")

#: wait_for's poll interval -- no busy spin, short enough not to blow past a
#: caller's timeout_ms by more than one interval.
_WAIT_FOR_POLL_S = 0.2

#: wait_for's default budget when a step doesn't specify one.
_DEFAULT_TIMEOUT_MS = 3000

#: click's bounded retry budget for a "swallowed" result (see
#: bench_test/cases_lcd.py's matching _CLICK_THEN_PAGE_SWALLOW_RETRIES).
_CLICK_SWALLOW_MAX_RETRIES = 2


class UiScriptError(ValueError):
    """Raised for a missing script, or one that fails validation."""


def ui_scripts_dir() -> str:
    """``tools/PcTools/ui_scripts`` -- resolved relative to this file, not
    the caller's cwd, same reasoning as config_presets.presets_dir()."""
    here = os.path.abspath(os.path.dirname(__file__))
    # here = .../tools/PcTools/src/kilnctrl
    return os.path.abspath(os.path.join(here, "..", "..", "ui_scripts"))


def _script_path(name: str) -> str:
    if not name or any(c in name for c in ("/", "\\", "..")):
        raise UiScriptError(f"invalid script name: {name!r}")
    return os.path.join(ui_scripts_dir(), f"{name}.json")


def list_ui_scripts() -> "list[dict]":
    """Every script file in :func:`ui_scripts_dir`, as ``{name, description, backend}``.

    A script that fails to parse or validate is still listed, with
    ``description`` replaced by the error -- same visibility contract as
    config_presets.list_presets()."""
    directory = ui_scripts_dir()
    if not os.path.isdir(directory):
        return []
    out = []
    for fname in sorted(os.listdir(directory)):
        if not fname.endswith(".json"):
            continue
        name = fname[: -len(".json")]
        try:
            script = load_ui_script(name)
            out.append({
                "name": name,
                "description": script.get("description", ""),
                "backend": script["backend"],
            })
        except (UiScriptError, OSError) as exc:
            out.append({"name": name, "description": f"error: {exc}", "backend": "?"})
    return out


def load_ui_script(name: str) -> dict:
    """Read and validate one script file. Raises :class:`UiScriptError` on a
    missing file, bad JSON, or a schema violation -- never returns a
    partially-valid script."""
    path = _script_path(name)
    if not os.path.isfile(path):
        raise UiScriptError(f"no ui script named {name!r} (looked for {path})")
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        raise UiScriptError(f"could not read ui script {name!r}: {exc}") from exc
    _validate(name, data)
    return data


def _validate(name: str, data: object) -> None:
    if not isinstance(data, dict):
        raise UiScriptError(f"ui script {name!r}: top level must be a JSON object")
    for field in ("name", "backend", "steps"):
        if field not in data:
            raise UiScriptError(f"ui script {name!r}: missing required field {field!r}")
    if data["name"] != name:
        raise UiScriptError(
            f"ui script file {name!r}.json: 'name' field is {data['name']!r}, must match the filename"
        )
    if data["backend"] not in _VALID_BACKENDS:
        raise UiScriptError(
            f"ui script {name!r}: backend must be one of {_VALID_BACKENDS}, got {data['backend']!r}"
        )
    steps = data["steps"]
    if not isinstance(steps, list) or not steps:
        raise UiScriptError(f"ui script {name!r}: 'steps' must be a non-empty list")
    for i, step in enumerate(steps):
        if not isinstance(step, dict) or "action" not in step:
            raise UiScriptError(f"ui script {name!r}: steps[{i}] must be an object with 'action'")
        if step["action"] not in _VALID_ACTIONS:
            raise UiScriptError(
                f"ui script {name!r}: steps[{i}].action must be one of {_VALID_ACTIONS}, "
                f"got {step['action']!r}"
            )


def run_ui_step(backend: str, ui_test_client, web_client, step: dict) -> dict:
    """Dispatch one step directly, without loading a script -- the debug
    entry point for trying a single action interactively.

    Returns ``{"ok":bool,"detail":str|None}``. Never raises for an action
    failure (not_found, timeout, mismatch, ...); it only raises for a
    programming error (unknown backend/action, which :func:`_validate`
    should already have caught for anything loaded from a script file).
    """
    action = step["action"]
    if backend == "lcd":
        return _run_lcd_step(ui_test_client, action, step)
    if backend == "web":
        return _run_web_step(web_client, action, step)
    raise UiScriptError(f"unknown backend {backend!r}")


def _run_lcd_step(client, action: str, step: dict) -> dict:
    from .ui_test_client import UiTestQueryError, UiTestResponseError

    target = step.get("target")
    try:
        if action == "click":
            # 2026-09-24: "swallowed" (screen_idle_touch_swallow() ate the
            # injected press -- a wake or ERROR_HOLD dismissal, see
            # bench_test/cases_lcd.py's _click_resolving_swallow()) is not a
            # click failure: the press was delivered, just not to the target.
            # Retry a small, bounded number of times before treating it like
            # any other non-ok result. "verdict_unknown" (the firmware's own
            # bounded verdict wait timed out -- see ui_test_client.py's
            # click_by_name() doc comment) is deliberately NOT re-clicked:
            # unlike a confirmed swallow, that press may well have reached
            # the widget (a slow flush delays the verdict, not the press),
            # and a script step can name any target, including Start/Stop/
            # Confirm or a PIN digit, where a blind second tap would
            # double-actuate. It is reported as a failed step instead, never
            # as a pass.
            result = client.click_by_name(target)
            retries = 0
            while result["result"] == "swallowed" and retries < _CLICK_SWALLOW_MAX_RETRIES:
                retries += 1
                result = client.click_by_name(target)
            if result["result"] != "ok":
                return {"ok": False, "detail": f"click {target!r}: {result['result']}"}
            return {"ok": True, "detail": None}
        if action == "wait_for":
            timeout_ms = step.get("timeout_ms", _DEFAULT_TIMEOUT_MS)
            deadline = time.monotonic() + timeout_ms / 1000.0
            while True:
                dump = client.list_tap_targets()
                names = {t["name"] for t in dump["targets"] if not t["hidden"]}
                if target in names:
                    return {"ok": True, "detail": None}
                if time.monotonic() >= deadline:
                    return {
                        "ok": False,
                        "detail": f"wait_for {target!r} timed out after {timeout_ms}ms; "
                        f"visible targets: {sorted(names)}",
                    }
                time.sleep(_WAIT_FOR_POLL_S)
        if action == "assert_text":
            page = client.get_current_page()
            contains = step.get("contains", target)
            if contains not in page:
                return {"ok": False, "detail": f"current page {page!r} does not contain {contains!r}"}
            return {"ok": True, "detail": None}
        if action == "sleep_ms":
            time.sleep(step.get("value", 0) / 1000.0)
            return {"ok": True, "detail": None}
        return {"ok": False, "detail": f"action {action!r} not supported on backend 'lcd'"}
    except (UiTestQueryError, UiTestResponseError) as exc:
        return {"ok": False, "detail": str(exc)}


def _run_web_step(client, action: str, step: dict) -> dict:
    from .web_ui_client import WebUiError

    target = step.get("target")
    try:
        if action == "goto":
            client.goto(target)
            return {"ok": True, "detail": None}
        if action == "click":
            client.click(target, body=step.get("value"))
            return {"ok": True, "detail": None}
        if action == "wait_for":
            timeout_ms = step.get("timeout_ms", _DEFAULT_TIMEOUT_MS)
            deadline = time.monotonic() + timeout_ms / 1000.0
            while True:
                el = client.find_element(target)
                if el is not None and not el["hidden"]:
                    return {"ok": True, "detail": None}
                if time.monotonic() >= deadline:
                    return {
                        "ok": False,
                        "detail": f"wait_for {target!r} timed out after {timeout_ms}ms "
                        f"(element {'not found' if el is None else 'hidden'})",
                    }
                time.sleep(_WAIT_FOR_POLL_S)
                client.goto(client.current_path)
        if action == "assert_text":
            el = client.find_element(target)
            contains = step.get("contains", "")
            if el is None:
                return {"ok": False, "detail": f"element {target!r} not found"}
            if contains not in el["text"]:
                return {
                    "ok": False,
                    "detail": f"element {target!r} text {el['text']!r} does not contain {contains!r}",
                }
            return {"ok": True, "detail": None}
        if action == "fill":
            # No form-fill endpoint exists on this dashboard today (every
            # input main_page.html has is read via its own dedicated
            # button handler, e.g. kcSaveNewName + kcSaveNewBtn) -- fill is
            # accepted by the schema for forward compatibility but has no
            # backend behavior yet.
            return {"ok": False, "detail": "fill is not implemented for backend 'web'"}
        if action == "sleep_ms":
            time.sleep(step.get("value", 0) / 1000.0)
            return {"ok": True, "detail": None}
        return {"ok": False, "detail": f"action {action!r} not supported on backend 'web'"}
    except WebUiError as exc:
        return {"ok": False, "detail": str(exc)}


def run_ui_script(name: str, ui_test_client=None, web_client=None,
                   zones_host: "Optional[str]" = None, apply_preset: bool = False) -> dict:
    """Load and run one script, dispatching each step to the backend client
    for ``script["backend"]``.

    Returns a COMPACT result: ``{"script":name,"ok":bool,"steps":[{"action",
    "target","ok","detail"}],"failed_at":int|None}`` -- ``detail`` stays a
    short one-liner on a passing step; only the first failing step gets the
    fuller diagnostic each action's own handler produces (a tap-target dump,
    an HTML snippet, ...). Every step after the first failure is still
    listed (ok=False, detail=None) so the report shows what never ran,
    without re-running anything past the failure.
    """
    script = load_ui_script(name)
    if apply_preset and "preset" in script:
        from .control import ControlClient  # deferred: only needed on this path

        if ui_test_client is None:
            raise UiScriptError(f"script {name!r} carries a preset but no ui_test_client (for its link) was given")
        control = ControlClient(ui_test_client.link)
        try:
            preset = config_presets.load_preset_data(script["preset"])
            applied = config_presets.apply_preset(control, preset, zones_host=zones_host)
            if not applied.all_ok:
                raise UiScriptError(
                    f"script {name!r}: preset {script['preset']!r} was only PARTIALLY applied, "
                    f"no steps run:\n{applied.describe()}")
        finally:
            control.close()

    backend = script["backend"]
    results = []
    failed_at = None
    for i, step in enumerate(script["steps"]):
        if failed_at is not None:
            results.append({"action": step["action"], "target": step.get("target"), "ok": False, "detail": None})
            continue
        outcome = run_ui_step(backend, ui_test_client, web_client, step)
        results.append({
            "action": step["action"],
            "target": step.get("target"),
            "ok": outcome["ok"],
            "detail": outcome["detail"],
        })
        if not outcome["ok"]:
            failed_at = i

    return {
        "script": name,
        "ok": failed_at is None,
        "steps": results,
        "failed_at": failed_at,
    }
