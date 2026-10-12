# Review: PcTools settings override and guard probes for REVIEW_TESTNET4 (2026-10-12)

Scope: origin/dev `52861ef7c` (fix) and `bb78b35f3` (doc), which fix
`docs/audits/REVIEW_TESTNET4_2026-10-12.md` L1-L3 and I2. The reviewer did
not touch the board. No subagents were used.

What changed:

- `tools/PcTools/src/kilnctrl/settings.py:30-32` reads
  `KILNCTL_TEST_SETTINGS_PATH` once at import and, if set, points
  `SETTINGS_PATH` at it. `tools/PcTools/tests/conftest.py:77-82` sets that
  variable outright to a `mkdtemp` file, removed at exit, before any
  `kilnctrl` import. This is the L1 fix: tests no longer see the real board
  address that the MCP server caches in the gitignored `settings.json`.
- `bench_hotspot.ps1` now refuses every action (`start`, `stop` and `status`)
  under `KILNCTL_TEST_NO_HUB=1`, not only `start`. `make_release.ps1` also
  refuses network git (`fetch`/`ls-remote`, line 235) under that variable.
  This is L3.
- A new probe variable, `KILNCTL_TEST_GUARD_PROBE=1`, makes each guarded
  script print `GUARD-PROBE-PAST-GUARD` and exit 0 right after its guard
  (`bench_hotspot.ps1:95`, `web_commission_row.ps1:35`,
  `make_release.ps1:83` and `:238`). The refusal tests set the probe. A
  regressed guard therefore stops at the probe, not in the real network or
  credential code. This is L2.
- `test_network_guard.py` gained:
  - refusal tests over six invocations
  - one probe control
  - two settings-override tests
  - a keep-prefix boundary test (I2)

**Verdict: not a promote blocker.** No HIGH or MED findings.

- **L1 is complete.** Every `settings.json` reader goes through
  `settings.SETTINGS_PATH`, and there is no PowerShell or `mcpkit` reader.
- **No caller is affected.** The new variables have no production caller,
  and `land.ps1`, the release flow and the MCP tools never invoke the guarded
  scripts.
- **The remaining findings are LOW.** They concern the probe's success exit
  code, a control test that would stop the real hotspot if the probe broke,
  and probes no control proves live.

## Findings, by severity

### L1 (LOW): the guard probe exits 0, so a stray probe variable reads as success

**Sites:**

- `tools/PcTools/scripts/bench_hotspot.ps1:95`
- `tools/PcTools/scripts/web_commission_row.ps1:35`
- `tools/make_release.ps1:83` (`-Publish` only)
- `tools/make_release.ps1:238` (unconditional)

**Scenario:** `KILNCTL_TEST_GUARD_PROBE=1` is left in a developer shell, for
example after reproducing a test by hand, while `KILNCTL_TEST_NO_HUB` is not
set. Then:

- every `bench_hotspot.ps1` action exits 0 having done nothing, including
  `stop`
- `make_release.ps1` exits 0 after its gates, both with `-Publish` and in a
  plain dry run, without building or publishing anything
- live `web_commission_row.ps1` exits 0 without running the row

Each prints only one line, which is easy to miss in a long release log.

`git grep` finds no automated caller of these scripts (land.ps1,
release_merge.ps1, dev_promote.ps1, MCP sources, templates and the workspace
file all lack one), and no User-scope value exists for the variable. So
today only a human running the script is affected.

**Fix:**

- Exit with a distinct nonzero code (for example 86) and write the marker to
  stderr: `[Console]::Error.WriteLine(...)`, then `exit 86`.
- Have the tests assert that code, so a probe exit can never be mistaken for
  a successful run.

### L2 (LOW): the probe control runs the real `bench_hotspot -Action stop`

**Site:** `tools/PcTools/tests/test_network_guard.py:297-307`,
`test_guard_probe_reaches_past_guard_without_no_hub`.

**Scenario:** the control removes `KILNCTL_TEST_NO_HUB`, sets the probe and
runs `bench_hotspot.ps1 -Action stop`. Suppose a later edit deletes the
probe, moves it below the `switch`, or changes its spelling. Then the control
stops the real Windows mobile hotspot on the bench PC. That disconnects the
board mid-run before the test fails. This is the same class as REVIEW_TESTNET4
L2, which this change was meant to close.

This reviewer did not negative-test that mutation for exactly this reason.

**Fix:**

