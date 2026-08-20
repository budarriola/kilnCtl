"""``kilnsim`` console script -- ``firmware/SimFW/docs/PLAN.md`` section 6.2.

Subcommands: ``state``, ``preset <name>``, ``fault <target> <type>
[--at-temp N] [--zone N]``, ``estop <open|closed>``, ``power
<cycle|on|off>``, ``run <scenario.yaml> [--seed N] [--report out.json]``
(exit code = pass/fail -- the CI entry point), ``monitor [--json]``.

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
from typing import Optional

from .link import MockSimLink, SerialSimLink, SimLink, SimLinkError
from .protocol import CommandGroup, IoCmd, ModelCmd, SysCmd
from .report import evaluate_expectations
from .scenario import ScenarioError, load_scenario


def _make_link(args) -> SimLink:
    return MockSimLink() if args.mock else SerialSimLink()


def _connect(link: SimLink, args) -> None:
    port = link.connect(args.port)
    print(f"connected: {port}", file=sys.stderr)


# ---------------------------------------------------------------------------
# subcommands
# ---------------------------------------------------------------------------
def cmd_state(args) -> int:
    link = _make_link(args)
    _connect(link, args)
    try:
        # See link.MockSimLink._default_response's SYS/100 note: state is a
        # kilnsim-local convenience read (telemetry is push-only per PLAN.md
        # sec 5.3), fetched here the same way against either link type.
        state = link.send_command(CommandGroup.SYS, 100)
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
        "duration": "permanent",
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


def cmd_power(args) -> int:
    link = _make_link(args)
    _connect(link, args)
    action = args.action
    on = {"on": True, "off": False, "cycle": False}[action]
    try:
        link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SET, {"on": on})
        if action == "cycle":
            time.sleep(max(0.0, args.off_ms) / 1000.0)
            link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SET, {"on": True})
    except SimLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"dut power: {action}")
    return 0


def cmd_run(args) -> int:
    try:
        scenario = load_scenario(args.scenario_path)
    except ScenarioError as exc:
        print(f"error: could not load scenario: {exc}", file=sys.stderr)
        return 2

    link = _make_link(args)
    _connect(link, args)

    seed = args.seed if args.seed is not None else scenario.seed
    try:
        link.send_command(CommandGroup.SYS, SysCmd.SET_SEED, {"value": seed})
        link.send_command(CommandGroup.SYS, SysCmd.SET_TIMESCALE, {"value": scenario.timescale})
        if scenario.preset:
            link.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": scenario.preset})
    except SimLinkError as exc:
        print(f"error: could not arm scenario: {exc}", file=sys.stderr)
        return 1

    # Real run orchestration -- watching the EVT stream for the scenario's
    # actual duration and driving the DUT -- is `run_test_scenario`'s job
    # (mcp_server.py); this CLI path exercises the same primitives against
    # whatever events the link already has buffered (real hardware mid-run,
    # or a MockSimLink a test has pre-loaded via inject_event/make_event),
    # so `kilnsim run --mock` is a meaningful smoke test without a full
    # scenario runner having to be simulated here too.
    events = link.read_events(timeout=0.0)
    report = evaluate_expectations(scenario, events, seed=seed, timescale=scenario.timescale)

    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(report.to_json())
        print(f"report written: {args.report}", file=sys.stderr)
    print(report.to_json())
    return 0 if report.passed else 1


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
    sub = p.add_subparsers(dest="command", required=True)

    sp = sub.add_parser("state", help="print the current telemetry snapshot as JSON")
    sp.set_defaults(func=cmd_state)

    sp = sub.add_parser("preset", help="load a named thermal-model preset")
    sp.add_argument("name")
    sp.set_defaults(func=cmd_preset)

    sp = sub.add_parser("fault", help="schedule a fault")
    sp.add_argument("target", help="e.g. tc0, relay:K1, ct1")
    sp.add_argument("fault_type", help="fault catalog name, PLAN.md sec 7.1")
    sp.add_argument("--at-temp", type=float, default=None, dest="at_temp")
    sp.add_argument("--zone", type=int, default=None)
    sp.set_defaults(func=cmd_fault)

    sp = sub.add_parser("estop", help="open or close the E-stop loop")
    sp.add_argument("state", choices=["open", "closed"])
    sp.set_defaults(func=cmd_estop)

    sp = sub.add_parser("power", help="control the DUT's 12V power relay")
    sp.add_argument("action", choices=["cycle", "on", "off"])
    sp.add_argument("--off-ms", type=int, default=200, dest="off_ms")
    sp.set_defaults(func=cmd_power)

    sp = sub.add_parser("run", help="run a scenario YAML; exit code = pass/fail")
    sp.add_argument("scenario_path")
    sp.add_argument("--seed", type=int, default=None)
    sp.add_argument("--report", default=None, help="write the report JSON to this path")
    sp.set_defaults(func=cmd_run)

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
