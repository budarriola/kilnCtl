"""REVIEW_ZONESPRECFX6 fixes: current GET fixture + drift, key classification, bounds, name,
read-back-exception note, partial-note wording."""
import copy
import json
import os
import re
import sys
import types
import unittest
import unittest.mock as um

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import config_presets, mcp_server_config_presets, mcp_server_ui_test, zones_http_client  # noqa: E402

HERE = os.path.dirname(__file__)
FW = os.path.join(HERE, "..", "..", "..", "firmware", "KilnFW", "App", "drivers")


def _fixture():
    with open(os.path.join(HERE, "fixtures", "zones_get_capture.json"), encoding="utf-8") as f:
        return json.load(f)


def _all_keys(obj, out):
    if isinstance(obj, dict):
        for k, v in obj.items():
            out.add(k)
            _all_keys(v, out)
    elif isinstance(obj, list):
        for v in obj:
            _all_keys(v, out)
    return out


_CH = 3  # MAX31856_CHANNEL_COUNT


def _src_group_names():
    with open(os.path.join(FW, "http", "zones_http_post_parse.c"), encoding="utf-8") as f:
        src = f.read()
    m = re.search(r"SRC_GROUP_NAMES\[SRC_GROUP_COUNT\]\s*=\s*\{(.*?)\};", src, re.S)
    return re.findall(r'"([a-z0-9_]+)"', m.group(1))


def _expand(key):
    """coupling_c%u -> coupling_c0..2 (every %u in these keys is a channel index)."""
    if "%u" not in key:
        return {key}
    return {key.replace("%u", str(j)) for j in range(_CH)}


def _emitted_keys(per_zone=False):
    with open(os.path.join(FW, "http", "zones_http_get.c"), encoding="utf-8") as f:
        src = f.read()
    start = src.index("esp_err_t zones_get_handler(")
    end = src.index("esp_err_t zones_diag_get_handler(")
    body = re.sub(r"/\*.*?\*/", "", src[start:end], flags=re.S)
    if per_zone:
        body = body[body.index('\\"zones\\":['):]
    out = set()
    # "ok"/"error" are the error-response envelope, not GET payload keys.
    for k in re.findall(r'\\"([a-z][a-z0-9_]*(?:%u[a-z0-9_]*)*)\\":', body):
        out |= _expand(k)
    return out - {"ok", "error"}


def _preset():
    return copy.deepcopy(config_presets.load_preset_data("bench_fixture"))


class FixtureDriftTest(unittest.TestCase):
    def test_fixture_carries_every_key_the_firmware_emits(self):
        emitted = _emitted_keys()
        self.assertGreater(len(emitted), 80)  # parser sanity: not an empty match
        have = _all_keys(_fixture(), set())
        self.assertEqual(sorted(emitted - have), [])

    def test_template_built_keys_are_expanded(self):
        emitted = _emitted_keys()
        for j in range(_CH):
            self.assertIn(f"coupling_c{j}", emitted)

    def test_settings_source_group_names_match_source(self):
        names = _src_group_names()
        self.assertGreaterEqual(len(names), 5)
        for z in _fixture()["zones"]:
            self.assertEqual(sorted(z["settings_source_groups"]), sorted(names))

    def test_every_zone_carries_every_per_zone_key(self):
        per_zone = _emitted_keys(per_zone=True)
        self.assertGreater(len(per_zone), 60)
        cap = _fixture()
        for z in cap["zones"]:
            self.assertEqual(sorted(per_zone - {"zones"} - set(z)), [], f"zone {z.get('index')}")

    def test_every_fixture_zone_key_is_classified(self):
        for z in _fixture()["zones"]:
            for key in z:
                self.assertTrue(zones_http_client.preset_zone_key_known(key), key)

    def test_ignored_and_override_sets_are_disjoint(self):
        self.assertEqual(zones_http_client._PRESET_ZONE_OVERRIDE_FIELDS
                         & zones_http_client._PRESET_ZONE_KNOWN_IGNORED_FIELDS, set())

    def test_classification_of_named_keys(self):
        ov = zones_http_client._PRESET_ZONE_OVERRIDE_FIELDS
        ig = zones_http_client._PRESET_ZONE_KNOWN_IGNORED_FIELDS
        for k in ("relay_type", "progress_band_c", "coil_power_w"):
            self.assertIn(k, ov)
        for k in ("fuzzy_model_valid", "autotune_baseline_k_dc"):
            self.assertIn(k, ig)

    def test_current_get_zones_load_and_post(self):
        cap = _fixture()
        d = _preset()
        d["zones"] = cap["zones"]
        config_presets._validate("cap", d)
        zones_http_client.build_post_body(cap, d)  # must not raise

    def test_unknown_zone_key_refused_by_validate(self):
        d = _preset()
        d["zones"][0]["bogus_field"] = 1
        with self.assertRaises(config_presets.ConfigPresetError):
            config_presets._validate("t", d)


