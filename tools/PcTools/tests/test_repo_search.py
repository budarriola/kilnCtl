#!/usr/bin/env python3
"""Unit tests for kilnctrl.repo_search and its MCP wrapper repo_grep.

What these guard, in the order the task brief states them:

1. **A deadline that is actually enforced, and reported distinguishably.**
   The expensive failure here is a timed-out search reading as "no matches".
   A real subprocess that outlives its deadline is started (a python -c
   sleeper, not a mock) so the kill path is exercised for real, and the
   formatted output is asserted to lead with INCOMPLETE and to say in words
   that an empty list is not a conclusion. The child is asserted dead
   afterwards -- a search process must not outlive the call.

2. **Engine selection and reporting.** Both branches (rg present / rg absent)
   are exercised through the injectable ``which``, so the test does not
   depend on what happens to be installed on the machine running it.

3. **Caps.** timeout and max_results clamp to their ceilings, say so, and
   truncation is reported rather than silently applied.

4. **Discoverability through the real facade.** A tool nobody can find is
   close to unregistered (this repo's own standard), so the REAL production
   registry is queried with the questions a person would actually ask.

Run with: kiln_call(name="run_pctools_tests") -- or
python -m pytest tools/PcTools/tests -q
"""
from __future__ import annotations

import os
import shutil
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import repo_search  # noqa: E402


class EngineSelectionTest(unittest.TestCase):
    def test_prefers_ripgrep_when_present(self):
        name, path = repo_search.find_engine(which=lambda exe: {"rg": "/usr/bin/rg"}.get(exe))
        self.assertEqual(name, "rg")
        self.assertEqual(path, "/usr/bin/rg")

    def test_falls_back_to_grep(self):
        name, path = repo_search.find_engine(which=lambda exe: {"grep": "/usr/bin/grep"}.get(exe))
        self.assertEqual(name, "grep")

    def test_raises_when_neither_is_available(self):
        with self.assertRaises(repo_search.RepoSearchError) as ctx:
            repo_search.find_engine(which=lambda exe: None)
        self.assertIn("rg", str(ctx.exception))
        self.assertIn("grep", str(ctx.exception))


class ArgvTest(unittest.TestCase):
    """The argv is the contract with the engine; each mode must ask for the
    cheap output it promises, and the pattern must stay an argv element."""

    def test_rg_files_mode_is_paths_only(self):
        argv = repo_search.build_argv("rg", "rg", "needle", "src", mode="files")
        self.assertIn("--files-with-matches", argv)
        self.assertNotIn("--line-number", argv)

    def test_grep_count_mode(self):
        argv = repo_search.build_argv("grep", "grep", "needle", "src", mode="count")
        self.assertIn("-c", argv)
        self.assertIn("-r", argv)

    def test_lines_mode_carries_context(self):
        rg = repo_search.build_argv("rg", "rg", "n", ".", mode="lines", context=3)
        self.assertIn("--context", rg)
        self.assertIn("3", rg)
        gnu = repo_search.build_argv("grep", "grep", "n", ".", mode="lines", context=3)
        self.assertIn("-C", gnu)

    def test_glob_and_case_flags(self):
        rg = repo_search.build_argv("rg", "rg", "n", ".", glob="*.c", ignore_case=True)
        self.assertIn("--glob", rg)
        self.assertIn("*.c", rg)
        self.assertIn("--ignore-case", rg)
        gnu = repo_search.build_argv("grep", "grep", "n", ".", glob="*.c", ignore_case=True)
        self.assertIn("--include=*.c", gnu)
        self.assertIn("-i", gnu)

    def test_pattern_is_an_argv_element_after_a_double_dash(self):
        argv = repo_search.build_argv("grep", "grep", "-v; rm -rf /", "src")
        self.assertIn("--", argv)
        self.assertEqual(argv[argv.index("--") + 1], "-v; rm -rf /")

    def test_rejects_empty_pattern_bad_mode_and_bad_context(self):
        with self.assertRaises(repo_search.RepoSearchError):
            repo_search.build_argv("grep", "grep", "", ".")
        with self.assertRaises(repo_search.RepoSearchError):
            repo_search.build_argv("grep", "grep", "x", ".", mode="everything")
        with self.assertRaises(repo_search.RepoSearchError):
            repo_search.build_argv("grep", "grep", "x", ".", mode="lines", context=999)


