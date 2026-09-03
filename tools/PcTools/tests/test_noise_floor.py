"""Tests for kilnctrl.noise_floor -- the run-to-run noise-floor measurement
(PID_EXPANSION_PLAN.md SS3.3's iterative-tuning item) and the artifact it
produces for pid_ab_compare.py to consume.

Uses the same p7_fuzzy0_http_excerpt.jsonl fixture pid_ab_compare's own
tests use, duplicated with small per-run offsets to stand in for "N repeats
of one configuration" -- the offsets are deliberately small (well under the
4.8C confound example) so this is exercising spread measurement, not the
start-temp confound gate (that is pid_ab_compare's own test file's job).

Plain ``unittest`` throughout (no ``pytest``): pytest is not installed in
this environment, and a module-level ``import pytest`` makes the WHOLE file
un-importable under ``python -m unittest`` (confirmed: every other
pytest-only test file in this tree currently errors out at collection the
same way). Converting this file's own tests off pytest is what lets them
actually run and be watched to fail during development, per this project's
"a test you did not watch fail does not count" rule.
"""
from __future__ import annotations

import json
import math
import os
import shutil
import tempfile
import unittest

from kilnctrl import noise_floor as nf
from kilnctrl import pid_ab_compare as ab

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")
EXCERPT = os.path.join(FIXTURES, "p7_fuzzy0_http_excerpt.jsonl")


