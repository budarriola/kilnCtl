#!/usr/bin/env python3
"""capability_preflight.py -- ask a board what it actually supports BEFORE a
long unattended run starts, instead of finding out mid-campaign from a
Python traceback.

THE INCIDENT THIS EXISTS FOR: commit 906d026 added ``ramp_assist_enabled``
as a REQUIRED preset field (config_presets.py) whose apply path
(``apply_preset(..., zones_host=...)``) calls ``POST /api/ramp_assist``. A
board running firmware built before that endpoint existed answered with
``{"ok":false,"error":"no such endpoint"}``, and the campaign died mid-
unattended-run. ``ramp_assist_http_client.pin_enabled()`` already knows how
to tell a FATAL missing capability (preset pins ``True``, firmware cannot
provide it) from a BENIGN one (preset pins ``False``, which firmware that
has never heard of the feature trivially satisfies) -- but nothing calls
that check before a run starts. This module is that check, generalised
beyond ramp_assist to any HTTP capability a preset's apply path might need,
and run as a PREFLIGHT rather than discovered by falling over.

HOW A CAPABILITY IS DETECTED: by probing the specific endpoint the apply
path would call, the same way ``ramp_assist_http_client._is_no_such_endpoint_
error()`` already does for that one case -- a GET against the endpoint,
checked against the board's own precise ``{"ok":false,"error":"no such
endpoint"}`` body (never against HTTP status alone, so an unrelated 4xx/5xx
never gets misread as "board is fine, feature is just off"). ``GET /api/
status`` is also read once per preflight and surfaced in the report
(``fw_version``/``fw_build``/``self_protocol_version`` when the board knows
them) -- useful context for an operator deciding whether a reflash is the
right move, but NOT used to decide fatal/benign: this project has no
firmware-version-to-feature-set table anywhere, and building one would be
exactly the kind of hand-maintained mapping that rots the moment a preset
gains a field (see DERIVATION below). Endpoint probing needs no such table:
it asks the board directly, every time.

HOW REQUIRED CAPABILITIES ARE DERIVED FROM A PRESET: automatically, for the
part that can be automatic. ``derive_required_capabilities()`` walks a small
manifest (``_CAPABILITY_MANIFEST``) of ``{preset field name: Capability}``
against the ACTUAL preset dict handed to it -- a capability is only required
when its trigger field is present in the preset AND the apply-path
precondition that would actually reach the endpoint holds (e.g. ramp_assist
is only pinned over HTTP when ``zones_host`` is given -- see
``config_presets.apply_preset()``). Adding a new PRESET (a new JSON file) or
a new plain field on an EXISTING write path needs no code change here at
all -- the manifest is keyed on field names, not preset names, and the same
handful of fields recur across every preset.

THE ONE MANUAL STEP THAT REMAINS, stated plainly because a fully automatic
derivation is not achievable: when ``config_presets.apply_preset()`` (or
another apply path) gains a NEW HTTP write for a NEW preset field -- the
same kind of change 906d026 made -- a matching ``Capability`` entry must be
added to ``_CAPABILITY_MANIFEST`` by hand, in the same commit. Nothing here
can infer "this Python code now calls a new endpoint" from the preset
schema alone; the schema is just JSON, and the mapping from a JSON field to
an HTTP call lives in ``apply_preset()``'s logic, not in any data this
module can walk. This is the same shape of maintenance burden as
``_REQUIRED_ZONE_FIELDS``/``_REQUIRED_TOP_FIELDS`` in config_presets.py
already carries, kept in ONE place (this manifest) rather than duplicated,
and covered by ``test_capability_preflight.py``'s
``test_manifest_matches_apply_preset_ramp_assist_pin`` as a tripwire: that
test fails loudly if ``apply_preset`` starts pinning ramp_assist under a
different condition than this manifest assumes.

FATAL VS. BENIGN: a missing capability is BENIGN exactly when the preset's
own pinned value is what firmware lacking the feature already does by
default (``Capability.benign_when(preset_value)`` -- for ramp_assist that is
``preset_value is False``, mirroring ``pin_enabled()``'s own rule). It is
FATAL when the preset pins a state the board cannot provide -- the exact
906d026 failure mode. A capability with no ``benign_when`` predicate is
always fatal when missing (there is no trivially-satisfied-by-absence case
for it)."""
from __future__ import annotations

