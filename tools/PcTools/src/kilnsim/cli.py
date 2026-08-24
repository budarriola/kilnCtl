"""``kilnsim`` console script -- ``firmware/SimFW/docs/DESIGN_NOTES.md`` section 6.2.

Subcommands: ``state``, ``preset <name>``, ``fault <target> <type>
[--at-temp N] [--zone N]``, ``estop <open|closed>``, ``power
<cycle|on|off> [--domain main|safety]`` (two independent DUT-power relays,
PROTOCOL.md sec 5.5 -- main=J18 default, safety=J19; no combined "both"
option, by design), ``run <scenario.yaml> [--seed N] [--report out.json]``
(exit code = pass/fail/blocked -- the CI entry point: 0 PASS, 1 FAIL, 2
BLOCKED-only -- see ``cmd_run``'s own comment for the full reasoning),
``monitor [--json]``.

Defaults to the real :class:`~kilnsim.link.SerialSimLink`; pass ``--mock``
to run any subcommand against :class:`~kilnsim.link.MockSimLink` instead --
useful for demos, CI smoke tests, and this module's own tests, none of which
need real SimFW hardware.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Optional

from . import selftest as _selftest
from . import testmgr as _testmgr
from .link import MockSimLink, SerialSimLink, SimLink, SimLinkError, TcpSimLink, get_state_snapshot
from .protocol import (
    CommandGroup,
    CtCmd,
    FaultCmd,
    IoCmd,
    ModelCmd,
    RelayCmd,
    SysCmd,
    SYS_REBOOT_BOOTLOADER_MAGIC,
    TASK_STACK_MARGIN_WARN_THRESHOLD,
)
from .report import evaluate_expectations
from .runner import run_scenario
from .scenario import ScenarioError, load_scenario


def _make_link(args) -> SimLink:
    if args.mock:
        return MockSimLink()
    if getattr(args, "virtual", None) is not None:
        return TcpSimLink()
    return SerialSimLink()


def _connect(link: SimLink, args) -> None:
    if isinstance(link, TcpSimLink):
        address = getattr(args, "virtual", None) or None
        port = link.connect(address if address else None)
    else:
        port = link.connect(args.port)
    print(f"connected: {port}", file=sys.stderr)


# ---------------------------------------------------------------------------
# subcommands
# ---------------------------------------------------------------------------
def cmd_state(args) -> int:
    link = _make_link(args)
    _connect(link, args)
    try:
        # get_state_snapshot() papers over MockSimLink's SYS/100 convenience
        # vs. a real link's TELEMETRY-broadcast-only reality -- see its own
        # docstring in link.py for the bug this replaced.
        state = get_state_snapshot(link)
    except SimLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(state, indent=2))
    return 0


def cmd_preset(args) -> int:
    link = _make_link(args)
    _connect(link, args)
    try:
        link.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": args.name})
    except SimLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"preset loaded: {args.name}")
    return 0


def cmd_fault(args) -> int:
    link = _make_link(args)
    _connect(link, args)
    trigger: dict
    if args.at_temp is not None:
        if args.zone is None:
            print("error: --at-temp requires --zone", file=sys.stderr)
            return 2
        trigger = {"at_zone_temp": {"zone": args.zone, "temp_c": args.at_temp, "edge": "rising"}}
    else:
        trigger = {"manual": {}}
    payload = {
        "fault_slot": 0,
        "fault_type": args.fault_type,
        "target": args.target,
        "trigger": trigger,
        "duration": {"kind": "permanent"},
    }
    try:
        link.send_command(CommandGroup.FAULT, 1, payload)  # FaultCmd.SCHEDULE
    except SimLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"fault scheduled: {args.fault_type} on {args.target}")
    return 0


def cmd_estop(args) -> int:
    link = _make_link(args)
    _connect(link, args)
    try:
        link.send_command(CommandGroup.IO, IoCmd.ESTOP_SET, {"open": args.state == "open"})
    except SimLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"e-stop set: {args.state}")
    return 0


#: DUT power relay domain -> its own SET command (PROTOCOL.md sec 5.5): main
#: (J18, the legacy DUT_POWER_SET) vs safety (J19, the new
#: DUT_POWER_SAFETY_SET). Naming this by domain, not by wire command number,
#: is deliberate -- a domain mix-up in a review or on a terminal should be
#: obvious from the word "main"/"safety" alone. There is intentionally no
#: "both" entry here: cmd_power only ever issues one domain's SET per
#: invocation.
_POWER_DOMAIN_SET_CMD = {
    "main": IoCmd.DUT_POWER_SET,
    "safety": IoCmd.DUT_POWER_SAFETY_SET,
}


def cmd_power(args) -> int:
    link = _make_link(args)
    _connect(link, args)
    action = args.action
    domain = args.domain
    set_cmd = _POWER_DOMAIN_SET_CMD[domain]
    on = {"on": True, "off": False, "cycle": False}[action]
    try:
        link.send_command(CommandGroup.IO, set_cmd, {"on": on})
        if action == "cycle":
            time.sleep(max(0.0, args.off_ms) / 1000.0)
            link.send_command(CommandGroup.IO, set_cmd, {"on": True})
    except SimLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"dut power ({domain}): {action}")
    return 0


def cmd_reboot_bootloader(args) -> int:
    if not args.yes:
        print("error: refusing to reboot into the USB bootloader without --yes -- this ends the current "
              "firmware session; the fixture must be reflashed (or power-cycled) to run again", file=sys.stderr)
        return 1
    link = _make_link(args)
    _connect(link, args)
    try:
        # send_command_expect_reboot(), not send_command(): the firmware's
        # SUCCESS path never replies at all (PROTOCOL.md sec 4 -- it jumps
        # into the ROM bootloader before it can ACK), so an ordinary
        # send_command() would spend three full retry timeouts waiting for a
        # reply that only exists on the refusal path. `reply is None` here
        # is the expected, good outcome. REBOOT_BOOTLOADER's own reply (when
        # one does arrive) never carries STATUS_OK -- there is no "OK, about
        # to reboot" reply, only ERR_BAD_ARGS (wrong/missing magic) or
        # ERR_BUSY (safe-state confirmation timed out) -- so decode_reply()
        # always raises CommandStatusError for it, surfaced here as
        # SimLinkError, not returned as a dict.
        reply = link.send_command_expect_reboot(
            CommandGroup.SYS, SysCmd.REBOOT_BOOTLOADER, {"confirm": SYS_REBOOT_BOOTLOADER_MAGIC}
        )
    except SimLinkError as exc:
        print(f"reboot refused: {exc}", file=sys.stderr)
        return 1
    if reply is None:
        print("rebooting into USB bootloader (no reply expected -- this is success)")
        return 0
    print(f"unexpected reply: {reply}", file=sys.stderr)  # should not happen -- see comment above
    return 1


def _warn_if_k4_never_closed(report) -> None:
    """``kilnsim run``'s own :class:`~kilnsim.link.SimLink` reaches only the
    plant/fixture simulator (:mod:`kilnsim.protocol`'s benchproto wire has no
    SAFETY command group at all -- see :mod:`kilnsim.link`'s module
    docstring and ``firmware/SimFW/docs/BENCH_RUNBOOK.md`` step 10). The real
    safety processor is a second, physically separate device on its own USB
    link, owned by the ``kilnctrl`` package, not this one -- so a
    non-passing run whose K4 relay never closed at all is exactly as likely
    to mean "nobody sent ``SAFETY_CMD_REQUEST_ENABLE``" as "a real defect",
    and this module has no way to tell the two apart or fix the first one
    itself (it holds no link to the device that command targets). This is a
    diagnostic hint only -- printed, never asserted on -- so it can never
    turn a genuine FAIL into a false PASS or vice versa."""
    for e in report.events:
        if e.event_type.name == "RELAY_EDGE" and e.payload.get("relay") == "K4" and e.payload.get("edge") == "close":
            return
    print(
        "hint: K4 never closed during this run. If that's why an expectation "
        "didn't pass, remember kilnsim's own link never reaches the safety "
        "processor (it only drives the plant/fixture simulator) -- confirm "
        "whether SAFETY_CMD_REQUEST_ENABLE was sent separately, e.g. via "
        "`mcp__kilnctrl__safety_request_enable`, before or during this run "
        "(BENCH_RUNBOOK.md step 10).",
        file=sys.stderr,
    )


def cmd_run(args) -> int:
    try:
        scenario = load_scenario(args.scenario_path)
    except ScenarioError as exc:
        print(f"error: could not load scenario: {exc}", file=sys.stderr)
        return 2

    link = _make_link(args)
    _connect(link, args)

    seed = args.seed if args.seed is not None else scenario.seed

    if args.mock:
        # Against a MockSimLink there is nothing to wait on -- exercise the
        # same primitives against whatever events a test pre-loaded via
        # inject_event/make_event, so `kilnsim run --mock` stays a useful
        # smoke test without a full wait loop being simulated for it.
        try:
            link.send_command(CommandGroup.SYS, SysCmd.SET_SEED, {"value": seed})
            link.send_command(CommandGroup.SYS, SysCmd.SET_TIMESCALE, {"value": scenario.timescale})
            if scenario.preset:
                link.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": scenario.preset})
        except SimLinkError as exc:
            print(f"error: could not arm scenario: {exc}", file=sys.stderr)
            return 1
        events = link.read_events(timeout=0.0)
        report = evaluate_expectations(scenario, events, seed=seed, timescale=scenario.timescale)
    else:
        # Real run: kilnsim.runner drives the whole thing -- arm, wait for
        # the estimated scenario duration while draining/translating the
        # EVT stream and polling TELEMETRY, then evaluate expectations.
        try:
            report = run_scenario(link, scenario, seed=seed,
                                   duration_s=args.duration, timescale=args.timescale)
        except SimLinkError as exc:
            print(f"error: run failed: {exc}", file=sys.stderr)
            return 1
        finally:
            link.disconnect()

    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(report.to_json())
        print(f"report written: {args.report}", file=sys.stderr)
    print(report.to_json())

    # Loud-vacuity check (5th unfailable-check-class finding, see
    # kilnsim.testmgr's module docstring point 3): a scenario whose
    # guard-evidence clause(s) target GUARD_TRIP/GUARD_WARN/LINK_UP/
    # TRIP_INEFFECTIVE_LATCHED can look like a clean hardware PASS here even
    # though kilnsim.runner never synthesizes those event types against real
    # hardware -- `kilnsim testmgr` already carries this warning in its own
    # per-scenario `detail` (testmgr.run_one_scenario), but `kilnsim run`
    # (this function, the single-scenario/CI-gate path) printed only the raw
    # report and had no equivalent notice. Reusing testmgr.classify_scenario/
    # describe_runner_gap rather than re-deriving the same judgment a second
    # time, per that module's own has_runner_gap concept.
    gap_req = _testmgr.classify_scenario(scenario)
    gap_note = _testmgr.describe_runner_gap(gap_req, report.verdict)
    if gap_note:
        print(f"WARNING: {gap_note}", file=sys.stderr)

    # Exit-code semantics (kilnsim run is the CI gate, module docstring):
    #   0 -- PASS. Every expectation passed (or was SKIPPED -- its
    #        triggering condition never arose) and the run was valid.
    #   2 -- BLOCKED. No genuine FAIL anywhere, but at least one expectation
    #        is BLOCKED on documented, tracked DUT incompleteness
    #        (scenario's own blocked_on:, report.py's BLOCKED verdict). This
    #        is deliberately NOT exit 0 (an unqualified pass would hide that
    #        something genuinely did not pass) and NOT exit 1 (a CI gate
    #        that reds out on a known, already-tracked roadmap gap trains
    #        people to ignore red, and re-reports old news as a fresh
    #        failure every run) -- callers that want a strict "only 0 is OK"
    #        gate still get a nonzero exit distinct from a real failure to
    #        alert on if they choose to; callers that want "don't block CI
    #        on known gaps" treat 2 as non-fatal.
    #   1 -- FAIL. A genuine expectation failure, or the run itself was
    #        invalid (SPI underrun / event-seq gap, DESIGN_NOTES.md sec 8.2).
    if not report.passed and not args.mock:
        _warn_if_k4_never_closed(report)

    if report.passed:
        return 0
    if report.blocked:
        n_blocked = sum(1 for e in report.expectations if e.verdict == "BLOCKED")
        n_stale = sum(1 for e in report.expectations if e.stale_annotation)
        print(f"BLOCKED: {n_blocked} expectation(s) blocked on documented DUT incompleteness "
              f"(exit 2, not a CI failure)"
              + (f"; {n_stale} blocked_on annotation(s) unexpectedly PASSED -- "
                 f"consider removing them from their scenario" if n_stale else ""),
              file=sys.stderr)
        return 2
    return 1


def cmd_io(args) -> int:
    link = _make_link(args)
    _connect(link, args)
    try:
        if args.io_command == "read":
            result = link.send_command(CommandGroup.IO, IoCmd.READ, {"exp": args.exp, "pin": args.pin})
        elif args.io_command == "write":
            result = link.send_command(
                CommandGroup.IO, IoCmd.WRITE,
                {"exp": args.exp, "pin": args.pin, "level": args.level == "on"},
            )
        elif args.io_command == "dir":
            result = link.send_command(
                CommandGroup.IO, IoCmd.SET_DIR,
                {"exp": args.exp, "pin": args.pin, "is_input": args.direction == "in", "pullup": args.pullup},
            )
        elif args.io_command == "fault-line":
            result = link.send_command(CommandGroup.IO, IoCmd.FAULT_LINE_GET)
        elif args.io_command == "estop-get":
            result = link.send_command(CommandGroup.IO, IoCmd.ESTOP_GET)
        elif args.io_command == "power-get":
            result = link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET)
        elif args.io_command == "power-safety-get":
            result = link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET)
        elif args.io_command == "scan":
            result = link.send_command(CommandGroup.IO, IoCmd.BUS_SCAN)
            _print_io_scan_result(result)
            return 0
        else:  # pragma: no cover - argparse `choices` already guards this
            print(f"error: unknown io subcommand {args.io_command!r}", file=sys.stderr)
            return 2
    except SimLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


def _print_io_scan_result(result: dict) -> None:
    """Human-readable `kilnsim io scan` output -- the whole point of this
    command is a bench operator not having to compare hex addresses by eye
    (see the incident this command was built for: the firmware's own
    MCP23017_ADDR_1/_2 vs. what actually answers on the bus went unnoticed
    for an hour). Prints the configured-vs-found comparison up front, in
    plain words, before the raw address lists."""
    configured = [result["configured_addr1"], result["configured_addr2"]]
    found = result["found_addresses"]
    configured_str = "/".join(f"0x{a:02X}" for a in configured)
    found_str = "/".join(f"0x{a:02X}" for a in found) if found else "(none)"
    if result["match"]:
        print(f"configured {configured_str}, found {found_str} -- OK")
    else:
        print(f"configured {configured_str}, found {found_str} -- MISMATCH")
        missing = [a for a in configured if a not in found]
        if missing:
            missing_str = "/".join(f"0x{a:02X}" for a in missing)
            print(f"  configured address(es) not found on the bus: {missing_str}")
    extra = [a for a in found if a not in configured]
    if extra:
        extra_str = "/".join(f"0x{a:02X}" for a in extra)
        print(f"  additional address(es) found but not configured: {extra_str}")


def cmd_ct(args) -> int:
    link = _make_link(args)
    _connect(link, args)
    try:
        if args.ct_command == "state":
            result = link.send_command(CommandGroup.CT, CtCmd.GET_STATE, {"channel": args.channel})
        elif args.ct_command == "mode":
            result = link.send_command(
                CommandGroup.CT, CtCmd.SET_MODE, {"channel": args.channel, "mode": args.mode}
            )
        elif args.ct_command == "amps":
            result = link.send_command(
                CommandGroup.CT, CtCmd.SET_AMPS, {"channel": args.channel, "amps": args.amps}
            )
        elif args.ct_command == "phase":
            result = link.send_command(
                CommandGroup.CT, CtCmd.SET_PHASE, {"channel": args.channel, "phase_deg": args.phase_deg}
            )
        elif args.ct_command == "distortion":
            distortion = {
                "dc_offset": args.dc_offset,
                "clip_fraction": args.clip_fraction,
                "dropout_half_cycle": args.dropout_half,
                "dropout_negative_half": args.dropout_negative,
                "apply_immediately": not args.defer,
            }
            result = link.send_command(
                CommandGroup.CT, CtCmd.SET_DISTORTION, {"channel": args.channel, "distortion": distortion}
            )
        else:  # pragma: no cover - argparse `choices` already guards this
            print(f"error: unknown ct subcommand {args.ct_command!r}", file=sys.stderr)
            return 2
    except SimLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


def cmd_relay(args) -> int:
    link = _make_link(args)
    _connect(link, args)
    try:
        if args.relay_command == "states":
            result = link.send_command(CommandGroup.RELAY, RelayCmd.GET_STATES)
        elif args.relay_command == "edges":
            result = link.send_command(
                CommandGroup.RELAY, RelayCmd.GET_EDGES,
                {"since_seq": args.since_seq, "max_count": args.max_count},
            )
        else:  # pragma: no cover - argparse `choices` already guards this
            print(f"error: unknown relay subcommand {args.relay_command!r}", file=sys.stderr)
            return 2
    except SimLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


def cmd_fault_list(args) -> int:
    """``kilnsim fault-list`` / ``kilnsim fault-cancel`` / ``kilnsim
    fault-fire`` -- the FAULT_LIST/CANCEL/FIRE_NOW half of the FAULT group
    that ``cmd_fault`` (the pre-existing ``kilnsim fault`` subcommand,
    FAULT_SCHEDULE only) didn't cover."""
    link = _make_link(args)
    _connect(link, args)
    try:
        if args.fault_command == "list":
            result = link.send_command(
                CommandGroup.FAULT, FaultCmd.LIST,
                {"start_index": args.start_index, "max_count": args.max_count},
            )
        elif args.fault_command == "cancel":
            result = link.send_command(CommandGroup.FAULT, FaultCmd.CANCEL, {"fault_slot": args.slot})
        elif args.fault_command == "fire-now":
            result = link.send_command(CommandGroup.FAULT, FaultCmd.FIRE_NOW, {"fault_slot": args.slot})
        else:  # pragma: no cover - argparse `choices` already guards this
            print(f"error: unknown fault subcommand {args.fault_command!r}", file=sys.stderr)
            return 2
    except SimLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


