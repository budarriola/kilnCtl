#!/usr/bin/env python3
"""firing_readiness.py -- run the three firing-time checks in sequence and
write ONE verdict JSON, so the owner's single real firing is not spent
discovering a harness problem.

Sequence (stops at the first failing step; later steps are recorded as
"skipped", never silently omitted):

  1. crash_report absent  -- GET /api/crash_report must read cleanly and show
     no present-and-unacknowledged record. An unreadable report is a FAIL,
     never a pass.
  2. soak                 -- stability_soak.py --firing-in-progress (exit 0 =
     PASS). It does NOT refuse an idle board under that flag (it warns and
     soaks anyway); a missing firing is caught by step 3's own RUNNING check,
     which FAILs, so it can never reach an overall PASS.
  3. stopwatch            -- bench_firing_abort_stopwatch.py
     --i-am-aborting-a-real-firing. DESTRUCTIVE BY DESIGN: it silences the
     safety link and aborts the firing, which is why it runs last and only
     when 1 and 2 passed.

Because step 3 aborts the firing, this script refuses to run unless
--i-am-aborting-a-real-firing is passed (the same ack the stopwatch demands).

THIS SCRIPT DOES NOT STOP THE FIRING. Only a passing step 3 is known to have
faulted it. On any other outcome (a failed or timed-out step, a skipped
stopwatch, a stopwatch that never saw FAULTED) the firing may still be
RUNNING; killing this script or the host does not stop it either. Check
profiles_get_exec_status() and call profiles_stop() if it is still running.
The verdict JSON says so in "firing_may_still_be_running".

Verdict file: <repo>/logs/firing_readiness/<UTC timestamp>.json (gitignored):
  {"verdict": "PASS"|"FAIL", "started": ..., "steps": [{"name", "status":
   "PASS"|"FAIL"|"skipped", "detail", "rc"?}]}

Usage:
    uv run --project tools/PcTools python \\
        tools/PcTools/scripts/firing_readiness.py --i-am-aborting-a-real-firing \\
        [--soak-duration 600] [--soak-interval 30] [--host H] [--out-dir DIR]

Exit status: 0 = every step PASS, 1 = a step failed, 2 = usage refusal.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
DEFAULT_OUT_DIR = REPO_ROOT / "logs" / "firing_readiness"


def check_crash_absent(host: "str | None", get_crash_report=None) -> "tuple[bool, str]":
    """True only when the crash report was read successfully and shows no
    present-and-unacknowledged record."""
    try:
        if get_crash_report is None:
            sys.path.insert(0, str(HERE.parent / "src"))
            from kilnctrl import dashboard_http_client
            from kilnctrl import mcp_server as m
            from kilnctrl.mcp_server_ota import _ota_resolve_host
            m.connect()
            resolved = _ota_resolve_host(host)
            report = dashboard_http_client.get_crash_report(resolved)
        else:
            report = get_crash_report(host)
    except Exception as exc:  # noqa: BLE001
        return False, f"crash_report unreadable: {exc}"
    if not isinstance(report, dict):
        return False, f"crash_report returned a non-object: {report!r}"
    if not isinstance(report.get("present"), bool):
        # {} or an error object must not read as "no crash on record".
        return False, f"crash_report has no boolean 'present' field: {report!r}"
    if report.get("present") and not report.get("acknowledged", False):
        return False, "UNACKNOWLEDGED crash report present"
    return True, "no unacknowledged crash report"


def run_script(argv: "list[str]", timeout: "float | None") -> "tuple[int, str]":
    """Run a sibling script; returns (rc, last 20 output lines)."""
    try:
        proc = subprocess.run([sys.executable, *argv], capture_output=True,
                              text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {timeout}s"
    except Exception as exc:  # noqa: BLE001 -- any launch failure is a FAIL, never a crash with no verdict
        return 125, f"could not run {argv[0]}: {exc}"
    tail = "\n".join((proc.stdout + proc.stderr).strip().splitlines()[-20:])
    return proc.returncode, tail


def run_readiness(args, *, crash_check=check_crash_absent, runner=run_script,
                  now=lambda: time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())) -> dict:
    steps: "list[dict]" = []
    verdict = {"verdict": "FAIL", "started": now(), "steps": steps}
    failed = False

    def record(name, ok, detail, rc=None):
        nonlocal failed
        entry = {"name": name, "status": "PASS" if ok else "FAIL", "detail": detail}
        if rc is not None:
            entry["rc"] = rc
        steps.append(entry)
        if not ok:
            failed = True

    def skip(name):
        steps.append({"name": name, "status": "skipped",
                      "detail": "an earlier step failed"})

    ok, detail = crash_check(args.host)
    record("crash_report_absent", ok, detail)

    soak_argv = [str(HERE / "stability_soak.py"), "--firing-in-progress",
                 "--duration", str(args.soak_duration),
                 "--interval", str(args.soak_interval)]
    if args.host:
        soak_argv += ["--host", args.host]
    if failed:
        skip("soak_firing_in_progress")
    else:
        rc, tail = runner(soak_argv, args.soak_duration + 300)
        record("soak_firing_in_progress", rc == 0, tail, rc)

    if failed:
        skip("abort_stopwatch")
    else:
        rc, tail = runner([str(HERE / "bench_firing_abort_stopwatch.py"),
                           "--i-am-aborting-a-real-firing"], 300)
        record("abort_stopwatch", rc == 0, tail, rc)

    # PASS only when every expected step actually ran and passed; a skipped
    # or missing step can never contribute to a PASS.
    expected = ["crash_report_absent", "soak_firing_in_progress", "abort_stopwatch"]
    all_passed = ([s["name"] for s in steps] == expected
                  and all(s["status"] == "PASS" for s in steps))
    verdict["verdict"] = "PASS" if (all_passed and not failed) else "FAIL"
    verdict["firing_may_still_be_running"] = verdict["verdict"] != "PASS"
    verdict["finished"] = now()
    return verdict


def write_verdict(verdict: dict, out_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / (time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + ".json")
    path.write_text(json.dumps(verdict, indent=2) + "\n", encoding="utf-8")
    return path


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--i-am-aborting-a-real-firing", action="store_true")
    ap.add_argument("--soak-duration", type=float, default=600.0)
    ap.add_argument("--soak-interval", type=float, default=30.0)
    ap.add_argument("--host", default=None)
    ap.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    args = ap.parse_args(argv)
    if not args.i_am_aborting_a_real_firing:
        print("REFUSED: the final step aborts the running firing on purpose; "
              "pass --i-am-aborting-a-real-firing to confirm.")
        return 2
    verdict = run_readiness(args)
    path = write_verdict(verdict, args.out_dir)
    for s in verdict["steps"]:
        print(f"  {s['status']:7s} {s['name']}: {s['detail'].splitlines()[-1] if s['detail'] else ''}")
    print(f"{verdict['verdict']}: {path}")
    if verdict.get("firing_may_still_be_running", True):
        print("WARNING: the firing was NOT confirmed aborted and may still be "
              "RUNNING. Check profiles_get_exec_status() and call "
              "profiles_stop() if so; stopping this script does not stop it.")
    return 0 if verdict["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
