import unittest
from unittest import mock
from kilnctrl.bench_test import cases_ota as C, registry as R, runner
from kilnctrl.bench_test.registry import CaseResult, Verdict as V

def res(**kw):
    return {"_run_results": {k.replace("_", "-"): CaseResult(v, reason="x") for k, v in kw.items()}}

class OtB02(unittest.TestCase):
    def test_suite_last(self):
        self.assertEqual(R.SUITES["ota"][-1], "OT-B02")
        self.assertIsNotNone(R.get_case("OT-B02").judge)
    def test_pass(self):
        r = C._case_otb02(res(OT_B01=V.PASS, OT_E01=V.SKIP))
        self.assertEqual(r.verdict, V.PASS); self.assertEqual(len(r.observed["table"]), 2)
    def test_fail(self):
        self.assertEqual(C._case_otb02(res(OT_B01=V.PASS, OT_E01=V.FAIL)).verdict, V.FAIL)
    def test_inconclusive(self):
        self.assertEqual(C._case_otb02(res(OT_B01=V.NOT_RUN, OT_E01=V.SKIP)).verdict, V.INCONCLUSIVE)
    def test_inconclusive_mixed_with_not_run_is_inconclusive(self):
        r = C._case_otb02(res(OT_B01=V.INCONCLUSIVE, OT_E01=V.NOT_RUN, OT_E02=V.SKIP))
        self.assertEqual(r.verdict, V.INCONCLUSIVE)
    def test_pass_plus_inconclusive_is_not_pass(self):
        r = C._case_otb02(res(OT_B01=V.PASS, OT_E01=V.INCONCLUSIVE))
        self.assertEqual(r.verdict, V.INCONCLUSIVE); self.assertIn("OT-E01", r.reason)
    def test_no_results_error_and_no_recursion(self):
        with mock.patch.object(runner.BenchTestRunner, "run") as run:
            r = C._case_otb02({})
            run.assert_not_called()
        self.assertIn("ERROR", r.reason); self.assertEqual(r.verdict, V.FAIL)