import json
import logging
import urllib.error
import urllib.request

from . import host_resolve, http_auth
from dataclasses import dataclass, field
from typing import Callable, Optional

_module_log = logging.getLogger(__name__)

#: Same "board's fallback-AP address" default every other HTTP client in
#: this package uses (ramp_assist_http_client.RAMP_ASSIST_AP_DEFAULT_HOST,
#: dashboard_http_client.DASHBOARD_AP_DEFAULT_HOST, ...).
PREFLIGHT_AP_DEFAULT_HOST = host_resolve.resolve_default_host()  # was a hardcoded "192.168.4.1"
PREFLIGHT_HTTP_TIMEOUT_S = 8.0

#: The board's own precise error body for a route the httpd never
#: registered -- matched verbatim, never on HTTP status alone, so an
#: unrelated 4xx/5xx is never misread as "feature absent". Identical string
#: to ramp_assist_http_client._NO_SUCH_ENDPOINT_ERROR; kept as a separate
#: literal (not imported) so this module has no import-time dependency on
#: any one feature's client -- it probes raw HTTP itself.
_NO_SUCH_ENDPOINT_ERROR = "no such endpoint"


class PreflightTransportError(Exception):
    """A capability probe or the /api/status read could not get an answer
    from the board at all -- unreachable host, timeout, non-JSON body, or
    any HTTP status/error that is NOT the board's own precise "no such
    endpoint" shape. Distinct from "capability absent": this means the
    board did not answer the question, not that it answered "no"."""


def _url(host: str, path: str) -> str:
    return f"http://{host}{path}"


