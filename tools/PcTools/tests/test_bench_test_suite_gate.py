"""Fail-closed write gate: with ctx["suite"] absent (or read-only), no WEB
judge may issue a write, and WEB-COMM-07 reports ERROR on restore failure."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import board_lock  # noqa: E402
from kilnctrl.bench_test import registry as R  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402
import kilnctrl.bench_test.cases_web  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_dash  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_diag  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_misc  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_prof  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_rw  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_safety  # noqa: E402,F401

# Judges whose body writes to the board (POSTs, import, restore round trips).
# Kept honest by WriterCoverage below, which derives the set from the judge code.
WRITING_IDS = [
    "WEB-COMM-07", "WEB-WIZ-03", "WEB-WIZ-10", "WEB-DISP-02",
    "WEB-DASH-13", "WEB-DIAG-07", "WEB-DIAG-08", "WEB-SEC-03", "WEB-SEC-04",
    "WEB-DIAG-09", "WEB-KCFG-02", "WEB-LOG-02", "WEB-PROF-02", "WEB-PROF-08",
    "WEB-ZONE-05",
    "WEB-DASH-05", "WEB-DASH-07", "WEB-PROF-03", "WEB-PROF-04", "WEB-PROF-05",
    "WEB-PROF-06", "WEB-PROF-07", "WEB-ZONE-09",
]

# Judges that reach a write seam but are deliberately not behind write_refusal().
EXEMPT_WRITERS = {
    "WEB-WIFI-05": "refusal probes: proves the POST helpers RAISE on denied paths, never reaches the board",
    "WEB-PROF-11": "user-role login probe expecting a refusal; web auth is never changed",
    "WEB-ZONE-10": "KNOWN GAP (review 6 M2 follow-up): POSTs current_sweep/abort with no "
                   "write_refusal() gate; only reached when the sweep status is not running",
}

# Judges that read the board BEFORE their gate need plausible GET bodies to reach it.
_PRE_GATE_GETS = {
    "/api/safety/commissioning": {"relay_type": "contactor"},
    "/api/status": {"temp_unit": "C", "time_tz": "UTC0"},
    "/api/setup/progress": {"version": 1, "steps": {"10": {"state": "todo"}, "11": {"state": "todo"}}},
    "/api/settings/display_power": {"brightness_percent": 50, "timeout_setting": 0,
                                    "keep_on_while_firing": True, "display_on_error": True},
    "/api/kiln_configs": {"configs": [], "max_count": 10, "active_id": 0},
    "/api/diagnostics/danger": {"active": False},
    "/api/profile_exec": {"state": "idle", "last_run": {
        "present": True, "interrupted": False, "phase": "done", "profile_id": 7}},
}
_DIAG_HTML = ('<button id="dangerEnterBtn" disabled></button><div id="dangerAccept"></div>'
              '<div id="dangerLive"></div><div id="dangerExitBtn"></div>')
_PAGE_HTML = 'id="lastRunBanner" ackLastRunBtn /api/profile_exec/ack_last_run'

# DASH-07 is read-only by design: when the gate refuses it silently skips the
# ack and still PASSes its read-only checks, so only "nothing was written"
# is assertable for it.
_NO_REASON = {"WEB-DASH-07"}

_ABSENT_TEXT = "refusing to write"
_SMOKE_TEXT = "is not a mutating suite"


class _Boom:
    def __getattr__(self, name):
        def f(*a, **k):
            raise AssertionError(f"client.{name} used without a mutating suite")
        return f


class _Web:
    def goto(self, path):
        return _DIAG_HTML + _PAGE_HTML

    def __getattr__(self, name):
        return getattr(_Boom(), name)


def _ctx(posts, **extra):
    def get(path):
        if path == "/api/autotune":
            return 200, {"state": "idle"}
        return 200, dict(_PRE_GATE_GETS.get(path, {}))

    def post(path, fields):
        posts.append(path)
        return 200, {"ok": True}

    def post_raw(path, fields):
        posts.append(path)
        return 200, "ok"

    def post_body(path, body):
        posts.append(path)
        return 200, "ok"

    c = {"http_get_json": get, "http_post_json": post, "http_post_raw": post_raw,
         "http_post_json_body": post_body,
         "http_get_text": lambda p: (200, _PAGE_HTML),
         "web_client": _Web(), "sec_client": _Boom(), "host": "127.0.0.1:1",
         "web_username": "u", "web_password": "p", "_sleep": lambda s: None}
    c.update(extra)
    return c


def _violations(cid, **extra):
    """Problems found running judge `cid` with the given ctx extras (no suite
    key = absent). Empty list means the gate held and said so."""
    posts = []
    try:
        res = R.get_case(cid).judge(_ctx(posts, **extra))
    except KeyError:
        return [f"{cid} not registered"]
    except Exception as exc:  # noqa: BLE001 - a client call is a write-path escape
        return [f"{cid} raised {exc!r} (reached a client without a mutating suite)"]
    out = []
    if posts:
        out.append(f"{cid} wrote {posts} with no mutating suite")
    if res.verdict == Verdict.PASS and cid not in _NO_REASON:
        out.append(f"{cid} PASSed with no mutating suite")
    want = _SMOKE_TEXT if extra.get("suite") == "smoke" else _ABSENT_TEXT
    if cid not in _NO_REASON and want not in (res.reason or ""):
        out.append(f"{cid} did not report the refusal ({want!r}); reason={res.reason!r}")
    return out


class AbsentSuiteNeverWrites(unittest.TestCase):
    def test_helper(self):
        self.assertIsNotNone(board_lock.write_refusal({}))
        self.assertIsNotNone(board_lock.write_refusal({"suite": "smoke"}))
        self.assertIsNone(board_lock.write_refusal({"suite": "web"}))

    def _check(self, **extra):
        for cid in WRITING_IDS:
            with self.subTest(case=cid, suite=extra.get("suite", "<absent>")):
                self.assertEqual(_violations(cid, **extra), [])

    def test_absent_suite(self):
        self._check()

    def test_read_only_suite(self):
        self._check(suite="smoke")

    def test_gate_removal_is_caught_for_every_case(self):
        """Negative test kept permanently: with the gate always allowing, EVERY
        writing judge must produce a violation, not just the ones that blow up."""
        orig = board_lock.write_refusal
        board_lock.write_refusal = lambda ctx: None
        try:
            survivors = [cid for cid in WRITING_IDS if not _violations(cid)]
        finally:
            board_lock.write_refusal = orig
        self.assertEqual(survivors, [], "gate removal went unnoticed for these cases")

    def test_mutating_suite_is_not_refused(self):
        for cid in WRITING_IDS:
            posts = []
            with self.subTest(case=cid):
                try:
                    res = R.get_case(cid).judge(_ctx(posts, suite="web"))
                except Exception:  # noqa: BLE001 - reached a client: past the gate
                    continue
                self.assertNotIn(_ABSENT_TEXT, res.reason or "")
                self.assertNotIn(_SMOKE_TEXT, res.reason or "")


def _write_reaching_web_ids():
    """WEB judges whose code (transitively, within the cases_web* modules)
    references an HTTP write seam."""
    import types
    seams = {"_post_json", "_post_raw", "_post_json_body", "_http_post_raw",
             "_http_post_raw_authed", "_http_post_json_body_authed", "post"}

    def codes(co):
        yield co
        for k in co.co_consts:
            if isinstance(k, types.CodeType):
                yield from codes(k)

    def reaches(fn):
        seen, stack = set(), [fn]
        while stack:
            f = stack.pop()
            if f in seen or not hasattr(f, "__code__"):
                continue
            seen.add(f)
            for co in codes(f.__code__):
                for n in co.co_names:
                    if n in seams:
                        return True
                    o = f.__globals__.get(n)
                    if (isinstance(o, types.FunctionType)
                            and o.__module__.startswith("kilnctrl.bench_test.cases_web")):
                        stack.append(o)
            for c in (f.__closure__ or ()):
                try:
                    if isinstance(c.cell_contents, types.FunctionType):
                        stack.append(c.cell_contents)
                except ValueError:
                    pass
        return False

    return {cid for cid, spec in R.REGISTRY.items()
            if cid.startswith("WEB-") and spec.judge is not None and reaches(spec.judge)}


class WriterCoverage(unittest.TestCase):
    def test_writing_ids_cover_every_writer(self):
        derived = _write_reaching_web_ids()
        # WEB-SEC-03/04 and WEB-LOG-02 write through sec_client, not an HTTP seam.
        derived |= {"WEB-SEC-03", "WEB-SEC-04", "WEB-LOG-02"}
        missing = sorted(derived - set(WRITING_IDS) - set(EXEMPT_WRITERS))
        self.assertEqual(missing, [], "new writing judges must be added to WRITING_IDS "
                         "(and gated by write_refusal) or to EXEMPT_WRITERS with a reason")
        stale = sorted(set(WRITING_IDS) - derived)
        self.assertEqual(stale, [], "WRITING_IDS lists judges that no longer write")


class DiagAutotuneMissingState(unittest.TestCase):
    def test_missing_autotune_state_refuses(self):
        from kilnctrl.bench_test import cases_web_diag as D
        c = _ctx([], suite="web")
        orig = c["http_get_json"]
        c["http_get_json"] = lambda p: (200, {}) if p == "/api/autotune" else orig(p)
        self.assertIn("autotune", D.mutating_gate(c))


class Comm07RestoreError(unittest.TestCase):
    def test_restore_failure_is_error(self):
        sys.path.insert(0, os.path.dirname(__file__))
        import test_bench_test_cases_web_safety as T
        state = {"n": 0, "relay": "contactor"}

        def handler(fields):
            state["n"] += 1
            if state["n"] == 1:  # the write "succeeds" but leaves the board changed
                state["relay"] = "mercury"
                return 200, {"ok": True, "persisted": True}
            return 500, {"ok": False}  # the restore fails

        g = {"/api/safety/commissioning": lambda: dict(
            T.COMM_GOOD["/api/safety/commissioning"], relay_type=state["relay"])}
        b = T.Board(gets=g, posts={"/api/safety/commissioning/relay_type": handler})
        res = T.run("WEB-COMM-07", b.ctx())
        self.assertEqual(res.verdict, Verdict.FAIL)
        self.assertIn("ERROR:", res.reason)
        self.assertEqual(state["n"], 2)


if __name__ == "__main__":
    unittest.main()
