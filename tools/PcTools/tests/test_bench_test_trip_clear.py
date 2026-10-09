"""wait_for_trip_clear (cases_smoke.py): fake-board tests for the 10 s window
and the clear_ack / trip_reason_timeline diagnostics."""
import unittest

from kilnctrl.bench_test import cases_smoke as CS


class _Srv:
    """Diag reads 6 until `clear_lands_after_s` of fake time has passed since
    safety_clear_trip(), then 0."""

    def __init__(self, clock, clear_lands_after_s, ack="ACK"):
        self.clock = clock
        self.land = clear_lands_after_s
        self.ack = ack
        self.cleared_at = None
        self.clear_calls = 0

    def safety_clear_trip(self):
        self.clear_calls += 1
        self.cleared_at = self.clock["t"]
        return self.ack

    def safety_get_diag(self):
        done = (self.land is not None
                and self.clock["t"] - self.cleared_at >= self.land)
        return f"trip_reason: {0 if done else 6} | trip_mask: 0x0000"


def _ctx():
    clock = {"t": 0.0}
    return clock, {
        "_now": lambda: clock["t"],
        "_sleep": lambda s: clock.__setitem__("t", clock["t"] + s),
    }


class WaitForTripClearTests(unittest.TestCase):
    def test_timeout_is_ten_seconds(self):
        self.assertEqual(CS._TRIP_CLEAR_POLL_TIMEOUT_S, 10.0)

    def test_clear_landing_after_3s_before_10s_passes(self):
        clock, ctx = _ctx()
        srv = _Srv(clock, clear_lands_after_s=3.5)
        obs = {}
        reason, elapsed = CS.wait_for_trip_clear(ctx, srv, observed=obs)
        self.assertEqual(reason, 0)
        self.assertGreater(elapsed, 3.04)
        self.assertLess(elapsed, 10.0)
        self.assertEqual(srv.clear_calls, 1)

    def test_observed_keys(self):
        clock, ctx = _ctx()
        srv = _Srv(clock, clear_lands_after_s=0.6, ack="clear sent")
        obs = {"existing": 1}
        CS.wait_for_trip_clear(ctx, srv, observed=obs)
        self.assertEqual(obs["existing"], 1)
        self.assertEqual(obs["clear_ack"], "clear sent")
        tl = obs["trip_reason_timeline"]
        self.assertEqual(tl[0], (0.0, 6))
        self.assertEqual(tl[-1][1], 0)
        self.assertTrue(all(isinstance(t, float) and t == round(t, 2) for t, _ in tl))
        self.assertEqual([r for _, r in tl][:-1], [6] * (len(tl) - 1))

    def test_never_clears_fails_with_timeline(self):
        clock, ctx = _ctx()
        srv = _Srv(clock, clear_lands_after_s=None)
        obs = {}
        reason, elapsed = CS.wait_for_trip_clear(ctx, srv, observed=obs)
        self.assertEqual(reason, 6)
        self.assertGreaterEqual(elapsed, 10.0)
        self.assertTrue(all(r == 6 for _, r in obs["trip_reason_timeline"]))

    def test_observed_is_optional(self):
        clock, ctx = _ctx()
        reason, _ = CS.wait_for_trip_clear(ctx, _Srv(clock, 0.0))
        self.assertEqual(reason, 0)


if __name__ == "__main__":
    unittest.main()
