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
import logging
import os
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import config_presets, safety_cfg_http_client, zones_http_client  # noqa: E402

log = logging.getLogger("bench_fixture_session")

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

#: Cooldown-wait defaults. The TOLERANCE, not the target, is the tunable
#: number here: the target itself is derived from a live ambient reading on
#: every call (see BenchSession.wait_for_cooldown). 3 C is about 4x this
#: bench's type-K + cold-junction noise, so it is reachable without being so
#: loose that a genuinely warm zone passes as cold.
COOLDOWN_DEFAULT_TOLERANCE_C = 3.0
#: 45 minutes. This jig rises 2.8-3.8 C/min under full duty and falls far
#: more slowly than that, so the budget has to be generous or the gate
#: becomes a coin flip. It is still a BUDGET: exceeding it raises rather
#: than proceeding onto residual heat.
COOLDOWN_DEFAULT_TIMEOUT_S = 2700.0
#: Poll period. The plant's time constant is ~167 s (measured on this bench,
#: 2026-08-29), so nothing is learned by asking more often, and every poll is
#: an HTTP round trip on a board that is also running a control loop.
COOLDOWN_DEFAULT_POLL_S = 20.0


class BenchSessionError(RuntimeError):
    """The bench could not be put into, or confirmed in, the known-good state."""


def cooldown_target_c(ambient_c: float, explicit_target_c: "Optional[float]",
                      tolerance_c: float) -> float:
    """The temperature a cooldown wait is waiting FOR -- pure, so the policy
    is host-testable without a board.

    An explicit target wins outright and is returned unchanged, INCLUDING a
    target below ambient. A caller who asks to wait for 20 C in a 34 C room
    gets exactly the wait -- and the eventual timeout -- they asked for; this
    function must not quietly substitute a reachable number for an
    unreachable requested one, because that substitution would be invisible
    in the result.

    Otherwise the target is ambient + tolerance. A non-finite ambient means
    the board reported no usable cold junction, so there is no honest target
    to compute and this refuses rather than falling back to a constant. That
    constant is precisely the failure this whole helper replaces: a
    hardcoded 25 C gate is unreachable in a room the owner reports at ~100 F.
    """
    if explicit_target_c is not None:
        return float(explicit_target_c)
    if ambient_c != ambient_c or ambient_c in (float("inf"), float("-inf")):
        raise BenchSessionError(
            "no valid cold-junction reading, so no ambient-relative cooldown target can be "
            "computed -- pass an explicit target_c if that is really what you want")
    if tolerance_c < 0.0:
        raise BenchSessionError(f"cooldown tolerance {tolerance_c} C is negative")
    return float(ambient_c) + float(tolerance_c)


