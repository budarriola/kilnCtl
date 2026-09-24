#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.lcd_sampler: the affine widget->frame
transform, the tolerance math, and JSON parsing of sample_lcd_region.ps1
-Json output (fake subprocess, per the task's negative-test requirement).

Run with: python -m pytest tools/PcTools/tests/test_bench_test_lcd_sampler.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import lcd_sampler as S  # noqa: E402


class AffineTransformTest(unittest.TestCase):
    """The four CLAUDE.md corners must map exactly onto the measured camera
    corners (a 4-point homography is an exact fit, not a least-squares
    approximation), and interior points must land correctly too."""

    def test_corners_map_within_a_pixel(self):
        for widget_pt, frame_pt in zip(S.WIDGET_CORNERS, S.FRAME_CORNERS):
            mapped = S.DEFAULT_TRANSFORM.apply(*widget_pt)
            dist = ((mapped[0] - frame_pt[0]) ** 2 + (mapped[1] - frame_pt[1]) ** 2) ** 0.5
            # A 4-point homography passes exactly through all 4
            # correspondences by construction (8 unknowns, 8 equations) --
            # any residual here is floating point / solve error only.
            self.assertLess(dist, 1.0, f"{widget_pt} -> {mapped}, expected near {frame_pt}")

    def test_lcd_centre_lands_inside_the_measured_panel(self):
        cx, cy = S.widget_to_frame(S.LCD_WIDTH / 2.0, S.LCD_HEIGHT / 2.0)
        xs = [p[0] for p in S.FRAME_CORNERS]
        ys = [p[1] for p in S.FRAME_CORNERS]
        # A generous bounding-box check (the panel is a skewed
        # quadrilateral, not an axis-aligned rectangle) -- the centre must
        # at minimum land inside the box that contains all four corners.
        self.assertGreaterEqual(cx, min(xs))
        self.assertLessEqual(cx, max(xs))
        self.assertGreaterEqual(cy, min(ys))
        self.assertLessEqual(cy, max(ys))

    def test_widget_centre_lands_near_the_quadrilateral_diagonal_crossing(self):
        # The panel centre (240, 160) should map to (approximately) where
        # the two diagonals of the measured quadrilateral (TL-BR and
        # TR-BL) cross -- an independent geometric check on the interior
        # mapping, not just the four corners themselves.
        tl, tr, bl, br = S.FRAME_CORNERS

        def _line_intersection(p1, p2, p3, p4):
            x1, y1 = p1
            x2, y2 = p2
            x3, y3 = p3
            x4, y4 = p4
            denom = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4)
            px = ((x1 * y2 - y1 * x2) * (x3 - x4) - (x1 - x2) * (x3 * y4 - y3 * x4)) / denom
            py = ((x1 * y2 - y1 * x2) * (y3 - y4) - (y1 - y2) * (x3 * y4 - y3 * x4)) / denom
            return px, py

        expected = _line_intersection(tl, br, tr, bl)
        mapped = S.widget_to_frame(S.LCD_WIDTH / 2.0, S.LCD_HEIGHT / 2.0)
        dist = ((mapped[0] - expected[0]) ** 2 + (mapped[1] - expected[1]) ** 2) ** 0.5
        self.assertLess(dist, 3.0, f"centre mapped to {mapped}, expected near diagonal crossing {expected}")

    def test_fit_is_exact_for_a_pure_scale_with_no_rotation(self):
        # Sanity check on the solve machinery itself, independent of the
        # real (skewed) calibration: an axis-aligned scale+translate must
        # be recovered exactly.
        src = [(0, 0), (10, 0), (0, 10), (10, 10)]
        dst = [(100, 200), (300, 200), (100, 400), (300, 400)]
        t = S.AffineTransform.fit(src, dst)
        for s, d in zip(src, dst):
            mapped = t.apply(*s)
            self.assertAlmostEqual(mapped[0], d[0], places=6)
            self.assertAlmostEqual(mapped[1], d[1], places=6)

    def test_fit_round_trips_a_pure_affine_parallelogram(self):
        # A parallelogram (pure affine: shear + scale + translate, no
        # perspective) must still be recovered correctly by the more
        # general homography solve -- g and h should come out ~0 and the
        # interior point should match the affine prediction exactly.
        src = [(0.0, 0.0), (10.0, 0.0), (0.0, 10.0), (10.0, 10.0)]
        # x' = 2x + y + 5 ; y' = 0.5x + 3y + 1 (a genuine affine map)
        dst = [(x * 2 + y + 5, x * 0.5 + y * 3 + 1) for x, y in src]
        t = S.AffineTransform.fit(src, dst)
        self.assertAlmostEqual(t.g, 0.0, places=6)
        self.assertAlmostEqual(t.h, 0.0, places=6)
        mapped = t.apply(4.0, 7.0)
        self.assertAlmostEqual(mapped[0], 4.0 * 2 + 7.0 + 5, places=6)
        self.assertAlmostEqual(mapped[1], 4.0 * 0.5 + 7.0 * 3 + 1, places=6)

    def test_fit_maps_interior_point_of_a_perspective_quadrilateral(self):
        # A genuine perspective (non-affine) quadrilateral: unit square ->
        # a trapezoid whose right edge is shorter than the left.
        src = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
        dst = [(0.0, 0.0), (10.0, 2.0), (0.0, 10.0), (6.0, 9.0)]
        t = S.AffineTransform.fit(src, dst)

        # Hand-solved for this exact correspondence set (b=c=f=0 from the
        # (0,0)->(0,0) and (0,1)->(0,10) equations; then solving the
        # remaining two corner equations for g,h):
        #   2g - 3h + 2 = 0 ; -7g + h + 3 = 0
        #   => g = 11/19, h = 20/19
        #   a = 10(g+1) = 300/19 ; d = 2(g+1) = 60/19 ; e = 10(h+1) = 390/19
        # At the centre (0.5, 0.5): w = 0.5*(g+h) + 1 = 69/38, giving
        #   x' = (0.5*a) / w = 100/23 ; y' = (0.5*(d+e)) / w = 150/23
        expected_x = 100.0 / 23.0
        expected_y = 150.0 / 23.0
        mapped = t.apply(0.5, 0.5)
        self.assertAlmostEqual(mapped[0], expected_x, places=6)
        self.assertAlmostEqual(mapped[1], expected_y, places=6)
        # And independently, the point must land inside the bounding box of
        # the 4 mapped corners (a sanity floor: a perspective map of an
        # interior point of the unit square can never land outside the
        # quadrilateral it warps into).
        xs = [p[0] for p in dst]
        ys = [p[1] for p in dst]
        self.assertGreaterEqual(mapped[0], min(xs))
        self.assertLessEqual(mapped[0], max(xs))
        self.assertGreaterEqual(mapped[1], min(ys))
        self.assertLessEqual(mapped[1], max(ys))

    def test_fit_rejects_collinear_points(self):
        src = [(0, 0), (1, 0), (2, 0), (3, 0)]
        dst = [(0, 0), (1, 1), (2, 2), (3, 3)]
        with self.assertRaises(ValueError):
            S.AffineTransform.fit(src, dst)

    def test_fit_rejects_wrong_point_count(self):
        with self.assertRaises(ValueError):
            S.AffineTransform.fit([(0, 0), (1, 1)], [(0, 0), (1, 1)])