class BoundsTest(unittest.TestCase):
    def test_constants_mirror_firmware_defines(self):
        with open(os.path.join(FW, "persist", "zones_config_accessors.h"), encoding="utf-8") as f:
            acc = f.read()
        with open(os.path.join(FW, "persist", "zones_config_json.h"), encoding="utf-8") as f:
            js = f.read()
        self.assertIn(f"#define ZONE_PC_LINK_SILENCE_MS_MAX {config_presets.PC_LINK_ABORT_SILENCE_MS_MAX:.1f}f", acc)
        self.assertIn(f"#define ZONE_NAME_MAX_LEN {config_presets.ZONE_NAME_MAX_LEN}", acc)
        self.assertIn(f"#define ZONE_EASE_OFF_WINDOW_MULT_MIN {config_presets.EASE_OFF_WINDOW_MULT_MIN}f", js)
        self.assertIn(f"#define ZONE_EASE_OFF_WINDOW_MULT_MAX {config_presets.EASE_OFF_WINDOW_MULT_MAX:.1f}f", js)

    def test_top_level_ranges(self):
        for key, bad, good in (("pc_link_abort_silence_ms", (-1, 600001, 1e9), (0, 600000)),
                               ("ease_off_window_mult", (-1, -0.5, 0.05, 10.5, 50.0), (0, 0.1, 10.0, 3.5))):
            for v in bad:
                d = _preset()
                d[key] = v
                with self.assertRaises(config_presets.ConfigPresetError, msg=(key, v)):
                    config_presets._validate("t", d)
            for v in good:
                d = _preset()
                d[key] = v
                config_presets._validate("t", d)

    def test_per_zone_ease_off_range(self):
        for v in (-3.0, 0.05, 10.5):
            d = _preset()
            d["zones"][0]["ease_off_window_mult"] = v
            with self.assertRaises(config_presets.ConfigPresetError, msg=v):
                config_presets._validate("t", d)
        for v in (0, 0.1, 10.0):
            d = _preset()
            d["zones"][0]["ease_off_window_mult"] = v
            config_presets._validate("t", d)

    def test_zone_name_checked(self):
        for bad in (None, "", ["a"], 5, "x" * 16, "bad\x01name", "bad\x7fname", "é" * 8):
            d = _preset()
            d["zones"][0]["name"] = bad
            with self.assertRaises(config_presets.ConfigPresetError, msg=bad):
                config_presets._validate("t", d)
        d = _preset()
        d["zones"][0]["name"] = "x" * 15
        config_presets._validate("t", d)