def cooldown_reached(hottest_c: float, target_c: float) -> bool:
    """Has the bench cooled to the target? NaN is NOT cool -- a channel that
    stopped reporting must not read as a bench that finished cooling."""
    if hottest_c != hottest_c:
        return False
    return hottest_c <= target_c


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

    # -- ambient reference and the cooldown gate ---------------------------
    def channel_readings(self) -> "list[dict]":
        """Every VALID thermocouple channel as ``{channel, temp_c, cj_c}``.

        ``cj_c`` is the MAX31856's cold junction -- the die temperature of
        the converter on the thermocouple board. It is the only ambient
        reference this board publishes, and unlike a hardcoded room
        temperature it moves with the room: this bench sits at ~34 C cold
        junction while the owner reports ~100 F ambient.
        """
        out: "list[dict]" = []
        for ch in self.status().get("channels", []):
            if not ch.get("valid"):
                continue
            cj = ch.get("cj_c")
            out.append({"channel": ch.get("channel"), "temp_c": float(ch["temp_c"]),
                        "cj_c": None if cj is None else float(cj)})
        return out

    def ambient_reference_c(self) -> float:
        """The board's own "how warm is the room" number, read at call time.

        The MINIMUM valid cold junction, not the mean: a converter whose own
        channel has just been driven hot picks some of that heat up through
        the board, so the coolest cold junction is the least-contaminated
        estimate of the room. NaN when nothing valid is reporting, which
        callers must treat as "no target can be computed", never as 0.
        """
        cjs = [r["cj_c"] for r in self.channel_readings() if r["cj_c"] is not None]
        return min(cjs) if cjs else float("nan")

    def wait_for_cooldown(self, target_c: "Optional[float]" = None,
                          tolerance_c: float = COOLDOWN_DEFAULT_TOLERANCE_C,
                          timeout_s: float = COOLDOWN_DEFAULT_TIMEOUT_S,
                          poll_s: float = COOLDOWN_DEFAULT_POLL_S,
                          channels: "Optional[list[int]]" = None) -> dict:
        """Block until the bench has shed the previous test's heat.

        WHY THIS EXISTS. Every thermal measurement in this suite -- a step
        rise rate, an autotune fit, a cross-zone coupling gain -- measures a
        plant starting from somewhere. Run back to back with no gate, the
        second test starts on top of the first one's residual heat and
        reports a smaller step, a smaller gain and a shorter dead time: all
        wrong in the same direction, and none of them visibly so.

        WHAT "COOL" MEANS. Not a fixed number. ``target_c`` defaults to
        ``ambient_reference_c() + tolerance_c``, read at the moment of the
        call, so the gate is "this zone has come back to the room" -- which
        is the real precondition, and one that stays correct on a 100 F day.
        An explicit ``target_c`` overrides that entirely.

        HOT JUNCTION vs COLD JUNCTION, and why both appear. The cold junction
        supplies the REFERENCE (what is the room doing); the hot junctions
        supply the SUBJECT (has the element's heat dissipated). Judging each
        hot junction only against its OWN cold junction would be tighter in
        principle, but on this jig both sit on the same small board, so the
        cold junction lags the room upward during a firing and that
        comparison closes early -- exactly when it should not. Reference from
        the coolest cold junction; judge the hot junctions against it.

        NEVER SILENT. On timeout this raises, with the temperatures actually
        reached. A helper that returned "close enough" once its budget
        expired would hand the next test a hot start and a green tick.

        Batching note: this deliberately lives here and not behind
        ``kiln_call``. A multi-minute blocking wait does not belong in a
        ``kiln_batch`` round trip -- that tool's contract is one request, in
        order, stopping at the first failure. As a session method it composes
        the way a precondition should: a test (or a pytest fixture) calls it
        before the batch of hardware operations that needs a cold start.
        """
        deadline = time.monotonic() + timeout_s
        t0 = time.monotonic()
        readings = self.channel_readings()
        if not readings:
            raise BenchSessionError("no valid thermocouple channel -- cannot judge cooldown")
        ambient = self.ambient_reference_c()
        goal = cooldown_target_c(ambient, target_c, tolerance_c)
        history: "list[dict]" = []
        while True:
            watched = [r for r in readings if channels is None or r["channel"] in channels]
            if not watched:
                raise BenchSessionError(
                    f"none of channels {channels} are reporting valid readings")
            hottest = max(r["temp_c"] for r in watched)
            history.append({"t_s": round(time.monotonic() - t0, 1), "hottest_c": hottest})
            if hottest > FIXTURE_MAX_TEMP_C:
                raise BenchSessionError(
                    f"bench is at {hottest:.1f} C, above the {FIXTURE_MAX_TEMP_C} C fixture "
                    "ceiling, while waiting for it to cool")
            if cooldown_reached(hottest, goal):
                return {"reached": True, "hottest_c": hottest, "target_c": goal,
                        "ambient_c": ambient, "waited_s": round(time.monotonic() - t0, 1),
                        "samples": len(history), "history": history}
            if time.monotonic() >= deadline:
                raise BenchSessionError(
                    f"cooldown budget of {timeout_s:.0f} s expired with the bench still at "
                    f"{hottest:.2f} C, above the {goal:.2f} C target (ambient reference "
                    f"{ambient:.2f} C + {tolerance_c:.2f} C tolerance). Refusing to start a "
                    f"thermal test on residual heat; last samples: {history[-6:]}")
            time.sleep(poll_s)
            readings = self.channel_readings()

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
        """Stop the executor, abort any autotune, acknowledge any
        completed/faulted run, and report the relay state afterwards. Safe to
        call when nothing is running (every endpoint here is idempotent) --
        which is the point: tests call it from ``finally``.

        The autotune abort is here rather than only in the tuning test
        because the two heat owners are peers: ``profile_executor.c`` and
        ``autotune_engine.c`` each claim a zone's relays through
        relay_authority, and a teardown that stopped only one of them would
        assert "relays off" while the other was still free to close them.
        POST /api/autotune/abort answers "ok" whether or not a run is live,
        so calling it unconditionally costs one request and removes the
        "which heat owner was it this time?" question from every teardown.
        """
        detail: "dict[str, Any]" = {}
        try:
            detail["stop"] = _http(self.host, "/api/profile_exec/stop", "")
            detail["autotune_abort"] = _http(self.host, "/api/autotune/abort", "")
            detail["ack"] = _http(self.host, "/api/profile_exec/ack_last_run", "")
        finally:
            try:
                detail["relays_on"] = self.relays_on()
                detail["state"] = self.exec_status().get("state")
                detail["autotune_state"] = self.autotune_status().get("state")
            except BenchSessionError as exc:
                detail["relays_on"] = f"unreadable: {exc}"
        return detail

    # -- PID gains / thermal model / autotune -----------------------------
    #
    # All four read over HTTP. The module docstring's "PID gains have no HTTP
    # setter" is about config_presets.py's UART CONTROL path; GET/POST
    # /api/zones does carry kp/ki/kd and the model triple
    # (zones_http_client._PRESET_ZONE_OVERRIDE_FIELDS), which is how
    # apply_known_good() gets the preset's gains onto the board. So a test
    # CAN read back the gains it just wrote without the COM port -- and must,
    # because "did the autotune change them?" is a question about the value
    # on the board, not about the value in the preset.

    def zone_pid(self, index: int) -> "dict[str, float]":
        for zone in self.zones()["zones"]:
            if zone["index"] == index:
                return {k: float(zone[f"pid_{k}"]) for k in ("kp", "ki", "kd")}
        raise BenchSessionError(f"no zone {index} in GET /api/zones")

    def zone_model(self, index: int) -> "dict[str, float]":
        """The FOPDT model a step autotune writes on accept: k_dc, tau_s,
        dead_time_s. All zero on a board that has never had a fit accepted --
        which is a fact worth asserting, not a reason to skip reading it."""
        for zone in self.zones()["zones"]:
            if zone["index"] == index:
                return {k: float(zone[f"model_{k}"]) for k in ("k_dc", "tau_s", "dead_time_s")}
        raise BenchSessionError(f"no zone {index} in GET /api/zones")

    def autotune_status(self) -> dict:
        code, text = _http(self.host, "/api/autotune")
        if code != 200:
            raise BenchSessionError(f"GET /api/autotune -> {code}: {text[:200]}")
        return json.loads(text)

    def autotune_matrix(self) -> dict:
        """GET /api/autotune/matrix -- the cross-zone coupling matrix K, plus
        the Relative Gain Array derived from it in the same response.

        This is the board's OWN "zone interaction measurement", and it is not
        a thing a test can fake up from temperature samples: cell K[i][j] is
        the FOPDT fit of zone j's response to a duty step on zone i, and a
        row is filled only when an autotune run on zone i finishes with a
        valid fit. So an n x n RGA needs n zones autotuned, in the same power
        cycle -- the matrix lives in the engine's RAM, not in NVS.
        """
        code, text = _http(self.host, "/api/autotune/matrix")
        if code != 200:
            raise BenchSessionError(f"GET /api/autotune/matrix -> {code}: {text[:200]}")
        return json.loads(text)

    def start_autotune_step(self, zone_index: int, step_duty: float) -> "tuple[int, str]":
        """Start the OPEN-LOOP STEP test (autotune_engine.c's
        AUTOTUNE_METHOD_STEP), never the relay method.

        The relay method is not reachable from this harness on purpose. It
        requires ``setpoint_c`` and refuses any setpoint within
        AUTOTUNE_RELAY_SETPOINT_HEADROOM_C (50 C) of the zone's max_temp_c;
        with the fixture ceiling at 80 C that leaves setpoints below 30 C,
        which is under this bench's own ambient. The relay method is
        therefore not runnable here at all, and a helper that offered it
        would only produce a refusal that looks like a bug.

        Live temperatures are checked before the request, exactly as
        start_profile() does: this endpoint commands real duty.
        """
        if not 0.0 < step_duty <= 1.0:
            raise BenchSessionError(f"step_duty {step_duty} outside (0, 1]")
        self.assert_within_fixture_ceiling()
        return _http(self.host, "/api/autotune/start",
                     f"zone={zone_index}&method=step&step_duty={step_duty}")

    def abort_autotune(self) -> "tuple[int, str]":
        return _http(self.host, "/api/autotune/abort", "")

    def wait_for_autotune_state(self, states: "tuple[str, ...]", timeout_s: float,
                                poll_s: float = 2.0) -> dict:
        """Bounded wait. Returns the LAST status either way -- a timeout is
        an outcome a tuning test asserts on (an engine that never leaves
        'settling' is the failure this bench has seen before), not an
        exception that hides which state it was stuck in."""
        deadline = time.monotonic() + timeout_s
        last = self.autotune_status()
        while last.get("state") not in states:
            if time.monotonic() >= deadline:
                return last
            time.sleep(poll_s)
            last = self.autotune_status()
            hottest = self.hottest_channel_c()
            if hottest == hottest and hottest > FIXTURE_MAX_TEMP_C:
                raise BenchSessionError(
                    f"live temperature {hottest:.1f} C breached the {FIXTURE_MAX_TEMP_C} C "
                    "fixture ceiling while waiting for autotune state")
        return last

    def autotune_trace(self) -> "list[tuple[float, float]]":
        """The engine's own recorded trace (elapsed_s, measurement_c). Empty
        until the run is STEPPING -- the settle phase records nothing."""
        code, text = _http(self.host, "/api/autotune/trace.csv")
        if code != 200:
            raise BenchSessionError(f"GET /api/autotune/trace.csv -> {code}: {text[:200]}")
        rows = []
        for line in text.splitlines()[1:]:
            parts = line.split(",")
            if len(parts) >= 2:
                rows.append((float(parts[0]), float(parts[1])))
        return rows

    # -- step-response sampling -------------------------------------------
    def sample_response(self, duration_s: float, period_s: float = 2.0,
                        zone_index: int = 0, on_sample=None) -> "list[dict]":
        """Poll the board for ``duration_s`` and return one row per poll.

        This is the PV trace a step test is actually about, taken from the
        board's own /api/status and /api/profile_exec rather than from any
        model: measured temperature, which relays are closed, whether the
        safety processor says heating is enabled, and what the executor
        thinks its setpoint is.

        The fixture ceiling is asserted on EVERY sample, not once at the end
        -- the whole point of sampling during a heat-adjacent operation is to
        catch the excursion while it is happening. A breach raises, which
        puts the caller into its ``finally`` and force_all_stop().

        SAFETY ORDERING (S1/S2 fix -- do not reorder this again). The
        ceiling check runs on ``hottest`` the instant it is known, straight
        off ``self.status()``, with NOTHING else -- no second HTTP call, no
        callback -- between that reading and the ``raise``. Before this fix,
        ``self.exec_status()`` (its own ~8s-timeout urlopen) ran ahead of the
        raise: a breach on a genuinely hung board could sit ~8s with the
        heaters still on before force_all_stop() ever got called, and if
        exec_status() itself raised first the breach was never even
        detected. ``exec_status()`` and ``duty_by_zone`` are only gathered,
        and ``on_sample`` only called, for a row that is NOT the breach --
        the trace does not need the breach row (see the docstring on
        ``stage_profile_tracking._on_sample`` for what the caller already
        does with the last good row plus the error message).

        ``on_sample``, when given, is called with every NON-breach row.
        Wrapped in try/except: an exception inside a caller's callback (a
        malformed ``channels_c``, a MemoryError on a very long run) must
        never propagate from here in place of a real ceiling breach --
        before this fix that exact failure mode discarded the whole partial
        trace the callback exists to preserve, no different from the bug
        this callback was added to fix in the first place. It is logged and
        swallowed instead.
        """
        rows: "list[dict]" = []
        deadline = time.monotonic() + duration_s
        t0 = time.monotonic()
        #: A persistently-throwing on_sample must not flood the log once
        #: per poll for the rest of a multi-hour run -- log the first
        #: occurrence at full detail (so the cause is diagnosable) and every
        #: one after that as a single-line count, not a new traceback.
        on_sample_error_count = 0
        while True:
            status = self.status()
            # Keyed by the channel's OWN index, not by position in a filtered
            # list. The two differ the moment any channel reports invalid,
            # and the positional form silently relabels every channel after
            # the gap -- which on a cross-zone measurement would attribute
            # zone 2's rise to zone 1.
            chan_c = {int(c["channel"]): float(c["temp_c"])
                      for c in status.get("channels", []) if c.get("valid")}
            temps = list(chan_c.values())
            hottest = max(temps) if temps else float("nan")
            # THE RAISE COMES FIRST. Nothing above this line is a second
            # network call, and nothing below it runs before the raise.
            # ``channels_c``/``hottest_c`` are attached to the exception --
            # pure computation on data already in hand (chan_c came from the
            # SAME status() response as hottest, no extra I/O), so a caller
            # can identify which channel actually tripped the ceiling
            # without that costing the raise any delay.
            if hottest == hottest and hottest > FIXTURE_MAX_TEMP_C:
                exc = BenchSessionError(
                    f"live temperature {hottest:.1f} C breached the {FIXTURE_MAX_TEMP_C} C "
                    "fixture ceiling during sampling")
                exc.channels_c = dict(chan_c)
                exc.hottest_c = hottest
                raise exc
            exec_st = self.exec_status()
            # Per-zone duty/output command, best-effort: not every firmware
            # build's /api/profile_exec carries this yet, so it is pulled
            # defensively and left absent (never fabricated) when it is not
            # there. Units are normalized and recorded explicitly (S4/item 4
            # fix) -- "duty" (a live 0-1 fraction) and "output_pct" (0-100)
            # are NOT the same number, and a caller comparing one against a
            # threshold meant for the other silently misjudges "at clamp".
            #
            # THE KEY IS "zone", NOT "index". append_zone_status_json()
            # (firmware/KilnFW/App/drivers/dashboard_http.c:1220-1258, shared
            # by /api/profile_exec and /api/control) emits
            # {"zone":%u,...,"duty":%.3f,...} -- "index" is a DIFFERENT
            # endpoint's config key (/api/zones, zones_http.c:4184/4210) and
            # never appears here. Getting this wrong left duty_by_zone == {}
            # on every real-hardware row, silently killing the whole
            # full_power-vs-commanded_off diagnosis summarize_breach()
            # exists to make (see test_real_sample_response_duty_by_zone_
            # uses_firmware_zone_key in test_pid_validation.py for the
            # regression proof). "duty" is already a 0-1 fraction on the
            # wire (profile_executor.c:130 multiplies by 100 only for its
            # OWN display-percent conversion, not the JSON value) --
            # "fraction_0_1" is correct. Nothing in the firmware currently
            # emits "output_pct"; that branch is dead code today, kept only
            # in case a future/alternate build adds a percent-based field
            # under that name.
            duty_by_zone = {}
            for z in (exec_st.get("zones") or status.get("zones") or []):
                idx = z.get("zone")
                if idx is None:
                    continue
                raw_duty = z.get("duty")
                if raw_duty is not None:
                    duty_by_zone[int(idx)] = {"value": raw_duty, "unit": "fraction_0_1"}
                    continue
                raw_pct = z.get("output_pct")  # dead today -- see comment above
                if raw_pct is not None:
                    duty_by_zone[int(idx)] = {"value": raw_pct, "unit": "percent_0_100"}
            row = {
                "t_s": round(time.monotonic() - t0, 2),
                "zone_c": chan_c.get(zone_index, float("nan")),
                # EVERY zone's reading on every sample. Free (one /api/status
                # already carries them all) and it is the raw material of a
                # cross-zone coupling measurement, which cannot be
                # reconstructed afterwards from the zone-under-test alone.
                "channels_c": chan_c,
                "hottest_c": hottest,
                "relays_on": [r["relay"] for r in status["relays"] if r["on"]],
                "safety_heating_enabled": status["safety_heating_enabled"],
                # K4 -- the contact that decides whether any element current
                # flows. Sampled alongside the ARMED flag because the two are
                # routinely confused, and only this one answers "is heat
                # actually getting through".
                "safety_relay_energized": status["safety_relay_energized"],
                "heat_block_sources_words": status.get("heat_block_sources_words"),
                "exec_state": exec_st.get("state"),
                "target_c": exec_st.get("target_c"),
                "segment_index": exec_st.get("segment_index"),
                "duty_by_zone": duty_by_zone,
            }
            rows.append(row)
            if on_sample is not None:
                try:
                    on_sample(row)
                except Exception:  # noqa: BLE001 - see docstring: never mask a real breach
                    on_sample_error_count += 1
                    if on_sample_error_count == 1:
                        log.exception("on_sample callback raised -- swallowed so it cannot "
                                      "mask or delay a ceiling breach on the next iteration")
                    elif on_sample_error_count % 100 == 0:
                        # Rate-limited: a callback that keeps throwing for a
                        # multi-hour run must not write one traceback per
                        # poll -- one full traceback up front, then a
                        # count every 100 polls so a long-running failure
                        # is still visible without flooding the log.
                        log.warning("on_sample callback has now raised %d times this run "
                                   "(suppressing individual tracebacks)", on_sample_error_count)
            if time.monotonic() >= deadline:
                return rows
            time.sleep(period_s)

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
