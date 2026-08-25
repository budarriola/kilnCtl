#!/usr/bin/env python3
"""The second half of a run boundary: a seq floor for events still on the wire.

Flushing the PC's EVT buffer at reset (test_kilnsim_stale_event_discard.py)
catches what has already arrived. It cannot catch a frame that is still in
flight at that moment and lands a few milliseconds later, during the new
run -- the residual race the discard's own docstring used to admit to.

`SimLink.begin_run_event_boundary()` closes it: flush the buffer, then arm a
floor at the highest seq seen so far. Everything at or below that floor
belongs to a previous run and is dropped on the way out of `read_events()`.

The floor is only possible because the firmware's event seq became monotonic
for its whole boot lifetime on 2026-08-24 (`sim_engine.c`'s `apply_reset()`
no longer zeroes `s_ring_next_seq`; `tools/check_event_seq_monotonic.ps1`
and `test_sim_engine_event_seq_monotonic.c` hold it there). While the seq
restarted at 0 on every RESET_SIM, a floor could not have worked at all: it
would have rejected the genuine post-reset events along with the stale ones,
because they carried the same numbers. That case is pinned below too --
it is the reason the firmware change and this one belong together.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim.link import MockSimLink  # noqa: E402
from kilnsim.protocol import Event, EventType  # noqa: E402


def _evt(seq: int) -> Event:
    return Event(seq=seq, sim_time_us=seq * 1000,
                 event_type=EventType.FAULT_FIRED, payload={}, a=0, b=0, f0=0.0)


class SeqFloorTests(unittest.TestCase):
    def _link(self) -> MockSimLink:
        link = MockSimLink()
        link.connect()
        return link

    def test_no_floor_is_armed_by_default(self):
        """Nothing is filtered until a run explicitly asks for it -- a bare
        link must behave exactly as it always did."""
        link = self._link()
        link.inject_event(_evt(3))
        self.assertEqual([e.seq for e in link.read_events(timeout=0.0)], [3])

    def test_floor_rejects_a_straggler_that_arrives_after_the_flush(self):
        link = self._link()
        link.inject_event(_evt(6))
        link.inject_event(_evt(7))

        dropped, floor = link.begin_run_event_boundary()
        self.assertEqual(dropped, 2, "both buffered events are flushed")
        self.assertEqual(floor, 7, "floor is armed at the highest seq seen")

        # Now the race: a frame from the PREVIOUS run lands after the flush.
        link.inject_event(_evt(7))
        # ...alongside this run's own genuine events.
        link.inject_event(_evt(8))
        link.inject_event(_evt(9))

        got = [e.seq for e in link.read_events(timeout=0.0)]
        self.assertEqual(got, [8, 9],
                         "the late straggler is rejected; both genuine events survive")

    def test_the_rejection_is_counted_not_swallowed(self):
        link = self._link()
        link.inject_event(_evt(4))
        link.begin_run_event_boundary()
        self.assertEqual(link.stale_events_dropped, 0,
                         "the flush is not a floor rejection and must not be counted as one")

        link.inject_event(_evt(4))
        link.inject_event(_evt(5))
        link.read_events(timeout=0.0)
        self.assertEqual(link.stale_events_dropped, 1,
                         "a straggler rejected by the floor is reported, not hidden")

    def test_floor_on_a_link_that_has_seen_nothing_filters_nothing(self):
        """First scenario of a suite: no events seen yet, so there is no
        floor to arm and nothing may be filtered. A floor of 0 here would
        silently eat a genuine seq-0 event."""
        link = self._link()
        dropped, floor = link.begin_run_event_boundary()
        self.assertEqual(dropped, 0)
        self.assertIsNone(floor, "no events seen means no floor, NOT a floor of 0")

        link.inject_event(_evt(0))
        self.assertEqual([e.seq for e in link.read_events(timeout=0.0)], [0],
                         "a genuine first event at seq 0 must survive")

    def test_clear_disarms_the_floor(self):
        link = self._link()
        link.inject_event(_evt(5))
        link.begin_run_event_boundary()
        link.clear_run_event_boundary()
        link.inject_event(_evt(5))
        self.assertEqual([e.seq for e in link.read_events(timeout=0.0)], [5],
                         "nothing is filtered once the boundary is cleared")

    def test_a_later_boundary_raises_the_floor(self):
        link = self._link()
        link.inject_event(_evt(10))
        link.begin_run_event_boundary()
        link.inject_event(_evt(11))
        link.read_events(timeout=0.0)

        _, floor = link.begin_run_event_boundary()
        self.assertEqual(floor, 11, "each run's boundary moves the floor up to date")
        link.inject_event(_evt(11))
        link.inject_event(_evt(12))
        self.assertEqual([e.seq for e in link.read_events(timeout=0.0)], [12])

    def test_a_floor_would_be_useless_under_the_old_zeroing_reset(self):
        """Why the firmware change and this one are one fix, not two.

        With the pre-2026-08-24 firmware the seq restarted at 0 on every
        RESET_SIM. Arm a floor at the previous run's high-water mark and the
        NEXT run's own events -- numbered from 0 again -- all fall at or
        below it. The floor rejects the entire run.
        """
        link = self._link()
        for seq in (0, 1, 2, 3, 4, 5, 6, 7):
            link.inject_event(_evt(seq))
        _, floor = link.begin_run_event_boundary()
        self.assertEqual(floor, 7)

        # The old firmware's post-reset events, renumbered from 0.
        for seq in (0, 1):
            link.inject_event(_evt(seq))
        self.assertEqual(link.read_events(timeout=0.0), [],
                         "under a zeroing reset the floor would reject a run's real events -- "
                         "which is why the seq had to become monotonic first")


if __name__ == "__main__":
    unittest.main()