def _get_json(host: str, path: str, timeout: float) -> "tuple[Optional[dict], Optional[str]]":
    """GET ``path``. Returns ``(decoded_json_or_None, raw_body_text)`` for a
    clean 2xx response OR for the board's own precise "no such endpoint"
    body (that specific shape is a successful transport result -- the board
    answered, just in the negative -- so it is returned rather than
    raised). Raises :class:`PreflightTransportError` for everything else:
    unreachable host, timeout, a non-JSON body, or ANY OTHER non-2xx status
    -- including a 4xx/5xx that happens to carry a JSON body, which must
    NOT be misread as "capability present" just because it parsed. Only the
    board's own exact ``{"ok":false,"error":"no such endpoint"}`` shape is
    treated as a meaningful negative answer; every other error status is
    "board did not answer this question", full stop."""
    req = urllib.request.Request(_url(host, path), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        try:
            body_text = exc.read().decode("utf-8", errors="replace")
        except Exception:
            raise PreflightTransportError(f"GET {path} failed: HTTP {exc.code}") from exc
        try:
            data = json.loads(body_text)
        except Exception:
            data = None
        if not _is_no_such_endpoint(data):
            raise PreflightTransportError(
                f"GET {path} failed: HTTP {exc.code}: {body_text!r}") from exc
        return data, body_text
    except urllib.error.URLError as exc:
        raise PreflightTransportError(f"GET {path} unreachable: {exc.reason}") from exc
    except Exception as exc:  # noqa: BLE001
        raise PreflightTransportError(f"GET {path} failed: {exc}") from exc
    try:
        return json.loads(body_text), body_text
    except Exception:
        return None, body_text


def _is_no_such_endpoint(data: Optional[dict]) -> bool:
    return (
        isinstance(data, dict)
        and data.get("ok") is False
        and data.get("error") == _NO_SUCH_ENDPOINT_ERROR
    )


@dataclass(frozen=True)
class Capability:
    """One probeable HTTP feature. ``name`` is what shows up in reports and
    what preset/manifest code refers to it by; ``probe_path`` is the GET
    endpoint whose absence signature (``{"ok":false,"error":"no such
    endpoint"}``) is checked; ``description`` is the operator-facing
    sentence explaining what the feature does; ``benign_when`` (optional)
    decides whether a MISSING capability is tolerable for a given preset
    value -- omit it for a capability that is always fatal when absent."""

    name: str
    probe_path: str
    description: str
    benign_when: Optional[Callable[[object], bool]] = None


#: preset field name -> (Capability, precondition). The precondition
#: mirrors the actual gating in config_presets.apply_preset(): a preset can
#: carry ``ramp_assist_enabled`` and still never reach the HTTP write if the
#: caller didn't pass ``zones_host`` (see that function's ``not_written``
#: accounting) -- so a preflight that ignored the precondition would flag a
#: capability the run was never going to touch. See this module's docstring
#: for what "add an entry here" means and when it is required.
_CAPABILITY_MANIFEST: "dict[str, tuple[Capability, Callable[[bool, bool], bool]]]" = {
    "ramp_assist_enabled": (
        Capability(
            name="ramp_assist",
            probe_path="/api/ramp_assist",
            description=(
                "kiln-wide ramp-assist on/off flag (ramp_assist_cfg.h, "
                "POST /api/ramp_assist) -- lets the executor stretch a ramp "
                "or shorten a dwell when the kiln can't keep up with the "
                "commanded rate"
            ),
            benign_when=lambda preset_value: preset_value is False,
        ),
        # precondition(have_zones_host, have_safety_host): apply_preset()
        # only calls POST /api/ramp_assist when zones_host is given.
        lambda have_zones_host, have_safety_host: have_zones_host,
    ),
}


@dataclass(frozen=True)
class CapabilityCheck:
    capability: Capability
    required: bool
    present: Optional[bool]  # None when the board never answered (unreachable)
    preset_value: object
    fatal: bool
    message: str


# The four /api/readiness item keys the board's own firing interlock refuses
# on (firmware/KilnFW/App/drivers/safety/readiness_gate.h's
# READINESS_GATE_KEY_*). Keys only -- the decision for each item is made
# firmware-side by one shared predicate and read from the wire here, so this
# is a pointer at the rule, not a copy of it. Everything NOT in this set on
# /api/readiness is advisory and must not fail a preflight.
READINESS_BLOCKING_KEYS = frozenset(
    {"recovery_mode", "safety_trip", "crash_report", "estop_verified"}
)

# Subset of READINESS_BLOCKING_KEYS that only matters to a run that STARTS a
# firing or autotune (heat). The firmware still refuses heat starts on it
# (readiness_gate.h, unchanged); PC-side, a non-heat run proceeds past it and
# heat cases SKIP (reason ``estop_unverified``) instead. Owner decision
# 2026-10-10: bench testing must not wait on the physical E-stop check, which
# is NEVER marked verified from tooling.
HEAT_ONLY_BLOCKING_KEYS = frozenset({"estop_verified"})
ESTOP_UNVERIFIED_LINE = "E-stop not physically verified: heat cases skipped"


@dataclass(frozen=True)
class BoardInfo:
    reachable: bool
    fw_version: Optional[str] = None
    fw_build: Optional[str] = None
    self_protocol_version: Optional[int] = None
    error: str = ""
    # reset_reason/uptime_s: straight from the same /api/status body already
    # read for fw identity -- no extra probe. crash_unacknowledged/
    # crash_summary come from a second, always-attempted GET /api/
    # crash_report: this is the field the 2026-08-31 incident showed going
    # unread everywhere -- present:true, acknowledged:false sat in the API
    # the whole afternoon a board ran five hours post-panic with nobody
    # noticing, including through this very preflight before this fix.
    # NEVER acknowledge or clear it from here -- this module only reads.
    reset_reason: Optional[str] = None
    uptime_s: Optional[float] = None
    crash_unacknowledged: bool = False
    crash_summary: Optional[str] = None
    # THE READINESS FIRING INTERLOCK (owner decision 2026-09-09,
    # firmware/KilnFW/App/drivers/safety/readiness_gate.h). Firmware now
    # REFUSES a start on four /api/readiness items: recovery_mode,
    # safety_trip, crash_report and estop_verified. Reading the same
    # endpoint here means this preflight refuses BEFORE the board does,
    # with the same reason -- instead of a run script cheerfully POSTing a
    # start and reporting an opaque 409 several steps later.
    #
    # Deliberately NOT a second copy of the rule: this reads each item's
    # own rendered `status` from the board and refuses on not_done. The
    # decision stays firmware-side, exactly once, in readiness_gate.h. If
    # this file grew its own "is the board tripped?" logic it would be the
    # third copy of a rule that already has a documented drift hazard.
    #
    # An older firmware with no /api/readiness route (or one whose items
    # this build predates) leaves this empty and changes nothing -- same
    # tolerance get_board_info() already applies to /api/crash_report.
    readiness_blocked: "tuple[tuple[str, str, str], ...]" = ()
    # Same shape, but only the HEAT_ONLY_BLOCKING_KEYS items (estop_verified):
    # these do NOT make PreflightReport.ok False; they make ok_for_heat False.
    heat_blocked: "tuple[tuple[str, str, str], ...]" = ()
    # Safety reads (/api/crash_report, /api/readiness) that got NO usable answer
    # -- 401 (both routes are admin tier), 5xx, timeout, non-JSON. That is
    # "could not determine", never "no crash"/"not blocked": PreflightReport.ok
    # refuses on it unless the caller passes the explicit allow_undetermined
    # override. A board whose firmware predates a route answers the exact
    # "no such endpoint" shape, which is NOT undetermined (see _get_json).
    undetermined: "tuple[str, ...]" = ()


@dataclass(frozen=True)
class PreflightReport:
    preset_name: str
    host: str
    board: BoardInfo
    checks: "list[CapabilityCheck]" = field(default_factory=list)
    # Task-liveness cross-check (task_liveness.py's check_task_liveness()
    # result), supplied by the caller -- this module has no link/serial
    # access of its own, only HTTP, so it cannot compute this itself. None
    # means the caller did not check (e.g. no link available); an empty-
    # but-present TaskLivenessReport with dead/absent tasks blocks the run
    # exactly like an unacknowledged crash report does, unless
    # ``allow_missing_tasks`` is set.
    task_liveness: "object" = None
    allow_missing_tasks: bool = False
    # Non-empty when the caller TRIED the task-liveness check and could not
    # read it (no link, malformed reply, unparseable script): the reason.
    # Refuses like an undetermined board read unless allow_undetermined.
    task_liveness_unavailable: str = ""
    allow_undetermined: bool = False

    @property
    def undetermined_reads(self) -> "tuple[str, ...]":
        out = list(self.board.undetermined)
        if self.task_liveness_unavailable:
            out.append(f"task_liveness ({self.task_liveness_unavailable})")
        return tuple(out)

    @property
    def ok(self) -> bool:
        """False if the board never answered at all, if any required
        capability is FATALLY missing, if the board is carrying an
        unacknowledged crash report, if any of the four blocking
        /api/readiness items is red, or (unless ``allow_missing_tasks``) if
        the task-liveness cross-check found a required task dead or absent.
        The crash/readiness/task-liveness checks are deliberately checked
        regardless of what the preset needs -- a panic five hours ago, an
        unverified E-stop interlock, or a task that silently failed to
        start at boot is a reason to not start ANY unattended run, not just
        ones that happen to probe a capability. The readiness items are
        also what the BOARD itself will refuse on (readiness_gate.h), so a
        run that skipped this check would simply be refused a few steps
        later with less context."""
        if not self.board.reachable:
            return False
        if self.board.crash_unacknowledged:
            return False
        if self.board.readiness_blocked:
            return False
        if self.undetermined_reads and not self.allow_undetermined:
            return False
        if (
            self.task_liveness is not None
            and not self.task_liveness.ok
            and not self.allow_missing_tasks
        ):
            return False
        return not any(c.fatal for c in self.checks)

    @property
    def ok_for_heat(self) -> bool:
        """``ok`` AND no heat-only readiness item (estop_verified) is red.
        Anything that starts a firing/autotune must use this, not ``ok``."""
        return self.ok and not self.board.heat_blocked

    @property
    def fatal_checks(self) -> "list[CapabilityCheck]":
        return [c for c in self.checks if c.fatal]

    def describe(self) -> str:
        """Human-readable report: legible to an operator who has not read
        this module's code. Named for exactly what CLAUDE.md's constraint 3
        asked for -- missing capability, what it means, and that a reflash
        is the remedy."""
        lines = [f"capability preflight for preset {self.preset_name!r} against {self.host}:"]
        if not self.board.reachable:
            lines.append(f"  BOARD UNREACHABLE: {self.board.error}")
            lines.append("  -- no capability could be checked; do not start this run.")
            return "\n".join(lines)
        fw = self.board.fw_version or "(unknown)"
        build = self.board.fw_build or "(unknown)"
        proto = self.board.self_protocol_version
        proto_s = str(proto) if proto is not None else "(unknown)"
        lines.append(f"  board firmware: version={fw} build={build} link_protocol={proto_s}")
        lines.append(
            f"  reset_reason={self.board.reset_reason!r} uptime_s={self.board.uptime_s!r}"
        )
        if self.board.crash_unacknowledged:
            lines.append(
                "  [FATAL]  UNACKNOWLEDGED CRASH REPORT ON BOARD -- "
                f"{self.board.crash_summary}. This board panicked and nobody has "
                "acknowledged it (GET /api/crash_report). Do not start this run: "
                "either the crash is unrelated to this firing and should be "
                "reviewed and acknowledged, or it is exactly the failure mode "
                "this run would repeat. REMEDY: investigate, then "
                "POST /api/crash_report/ack once reviewed."
            )
        if self.board.heat_blocked:
            lines.append(f"  [WARN]   {ESTOP_UNVERIFIED_LINE} "
                         "(firmware still refuses firing/autotune starts; non-heat runs proceed).")
        for key, label, detail in self.board.readiness_blocked:
            lines.append(
                f"  [FATAL]  READINESS ITEM BLOCKS FIRING: {label} ({key}) -- {detail} "
                "The board's own firing interlock refuses a start on this item "
                "(firmware/KilnFW/App/drivers/safety/readiness_gate.h) and there is NO "
                "override. REMEDY: clear this item (see /readiness on the board), then "
                "start again."
            )
        if self.task_liveness is not None:
            tl = self.task_liveness
            if tl.fault_dead or tl.fault_absent:
                severity = "[FATAL]" if not self.allow_missing_tasks else "[allowed]"
                if tl.fault_dead:
                    lines.append(
                        f"  {severity} TASK(S) DEAD (registered, not running -- task "
                        f"creation failed this boot): {', '.join(sorted(tl.fault_dead))}"
                    )
                if tl.fault_absent:
                    lines.append(
                        f"  {severity} TASK(S) ABSENT (never registered on this board -- "
                        f"older firmware or a code regression): {', '.join(sorted(tl.fault_absent))}"
                    )
            if tl.info_dead:
                lines.append(
                    f"  [info]   task(s) dead but by design (config/hardware-conditional, "
                    f"on-demand, or a one-shot boot task that has since self-deleted -- "
                    f"not a fault): {', '.join(sorted(tl.info_dead))}"
                )
            if tl.info_absent:
                lines.append(
                    f"  [info]   task(s) absent but by design (config/hardware-conditional "
                    f"or on-demand -- not a fault): {', '.join(sorted(tl.info_absent))}"
                )
            if not (tl.fault_dead or tl.fault_absent or tl.info_dead or tl.info_absent):
                lines.append(f"  [ok]     task liveness: all {len(tl.expected)} expected task(s) alive")
        else:
            lines.append(
                "  [skip]   task liveness: not checked (no link) -- this preflight has no "
                "live UART link to the board, or the required-task list could not be "
                "loaded, so a dead or absent required task on this boot would not be "
                "caught here."
            )
        if not self.checks:
            lines.append("  no HTTP-gated capabilities required by this preset/apply plan.")
        for c in self.checks:
            if not c.required:
                continue
            if c.present:
                lines.append(f"  [ok]     {c.capability.name}: present")
            elif c.fatal:
                lines.append(
                    f"  [FATAL]  {c.capability.name}: MISSING -- {c.capability.description}. "
                    f"This preset needs it (pinned value={c.preset_value!r}) and this board's "
                    f"firmware does not have it. REMEDY: reflash this board with firmware that "
                    f"includes {c.capability.probe_path}, or change the preset."
                )
            else:
                lines.append(
                    f"  [benign] {c.capability.name}: missing, but trivially satisfied -- "
                    f"preset pins {c.preset_value!r}, which is what firmware lacking "
                    f"{c.capability.description} already does. No action needed."
                )
        task_liveness_blocks = (
            self.task_liveness is not None
            and not self.task_liveness.ok
            and not self.allow_missing_tasks
        )
        if self.undetermined_reads:
            lines.append(
                "  [UNDETERMINED] could not read: " + ", ".join(self.undetermined_reads)
                + " -- a failed read is NOT treated as a clean board."
            )
        if self.board.readiness_blocked:
            names = ", ".join(k for k, _l, _d in self.board.readiness_blocked)
            lines.append(
                f"  RESULT: the board's firing interlock blocks on {names} -- "
                "this run would be refused; do not start it."
            )
        elif self.undetermined_reads and not self.allow_undetermined:
            lines.append(
                "  RESULT: safety state could not be determined -- do not start this run. "
                "Fix the read (admin login, board health) or pass allow_undetermined=True once reviewed."
            )
        elif task_liveness_blocks:
            lines.append(
                "  RESULT: required task(s) dead or absent -- do not start this run. "
                "Pass allow_missing_tasks=True to override once reviewed."
            )
        elif self.board.crash_unacknowledged and self.fatal_checks:
            lines.append(
                f"  RESULT: unacknowledged crash report AND {len(self.fatal_checks)} "
                "FATAL capability gap(s) -- do not start this run."
            )
        elif self.board.crash_unacknowledged:
            lines.append("  RESULT: unacknowledged crash report -- do not start this run.")
        elif self.fatal_checks:
            lines.append(
                f"  RESULT: {len(self.fatal_checks)} FATAL capability gap(s) -- do not start this run."
            )
        else:
            lines.append("  RESULT: ok to start.")
        return "\n".join(lines)


def get_board_info(host: str, timeout: float = PREFLIGHT_HTTP_TIMEOUT_S) -> BoardInfo:
    """GET /api/status once, for the operator-facing firmware identity
    fields (dashboard_http.c: fw_version/fw_build/self_protocol_version),
    PLUS reset_reason/uptime_s from that same body, PLUS one more GET of
    /api/crash_report -- so an unacknowledged crash is ALWAYS part of board
    identity, never something a separate tool has to be remembered and
    called. fw_version/fw_build/self_protocol_version stay purely
    informational (see this module's docstring for why); crash_unacknowledged
    is NOT informational -- see PreflightReport.ok and describe() below,
    where it makes the whole preflight fail.

    This function only ever reads /api/crash_report. It must never call
    /api/crash_report/ack or /api/crash_report/clear -- doing so from a
    preflight would silence the very evidence an operator needs to see."""
    try:
        data, _raw = _get_json(host, "/api/status", timeout)
    except PreflightTransportError as exc:
        return BoardInfo(reachable=False, error=str(exc))
    if not isinstance(data, dict):
        return BoardInfo(reachable=False, error="GET /api/status did not return a JSON object")

    crash_unacknowledged = False
    crash_summary = None
    undetermined: "list[str]" = []
    try:
        crash, _raw2 = _get_json(host, "/api/crash_report", timeout)
    except PreflightTransportError as exc:
        # Board answered /api/status but not /api/crash_report -- older
        # firmware without this endpoint, most likely. Not fatal on its
        # own: there is no crash data to act on, so this preflight has
        # nothing to refuse over. (A firmware new enough to HAVE a crash
        # but too old to report it is not a case any client-side check can
        # detect.)
        crash = None
        undetermined.append(f"crash_report ({exc})")
    else:
        if not isinstance(crash, dict):
            undetermined.append("crash_report (response was not a JSON object)")
    # A missing `acknowledged` on a present record is NOT acknowledged.
    if isinstance(crash, dict) and crash.get("present") and crash.get("acknowledged") is not True:
        crash_unacknowledged = True
        crash_summary = (
            f"exc_task={crash.get('exc_task')!r} "
            f"exc_cause_str={crash.get('exc_cause_str')!r} "
            f"reset_reason={crash.get('found_on_boot_reset_reason')!r}"
        )
        if crash.get("stale_image") is True or crash.get("image_match") == "mismatch":
            # Old dump from another image: still unacknowledged (so still blocks until a
            # human acks/clears it) but never described as a crash of the running build.
            crash_summary += (
                " [STALE IMAGE: coredump is from a different firmware image, not the running one"
                + (f"; dump elf sha256 prefix {crash.get('dump_elf_sha')}" if crash.get("dump_elf_sha") else "")
                + "]"
            )

    # THE READINESS FIRING INTERLOCK (see BoardInfo.readiness_blocked).
    # One more GET, tolerated absent exactly like /api/crash_report above.
    # The blocking key set is mirrored from readiness_gate.h's
    # READINESS_GATE_KEY_* -- the KEYS only, never the rules: each item's
    # not_done/ok verdict is computed firmware-side by the one shared
    # predicate and simply read here. A key renamed on the firmware side
    # makes this list stop matching, which reads as "not blocked" -- so
    # firmware/KilnFW/App/test/check_readiness_gate_display_agreement.ps1
    # pins those key strings on the firmware side, and a rename has to go
    # through it.
    readiness_blocked = []
    heat_blocked = []
    try:
        readiness, _raw3 = _get_json(host, "/api/readiness", timeout)
    except PreflightTransportError as exc:
        readiness = None
        undetermined.append(f"readiness ({exc})")
    else:
        if not isinstance(readiness, dict):
            undetermined.append("readiness (response was not a JSON object)")
    if isinstance(readiness, dict):
        for item in readiness.get("items") or []:
            if not isinstance(item, dict):
                continue
            if item.get("key") not in READINESS_BLOCKING_KEYS:
                continue
            if item.get("status") != "not_done":
                continue
            (heat_blocked if item.get("key") in HEAT_ONLY_BLOCKING_KEYS
             else readiness_blocked).append(
                (
                    str(item.get("key")),
                    str(item.get("label") or item.get("key")),
                    str(item.get("detail") or "(no detail reported)"),
                )
            )

    return BoardInfo(
        reachable=True,
        readiness_blocked=tuple(readiness_blocked),
        heat_blocked=tuple(heat_blocked),
        fw_version=data.get("fw_version") or None,
        fw_build=data.get("fw_build") or None,
        self_protocol_version=data.get("self_protocol_version"),
        reset_reason=data.get("reset_reason"),
        uptime_s=data.get("uptime_s"),
        crash_unacknowledged=crash_unacknowledged,
        crash_summary=crash_summary,
        undetermined=tuple(undetermined),
    )


def derive_required_capabilities(
    preset: dict, zones_host: Optional[str], safety_host: Optional[str] = None
) -> "list[tuple[Capability, object]]":
    """Walk ``_CAPABILITY_MANIFEST`` against this preset and these apply-
    call arguments, returning ``[(Capability, preset_value), ...]`` for
    every capability the apply path would actually reach. See this module's
    docstring (DERIVATION / THE ONE MANUAL STEP THAT REMAINS) for what is
    and is not automatic here."""
    out = []
    have_zones = bool(zones_host)
    have_safety = bool(safety_host)
    for field_name, (capability, precondition) in _CAPABILITY_MANIFEST.items():
        if field_name not in preset:
            continue
        if not precondition(have_zones, have_safety):
            continue
        out.append((capability, preset[field_name]))
    return out


def run_preflight(
    preset: dict,
    host: str,
    zones_host: Optional[str] = None,
    safety_host: Optional[str] = None,
    preset_name: str = "(unnamed)",
    timeout: float = PREFLIGHT_HTTP_TIMEOUT_S,
    task_liveness: "object" = None,
    allow_missing_tasks: bool = False,
    task_liveness_unavailable: str = "",
    allow_undetermined: bool = False,
) -> PreflightReport:
    """The main entry point. Probes ``host`` once for board identity and
    once per required capability, and returns a :class:`PreflightReport`.
    Never raises for a board-side "capability absent" or "unreachable"
    result -- those are reported, not exceptions; see
    :func:`preflight_or_raise` for the fail-fast wrapper a caller like
    run_queue.py wants.

    ``task_liveness``: an optional ``task_liveness.TaskLivenessReport``
    (this module never computes one itself -- it has no link/serial access,
    only HTTP). Pass one to also refuse the run when a required task is
    dead or absent on this boot, exactly like an unacknowledged crash
    report, unless ``allow_missing_tasks=True``."""
    board = get_board_info(host, timeout=timeout)
    required = derive_required_capabilities(preset, zones_host, safety_host)
    checks: "list[CapabilityCheck]" = []
    if not board.reachable:
        for capability, preset_value in required:
            checks.append(CapabilityCheck(
                capability=capability, required=True, present=None,
                preset_value=preset_value, fatal=True,
                message="board unreachable, capability could not be checked",
            ))
        return PreflightReport(preset_name=preset.get("name", preset_name), host=host,
                                board=board, checks=checks,
                                task_liveness=task_liveness,
                                allow_missing_tasks=allow_missing_tasks,
                                task_liveness_unavailable=task_liveness_unavailable,
                                allow_undetermined=allow_undetermined)

    for capability, preset_value in required:
        try:
            data, _raw = _get_json(host, capability.probe_path, timeout)
        except PreflightTransportError as exc:
            # The board answered /api/status but not this endpoint -- still
            # "could not determine", not "confirmed absent". Treated as
            # fatal-and-unresolved rather than silently benign.
            checks.append(CapabilityCheck(
                capability=capability, required=True, present=None,
                preset_value=preset_value, fatal=True,
                message=f"probe failed: {exc}",
            ))
            continue
        if _is_no_such_endpoint(data):
            present = False
        else:
            present = True
        if present:
            fatal = False
        else:
            benign = capability.benign_when is not None and capability.benign_when(preset_value)
            fatal = not benign
        checks.append(CapabilityCheck(
            capability=capability, required=True, present=present,
            preset_value=preset_value, fatal=fatal,
            message="present" if present else ("benign" if not fatal else "FATAL"),
        ))
    return PreflightReport(preset_name=preset.get("name", preset_name), host=host,
                            board=board, checks=checks,
                            task_liveness=task_liveness,
                            allow_missing_tasks=allow_missing_tasks,
                            task_liveness_unavailable=task_liveness_unavailable,
                            allow_undetermined=allow_undetermined)


class PreflightFailed(Exception):
    """Raised by :func:`preflight_or_raise`. ``report`` carries the full
    :class:`PreflightReport`; ``str(exc)`` is the human-readable
    ``report.describe()`` text, so a bare ``print(exc)`` in a run-queue
    failure path already gives an operator everything constraint 3 asked
    for without any extra plumbing."""

    def __init__(self, report: PreflightReport):
        super().__init__(report.describe())
        self.report = report


def preflight_or_raise(
    preset: dict,
    host: str,
    zones_host: Optional[str] = None,
    safety_host: Optional[str] = None,
    preset_name: str = "(unnamed)",
    timeout: float = PREFLIGHT_HTTP_TIMEOUT_S,
    task_liveness: "object" = None,
    allow_missing_tasks: bool = False,
    task_liveness_unavailable: str = "",
    allow_undetermined: bool = False,
) -> PreflightReport:
    """Same as :func:`run_preflight`, but raises :class:`PreflightFailed`
    when ``report.ok`` is False (board unreachable, or any fatal capability
    gap). This is the fail-fast form meant to run once, before a long
    unattended queue starts -- see this module's docstring for the exact
    one-line wiring recommended for run_queue.py."""
    report = run_preflight(preset, host, zones_host=zones_host, safety_host=safety_host,
                            preset_name=preset_name, timeout=timeout,
                            task_liveness=task_liveness,
                            allow_missing_tasks=allow_missing_tasks,
                            task_liveness_unavailable=task_liveness_unavailable,
                            allow_undetermined=allow_undetermined)
    if not report.ok:
        _module_log.error("capability preflight failed:\n%s", report.describe())
        raise PreflightFailed(report)
    _module_log.info("capability preflight ok:\n%s", report.describe())
    return report