def cmd_selftest(args) -> int:
    link = _make_link(args)
    try:
        _connect(link, args)
    except SimLinkError as exc:
        print(f"error: could not connect: {exc}", file=sys.stderr)
        return 1
    try:
        report = _selftest.run_selftest(link)
    finally:
        if link.is_connected:
            link.disconnect()
    if args.json:
        print(report.to_json())
    else:
        print(report.to_text())
    return 0 if report.passed else 1


def _format_task_stats_table(result: dict) -> str:
    """Pure formatting, no I/O -- kept separate from :func:`cmd_tasks` so a
    test can call it directly against a decoded GET_TASK_STATS reply
    without needing a link (see tools/PcTools/tests/test_kilnsim_task_stats.py).
    Columns: task, allocated, peak used, headroom, margin -- flags anything
    under TASK_STACK_MARGIN_WARN_THRESHOLD with a leading ``!`` marker and a
    summary line, since that is the entire reason this command exists (this
    feature's own task instructions: "FLAG anything under 2x margin
    prominently -- that threshold is what this whole feature exists to
    police")."""
    tasks = result.get("tasks", [])
    if not tasks:
        return "(no tasks reported)"
    rows = sorted(tasks, key=lambda t: t["margin"])
    header = f"  {'task':<12} {'allocated':>10} {'peak used':>10} {'headroom':>10} {'margin':>8}"
    lines = [header, "-" * len(header)]
    flagged = []
    for t in rows:
        name = t.get("task_stats_id_name") or str(t["task_stats_id"])
        is_low = t["margin"] < TASK_STACK_MARGIN_WARN_THRESHOLD
        marker = "!" if is_low else " "
        margin_str = "inf" if t["margin"] == float("inf") else f"{t['margin']:.2f}x"
        lines.append(
            f"{marker} {name:<12} {t['allocated_bytes']:>9}B {t['peak_used_bytes']:>9}B "
            f"{t['hwm_free_bytes']:>9}B {margin_str:>8}"
        )
        if is_low:
            flagged.append(f"{name} ({margin_str})")
    if flagged:
        lines.append("")
        lines.append(
            f"! {len(flagged)}/{len(tasks)} task(s) under {TASK_STACK_MARGIN_WARN_THRESHOLD:.1f}x stack "
            f"margin: {', '.join(flagged)}"
        )
    return "\n".join(lines)