class ToleranceMathTest(unittest.TestCase):
    def test_color_distance_zero_for_identical(self):
        self.assertEqual(S.color_distance((10, 20, 30), (10, 20, 30)), 0.0)

    def test_is_off_true_close_to_bezel(self):
        self.assertTrue(S.is_off((26, 31, 43), (30, 35, 45)))

    def test_is_off_false_far_from_bezel(self):
        self.assertFalse(S.is_off((92, 192, 110), (26, 31, 43)))

    def test_matches_color_true_near_target_and_off_bezel(self):
        bezel = (26, 31, 43)
        target = (0x5C, 0xC0, 0x6E)
        sampled = (95, 190, 108)  # close to target, far from bezel
        self.assertTrue(S.matches_color(sampled, target, bezel))

    def test_matches_color_false_when_indistinguishable_from_bezel(self):
        # A region that reads basically the same as the bezel must never
        # "match" an accent color, even if it happens to be numerically
        # closer to the target than to some other color -- this is the
        # negative test for the bezel-contrast floor specifically.
        bezel = (26, 31, 43)
        target = (0x5C, 0xC0, 0x6E)
        sampled = (28, 33, 45)  # basically the bezel
        self.assertFalse(S.matches_color(sampled, target, bezel))

    def test_matches_color_false_when_far_from_target(self):
        bezel = (26, 31, 43)
        target = (0x5C, 0xC0, 0x6E)
        sampled = (200, 20, 20)  # distinct from bezel, but not the target
        self.assertFalse(S.matches_color(sampled, target, bezel))


