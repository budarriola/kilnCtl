"""OH-04 source scans: the app marks the install result confirmed after boot confirm, status reports the state."""
import re
import unittest
from pathlib import Path

APP = Path(__file__).resolve().parents[3] / "firmware" / "KilnFW" / "App"


def _read(rel: str) -> str:
    return (APP / rel).read_text(encoding="utf-8")


class Oh04SourceTests(unittest.TestCase):
    def test_confirm_marks_result_inside_done_branch(self):
        src = _read("main_network_http.c")
        m = re.search(r"boot_confirm_step\(&st, &ops\) == BOOT_CONFIRM_STEP_DONE\) \{(.*?)vTaskDelete", src, re.S)
        self.assertIsNotNone(m)
        body = m.group(1)
        self.assertIn("if (!is_factory)", body)
        self.assertIn("update_install_http_mark_confirmed()", body)
        self.assertLess(body.index("if (!is_factory)"), body.index("update_install_http_mark_confirmed()"))

    def test_status_state_comes_from_the_result_helper(self):
        src = _read("drivers/update/update_install_http.c")
        self.assertIn("update_install_res_state(&res, run.version, rid_p)", src)
        self.assertNotIn('have == 1 ? "recorded"', src)

    def test_page_names_pending_and_rolled_back(self):
        page = _read("drivers/net/ota_page.html")
        self.assertIn("'pending_confirm'", page)
        self.assertIn("'rolled_back'", page)
        self.assertIn("/api/update/status", page)


if __name__ == "__main__":
    unittest.main()
