# Review: PcTools test-env scrub for REVIEW_TESTNET3 (2026-10-12)

Scope: origin/dev `0e573b34c` (fix) and `b844322b3` (doc), which fix
`docs/audits/REVIEW_TESTNET3_2026-10-12.md` L1-L3 and I1-I3. The reviewer did
not touch the board. No subagents were used.

What changed:

- `tools/PcTools/tests/conftest.py` `_scrub_board_env()` drops every
  `KILNCTL_*` variable plus `MOUSER_API_KEY` and `GITHUB_TOKEN`. It keeps the
  control variables (`KILNCTL_TEST_*`, `KILNCTL_BUILD_*`,
  `KILNCTL_LIGHT_GATE_*`, four named switches). With `KILNCTRL_BENCH_HOST`
  set it keeps only `KILNCTL_HOST`, `KILNCTL_WEB_USERNAME` and
  `KILNCTL_WEB_PASSWORD`. `KILNCTL_TEST_NO_HUB` is now assigned outright.
- `web_commission_row.ps1` (live mode), `bench_hotspot.ps1` (`start`) and
  `make_release.ps1` (`-Publish`) refuse under `KILNCTL_TEST_NO_HUB=1`.

**Verdict: not a promote blocker.** No HIGH or MED findings. The scrub does
what it says, and the new tests catch every mutation of the behavior they
pin. The main remaining gap (L1) is older than this change: the board's LAN
address still reaches tests through the gitignored `settings.json` host
cache. The env scrub does not cover that file.

## Findings, by severity

### L1 (LOW): the board address still reaches tests through `settings.json`

`host_resolve.resolve_default_host()`
(`tools/PcTools/src/kilnctrl/host_resolve.py:85-104`) tries three sources in
order: `KILNCTL_HOST`, then `last_host` in
`tools/PcTools/src/kilnctrl/settings.json`, then `192.168.4.1`. The file is
gitignored (`tools/PcTools/.gitignore:10`) and the MCP server writes it at
runtime. 22 module-level `*_AP_DEFAULT_HOST` constants call this resolver at
import time. `conftest.py` scrubs the env var but never redirects
`settings.SETTINGS_PATH`.

So in the main tree, a test with no bench host still gets the real board
address. That is where `run_pctools_tests` runs the suite. Subprocess
children read the same file. Verified read-only in the main tree, with
`KILNCTL_HOST` popped:
`host_resolve.resolve_default_host() != FALLBACK_HOST` -> `True`.
A fresh worktree has no `settings.json`, so worktree runs are not affected.

