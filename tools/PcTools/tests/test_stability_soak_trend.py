"""Negative/positive tests for tools/PcTools/scripts/stability_soak.py's
trend classifier (RELEASE_HARDENING.md blocker 2, item "the trend test
is not a statistically significant slope test").

Before this fix, `_trend_direction()` compared only the first and last
sample -- a shape that is provably blind to a real monotone drift whenever
the series happens to return near its starting value by the end (a
mid-run peak-and-return), and is otherwise dominated by noise in just those
two endpoint samples. This file's MANDATORY NEGATIVE TEST constructs exactly
that series (a strong upward drift that dips back near its start value) and
shows:

  * the OLD algorithm (loaded verbatim from the parent commit, b5ee26bc,
    via `git show`, so this is the actual shipped code and not a
    hand-transcribed stand-in) reports "flat" -- it never saw the drift at
    all;
  * the NEW least-squares slope classifier in the current tree correctly
    reports "UP" with a large positive slope, over the exact same data.

A regression that reverts `_trend_direction()` back to a first-vs-last
comparison makes the second assertion below fail (new code would then also
report "flat"), so this test cannot pass against the pre-fix behaviour --
run it against HEAD~ with `git stash`/checkout of just this file and confirm
that for yourself before trusting the "fixed" half.

Also covers: a genuinely flat/noisy series must not falsely trigger DOWN/UP
under the new classifier (the "does not cry wolf on noise" half of
feedback_negative_test_every_check.md), and a real, mostly-monotone downward
drift with realistic sample-to-sample noise is correctly classified DOWN.

Run with: python -m pytest tools/PcTools/tests/test_stability_soak_trend.py -q
"""
from __future__ import annotations

import importlib.util
import os
import random
import subprocess
import sys
import types
import unittest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
_SCRIPT_PATH = os.path.join(_REPO_ROOT, "tools", "PcTools", "scripts", "stability_soak.py")
_SRC = os.path.join(_REPO_ROOT, "tools", "PcTools", "src")
if _SRC not in sys.path:
    sys.path.insert(0, _SRC)

# The commit stability_soak.py's fix was made on top of, i.e. the last commit
# that still has the old first-vs-last `_trend_direction(values)`. Pinned by
# hash (not "HEAD~1" etc.) so this test's meaning does not silently drift if
# unrelated commits land on top later.
_PARENT_COMMIT_WITH_OLD_TREND = "b5ee26bc5a6a08d62540237ead8e5e2072c4a510"


def _load_current_module():
    """Import the current tree's stability_soak.py without needing a live
    board -- module-level code only builds constants and defines functions;
    `main()` is what does the connecting, and it is never called here."""
    spec = importlib.util.spec_from_file_location("stability_soak_current", _SCRIPT_PATH)
    mod = importlib.util.module_from_spec(spec)
    # Dataclasses look themselves up via sys.modules[cls.__module__] at
    # decoration time, so the module must be registered before exec.
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


