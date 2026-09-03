#!/usr/bin/env python3
"""Regression tests for the run_queue "idle mid-run" defect an opus review
diagnosed and correctly declined to fix mid-campaign (run_queue.py was live,
driving unattended firings on real hardware, when the review happened).

THE BUG: run_queue.py's docstring at (current, unpatched) lines ~104-106
claims "profile_executor_status.c never transitions RUNNING/PAUSED back to
IDLE, only to DONE or FAULTED." That is false --
firmware/KilnFW/App/drivers/profile_executor_status.c:77 sets
``s_exec.state = PROFILE_EXEC_IDLE`` directly out of RUNNING/PAUSED on an
operator Stop, and that file's own comment at lines 65-67 names this exact
case: "the OTHER ending the tick loop's DONE/FAULTED branch doesn't see".

CONSEQUENCE: because "idle" is deliberately excluded from BOTH
``_ACTIVE_STATES`` and ``_TERMINAL_STATES``, a run that gets stopped
externally (operator, ``/api/profile_exec/stop``, a UI button) mid-firing
produces an idle sample that ``_poll_capture_until``'s ``_run_is_terminal``
predicate does not recognize -- so the capture loop just keeps polling
until the full run timeout (``total_planned_s * 1.25 + 600s`` -- about 72
minutes for profile 7) expires and THEN raises. Safe (loud, eventually
correct), just needlessly slow for an unattended campaign.

THE FIX (not applied by this test file -- see docs/patches/
run_queue_idle_stop_fix.patch and its accompanying description) adds
``check_not_idle_after_start()`` and a ``raise_on_idle`` flag to
``_poll_capture_until``, set True only for the main run-capture call (never
the cooldown capture, where idle/rested is the desired end state, and never
the pre-start wait, which ``_wait_until_run_active_or_terminal`` already
owns correctly).

THIS FILE IS SKIPPED BY DEFAULT so ``run_pctools_tests`` /
``run_all_checks.ps1`` stay green while run_queue.py is still driving live
hardware and must not be edited. Both tests below are written RED on
purpose -- they fail against today's unpatched run_queue.py and are
expected to start passing the moment docs/patches/
run_queue_idle_stop_fix.patch is applied.

To verify the red state (today, unpatched) or the fix (after applying the
patch), set the env var and run directly:

    RUN_IDLE_STOP_FIX_TESTS=1 python -m pytest \
        tools/PcTools/tests/test_run_queue_idle_stop_fix.py -q -v

Does NOT touch test_run_queue.py -- imports its scripting helpers
(``_ScriptedTransport``, ``_status``, ``_channel``, ``_exec``, ``_plan``,
``_zones``, ``_patched_run_entry``) read-only, same fixtures the existing
suite already exercises run_entry with.
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.dirname(__file__))

from kilnctrl import run_queue as rq  # noqa: E402
import test_run_queue as _base  # noqa: E402 -- read-only import, not modified

_RUN_FIX_TESTS = os.environ.get("RUN_IDLE_STOP_FIX_TESTS") == "1"
_SKIP_REASON = (
    "red-by-design until docs/patches/run_queue_idle_stop_fix.patch is applied to "
    "tools/PcTools/src/kilnctrl/run_queue.py (that file is currently driving a live "
    "unattended hardware campaign and must not be edited mid-run). Set "
    "RUN_IDLE_STOP_FIX_TESTS=1 to run this test now and see it fail against the "
    "unpatched module, or to confirm it passes once the patch has been applied."
)


def _run_running_then_idle(run_timeout_margin_s=3.0, plan_total_planned_s=2.0):
    """Drives the exec-state sequence running -> idle (the operator-Stop
    shape from profile_executor_status.c:65-77) through the REAL run_entry
    entry point -- no shortcuts into private helpers, so this exercises
    exactly the code path the live campaign runs. First get_exec() (the
    start-confirm poll) sees "running": the run genuinely started. Every
    get_exec() after that (the main run-capture loop) sees "idle".

    A small plan duration + run_timeout_margin_s keeps the unpatched
    module's timeout-based raise reachable in a handful of iterations
    instead of ~870 (the real DEFAULT_RUN_TIMEOUT_MARGIN_S/poll_interval_s
    ratio) -- the fake clock means no wall-clock time is actually spent
    either way, this only bounds the loop iteration count.

    Returns ``(transport, raised_exception)``.
    """
    tmpdir = tempfile.mkdtemp()
    log_path = os.path.join(tmpdir, "run.jsonl")
    rested = _base._status([_base._channel(25.1, 25.0)])
    exec_sequence = [_base._exec([0], state="running"), _base._exec([0], state="idle")]
    transport = _base._ScriptedTransport(
        status_sequence=[rested],
        exec_sequence=exec_sequence,
        plan_body=_base._plan([20, 45, 60], total_planned_s=plan_total_planned_s),
        zones_body=_base._zones([80.0, 80.0, 80.0]),
    )
    entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7,
                           log_path=log_path, label="t")
    cfg = rq.RunQueueConfig(
        host="203.0.113.10", poll_interval_s=1.0, sleep=transport.sleep,
        now=transport.now, cooldown_s=0.0, run_timeout_margin_s=run_timeout_margin_s)
    raised = None
    try:
        _base._patched_run_entry(entry, cfg, transport, entry.preset_name)
    except rq.RunQueueError as exc:
        raised = exc
    return transport, raised


class IdleAfterConfirmedRunningTest(unittest.TestCase):

    @unittest.skipUnless(_RUN_FIX_TESTS, _SKIP_REASON)
    def test_idle_after_confirmed_running_raises_immediately_with_a_clear_message(self):
        _transport, raised = _run_running_then_idle()
        self.assertIsNotNone(raised, "run_entry did not raise at all on running -> idle")
        message = str(raised).lower()
        self.assertIn("idle", message)
        self.assertIn("stopped externally", message)
        # NOT the old timeout-shaped message -- proves this is the new
        # immediate check firing, not the pre-existing deadline raise.
        self.assertNotIn("still not terminal after", message)

    @unittest.skipUnless(_RUN_FIX_TESTS, _SKIP_REASON)
    def test_idle_after_confirmed_running_raises_before_any_sleep(self):
        # The sharpest, least string-dependent proof: cfg.sleep() is the
        # LAST thing each _poll_capture_until iteration does. A raise on
        # the very first main-loop sample means sleep is never called.
        # Today's unpatched module sleeps repeatedly (once per poll
        # interval) while it waits out the run timeout on a state it can
        # never recognize as terminal, so transport.sleeps is non-empty
        # against the unpatched module and this assertion is false there.
        transport, raised = _run_running_then_idle()
        self.assertIsNotNone(raised, "run_entry did not raise at all on running -> idle")
        self.assertEqual(transport.sleeps, [])


if __name__ == "__main__":
    unittest.main()
