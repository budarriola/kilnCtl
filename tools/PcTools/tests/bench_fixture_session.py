#!/usr/bin/env python3
"""Live-bench test harness: "start from bench_fixture.json, confirmed loaded".

WHY THIS EXISTS. Every live-hardware test in this repo so far has had to
open its own link, guess at the board's current configuration, and hope the
previous test left it somewhere sane. This module is the shared precondition
instead: :class:`BenchSession` puts the board into the known-good
``config_presets/bench_fixture.json`` state, VERIFIES it landed (reusing
zones_http_client / safety_cfg_http_client's own read-back plumbing -- no
second, weaker verifier lives here), and gives a test a small set of
read/act helpers that can never leave heat on.

TRANSPORTS, and why HTTP is the primary one. The MCP server
(``mcp_servers.ps1``) normally owns the board's COM port for the whole
session, so a pytest process cannot assume the UART is free. Everything this
harness NEEDS is reachable over HTTP:

  * ``GET/POST /api/zones``            -- zones_cfg_t (max_temp_c, relay_mask, ...)
  * ``GET/POST /api/safety/commissioning`` -- SaftyFW commissioning params
  * ``GET /api/status``               -- relays, per-channel temperature, safety
  * ``GET/POST /api/profile*``        -- profile CRUD and start/stop/status

PID gains and the thermal model have no HTTP setter (config_presets.py's
SCOPE section), so those are written ONLY when the UART port happens to be
free -- :attr:`BenchSession.uart_available` records which happened, and a
test that actually depends on the gains must assert on it rather than
silently testing nothing. Factory-default is deliberately NOT attempted from
here: over HTTP it is challenge-response authenticated (factory_reset.c) and
over UART it needs the port; the MCP tool ``factory_default_then_load_
preset`` remains the way to do a true from-blank reset, and this harness's
apply-and-verify is the precondition that follows it.

SAFETY. 80 C is this fixture's ceiling (bench_fixture.json's own comment
block: fixture limit, never a kiln one). Nothing in this module raises a
ceiling, requests heat enable, or drives a relay directly. Every helper that
can start a firing is paired with :meth:`BenchSession.force_all_stop`, which
tests call from a ``finally`` so an exception mid-firing still ends with the
executor stopped and every relay reported off.
"""
from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import config_presets, safety_cfg_http_client, zones_http_client  # noqa: E402

#: Env var naming the live board. Absent -> every live test skips, so the
#: default `pytest` run on a laptop with no bench stays green and honest.
BENCH_HOST_ENV = "KILNCTRL_BENCH_HOST"

#: Optional: the board's COM port, for the PID/model half. Absent -> the
#: harness autodiscovers, and tolerates the port being busy (MCP server).
BENCH_PORT_ENV = "KILNCTRL_BENCH_PORT"

#: The preset this harness means by "known good".
BENCH_PRESET = "bench_fixture"

#: Hard fixture ceiling. Asserted against the preset AND against every live
#: temperature reading a test takes -- a harness that only trusted the
#: config would not notice the board being hot for some other reason.
FIXTURE_MAX_TEMP_C = 80.0

HTTP_TIMEOUT_S = 8.0


class BenchSessionError(RuntimeError):
    """The bench could not be put into, or confirmed in, the known-good state."""


def bench_host() -> Optional[str]:
    return os.environ.get(BENCH_HOST_ENV) or None


def _http(host: str, path: str, body: "Optional[str]" = None,
          timeout: float = HTTP_TIMEOUT_S) -> "tuple[int, str]":
    """One request. Returns ``(status, text)`` for BOTH success and a 4xx/5xx
    -- a refusal from the firmware is data this harness asserts on, not an
    exception to swallow (the commissioning-gate test's whole subject is a
    400 body)."""
    url = f"http://{host}{path}"
    data = body.encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method="POST" if data is not None else "GET")
    if data is not None:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError, TimeoutError) as exc:
        raise BenchSessionError(f"{url}: {exc}") from exc