class ParseSampleJsonTest(unittest.TestCase):
    def test_parses_region_and_bezel(self):
        stdout = '{"region":{"X":10,"Y":20,"W":8,"H":8,"R":92,"G":192,"B":110},"bezel":{"X":100,"Y":100,"W":8,"H":8,"R":26,"G":31,"B":43}}'
        sample = S.parse_sample_json(stdout)
        self.assertEqual(sample.region, (92, 192, 110))
        self.assertEqual(sample.bezel, (26, 31, 43))

    def test_parses_region_with_null_bezel(self):
        stdout = '{"region":{"X":10,"Y":20,"W":8,"H":8,"R":1,"G":2,"B":3},"bezel":null}'
        sample = S.parse_sample_json(stdout)
        self.assertEqual(sample.region, (1, 2, 3))
        self.assertIsNone(sample.bezel)

    def test_empty_output_raises(self):
        with self.assertRaises(S.LcdCaptureError):
            S.parse_sample_json("")

    def test_garbage_output_raises(self):
        with self.assertRaises(S.LcdCaptureError):
            S.parse_sample_json("region  (10,20,8x8): RGB(92,192,110)")  # old plain-text shape, not JSON

    def test_missing_region_key_raises(self):
        with self.assertRaises(S.LcdCaptureError):
            S.parse_sample_json('{"bezel":{"R":1,"G":2,"B":3}}')


class SampleRegionSubprocessTest(unittest.TestCase):
    """sample_region()/capture_full_frame() with subprocess faked out --
    never invokes powershell or ffmpeg in these tests."""

    def test_sample_region_parses_fake_subprocess_stdout(self):
        fake = mock.Mock(returncode=0, stdout='{"region":{"X":1,"Y":2,"W":8,"H":8,"R":9,"G":8,"B":7},"bezel":{"X":100,"Y":100,"W":8,"H":8,"R":1,"G":1,"B":1}}', stderr="")
        with mock.patch.object(S, "_run", return_value=fake):
            sample = S.sample_region("fake.jpg", 1, 2)
        self.assertEqual(sample.region, (9, 8, 7))
        self.assertEqual(sample.bezel, (1, 1, 1))

    def test_sample_region_nonzero_exit_raises(self):
        fake = mock.Mock(returncode=1, stdout="", stderr="ffmpeg failed sampling exit code -5")
        with mock.patch.object(S, "_run", return_value=fake):
            with self.assertRaises(S.LcdCaptureError):
                S.sample_region("fake.jpg", 1, 2)

    def test_capture_full_frame_nonzero_exit_raises(self):
        fake = mock.Mock(returncode=-5, stdout="", stderr="camera busy")
        with mock.patch.object(S, "_run", return_value=fake):
            with self.assertRaises(S.LcdCaptureError):
                S.capture_full_frame("out.jpg")

    def test_capture_full_frame_missing_output_file_raises(self):
        fake = mock.Mock(returncode=0, stdout="wrote out.jpg (12345 bytes)", stderr="")
        with mock.patch.object(S, "_run", return_value=fake):
            with mock.patch("os.path.isfile", return_value=False):
                with self.assertRaises(S.LcdCaptureError):
                    S.capture_full_frame("out.jpg")


if __name__ == "__main__":
    unittest.main()