def _write_offset_copy(tmp_dir, src_path, error_offset_c, name):
    """Copy the fixture, nudging every zone's mean_error_c-derived signal by
    changing actual_c only (target_c held fixed) -- a synthetic 'repeat run'
    with a small, controlled per-run offset standing in for real run-to-run
    noise. Leaves any 'status' block (start-condition covariate) untouched."""
    out = os.path.join(tmp_dir, name)
    lines = []
    with open(src_path) as fh:
        for line in fh:
            line = line.strip()
            if not line or not line.startswith("{"):
                continue
            obj = json.loads(line)
            body = obj.get("exec")
            if not isinstance(body, dict) or "zones" not in body:
                continue
            for z in body["zones"]:
                z["actual_c"] = z["actual_c"] + error_offset_c
            lines.append(json.dumps(obj))
    with open(out, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    return out


def _write_lines(tmp_dir, name, objs):
    """Write a list of dicts (or raw strings) as a JSONL capture file."""
    out = os.path.join(tmp_dir, name)
    with open(out, "w") as fh:
        for obj in objs:
            if isinstance(obj, str):
                fh.write(obj + "\n")
            else:
                fh.write(json.dumps(obj) + "\n")
    return out


def _status_row(t, elapsed_s, channels, enclosure_temp_c=25.0, actual_c=25.0):
    return {
        "t": t,
        "exec": {
            "state": "ramp",
            "segment_index": 0,
            "dwelling": False,
            "target_c": 100.0,
            "elapsed_s": elapsed_s,
            "zones": [{"zone": i, "actual_c": actual_c, "target_c": 100.0} for i in range(len(channels))],
        },
        "status": {
            "channels": channels,
            "enclosure_temp_c": enclosure_temp_c,
        },
    }


def _channel(i, temp_c, cj_c, valid=True):
    return {"channel": i, "temp_c": temp_c, "cj_c": cj_c, "valid": valid,
            "fault_status": 0, "spi_failed": False, "stale": False, "age_ms": 0}


class NoiseFloorTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.mkdtemp(prefix="noise_floor_test_")
        self.addCleanup(shutil.rmtree, self._tmp, ignore_errors=True)

    def repeat_paths(self):
        # Three "repeats" with small, distinct offsets so mean/std/range are
        # all non-degenerate (not every value identical).
        return [
            _write_offset_copy(self._tmp, EXCERPT, 0.0, "rep1.jsonl"),
            _write_offset_copy(self._tmp, EXCERPT, 0.2, "rep2.jsonl"),
            _write_offset_copy(self._tmp, EXCERPT, -0.3, "rep3.jsonl"),
        ]


# ---------------------------------------------------------------------------
# compute_repeat_spread
# ---------------------------------------------------------------------------

class ComputeRepeatSpreadTests(NoiseFloorTestCase):
    def test_requires_at_least_two_paths(self):
        with self.assertRaises(ValueError):
            nf.compute_repeat_spread([EXCERPT])

    def test_spread_covers_every_zone(self):
        stats = nf.compute_repeat_spread(self.repeat_paths())
        zones = {s.zone for s in stats}
        self.assertEqual(zones, {0, 1, 2})

    def test_spread_n_matches_repeat_count(self):
        stats = nf.compute_repeat_spread(self.repeat_paths())
        whole = [s for s in stats if s.metric == "iae_normalized_whole_c" and s.zone == 0][0]
        self.assertEqual(whole.n, 3)

    def test_spread_range_is_nonzero_for_offset_repeats(self):
        stats = nf.compute_repeat_spread(self.repeat_paths())
        whole = [s for s in stats if s.metric == "iae_normalized_whole_c" and s.zone == 0][0]
        self.assertIsNotNone(whole.range_c)
        self.assertGreater(whole.range_c, 0.0)

    def test_spread_range_equals_max_minus_min(self):
        stats = nf.compute_repeat_spread(self.repeat_paths())
        whole = [s for s in stats if s.metric == "iae_normalized_whole_c" and s.zone == 0][0]
        self.assertAlmostEqual(whole.range_c, max(whole.values) - min(whole.values))

    def test_identical_repeats_give_zero_range(self):
        stats = nf.compute_repeat_spread([EXCERPT, EXCERPT, EXCERPT])
        whole = [s for s in stats if s.metric == "iae_normalized_whole_c" and s.zone == 0][0]
        self.assertAlmostEqual(whole.range_c, 0.0)
        self.assertAlmostEqual(whole.std_c, 0.0)

    def test_single_value_key_has_no_floor(self):
        stats = nf.compute_repeat_spread(self.repeat_paths())
        for s in stats:
            if s.n < nf.MIN_REPEATS_FOR_FLOOR:
                self.assertIsNone(s.range_c)
                self.assertIsNone(s.std_c)


# ---------------------------------------------------------------------------
# build_artifact / load_artifact / floor_lookup
# ---------------------------------------------------------------------------

class ArtifactTests(NoiseFloorTestCase):
    def test_build_artifact_schema(self):
        paths = self.repeat_paths()
        artifact = nf.build_artifact(paths)
        self.assertEqual(artifact["schema_version"], nf.SCHEMA_VERSION)
        self.assertEqual(artifact["n_repeats"], 3)
        self.assertEqual(artifact["generated_from"], paths)
        self.assertIsInstance(artifact["entries"], dict)
        self.assertGreater(len(artifact["entries"]), 0)

    def test_build_artifact_entry_shape(self):
        artifact = nf.build_artifact(self.repeat_paths())
        key = nf._key_str(0, "iae_normalized_whole_c", None)
        entry = artifact["entries"][key]
        self.assertEqual(entry["zone"], 0)
        self.assertEqual(entry["metric"], "iae_normalized_whole_c")
        self.assertIsNone(entry["segment"])
        self.assertEqual(entry["n"], 3)
        self.assertGreaterEqual(entry["noise_floor_c"], 0.0)

    def test_build_artifact_carries_start_conditions_additively(self):
        # schema_version bump: the artifact grows a new "start_conditions"
        # key but "entries" (schema_version 1's whole payload) is untouched
        # -- an old reader that only ever looked at "entries"/floor_lookup
        # still works unmodified.
        artifact = nf.build_artifact(self.repeat_paths())
        self.assertEqual(nf.SCHEMA_VERSION, 2)
        self.assertIn("start_conditions", artifact)
        self.assertIn("entries", artifact)
        key = nf._key_str(0, "iae_normalized_whole_c", None)
        self.assertIn(key, artifact["entries"])  # schema_version-1 shape intact

    def test_floor_lookup_returns_none_for_missing_artifact(self):
        self.assertIsNone(nf.floor_lookup(None, 0, "iae_normalized_whole_c", None))

    def test_floor_lookup_returns_none_for_unknown_key(self):
        artifact = nf.build_artifact(self.repeat_paths())
        self.assertIsNone(nf.floor_lookup(artifact, 99, "no_such_metric", None))

    def test_floor_lookup_round_trips_through_json(self):
        artifact = nf.build_artifact(self.repeat_paths())
        out_path = os.path.join(self._tmp, "noise_floor.json")
        with open(out_path, "w") as fh:
            json.dump(artifact, fh)
        loaded = nf.load_artifact(out_path)
        key0 = nf._key_str(0, "iae_normalized_whole_c", None)
        self.assertAlmostEqual(
            nf.floor_lookup(loaded, 0, "iae_normalized_whole_c", None),
            artifact["entries"][key0]["noise_floor_c"],
        )

    def test_load_artifact_missing_file_returns_none(self):
        self.assertIsNone(nf.load_artifact("does/not/exist/noise_floor.json"))

    def test_load_artifact_malformed_json_returns_none(self):
        p = os.path.join(self._tmp, "bad.json")
        with open(p, "w") as fh:
            fh.write("{not json")
        self.assertIsNone(nf.load_artifact(p))


# ---------------------------------------------------------------------------
# extract_start_conditions -- the start-temperature covariate
# ---------------------------------------------------------------------------

class ExtractStartConditionsTests(NoiseFloorTestCase):
    def test_extracts_per_channel_temp_and_cj(self):
        path = _write_lines(self._tmp, "run.jsonl", [
            _status_row(0.0, 0.0, [_channel(0, 28.67, 28.14), _channel(1, 28.68, 28.52), _channel(2, 28.58, 28.80)],
                        enclosure_temp_c=28.55),
        ])
        cond = nf.extract_start_conditions(path)
        self.assertIsNotNone(cond)
        self.assertEqual(len(cond["channels"]), 3)
        self.assertAlmostEqual(cond["channels"][0]["temp_c"], 28.67)
        self.assertAlmostEqual(cond["channels"][0]["cj_c"], 28.14)
        self.assertAlmostEqual(cond["enclosure_temp_c"], 28.55)
        self.assertAlmostEqual(cond["start_temp_c_mean"], (28.67 + 28.68 + 28.58) / 3)

    def test_delta_c_is_temp_minus_cj_for_valid_channels(self):
        path = _write_lines(self._tmp, "run.jsonl", [
            _status_row(0.0, 0.0, [_channel(0, 30.0, 28.0)]),
        ])
        cond = nf.extract_start_conditions(path)
        self.assertAlmostEqual(cond["channels"][0]["delta_c"], 2.0)
        self.assertAlmostEqual(cond["delta_c_mean"], 2.0)

    def test_invalid_channel_excluded_from_summary_but_kept_visible(self):
        path = _write_lines(self._tmp, "run.jsonl", [
            _status_row(0.0, 0.0, [_channel(0, 30.0, 28.0, valid=True),
                                    _channel(1, 999.0, 999.0, valid=False)]),
        ])
        cond = nf.extract_start_conditions(path)
        self.assertEqual(len(cond["channels"]), 2)  # still visible
        self.assertFalse(cond["channels"][1]["valid"])
        self.assertIsNone(cond["channels"][1]["delta_c"])
        # summary mean uses only the valid channel
        self.assertAlmostEqual(cond["start_temp_c_mean"], 30.0)

    def test_missing_file_returns_none(self):
        self.assertIsNone(nf.extract_start_conditions("does/not/exist.jsonl"))

    def test_empty_file_returns_none(self):
        path = _write_lines(self._tmp, "empty.jsonl", [])
        self.assertIsNone(nf.extract_start_conditions(path))

    def test_first_row_with_no_status_returns_none(self):
        path = _write_lines(self._tmp, "run.jsonl", [
            {"t": 0.0, "exec": {"state": "ramp", "segment_index": 0, "dwelling": False,
                                 "target_c": 100.0, "elapsed_s": 0.0, "zones": [{"zone": 0, "actual_c": 25.0}]}},
        ])
        self.assertIsNone(nf.extract_start_conditions(path))

    def test_first_row_with_no_channels_returns_none(self):
        path = _write_lines(self._tmp, "run.jsonl", [
            {"t": 0.0, "exec": {"state": "ramp", "segment_index": 0, "dwelling": False,
                                 "target_c": 100.0, "elapsed_s": 0.0, "zones": [{"zone": 0, "actual_c": 25.0}]},
             "status": {"enclosure_temp_c": 25.0}},
        ])
        self.assertIsNone(nf.extract_start_conditions(path))

    def test_malformed_channel_entries_do_not_crash(self):
        path = _write_lines(self._tmp, "run.jsonl", [
            _status_row(0.0, 0.0, [_channel(0, 30.0, 28.0), "not a dict",
                                    {"channel": 2, "temp_c": "warm", "cj_c": 28.0, "valid": True}]),
        ])
        cond = nf.extract_start_conditions(path)
        self.assertIsNotNone(cond)
        # channel 0 kept and numeric; the string entry dropped; channel 2's
        # non-numeric temp_c comes back as None rather than raising
        self.assertEqual(len(cond["channels"]), 2)
        self.assertAlmostEqual(cond["start_temp_c_mean"], 30.0)

    def test_malformed_capture_lines_do_not_crash(self):
        path = os.path.join(self._tmp, "garbage.jsonl")
        with open(path, "w") as fh:
            fh.write("not json at all\n")
            fh.write("\n")
            fh.write(json.dumps({"t": 0.0, "status": {"channels": []}}) + "\n")  # no exec
        self.assertIsNone(nf.extract_start_conditions(path))

    def test_cooldown_sidecar_is_not_treated_as_a_run(self):
        # The real capture pipeline writes a `*.cooldown.jsonl` sidecar next
        # to every run file. This module never scans a directory for those
        # (callers pass explicit paths), but if one is ever handed in by
        # mistake it must not silently masquerade as a run: cooldown rows
        # are captured post-firing (board still hot/cooling), so a wrong
        # extraction here would be actively misleading, not just absent.
        # Guard: parse it like any other file (it IS a valid HTTP capture
        # shape) but confirm the extraction is exactly the sidecar's own
        # first-row numbers, not silently substituted from elsewhere/blank.
        sidecar = _write_lines(self._tmp, "run1.jsonl.cooldown.jsonl", [
            _status_row(0.0, 0.0, [_channel(0, 45.0, 40.0)], enclosure_temp_c=42.0),
        ])
        cond = nf.extract_start_conditions(sidecar)
        self.assertIsNotNone(cond)
        self.assertAlmostEqual(cond["start_temp_c_mean"], 45.0)
        self.assertEqual(cond["path"], sidecar)

    def test_ambiguous_multi_run_file_returns_none_without_explicit_index(self):
        # Two runs, BOTH carrying zone data anywhere inside them -- the same
        # ambiguity ab.load_run refuses via MultiRunError. extract_start_
        # conditions must not guess; it degrades to "no start condition".
        path = _write_lines(self._tmp, "two_run.jsonl", [
            _status_row(0.0, 0.0, [_channel(0, 28.0, 27.0)]),
            _status_row(1.0, 10.0, [_channel(0, 29.0, 27.5)]),
            _status_row(2.0, 0.0, [_channel(0, 50.0, 45.0)]),  # elapsed_s drop -> new run
            _status_row(3.0, 10.0, [_channel(0, 51.0, 45.5)]),
        ])
        self.assertIsNone(nf.extract_start_conditions(path))
        # but an explicit index resolves it
        cond0 = nf.extract_start_conditions(path, run_index=0)
        self.assertAlmostEqual(cond0["start_temp_c_mean"], 28.0)
        cond1 = nf.extract_start_conditions(path, run_index=1)
        self.assertAlmostEqual(cond1["start_temp_c_mean"], 50.0)

    def test_real_capture_noise_floor_p7_run1(self):
        path = os.path.join(os.path.dirname(__file__), "..", "..", "..", "logs", "coupling",
                             "noise_floor_p7_run1.jsonl")
        path = os.path.normpath(path)
        if not os.path.exists(path):
            self.skipTest("real capture not present in this checkout")
        cond = nf.extract_start_conditions(path)
        self.assertIsNotNone(cond)
        temps = sorted(c["temp_c"] for c in cond["channels"])
        self.assertEqual(temps, [28.58, 28.67, 28.68])
        self.assertAlmostEqual(cond["enclosure_temp_c"], 28.55)


# ---------------------------------------------------------------------------
# build_start_report -- spread, like-for-like gate, ordering, correlation
# ---------------------------------------------------------------------------

class BuildStartReportTests(NoiseFloorTestCase):
    def _paths_with_starts(self, starts):
        paths = []
        for i, start in enumerate(starts):
            paths.append(_write_lines(self._tmp, f"run{i}.jsonl", [
                _status_row(0.0, 0.0, [_channel(0, start, start - 0.5)], actual_c=start),
                _status_row(1.0, 10.0, [_channel(0, start + 5, start + 4.5)], actual_c=start + 5),
            ]))
        return paths

    def test_like_for_like_true_for_tight_spread(self):
        paths = self._paths_with_starts([28.6, 28.7, 28.5])
        report = nf.build_start_report(paths)
        self.assertLess(report["start_temp_c_range"], nf.LIKE_FOR_LIKE_THRESHOLD_C)
        self.assertTrue(report["like_for_like"])
        self.assertIsNone(report["warning"])

    def test_not_like_for_like_for_wide_spread(self):
        # Mirrors the real 3.9C spread cited in the campaign's own captures.
        paths = self._paths_with_starts([28.6, 27.1, 28.6, 31.0])
        report = nf.build_start_report(paths)
        self.assertGreater(report["start_temp_c_range"], nf.LIKE_FOR_LIKE_THRESHOLD_C)
        self.assertFalse(report["like_for_like"])
        self.assertIsNotNone(report["warning"])
        self.assertIn("NOT a like-for-like", report["warning"])

    def test_min_max_range_are_correct(self):
        paths = self._paths_with_starts([28.6, 27.1, 31.0])
        report = nf.build_start_report(paths)
        self.assertAlmostEqual(report["start_temp_c_min"], 27.1)
        self.assertAlmostEqual(report["start_temp_c_max"], 31.0)
        self.assertAlmostEqual(report["start_temp_c_range"], 31.0 - 27.1)

    def test_monotonic_increasing_detected(self):
        paths = self._paths_with_starts([27.0, 28.0, 29.0, 30.0])
        report = nf.build_start_report(paths)
        self.assertTrue(report["monotonic_with_run_order"])

    def test_monotonic_decreasing_detected(self):
        paths = self._paths_with_starts([30.0, 29.0, 28.0, 27.0])
        report = nf.build_start_report(paths)
        self.assertTrue(report["monotonic_with_run_order"])

    def test_non_monotonic_scatter_detected(self):
        paths = self._paths_with_starts([28.0, 31.0, 27.0, 29.0])
        report = nf.build_start_report(paths)
        self.assertFalse(report["monotonic_with_run_order"])

    def test_correlation_reports_n_and_raw_pairs(self):
        paths = self._paths_with_starts([27.0, 28.0, 29.0])
        report = nf.build_start_report(paths)
        vs = report["vs_outcome"]
        self.assertEqual(vs["n"], len(paths))
        self.assertEqual(len(vs["pairs"]), len(paths))
        for p in vs["pairs"]:
            self.assertIn("start_temp_c", p)
            self.assertIn("outcome", p)
        self.assertIn("too small", vs["caution"])

    def test_degenerate_input_missing_file_does_not_crash(self):
        paths = self._paths_with_starts([28.0, 29.0])
        paths[0] = "does/not/exist.jsonl"
        report = nf.build_start_report(paths)
        self.assertEqual(report["n_missing_start_temp"], 1)
        self.assertEqual(len(report["vs_outcome"]["pairs"]), 1)

    def test_no_runs_have_start_data_degrades_cleanly(self):
        paths = ["does/not/exist1.jsonl", "does/not/exist2.jsonl"]
        report = nf.build_start_report(paths)
        self.assertIsNone(report["start_temp_c_min"])
        self.assertIsNone(report["like_for_like"])
        self.assertIsNone(report["monotonic_with_run_order"])
        self.assertEqual(report["n_missing_start_temp"], 2)
        # must not raise formatting it either
        text = nf.format_start_report_text(report)
        self.assertIn("no runs had extractable start conditions", text)

    def test_real_four_captures_are_not_like_for_like(self):
        # The exact four captures named in the campaign's own record of the
        # confound: a 3.9C-class spread that must trip the gate.
        base = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "logs", "coupling"))
        paths = [
            os.path.join(base, "noise_floor_p7_run1.jsonl"),
            os.path.join(base, "noise_floor_p7b_run1.jsonl"),
            os.path.join(base, "p7_oldmatrix_runA.jsonl"),
            os.path.join(base, "p7_oldmatrix_runC.jsonl"),
        ]
        if not all(os.path.exists(p) for p in paths):
            self.skipTest("real captures not present in this checkout")
        report = nf.build_start_report(paths, run_indices=[None, None, None, 0])
        self.assertFalse(report["like_for_like"])
        self.assertGreater(report["start_temp_c_range"], 3.0)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

class CliTests(NoiseFloorTestCase):
    def test_cli_build_writes_file(self):
        paths = self.repeat_paths()
        out_path = os.path.join(self._tmp, "out.json")
        rc = nf.main(["build", *paths, "--out", out_path])
        self.assertEqual(rc, 0)
        self.assertTrue(os.path.exists(out_path))
        with open(out_path) as fh:
            data = json.load(fh)
        self.assertEqual(data["n_repeats"], 3)
        self.assertIn("start_conditions", data)

    def test_cli_report_runs(self):
        import contextlib
        import io
        paths = self.repeat_paths()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = nf.main(["report", *paths])
        self.assertEqual(rc, 0)
        out = buf.getvalue()
        self.assertIn("z0", out)
        self.assertIn("start temperature", out)

    def test_cli_report_rejects_single_path(self):
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = nf.main(["report", EXCERPT])
        self.assertEqual(rc, 1)
        self.assertIn("error", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