def cmd_tasks(args) -> int:
    """``kilnsim tasks`` -- GET_TASK_STATS (PROTOCOL.md sec 4): per-task
    FreeRTOS stack allocated/peak-used/headroom/margin, the PC-side half of
    the per-task-stack-margin feature (cmd_ids.h's
    SIMFW_CMD_SYS_GET_TASK_STATS). Exit code 1 (not just a printed flag) if
    any task is under TASK_STACK_MARGIN_WARN_THRESHOLD, so this is usable as
    a CI/bench gate on its own, not just a human-readable report."""
    link = _make_link(args)
    _connect(link, args)
    try:
        result = link.send_command(CommandGroup.SYS, SysCmd.GET_TASK_STATS)
    except SimLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(_format_task_stats_table(result))
    low_margin = [t for t in result.get("tasks", []) if t["margin"] < TASK_STACK_MARGIN_WARN_THRESHOLD]
    return 1 if low_margin else 0


def cmd_testmgr(args) -> int:
    link = _make_link(args)
    scenarios_dir = Path(args.scenarios_dir) if args.scenarios_dir else None
    report = _testmgr.run_suite(
        link, scenarios_dir,
        quick=args.quick, mock=args.mock, port=args.port,
    )
    if args.json:
        print(report.to_json())
    else:
        print(report.to_text())
    return report.exit_code


