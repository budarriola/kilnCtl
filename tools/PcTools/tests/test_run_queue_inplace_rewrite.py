"""save_profile_segments must be a true in-place rewrite: builtin ids are refused before
the POST, and a reply id different from the requested one fails loudly and deletes the stray."""
import json
import unittest
import unittest.mock

from kilnctrl import run_queue as rq

SEG = [{"seg_kind": 0, "target_c": 50.0, "ramp_c_per_hr": 60.0, "dwell_min": 5}]


class InPlaceRewrite(unittest.TestCase):
    def test_builtin_id_refused_before_post(self):
        calls = []
        with unittest.mock.patch.object(rq, "_post_form", lambda *a, **k: calls.append(a) or "{}"):
            with self.assertRaises(rq.RunQueueError):
                rq.save_profile_segments("h", 128, "n", 1, SEG)
        self.assertEqual(calls, [])

    def test_returned_id_mismatch_fails_and_deletes_stray(self):
        calls = []

        def fake(host, path, fields, timeout):
            calls.append((path, dict(fields)))
            return json.dumps({"ok": True, "id": 3}) if path == "/api/profile" else "{}"

        with unittest.mock.patch.object(rq, "_post_form", fake):
            with self.assertRaises(rq.RunQueueError) as cm:
                rq.save_profile_segments("h", 5, "n", 1, SEG)
        self.assertIn("returned id 3", str(cm.exception))
        self.assertEqual(calls[-1], ("/api/profile/delete", {"id": "3"}))

    def test_matching_id_is_returned(self):
        with unittest.mock.patch.object(rq, "_post_form", lambda *a, **k: json.dumps({"ok": True, "id": 5})):
            self.assertEqual(rq.save_profile_segments("h", 5, "n", 1, SEG)["id"], 5)


if __name__ == "__main__":
    unittest.main()