class ClampTest(unittest.TestCase):
    def test_timeout_defaults_and_ceiling(self):
        self.assertEqual(repo_search.clamp_timeout(None), (repo_search.DEFAULT_TIMEOUT_S, None))
        value, note = repo_search.clamp_timeout(repo_search.MAX_TIMEOUT_S * 10)
        self.assertEqual(value, repo_search.MAX_TIMEOUT_S)
        self.assertIn("ceiling", note)

    def test_non_positive_timeout_falls_back_and_says_so(self):
        value, note = repo_search.clamp_timeout(0)
        self.assertEqual(value, repo_search.DEFAULT_TIMEOUT_S)
        self.assertIn("default", note)

    def test_max_results_ceiling(self):
        value, note = repo_search.clamp_max_results(repo_search.MAX_MAX_RESULTS + 1)
        self.assertEqual(value, repo_search.MAX_MAX_RESULTS)
        self.assertIn("clamped", note)


def _python_argv_patch(script: str):
    """Replace build_argv so run_search launches ``python -c <script>``."""
    def fake_build_argv(engine, engine_path, pattern, path, glob="", ignore_case=False,
                        mode="files", context=0):
        return [engine_path, "-c", script]
    return fake_build_argv


class RunSearchTest(unittest.TestCase):
    def setUp(self):
        self._real_build_argv = repo_search.build_argv
        self.addCleanup(setattr, repo_search, "build_argv", self._real_build_argv)

    def test_deadline_kills_the_child_and_is_reported_distinguishably(self):
        """The load-bearing test: a search that outruns its deadline must be
        killed, must not report itself as complete, and must not be
        mistakable for 'no matches'."""
        repo_search.build_argv = _python_argv_patch(
            "import sys,time\n"
            "print('hits/before.c'); sys.stdout.flush()\n"
            "time.sleep(30)\n")
        result, _notes = repo_search.run_search(
            "needle", path=".", timeout_s=1.0, engine=("python", sys.executable),
            cwd=os.path.dirname(__file__))
        self.assertTrue(result.timed_out)
        self.assertFalse(result.complete)
        self.assertLess(result.elapsed_s, 20.0, "the child was not killed at the deadline")
        text = repo_search.format_result(result)
        self.assertTrue(text.startswith("INCOMPLETE:"), text.splitlines()[0])
        self.assertIn("NOT a complete answer", text)
        self.assertIn("does NOT mean", text)
        self.assertIn("status=INCOMPLETE", text)

    def test_an_honest_empty_result_says_no_matches_and_is_complete(self):
        """The other half of the same contract: a real zero-match search must
        NOT be dressed up as incomplete."""
        repo_search.build_argv = _python_argv_patch("pass")
        result, _notes = repo_search.run_search(
            "needle", path=".", timeout_s=10.0, engine=("python", sys.executable),
            cwd=os.path.dirname(__file__))
        self.assertFalse(result.timed_out)
        self.assertTrue(result.complete)
        self.assertFalse(result.matched)
        text = repo_search.format_result(result)
        self.assertIn("no matches", text)
        self.assertNotIn("INCOMPLETE", text)

    def test_result_cap_truncates_and_reports(self):
        repo_search.build_argv = _python_argv_patch(
            "for i in range(50): print('f%d.c' % i)")
        result, _notes = repo_search.run_search(
            "needle", path=".", timeout_s=20.0, max_results=10,
            engine=("python", sys.executable), cwd=os.path.dirname(__file__))
        self.assertEqual(result.total_lines, 50)
        self.assertEqual(len(result.lines), 10)
        self.assertTrue(result.truncated)
        self.assertFalse(result.complete)
        text = repo_search.format_result(result)
        self.assertIn("TRUNCATED:", text)

    def test_grep_count_mode_drops_zero_count_files(self):
        """GNU grep -c prints EVERY searched file, zeros included -- which on
        this tree buries the handful that matched under hundreds of `path:0`
        lines and makes `matches=` a file count. ripgrep already omits them;
        run_search() must make the two engines agree."""
        repo_search.build_argv = _python_argv_patch(
            "print('a.c:0'); print('b.c:3'); print('c.c:0'); print('d.c:12')")
        result, _notes = repo_search.run_search(
            "needle", path=".", mode="count", timeout_s=20.0,
            engine=("grep", sys.executable), cwd=os.path.dirname(__file__))
        self.assertEqual(result.lines, ["b.c:3", "d.c:12"])
        self.assertEqual(result.total_lines, 2)

    def test_files_mode_keeps_lines_that_merely_end_in_zero(self):
        """The zero filter is scoped to count mode: a real path ending in
        ':0' is not a thing, but a FILENAME ending in 0 must survive."""
        repo_search.build_argv = _python_argv_patch("print('zone0.c'); print('pid.c')")
        result, _notes = repo_search.run_search(
            "needle", path=".", mode="files", timeout_s=20.0,
            engine=("grep", sys.executable), cwd=os.path.dirname(__file__))
        self.assertEqual(result.lines, ["zone0.c", "pid.c"])

    def test_engine_name_is_reported(self):
        repo_search.build_argv = _python_argv_patch("print('a.c')")
        result, _notes = repo_search.run_search(
            "needle", path=".", timeout_s=20.0, engine=("rg", sys.executable),
            cwd=os.path.dirname(__file__))
        self.assertIn("engine=rg", repo_search.format_result(result))

    def test_missing_path_is_an_error_not_an_empty_result(self):
        with self.assertRaises(repo_search.RepoSearchError):
            repo_search.run_search("needle", path="no/such/directory/anywhere",
                                   cwd=os.path.dirname(__file__))