def cmd_monitor(args) -> int:
    link = _make_link(args)
    _connect(link, args)
    print("monitoring (Ctrl+C to stop)...", file=sys.stderr)
    try:
        while True:
            events = link.read_events(timeout=args.interval)
            for evt in events:
                if args.json:
                    print(json.dumps(evt.to_dict()))
                else:
                    print(f"[{evt.seq}] t={evt.sim_time_us}us {evt.event_type.name} {evt.payload}")
            sys.stdout.flush()
            if args.mock:
                # A MockSimLink never produces events on its own -- this
                # command exists to tail a real link; against --mock it just
                # drains whatever a test/demo pre-loaded and exits, rather
                # than spinning forever with nothing to show.
                break
    except KeyboardInterrupt:
        pass
    return 0


# ---------------------------------------------------------------------------
# argument parser
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="kilnsim", description="PC-side control for SimFW")
    p.add_argument("--mock", action="store_true", help="use an in-memory MockSimLink instead of real hardware")
    p.add_argument("--port", default=None, help="serial port (autodetected if omitted)")
    p.add_argument("--virtual", nargs="?", const="", default=None, metavar="HOST:PORT",
                    help="connect to firmware/SimFW/tools/virtual_simfw over TCP instead of real hardware "
                         "(default 127.0.0.1:8765 if no address given)")
    sub = p.add_subparsers(dest="command", required=True)

    sp = sub.add_parser("state", help="print the current telemetry snapshot as JSON")
    sp.set_defaults(func=cmd_state)

    sp = sub.add_parser("preset", help="load a named thermal-model preset")
    sp.add_argument("name")
    sp.set_defaults(func=cmd_preset)

    sp = sub.add_parser("fault", help="schedule a fault")
    sp.add_argument("target", help="e.g. tc0, relay:K1, ct1")
    sp.add_argument("fault_type", help="fault catalog name, DESIGN_NOTES.md sec 7.1")
    sp.add_argument("--at-temp", type=float, default=None, dest="at_temp")
    sp.add_argument("--zone", type=int, default=None)
    sp.set_defaults(func=cmd_fault)

    sp = sub.add_parser("estop", help="open or close the E-stop loop")
    sp.add_argument("state", choices=["open", "closed"])
    sp.set_defaults(func=cmd_estop)

    sp = sub.add_parser(
        "power",
        help="control a DUT 12V power relay (--domain main=J18 or safety=J19; there is no "
             "'both' option -- the two domains are always switched separately)",
    )
    sp.add_argument("action", choices=["cycle", "on", "off"])
    sp.add_argument("--off-ms", type=int, default=200, dest="off_ms")
    sp.add_argument(
        "--domain", choices=["main", "safety"], default="main",
        help="which relay domain to command: main = J18 (default, legacy DUT_POWER_SET), "
             "safety = J19 (DUT_POWER_SAFETY_SET). Run the command twice, once per domain, "
             "to power-cycle both -- there is no single flag that does both at once.",
    )
    sp.set_defaults(func=cmd_power)

    sp = sub.add_parser(
        "reboot-bootloader",
        help="drop the fixture into its USB ROM bootloader for reflashing, without pressing BOOTSEL "
             "(requires --yes: this ends the current firmware session)",
    )
    sp.add_argument("--yes", action="store_true",
                     help="required: confirms you intend to end the current firmware session")
    sp.set_defaults(func=cmd_reboot_bootloader)

    sp = sub.add_parser("run", help="run a scenario YAML; exit code 0=PASS, 1=FAIL, 2=BLOCKED-only")
    sp.add_argument("scenario_path")
    sp.add_argument("--seed", type=int, default=None)
    sp.add_argument("--timescale", type=float, default=None, help="override the scenario's own timescale")
    sp.add_argument("--duration", type=float, default=None,
                     help="sim-clock seconds to run before evaluating expectations "
                          "(default: estimated from the scenario's faults/expect deadlines)")
    sp.add_argument("--report", default=None, help="write the report JSON to this path")
    sp.set_defaults(func=cmd_run)

    sp = sub.add_parser("io", help="discrete I/O group (I2C expander pins, E-stop/DUT-power readback, fault line)")
    io_sub = sp.add_subparsers(dest="io_command", required=True)

    iosp = io_sub.add_parser("read", help="read a discrete I/O pin")
    iosp.add_argument("pin", type=int, help="pin 0..15 (0..7 = port A, 8..15 = port B)")
    iosp.add_argument("--exp", type=int, default=0, help="expander index: 0 = EXP_1 (0x20), 1 = EXP_2 (0x21)")
    iosp.set_defaults(func=cmd_io)

    iosp = io_sub.add_parser("write", help="drive a discrete I/O pin")
    iosp.add_argument("pin", type=int)
    iosp.add_argument("level", choices=["on", "off"])
    iosp.add_argument("--exp", type=int, default=0)
    iosp.set_defaults(func=cmd_io)

    iosp = io_sub.add_parser("dir", help="set a discrete I/O pin's direction")
    iosp.add_argument("pin", type=int)
    iosp.add_argument("direction", choices=["in", "out"])
    iosp.add_argument("--exp", type=int, default=0)
    iosp.add_argument("--pullup", action="store_true")
    iosp.set_defaults(func=cmd_io)

    iosp = io_sub.add_parser("fault-line", help="read the fault-line sense input")
    iosp.set_defaults(func=cmd_io)

    iosp = io_sub.add_parser("estop-get", help="read back the E-stop loop's commanded state")
    iosp.set_defaults(func=cmd_io)

    iosp = io_sub.add_parser(
        "power-get", help="read back the main-domain (J18) DUT power relay's commanded state"
    )
    iosp.set_defaults(func=cmd_io)

    iosp = io_sub.add_parser(
        "power-safety-get", help="read back the safety-domain (J19) DUT power relay's commanded state"
    )
    iosp.set_defaults(func=cmd_io)

    iosp = io_sub.add_parser(
        "scan",
        help="sweep I2C0 (0x08..0x77) and report which addresses ACKed, vs. what this firmware "
             "build is configured to use -- diagnoses a re-strapped/mismatched MCP23017 in one round trip",
    )
    iosp.set_defaults(func=cmd_io)

    sp = sub.add_parser("ct", help="current-transformer emulation group")
    ct_sub = sp.add_subparsers(dest="ct_command", required=True)

    ctsp = ct_sub.add_parser("state", help="read a CT channel's full state (mode, amps, distortion, readback)")
    ctsp.add_argument("channel", type=int)
    ctsp.set_defaults(func=cmd_ct)

    ctsp = ct_sub.add_parser("mode", help="set a CT channel's mode")
    ctsp.add_argument("channel", type=int)
    ctsp.add_argument("mode", choices=["model", "manual"])
    ctsp.set_defaults(func=cmd_ct)

    ctsp = ct_sub.add_parser("amps", help="set a CT channel's commanded amplitude (manual mode)")
    ctsp.add_argument("channel", type=int)
    ctsp.add_argument("amps", type=float)
    ctsp.set_defaults(func=cmd_ct)

    ctsp = ct_sub.add_parser("phase", help="set a CT channel's phase offset in degrees")
    ctsp.add_argument("channel", type=int)
    ctsp.add_argument("phase_deg", type=float)
    ctsp.set_defaults(func=cmd_ct)

    ctsp = ct_sub.add_parser("distortion", help="set a CT channel's distortion knobs")
    ctsp.add_argument("channel", type=int)
    ctsp.add_argument("--dc-offset", type=float, default=0.0, dest="dc_offset")
    ctsp.add_argument("--clip-fraction", type=float, default=0.0, dest="clip_fraction")
    ctsp.add_argument("--dropout-half", action="store_true", dest="dropout_half")
    ctsp.add_argument("--dropout-negative", action="store_true", dest="dropout_negative")
    ctsp.add_argument("--defer", action="store_true",
                       help="apply on the next natural waveform boundary instead of immediately")
    ctsp.set_defaults(func=cmd_ct)

    sp = sub.add_parser("relay", help="relay sense group")
    relay_sub = sp.add_subparsers(dest="relay_command", required=True)

    relsp = relay_sub.add_parser("states", help="current sensed state of every relay + the fault line")
    relsp.set_defaults(func=cmd_relay)

    relsp = relay_sub.add_parser("edges", help="timestamped relay edge log")
    relsp.add_argument("--since-seq", type=int, default=0, dest="since_seq")
    relsp.add_argument("--max-count", type=int, default=8, dest="max_count")
    relsp.set_defaults(func=cmd_relay)

    sp = sub.add_parser("fault-list", help="list armed/active fault slots")
    sp.add_argument("--start-index", type=int, default=0, dest="start_index")
    sp.add_argument("--max-count", type=int, default=8, dest="max_count")
    sp.set_defaults(func=cmd_fault_list, fault_command="list")

    sp = sub.add_parser("fault-cancel", help="cancel a fault slot by id")
    sp.add_argument("slot", type=int)
    sp.set_defaults(func=cmd_fault_list, fault_command="cancel")

    sp = sub.add_parser("fault-fire", help="force an armed fault slot to fire now, bypassing its trigger")
    sp.add_argument("slot", type=int)
    sp.set_defaults(func=cmd_fault_list, fault_command="fire-now")

    sp = sub.add_parser(
        "selftest",
        help="PLAN.md sec 13 layer-2 loopback self-check: is the fixture itself healthy? "
             "(protocol round-trip, every command group reachable, telemetry cadence, "
             "event-sequence continuity, a determinism spot-check; hardware-only checks "
             "-- SPI master loopback, CT->ADC loopback -- are reported NOT_RUNNABLE, never faked)",
    )
    sp.add_argument("--json", action="store_true", help="machine-readable report instead of a text summary")
    sp.set_defaults(func=cmd_selftest)

    sp = sub.add_parser(
        "tasks",
        help="per-task FreeRTOS stack allocated/peak-used/headroom/margin (GET_TASK_STATS) -- "
             "flags anything under a 2x stack margin; exits 1 if any task is flagged",
    )
    sp.add_argument("--json", action="store_true", help="machine-readable reply instead of a text table")
    sp.set_defaults(func=cmd_tasks)

    sp = sub.add_parser(
        "testmgr",
        help="one-command tiered regression suite: hardware presence detection, "
             "kilnsim selftest, every runnable SimFW scenario, and a guard-coverage "
             "report against SaftyFW's S1..S13 list. Exit code 0=PASS, 1=FAIL, "
             "2=BLOCKED-only (same convention as `kilnsim run`).",
    )
    sp.add_argument("--quick", action="store_true",
                     help="fast subset only (the shortest-estimated-duration scenarios) -- "
                          "for 'I just reflashed, is it still sane'")
    sp.add_argument("--json", action="store_true", help="machine-readable report instead of a text summary")
    sp.add_argument("--scenarios-dir", default=None,
                     help="override the scenario directory (default: firmware/SimFW/scenarios)")
    sp.set_defaults(func=cmd_testmgr)

    sp = sub.add_parser("monitor", help="tail live telemetry/events")
    sp.add_argument("--json", action="store_true")
    sp.add_argument("--interval", type=float, default=1.0)
    sp.set_defaults(func=cmd_monitor)

    return p


def main(argv: Optional[list] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