- Run the control with `-Action status`, which is read-only.
- Add a static assertion on the script text that the probe line precedes
  the `switch ($Action)` line.

### L3 (LOW): only one of four probes is proven live

**Site:** `tools/PcTools/tests/test_network_guard.py:297-307` runs the
control for `bench_hotspot` only.

**Scenario:** the probes in `make_release.ps1:83`, `make_release.ps1:238` and
`web_commission_row.ps1:35` have no positive control. If one is deleted or
misspelled, the refusal tests still pass, because the guard still refuses.
But a later regression of that guard then runs the real code: GitHub token
read, `git fetch` through the credential manager, or the live commission
row. That defeats L2's purpose for three of the four sites.

The negative tests confirm the gap: N9 and N10 below are MISSED.

**Fix:** parametrize the control over every probed invocation, with the
`NO_HUB` variable removed and the probe set, and assert the marker. For
`make_release.ps1` that means `-Publish` and `-DevDryRun`.

L2's `status` change must come first, so that a broken probe can never
reach real code.

### L4 (LOW): `KILNCTL_TEST_SETTINGS_PATH` takes effect silently in production

**Site:** `tools/PcTools/src/kilnctrl/settings.py:30-32`.

**Scenario:** the variable leaks into a real shell, a VS Code task or a
User-scope setting, for example copied from a test reproduction. Then the
GUI and the MCP servers read and write a different `settings.json`. The
saved values below vanish, and nothing in any log says why:

- `last_port`
- `last_host`
- `openocd_exe`
- `keep_logs`

`save()` also recreates the directory (`mkdir(parents=True)`). So a long-lived
child that outlives pytest recreates the deleted temp dir.

Compare `mcp_server.py:126`, which warns when `KILNCTL_TEST_NO_HUB` is set.
There is no security effect, because the override only changes where the
file lives.

**Fix:** have the server and GUI log one warning naming the overriding path
when `_TEST_OVERRIDE` is active. Optionally, honor the override only when
`KILNCTL_TEST_NO_HUB=1` is also set.

### I1 (INFO): a test pops `KILNCTL_HOST` without restoring it

**Site:** `tools/PcTools/tests/test_network_guard.py:316`,
`os.environ.pop("KILNCTL_HOST", None)`.

**Scenario:** the scrub deliberately keeps `KILNCTL_HOST` when
`KILNCTRL_BENCH_HOST` is set, for the live-bench tests. Any of those tests
that runs after this one in the same process loses its host and falls back
to `192.168.4.1`. Today the live-bench tests sort before this file, so the
problem is latent.

**Fix:** use `monkeypatch.delenv("KILNCTL_HOST", raising=False)`.

### I2 (INFO): part of the override tests are vacuous

**Site:** `tools/PcTools/tests/test_network_guard.py:309-341`.

**Scenario:** in a fresh worktree, the real `settings.json` does not exist.
So the `host == FALLBACK` assertions would pass even without the override.
The file planted in `tmp_path` (lines 318-320) is read by nothing. What
actually pins L1 is the equality of the env value and `SETTINGS_PATH`
(lines 314 and 340). N1 is caught only through those lines.

**Fix:** add a positive control. Write `last_host` into the override file,
then assert that `resolve_default_host()` returns it, both in process and in
a child.

### I3 (INFO): `setdefault` regression is not caught (N11)

**Site:** `tools/PcTools/tests/conftest.py:82`.

**Scenario:** if the outright assignment became `os.environ.setdefault(...)`,
an inherited `KILNCTL_TEST_SETTINGS_PATH` would win. A developer who exported
one pointing at the real file would put the board address back in front of
tests. This is the same class as REVIEW_TESTNET3 I2, which was fixed for
`KILNCTL_TEST_NO_HUB` with an inherited-value test.

**Fix:** add the same kind of test. Run a child pytest or conftest import
with the variable preset to a sentinel, and assert that the sentinel is
replaced.

### I4 (INFO): refusal messages name the variable but not the remedy

**Site:** `bench_hotspot.ps1:92`, `web_commission_row.ps1:30`,
`make_release.ps1:79` and `:236`.

**Context:**

- `KILNCTL_TEST_NO_HUB` is set only inside the pytest process
  (`conftest.py`), and no User-scope value exists, so the variable cannot
  leak into a developer shell through this change.
- If it is ever set by hand, the message names the variable, but it does not
  say "unset it to run this for real".
- `bench_hotspot.ps1`'s exit 3 matches the repo's SKIP convention, but no
  caller interprets it.