class RealEngineSmokeTest(unittest.TestCase):
    """One end-to-end run against whichever engine this machine actually has,
    over this test file itself -- proves the argv this module builds is
    accepted by the real binary, which no amount of argv assertion can."""

    def test_finds_a_known_string_in_this_file(self):
        if not (shutil.which("rg") or shutil.which("grep")):
            self.skipTest("neither rg nor grep on PATH")
        here = os.path.dirname(os.path.abspath(__file__))
        result, _notes = repo_search.run_search(
            "UNIQUE_MARKER_FOR_REPO_SEARCH_TEST", path=".", glob="test_repo_search.py",
            mode="files", timeout_s=60.0, cwd=here)
        self.assertFalse(result.timed_out)
        self.assertTrue(result.matched, f"engine={result.engine} argv={result.argv}")
        self.assertTrue(any("test_repo_search" in line for line in result.lines))

    def test_count_mode_over_the_same_file(self):
        if not (shutil.which("rg") or shutil.which("grep")):
            self.skipTest("neither rg nor grep on PATH")
        here = os.path.dirname(os.path.abspath(__file__))
        result, _notes = repo_search.run_search(
            "UNIQUE_MARKER_FOR_REPO_SEARCH_TEST", path=".", glob="test_repo_search.py",
            mode="count", timeout_s=60.0, cwd=here)
        self.assertTrue(result.matched)
        self.assertTrue(any(":" in line for line in result.lines))


class FacadeDiscoverabilityTest(unittest.TestCase):
    """Exercises the REAL production registry, so a missing import in
    mcp_server.py or a keyword typo in mcp_facade.py fails here."""

    @classmethod
    def setUpClass(cls):
        from kilnctrl import mcp_server as m  # noqa: E402
        cls.registry = m.registry

    def _names(self, query: str):
        return [hit.name for hit in self.registry.search(query)]

    def test_registered_at_all(self):
        self.assertIn("repo_grep", self.registry.by_name)

    def test_not_directly_published_on_the_wire(self):
        """Everything but the facade (+ KEEP) stays behind kiln_call."""
        from kilnctrl import mcp_server as m
        self.assertNotIn("repo_grep", set(m.mcp._tool_manager._tools))

    def test_found_by_the_questions_a_person_asks(self):
        for query in ("grep the repository",
                      "search the source code for a string",
                      "which files mention this symbol",
                      "ripgrep the tree with a timeout"):
            with self.subTest(query=query):
                self.assertIn("repo_grep", self._names(query)[:3],
                              f"repo_grep not surfaced by kiln_find({query!r})")


if __name__ == "__main__":
    unittest.main()