class ReadbackExceptionTest(unittest.TestCase):
    def test_unexpected_readback_exception_still_reports_pid_landed(self):
        control = um.MagicMock()
        control.set_zone_pid.return_value = True
        control.get_zones.return_value = (None, None, [um.MagicMock(index=i, pid_kp=None, pid_ki=0.1, pid_kd=0.0)
                                                      for i in range(2)])
        preset = {"name": "p", "zones": [{"index": i, "pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0}
                                         for i in range(2)]}
        with self.assertRaises(TypeError) as cm:
            config_presets.apply_preset(control, preset)
        note = cm.exception.preset_partial
        self.assertIn("zone 0 pid=ok", note)
        self.assertIn("zone 1 pid=ok", note)


    def test_per_zone_ranges_refused_before_any_write(self):
        bad = {"relay_type": (3, -1), "zone_type": (2, -1, 7), "progress_band_c": (0.2, 25),
               "coil_power_w": (0.05, 2e5), "hyst_c": (-1, 0.2, 30), "min_on_s": (-5, 5000),
               "min_off_s": (-5, 5000), "approach_rate_cap_c_per_hr": (-4, 0.5, 1500)}
        good = {"relay_type": (0, 2), "zone_type": (0, 1), "progress_band_c": (0, 0.5, 20),
                "coil_power_w": (0, 0.1, 100000), "hyst_c": (0, 0.5, 25), "min_on_s": (0, 1, 3600),
                "min_off_s": (0, 1, 3600), "approach_rate_cap_c_per_hr": (0, 1, 1000)}
        for key, vals in bad.items():
            for v in vals:
                d = _preset()
                d["zones"][0][key] = v
                with self.assertRaises(config_presets.ConfigPresetError, msg=(key, v)):
                    config_presets._validate("t", d)
        for key, vals in good.items():
            for v in vals:
                d = _preset()
                d["zones"][0][key] = v
                config_presets._validate("t", d)

    def test_range_table_mirrors_firmware_defines(self):
        with open(os.path.join(FW, "persist", "zones_config_json.h"), encoding="utf-8") as f:
            js = f.read()
        def f_(n):
            return re.search(rf"#define {n}\s+([0-9.]+)f?", js).group(1)
        R = config_presets.ZONE_RANGES
        self.assertEqual(R["relay_type"][1], int(f_("ZONE_RELAY_TYPE_MAX")))
        for key, pre in (("progress_band_c", "ZONE_PROGRESS_BAND_C"), ("coil_power_w", "ZONE_COIL_POWER_W"),
                         ("hyst_c", "ZONE_HYST_C"), ("approach_rate_cap_c_per_hr", "ZONE_APPROACH_RATE_CAP_C_PER_HR")):
            self.assertEqual(R[key][0], float(f_(pre + "_MIN")), key)
            if key != "approach_rate_cap_c_per_hr":
                self.assertEqual(R[key][1], float(f_(pre + "_MAX")), key)
        for key in ("min_on_s", "min_off_s"):
            self.assertEqual((R[key][0], R[key][1]),
                             (int(f_("ZONE_MIN_ON_OFF_S_MIN")), int(f_("ZONE_MIN_ON_OFF_S_MAX"))))


class ReadbackFailureNoteTest(unittest.TestCase):
    def test_failed_readback_is_reflected_in_partial_note(self):
        control = um.MagicMock()
        control.set_zone_pid.return_value = True
        # Zone 1 reads back with a different Kp -> verification marks it FAILED.
        control.get_zones.return_value = (None, None, [
            um.MagicMock(index=0, pid_kp=1.0, pid_ki=0.1, pid_kd=0.0),
            um.MagicMock(index=1, pid_kp=9.0, pid_ki=0.1, pid_kd=0.0)])
        preset = {"name": "p", "zones": [{"index": i, "pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0}
                                         for i in range(2)]}
        with um.patch.object(config_presets.zones_http_client, "apply_zone_preset",
                             side_effect=RuntimeError("zones stage down")):
            with self.assertRaises(RuntimeError) as cm:
                config_presets.apply_preset(control, preset, zones_host="h")
        note = cm.exception.preset_partial
        self.assertIn("zone 0 pid=ok", note)
        self.assertIn("zone 1 pid=FAILED", note)
        self.assertNotIn("zone 1 pid=ok", note)


class PartialNoteTest(unittest.TestCase):
    def _exc(self, partial):
        e = RuntimeError("x")
        if partial is not None:
            e.preset_partial = partial
        return e

    def test_ui_test_note_wording(self):
        n = mcp_server_ui_test._partial_note
        self.assertEqual(n(self._exc(None)), "")
        self.assertIn("may still have applied", n(self._exc("")))
        self.assertIn("no stage had confirmed landing", n(self._exc("")))
        out = n(self._exc("PID written"))
        self.assertIn("ALREADY landed", out)
        self.assertIn("may also have applied", out)
        self.assertIn("PID written", out)

    def test_preset_tool_note_keeps_in_flight_caveat(self):
        out = mcp_server_config_presets._partial_note(self._exc("zone 0 pid=ok"))
        self.assertIn("ALREADY landed", out)
        self.assertIn("the failing stage's own write may also have applied", out)


if __name__ == "__main__":
    unittest.main()