The in-process socket guard still refuses the connect. Subprocess children
have no guard, though, so this is the same exposure that
REVIEW_TESTNET2 LOW-3 and testnet3 L1 set out to close, by another path.
`last_port` (the board's COM port) sits in the same file. Serial opens are
not covered by the socket guard at all.

Suggested fix: have `settings.py` honor an override such as
`KILNCTL_TEST_SETTINGS_PATH`. The `KILNCTL_TEST_` prefix is already kept, so
children inherit it. Point the override at a per-run temp file in
`conftest.py`, and add a child-pytest test that plants a `last_host` and
asserts the resolver returns the fallback.

### L2 (LOW): the script-refusal test is unsafe when it fails, and its sentinel check proves nothing

`test_user_scope_credential_scripts_refuse_under_test_env`
(`tests/test_network_guard.py:263-284`) plants a sentinel in the **process**
env. The scripts read the **User-scope registry**, never the process env, so
`assert secret not in out` cannot fail for the vector it describes. The
`returncode != 0` and `"refused" ... "KILNCTL_TEST_NO_HUB"` assertions are
the ones doing the work.

The failure mode is also unsafe. If a guard regresses (for example `-eq '1'`
becomes `-eq 'yes'`), the test itself runs the guarded action:

- `bench_hotspot.ps1 -Action start` reads the real hotspot SSID and password
  and starts Windows tethering on this PC.
- `make_release.ps1 -Publish` goes on to `git fetch` and `ls-remote` against
  origin, and possibly a full build.
- `web_commission_row.ps1` loads the real web credentials into a child that
  tries `192.0.2.1`.

For this reason the reviewer did **not** negtest guard removal for these
three scripts. Suggested fix: test each guard without running the action
after it. Options are a `-LoadFunctionsOnly`-style seam, or putting a stub
`powershell` profile and environment around the call so the guarded branch
can only print. At minimum, drop the vacuous sentinel assertion or point it
at a value the scripts actually read.

### L3 (LOW): the unscoped actions of the guarded scripts stay unguarded

Under `KILNCTL_TEST_NO_HUB=1`:

- `bench_hotspot.ps1 -Action stop` stops the real bench hotspot.
- `-Action status` still reads both User-scope variables. It prints only
  booleans.
- Without `-Publish`, `make_release.ps1` still does `git fetch origin
  release --tags` and `git ls-remote` (`tools/make_release.ps1:231-236`).
  These use the Windows git credential manager, a secret store outside the
  environment.

No current test reaches any of these paths: `grep` of `tools/PcTools/tests`
finds the three scripts only in the refusal test. Today this is a latent
gap, not a leak. Consider refusing `stop` under the test env too.

A related interaction: `tools/check_release_manifest.ps1:52,79` expects
`-Publish` runs to fail with "cannot be combined" and gate messages. The new
guard fires first, so that check fails if it is ever run from a shell that
inherited `KILNCTL_TEST_NO_HUB=1`, for example from inside a pytest. Today it
runs only from `run_all_checks`, so this is informational.

### I1 (INFO): loopback to the running MCP servers is open

The guard refuses loopback only on `HUB_PORT` (`conftest.py:319-323`).
`127.0.0.1:8766/8767` still reach the long-running MCP servers. Those
servers hold the real environment (credentials, board link). No test
connects there today: `grep 8767` finds only `call_log` port mocks. Adding
those two ports to the refused-loopback set would close the indirect path.

### I2 (INFO): the keep-prefix boundary is not pinned

Mutation M7 (`"KILNCTL_TEST_"` -> `"KILNCTL_TEST"`) is MISSED. No test plants
a `KILNCTL_TEST<letter>` name. No secret is named that way today. The
repo-wide `grep` and the User-scope variable *names* on this machine show
only build-gate and test switches under `KILNCTL_TEST_*`, `KILNCTL_BUILD_*`
and `KILNCTL_LIGHT_GATE_*`. Planting `KILNCTL_TESTPASSWORD` in the drop test
would pin it.

### I3 (INFO): other answers to the questions asked

- `KILNCTRL_*` (with an R) is never dropped. Every such name in the repo is
  a bench tuning knob: `BENCH_HOST`, `BENCH_PORT` and the `BENCH_*_S`/`_C`
  budgets. None is a secret, and `BENCH_HOST` must survive by design.
- Nothing calls `keyring`, `dotenv` or `winreg` in PcTools `src`, `tests` or
  `scripts`. No PowerShell profile exists for this user, so a child
  `powershell` started without `-NoProfile` (`test_buildgate.py`,
  `lcd_sampler.py`) cannot re-inject variables. Only the three scripts above
  and `conftest.py` read User scope. Nothing reads `HKCU:\Environment` or
  Machine scope.
- `tools/mykicadMcp/kicad_mouser_tool.py:35` looks for `.env` at
  `Path(__file__).parent.parent`. Since the move to `tools/mykicadMcp/` that
  resolves to `tools/.env`, not the repo-root `.env` its comment names, so
  the Mouser key file is silently not found. No PcTools test imports that
  module. This is a separate bug in the submodule, outside this review's
  scope.
- `KC_REUSE_SID` (a web session cookie, `web_commission_row.py:2189`) is not
  `KILNCTL_`-prefixed and so survives the scrub. A parent process sets it
  only for its own children, so a pytest run never inherits it in practice.
- Q2, live_bench: the three `test_live_bench_*.py` files and
  `bench_fixture_session.py` read only `KILNCTRL_BENCH_*`. They reach the
  board through `host_resolve`/`http_auth`, which use exactly the kept trio.
  Nothing a live_bench test needs is dropped. The dropped `KILNCTL_LCD_PIN`,
  `KILNCTL_AUX_BENCH_CONFIRM`, `KILNCTL_WEB_USER_*` and `KILNCTL_TOTP_CODE`
  are read only by `bench_test` cases, which run through the MCP
  `bench_test_run` tool, not pytest. The dropped tool paths
  (`KILNCTL_OPENOCD*`, `KILNCTL_XTENSA_GCC`, `KILNCTL_CLANGD`) are read only
  by `tools/setup.ps1`.

## Mutation evidence

Command, run from the review worktree at `b844322b3`, using the main venv
python inside negtest's throwaway copy:

```
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset pytest `
  -PresetArg "tests/test_network_guard.py tests/test_mcp_server_backup.py" `
  -Mutations <private scratch>\rvtn4\mut.json
```

Baseline (unmutated) PASS in 204 s. The JSON reported
`real_tree_unchanged: true` and `copies_removed: true`. Logs are in
`%TEMP%\negtest_logs\20261012_022131_4thh`.

| id | mutation | verdict | failing test |
|----|----------|---------|--------------|
| M1 | `KILNCTL_TEST_NO_HUB` assigned via `setdefault` | CAUGHT | `test_conftest_forces_no_hub_over_inherited_zero` |
| M2 | `KILNCTL_LCD_PIN` added to `_BENCH_KEEP` | CAUGHT | `test_conftest_keeps_board_env_with_bench_host_but_no_hub` |
| M3 | bench-keep branch removed | CAUGHT | same |
| M4 | `_EXTRA_SECRETS` branch disabled | CAUGHT | both conftest env tests |
| M5 | bench host read as `KILNCTL_BENCH_HOST` | CAUGHT | bench-host keep test |
| M6 | `KILNCTL_SLOW_TESTS` not kept | CAUGHT | drop test |
| M7 | prefix `KILNCTL_TEST_` -> `KILNCTL_TEST` | **MISSED** | (I2) |
| M8 | prefix `KILNCTL_BUILD_` -> `KILNCTL_B` | CAUGHT | both conftest env tests |
| M9 | NullLink warning text changed | CAUGHT | `test_nulllink_fallback_warning_logged` |
| M10 | backup "snapshot unavailable" text changed | CAUGHT | two `BackupImportSnapshotTest` tests |

Not run on purpose: removing the guard in any of the three scripts. When
the guard is absent, the test runs the real action (L2): it starts the real
hotspot, publishes over the network, or loads live credentials.

Mutation JSON:

```json
[
 {"name":"M1 no_hub setdefault","file":"tools/PcTools/tests/conftest.py","find":"os.environ[\"KILNCTL_TEST_NO_HUB\"] = \"1\"","replace":"os.environ.setdefault(\"KILNCTL_TEST_NO_HUB\", \"1\")"},
 {"name":"M2 bench keep leaks LCD_PIN","file":"tools/PcTools/tests/conftest.py","find":"_BENCH_KEEP = frozenset({\"KILNCTL_HOST\",","replace":"_BENCH_KEEP = frozenset({\"KILNCTL_LCD_PIN\", \"KILNCTL_HOST\","},
 {"name":"M3 bench keep branch removed","file":"tools/PcTools/tests/conftest.py","find":"        elif bench_host_set and name in _BENCH_KEEP:\n            continue\n","replace":""},
 {"name":"M4 extra secrets not dropped","file":"tools/PcTools/tests/conftest.py","find":"        if name in _EXTRA_SECRETS:","replace":"        if False:"},
 {"name":"M5 bench host wrong spelling","file":"tools/PcTools/tests/conftest.py","find":"bool(os.environ.get(\"KILNCTRL_BENCH_HOST\")))","replace":"bool(os.environ.get(\"KILNCTL_BENCH_HOST\")))"},
 {"name":"M6 SLOW_TESTS not kept","file":"tools/PcTools/tests/conftest.py","find":"\"KILNCTL_SLOW_TESTS\", ","replace":""},
 {"name":"M7 control prefix loses underscore (boundary)","file":"tools/PcTools/tests/conftest.py","find":"_CONTROL_PREFIXES = (\"KILNCTL_TEST_\",","replace":"_CONTROL_PREFIXES = (\"KILNCTL_TEST\","},
 {"name":"M8 control prefix broadened to KILNCTL_","file":"tools/PcTools/tests/conftest.py","find":"_CONTROL_PREFIXES = (\"KILNCTL_TEST_\", \"KILNCTL_BUILD_\",","replace":"_CONTROL_PREFIXES = (\"KILNCTL_TEST_\", \"KILNCTL_B\","},
 {"name":"M9 NullLink warning text","file":"tools/PcTools/src/kilnctrl/mcp_server.py","find":"link hub disabled, using NullLink","replace":"hub off"},
 {"name":"M10 backup snapshot reason text","file":"tools/PcTools/src/kilnctrl/mcp_server_info.py","find":"pre-import profile snapshot unavailable","replace":"pre-import profile snapshot missing"}
]
```

## Fix status

| id | status | fix |
|----|--------|-----|
| L1 | fixed | `KILNCTL_TEST_SETTINGS_PATH` override in `settings.py`; `conftest.py` points it at a temp file before kilnctrl imports; in-process and subprocess tests |
| L2 | fixed | scripts exit just past their guard under `KILNCTL_TEST_GUARD_PROBE=1`, so a regressed guard cannot act; vacuous sentinel assertion dropped (registry, not env) |
| L3 | fixed | `bench_hotspot.ps1` refuses every action; `make_release.ps1` refuses fetch/ls-remote path; `check_release_manifest.ps1` clears the vars |
| I1 | open | |
| I2 | fixed | `test_scrub_keep_prefix_boundary` | |