def _load_old_trend_direction():
    """Fetch the exact old `_trend_direction` (and its TREND_MIN_SAMPLES
    constant) as they existed at the parent commit, via `git show`, and exec
    just that source in an isolated namespace -- this is the real shipped
    algorithm, not a paraphrase, so a fix that silently reintroduces the old
    shape would still be caught even if this test file itself is stale."""
    try:
        old_source = subprocess.run(
            ["git", "show", f"{_PARENT_COMMIT_WITH_OLD_TREND}:tools/PcTools/scripts/stability_soak.py"],
            cwd=_REPO_ROOT, capture_output=True, text=True, check=True, timeout=30,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        raise unittest.SkipTest(f"could not fetch parent commit source via git: {exc}")

    # The old source's module-level `sys.path.insert(0, str(Path(__file__)...))`
    # needs a real __file__ to resolve against -- give it the actual script
    # path (paths only, this exec never imports kilnctrl for real work here).
    modname = "stability_soak_old_b5ee26bc"
    fake_mod = types.ModuleType(modname)
    ns: dict = fake_mod.__dict__
    ns["__file__"] = _SCRIPT_PATH
    # dataclasses resolve string annotations via sys.modules[cls.__module__]
    # at decoration time -- the old file also defines @dataclass classes.
    sys.modules[modname] = fake_mod
    exec(compile(old_source, modname, "exec"), ns)  # noqa: S102
    if "_trend_direction" not in ns:
        raise AssertionError(
            "parent commit's stability_soak.py has no _trend_direction -- "
            "the pinned commit hash is wrong, fix _PARENT_COMMIT_WITH_OLD_TREND"
        )
    return ns["_trend_direction"]


class TrendDirectionNegativeTest(unittest.TestCase):
    """The mandatory negative test: old code must get this series WRONG,
    new code must get it right."""

    @classmethod
    def setUpClass(cls):
        cls.current = _load_current_module()
        cls.old_trend_direction = staticmethod(_load_old_trend_direction())

    def test_old_code_is_blind_to_a_drift_that_returns_near_its_start(self):
        # A strong, sustained upward drift (roughly +50 units/hour if sampled
        # every 60s) that happens to dip back down near its starting value
        # right at the very last sample -- the exact shape a first-vs-last
        # comparison cannot see, and the exact shape a least-squares fit
        # over every sample correctly still calls UP.
        n = 20
        interval_s = 60.0
        t_values = [i * interval_s for i in range(n)]
        values = [100.0 + 50.0 * (i / (n - 1)) for i in range(n)]  # 100 -> 150, monotone up
        values[-1] = 101.0  # last sample dips back to ~its starting value

        old_label = self.old_trend_direction(values)
        self.assertEqual(
            old_label, "flat",
            "setup assumption broken: the OLD first-vs-last algorithm was expected to "
            f"miss this drift (report 'flat'), but reported {old_label!r} -- this test's "
            "series no longer demonstrates the defect it exists to catch",
        )

        # A small floor appropriate to this abstract 100-150 unit series
        # (the real HEAP_TREND_FLOOR_BYTES_PER_HOUR is scaled for byte-sized
        # heap readings, not this test's toy units).
        floor = 10.0
        new_label, slope_per_hour = self.current._trend_direction(t_values, values, floor)
        self.assertEqual(
            new_label, "UP",
            f"NEW least-squares classifier should have caught the sustained upward drift "
            f"(slope={slope_per_hour}), but reported {new_label!r} -- the fix regressed",
        )
        self.assertGreater(slope_per_hour, floor)

    def test_old_code_is_blind_to_a_downward_drift_that_returns_near_its_start(self):
        # Mirror case: strong downward drift, recovers right at the end.
        n = 20
        interval_s = 60.0
        t_values = [i * interval_s for i in range(n)]
        values = [40_000.0 - 3_000.0 * (i / (n - 1)) for i in range(n)]  # 40000 -> 37000
        values[-1] = 39_950.0  # recovers to ~its starting value at the very last sample

        old_label = self.old_trend_direction(values)
        self.assertEqual(
            old_label, "flat",
            f"setup assumption broken: OLD algorithm reported {old_label!r}, not 'flat'",
        )

        new_label, slope_per_hour = self.current._trend_direction(
            t_values, values, self.current.HEAP_TREND_FLOOR_BYTES_PER_HOUR
        )
        self.assertEqual(new_label, "DOWN")
        self.assertLess(slope_per_hour, -self.current.HEAP_TREND_FLOOR_BYTES_PER_HOUR)


class TrendDirectionPositiveTest(unittest.TestCase):
    """Sanity checks so the new classifier does not cry wolf on noise, and
    does correctly flag a realistic sustained drift with sample noise."""

    @classmethod
    def setUpClass(cls):
        cls.current = _load_current_module()

    def test_flat_noisy_series_is_not_flagged(self):
        rng = random.Random(1234)
        n = 30
        t_values = [i * 60.0 for i in range(n)]
        values = [36_500.0 + rng.uniform(-300.0, 300.0) for _ in range(n)]
        label, slope = self.current._trend_direction(
            t_values, values, self.current.HEAP_TREND_FLOOR_BYTES_PER_HOUR
        )
        self.assertEqual(label, "flat", f"noise alone should not trigger a trend (slope={slope})")

    def test_realistic_noisy_downward_drift_is_flagged_down(self):
        rng = random.Random(99)
        n = 30
        interval_s = 60.0
        t_values = [i * interval_s for i in range(n)]
        # ~10,000 B/hour real decline, sampled every minute, plus realistic
        # +/-200 B sample noise -- comfortably above the 2000 B/hour floor.
        drift_per_sample = -10_000.0 * (interval_s / 3600.0)
        values = [40_000.0 + i * drift_per_sample + rng.uniform(-200.0, 200.0) for i in range(n)]
        label, slope = self.current._trend_direction(
            t_values, values, self.current.HEAP_TREND_FLOOR_BYTES_PER_HOUR
        )
        self.assertEqual(label, "DOWN", f"expected a real ~10000 B/hour decline to be flagged (slope={slope})")

    def test_below_min_samples_is_insufficient(self):
        t_values = [0.0, 60.0, 120.0]
        values = [100.0, 90.0, 80.0]
        label, slope = self.current._trend_direction(
            t_values, values, self.current.HEAP_TREND_FLOOR_BYTES_PER_HOUR
        )
        self.assertEqual(label, "insufficient samples")
        self.assertEqual(slope, 0.0)


if __name__ == "__main__":
    unittest.main()
