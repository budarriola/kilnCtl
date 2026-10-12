"""profiles_save refuses to strip aux rules / relay-IO segments, reads back;
post_profile normalizes transport errors; stop/pause/resume refusals pass through."""
from __future__ import annotations

import json
import os
import sys
import types
import unittest
import unittest.mock as um

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import aux_http_client as ahc  # noqa: E402
from kilnctrl import http_auth, mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_profiles as mp  # noqa: E402
from kilnctrl import profile_edit_http_client as pec  # noqa: E402

SEGS = json.dumps([{"target_c": 100, "ramp_c_per_hr": 50, "dwell_min": 1}])


class _Prof:
    def __init__(self, detail_name="p", nsegs=1):
        self.saves = []
        self.detail_name = detail_name
        self.nsegs = nsegs

    def save(self, pid, name, mask, segs):
        self.saves.append(pid)
        return types.SimpleNamespace(ok=True, id=3, warning_count=0, error="")

    def get(self, pid):
        return types.SimpleNamespace(name=self.detail_name, segments=[types.SimpleNamespace(target_c=100.0, ramp_c_per_hr=50.0, dwell_min=1)] * self.nsegs)


class StripGuardTests(unittest.TestCase):
    def _run(self, detail, prof=None, **kw):
        prof = prof or _Prof()
        with um.patch.object(mp._srv, "_profiles", prof, create=True), \
                um.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="1.2.3.4"), \
                um.patch.object(ahc, "_get_json", side_effect=detail if isinstance(detail, Exception) else None,
                                return_value=None if isinstance(detail, Exception) else detail):
            return mp.profiles_save(3, "p", 1, SEGS, **kw), prof

    def test_readback_throw_or_field_mismatch_fails(self):
        class Throws(_Prof):
            def get(self, pid):
                raise OSError("x")
        out, _ = self._run({"on_off_rules": [], "segments": []}, prof=Throws())
        self.assertIn("FAILED", out)
        class Mask(_Prof):
            def get(self, pid):
                return types.SimpleNamespace(name="p", segments=[0], zone_mask=7)
        out, _ = self._run({"on_off_rules": [], "segments": []}, prof=Mask())
        self.assertIn("FAILED", out)

    def test_aux_rules_refused(self):
        out, prof = self._run({"on_off_rules": [{"zone": 8}], "segments": []})
        self.assertIn("STRIP", out)
        self.assertEqual(prof.saves, [])

    def test_relay_io_segment_refused(self):
        out, prof = self._run({"on_off_rules": [], "segments": [{"seg_kind": 1, "io_target": 2}]})
        self.assertIn("STRIP", out)
        self.assertEqual(prof.saves, [])

    def test_unreadable_fails_closed(self):
        out, prof = self._run(ahc.AuxHttpError("boom", 500, "x"))
        self.assertIn("could not read", out)
        self.assertEqual(prof.saves, [])

    def test_empty_slot_404_allowed(self):
        out, prof = self._run(ahc.AuxHttpError("nf", 404, "x"))
        self.assertTrue(out.startswith("ok"))
        self.assertEqual(prof.saves, [3])

    def test_plain_slot_saves_and_verifies(self):
        out, _ = self._run({"on_off_rules": [], "segments": [{"seg_kind": 0}]})
        self.assertIn("read back verified", out)

    def test_override_must_be_true(self):
        out, prof = self._run({"on_off_rules": [{"zone": 8}], "segments": []}, allow_strip="yes")
        self.assertIn("STRIP", out)
        out, prof = self._run({"on_off_rules": [{"zone": 8}], "segments": []}, allow_strip=True)
        self.assertTrue(out.startswith("ok"))

    def test_readback_mismatch(self):
        out, _ = self._run({"on_off_rules": [], "segments": []}, prof=_Prof(detail_name="other"))
        self.assertIn("FAILED", out)


class PostProfileTransportTests(unittest.TestCase):
    def test_timeout_and_auth_normalized(self):
        for exc in (TimeoutError("t"), http_auth.HttpAuthError("a"), OSError("o")):
            with um.patch.object(http_auth, "urlopen", side_effect=exc):
                with self.assertRaises(pec.ProfileEditHttpError):
                    pec.post_profile("h", 1, "n", 1, [])


class StopRefusalTests(unittest.TestCase):
    def test_reason_passthrough(self):
        p = types.SimpleNamespace(stop=lambda: types.SimpleNamespace(ok=False, reason="mode gate"))
        with um.patch.object(mp._srv, "_profiles", p, create=True):
            out = mp.profiles_stop()
        self.assertIn("mode gate", out)
        self.assertNotIn("nothing running to stop", out)


if __name__ == "__main__":
    unittest.main()