**Fix (optional):** append `(unset KILNCTL_TEST_NO_HUB to run for real)` to
each message.

## Other questions from the brief

- **L1 completeness.** Every reader goes through the `settings` module:
  - `host_resolve.py:100`, `:124`
  - `session_log.py:108`, whose default argument is bound after the override
  - `gui.py:454`, `:508`
  - `actions.py:863`
  - `mcp_server_link.py:103`
  - `mcp_server_debug`
  - `openocd_util`

  No PowerShell script or `mcpkit` module reads `settings.json`. Under
  xdist, each worker imports `conftest.py` and gets its own temp dir, which
  is correct.
- **REVIEW_TESTNET4 I1 (loopback 8766/8767 open).** It is unchanged.
  `conftest.py:330-331` still refuses only `HUB_PORT`. This change does not
  make it worse.
- **Probe exit "clearly non-success"?** It is not, see L1. No caller
  depends on it.
- **`check_release_manifest.ps1`.** It removes both `KILNCTL_TEST_NO_HUB` and
  `KILNCTL_TEST_GUARD_PROBE` before calling `make_release.ps1`, so a stray
  value cannot make that check a vacuous pass. It passed in the suite run.

## Negative tests

`tools\negtest.ps1 -Preset pytest -PresetArg tests/test_network_guard.py`
was run with the main venv python against a throwaway copy at `bd18871ef`,
which contains `bb78b35f3`. The baseline passed (27 tests).

- **Fixer's mutations:** N1-N8 cover the fixer's seven mutation classes:
  override off, conftest unset, the four script guards and the keep-prefix
  boundary.
- **Reviewer's mutations:** N9-N12 were added by this review.
- **Confirmation:** for each CAUGHT, the per-mutation log shows a real
  assertion failure (`FAILED ... AssertionError`), not a collection or
  import error.
- **Not run:** the probe removal in `bench_hotspot.ps1`. It would run the
  real `-Action stop`; see L2.

| # | Mutation | Verdict | Failing assertion |
|---|---|---|---|
| N1 | `settings.py`: `if _TEST_OVERRIDE:` -> `if False:` | CAUGHT | `test_network_guard.py:314`, `:340` (path mismatch) |
| N2 | `conftest.py` never sets `KILNCTL_TEST_SETTINGS_PATH` | CAUGHT | `:314` (KeyError on the env lookup), `:336` |
| N3 | override read but not assigned to `SETTINGS_PATH` | CAUGHT | `:314`, `:340` |
| N4 | `bench_hotspot.ps1` guard `-eq 'yes'` | CAUGHT | `:291` for start/stop/status |
| N5 | `make_release.ps1` publish guard `-eq "yes"` | CAUGHT | `:291` (`-Publish`) |
| N6 | `make_release.ps1` network guard `-eq "yes"` | CAUGHT | `:291` (`-DevDryRun`) |
| N7 | `web_commission_row.ps1` guard `-eq 'yes'` | CAUGHT | `:291` |
| N8 | keep prefix `KILNCTL_TEST_` -> `KILNCTL_TEST` | CAUGHT | `:351` |
| N9 | `make_release.ps1:238` probe removed, guard intact | **MISSED** | none (L3) |
| N10 | `web_commission_row.ps1:35` probe removed, guard intact | **MISSED** | none (L3) |
| N11 | `conftest.py` sets the override with `setdefault` | **MISSED** | none (I3) |
| N12 | publish probe moved above the publish guard | CAUGHT | `:291` (`-Publish`) |

N9 and N10 are not defects today, because the guards still refuse. They show
that the probe path at those sites is unproven (L3). N11 is I3.

## Suite

`tools\run_all_checks.ps1 -Fast` ran in the review worktree at `f70699dc8`
and exited 1: 175 passed, 17 SKIP-FAST and 5 failed. None of the five
failures touches this change:

| Check | Cause |
|---|---|
| `check_js_host_tests.ps1` | known `test_zones_type_toggle.js` failure (`kgRawAttrs is not defined`) |
| `check_page_js_tests.ps1` | same known failure |
| `check_main_baseline.ps1` | under investigation by another agent; noted only |
| `check_gate_negative_test_table.ps1` | no evidence row for `check_lcd_sink_callgraph.ps1` / its test; unrelated, being fixed separately |
| `check_land.ps1` | hit the 900 s cap under machine load; unrelated (no land tooling changed) |