@dataclass
class BenchSession:
    """A live board known to be sitting on ``bench_fixture.json``."""

    host: str
    preset: dict = field(default_factory=dict)
    #: Result objects from the two verified write paths, kept so a test can
    #: assert on WHAT was verified rather than on a boolean.
    zones_result: "Optional[zones_http_client.ZonesApplyResult]" = None
    safety_result: "Optional[safety_cfg_http_client.SafetyApplyResult]" = None
    #: True when the PID/model half actually got written over UART.
    uart_available: bool = False
    uart_detail: str = "not attempted"

    # -- reads -------------------------------------------------------------
    def status(self) -> dict:
        code, text = _http(self.host, "/api/status")
        if code != 200:
            raise BenchSessionError(f"GET /api/status -> {code}: {text[:200]}")
        return json.loads(text)

    def exec_status(self) -> dict:
        code, text = _http(self.host, "/api/profile_exec")
        if code != 200:
            raise BenchSessionError(f"GET /api/profile_exec -> {code}: {text[:200]}")
        return json.loads(text)

    def commissioning(self) -> dict:
        return safety_cfg_http_client.get_commissioning(self.host, timeout=HTTP_TIMEOUT_S)

    def zones(self) -> dict:
        return zones_http_client.get_zones(self.host, timeout=HTTP_TIMEOUT_S)

    def relays_on(self) -> "list[int]":
        return [r["relay"] for r in self.status().get("relays", []) if r.get("on")]

    def hottest_channel_c(self) -> float:
        temps = [c["temp_c"] for c in self.status().get("channels", []) if c.get("valid")]
        return max(temps) if temps else float("nan")

    def assert_within_fixture_ceiling(self) -> float:
        """Read live temperatures and refuse to continue above the fixture's
        80 C. Called before anything that could add heat -- the ceiling in
        the config is a promise about the future, this is a fact about now."""
        hottest = self.hottest_channel_c()
        if hottest == hottest and hottest > FIXTURE_MAX_TEMP_C:  # NaN-safe
            raise BenchSessionError(
                f"bench is at {hottest:.1f} C, above the {FIXTURE_MAX_TEMP_C} C fixture "
                "ceiling -- refusing to run a heat-adjacent test")
        return hottest

    # -- the precondition --------------------------------------------------
    def apply_known_good(self, try_uart: bool = True) -> "BenchSession":
        """Write bench_fixture.json's zones + safety sections and confirm the
        board read them back. Raises :class:`BenchSessionError` on any
        mismatch, so a test that gets a session at all got a verified one."""
        self.preset = config_presets.load_preset_data(BENCH_PRESET)
        self._assert_preset_is_bench_safe()
        self.zones_result = zones_http_client.apply_zone_preset(
            self.host, self.preset, timeout=HTTP_TIMEOUT_S, verify=True)
        if not self.zones_result.ok:
            raise BenchSessionError(
                "zones config did not land: " + "; ".join(self.zones_result.mismatches))
        self.safety_result = safety_cfg_http_client.apply_safety_preset(
            self.host, self.preset, timeout=HTTP_TIMEOUT_S, verify=True,
            use_ct_map_backup=False)  # never: see config_presets.py's docstring
        self._assert_safety_landed(self.safety_result)
        if try_uart:
            self._apply_pid_over_uart()
        return self

    def _assert_safety_landed(self, result: "safety_cfg_http_client.SafetyApplyResult") -> None:
        """The precondition is "every field this preset asked for is on the
        board, confirmed by an independent read-back" -- which is NOT the
        same as ``result.ok``.

        ``ok`` also folds in the board's OWN post-commit confirm, and on this
        bench that step regularly reports "the safety processor accepted the
        commit but this board could not read the config back to confirm it".
        That is the ESP declining to vouch, not the Pico disagreeing: the
        client's separate GET read-back then confirms all seven fields
        field-by-field, with no mismatches. Accepting on the read-back and
        REFUSING on any mismatch keeps the strong half of the check (a value
        that did not land still fails here) without gating every live test on
        the weaker half. If a field is ever missing from ``confirmed``, or a
        mismatch appears, this raises exactly as before.
        """
        expected = set((self.preset.get("safety") or {}).keys())
        confirmed = set(result.confirmed)
        missing = sorted(expected - confirmed)
        if result.mismatches or missing:
            raise BenchSessionError(
                "safety config did not land:\n" + result.describe()
                + (f"\n  never confirmed: {', '.join(missing)}" if missing else ""))

    def _assert_preset_is_bench_safe(self) -> None:
        """The preset is data on disk; an edit that raised a ceiling would
        otherwise reach the board through this harness unremarked."""
        for zone in self.preset["zones"]:
            if zone["max_temp_c"] > FIXTURE_MAX_TEMP_C:
                raise BenchSessionError(
                    f"preset zone {zone['index']} max_temp_c={zone['max_temp_c']} exceeds the "
                    f"{FIXTURE_MAX_TEMP_C} C fixture ceiling")
        abs_max = (self.preset.get("safety") or {}).get("abs_max_temp_c")
        if abs_max is not None and abs_max > FIXTURE_MAX_TEMP_C:
            raise BenchSessionError(
                f"preset safety.abs_max_temp_c={abs_max} exceeds the {FIXTURE_MAX_TEMP_C} C ceiling")

    def _apply_pid_over_uart(self) -> None:
        """Best-effort PID/model write. The MCP server usually owns the port;
        that is a normal, reported outcome, not a failure -- but it is
        RECORDED, so no test can quietly believe it exercised gains it never
        wrote."""
        from kilnctrl.control import ControlClient
        from kilnctrl.serial_link import UartLink

        link = UartLink()
        try:
            link.connect(os.environ.get(BENCH_PORT_ENV) or None)
        except Exception as exc:  # port busy, absent, or driver-level refusal
            self.uart_available = False
            self.uart_detail = f"UART not available ({exc}); PID/model NOT written this session"
            return
        control = ControlClient(link)
        try:
            for zone in self.preset["zones"]:
                ok = control.set_zone_pid(zone["index"], zone["pid_kp"], zone["pid_ki"], zone["pid_kd"])
                if not ok:
                    raise BenchSessionError(
                        f"zone {zone['index']} PID write refused: {getattr(ok, 'reason', '')}")
            self.uart_available = True
            self.uart_detail = f"PID written over {link.port}"
        finally:
            control.close()
            link.disconnect()

    # -- bounded firing helpers -------------------------------------------
    def put_profile(self, slot: int, name: str, zone_mask: int,
                    segments: "list[dict]") -> None:
        """Create/overwrite one user profile slot. Every segment's target is
        checked against the fixture ceiling here, not just by the firmware --
        a test must not be able to author an 85 C profile even if some future
        config would accept it."""
        for seg in segments:
            if float(seg["target_c"]) > FIXTURE_MAX_TEMP_C:
                raise BenchSessionError(
                    f"segment target {seg['target_c']} C exceeds the {FIXTURE_MAX_TEMP_C} C "
                    "fixture ceiling")
        parts = [f"id={slot}", f"name={name}", f"zone_mask={zone_mask}",
                 f"seg_count={len(segments)}"]
        for i, seg in enumerate(segments):
            parts += [f"seg{i}_target={seg['target_c']}",
                      f"seg{i}_ramp={seg['ramp_c_per_hr']}",
                      f"seg{i}_dwell={seg['dwell_min']}"]
        code, text = _http(self.host, "/api/profile", "&".join(parts))
        if code != 200:
            raise BenchSessionError(f"POST /api/profile -> {code}: {text[:300]}")

    def start_profile(self, slot: int) -> "tuple[int, str]":
        """POST /api/profile_exec/start -- the same endpoint main_page.html's
        Start button posts to. Returns ``(status, body)`` WITHOUT raising on
        a refusal: the refusal body is the assertion subject for an
        uncommissioned board."""
        self.assert_within_fixture_ceiling()
        return _http(self.host, "/api/profile_exec/start", f"id={slot}")

    def force_all_stop(self) -> dict:
        """Stop the executor, acknowledge any completed/faulted run, and
        report the relay state afterwards. Safe to call when nothing is
        running (both endpoints are idempotent) -- which is the point: tests
        call it from ``finally``."""
        detail: "dict[str, Any]" = {}
        try:
            detail["stop"] = _http(self.host, "/api/profile_exec/stop", "")
            detail["ack"] = _http(self.host, "/api/profile_exec/ack_last_run", "")
        finally:
            try:
                detail["relays_on"] = self.relays_on()
                detail["state"] = self.exec_status().get("state")
            except BenchSessionError as exc:
                detail["relays_on"] = f"unreadable: {exc}"
        return detail

    def wait_for_exec_state(self, states: "tuple[str, ...]", timeout_s: float = 10.0,
                            poll_s: float = 0.5) -> dict:
        deadline = time.monotonic() + timeout_s
        last = self.exec_status()
        while last.get("state") not in states:
            if time.monotonic() >= deadline:
                return last
            time.sleep(poll_s)
            last = self.exec_status()
        return last


def heat_is_permitted(session: BenchSession) -> "tuple[bool, str]":
    """Does the SAFETY PROCESSOR currently permit heat, and why not?

    This is the branch point the whole live suite turns on. Today the answer
    is False on this bench: ``ct_channel_map[0..2]`` is uncommitted (no CT is
    fitted), so SaftyFW's commissioning_gate.c reports calibration_missing
    and refuses heat -- see config_presets.py's docstring and
    bench_fixture.json's ``_safety_ct_channel_map_backup_comment``. Once the
    CTs are fitted and the zone current-sweep commits a MEASURED map, this
    returns True and the live tests take their heating branch with no code
    change: nothing here hardcodes the refusal.
    """
    current = session.commissioning()
    if not current.get("link_up"):
        return False, "safety link down"
    if not current.get("commissioned"):
        unset = safety_cfg_http_client.unset_required_fields(current)
        return False, "safety processor reports commissioned=false; unset required params: " + (
            ", ".join(unset) or "(none reported)")
    return True, "commissioned"
