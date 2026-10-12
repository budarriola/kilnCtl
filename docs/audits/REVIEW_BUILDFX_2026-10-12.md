# Review: buildfx (576aa2381, 2437f5019), 2026-10-12

Scope: `git diff 2437f5019~2 2437f5019` on origin/dev. The two commits fix
`build_kilnfw` / `build_kilnfw_start` reporting OK in two cases:

- `idf.py` did nothing because `MSYSTEM` leaked into the gated child env.
  `child_env(None)` re-copied `os.environ`.
- The build produced no artifacts.

What changed, in `tools/PcTools/src/mcpkit/workbench.py`:

- `_clean_env()` is applied in `_run`, on the gated path before `child_env`, and
  for the SaftyFW configure env.
- `_enforce_fresh_artifacts()` requires `KilnCtrl.bin`, `bootloader/bootloader.bin`
  and `partition_table/partition-table.bin` to exist with an mtime at or after the
  job start (2 s tolerance). It also requires that the report has no MSys message.
- New file `tests/test_build_kilnfw_artifact_gate.py`. The two older build test
  files gain an autouse fixture that disables the gate.

This review is read-only. It ran in worktree `C:\wt\buildfxrev_w27lxr`, detached
at origin/dev. Nothing was fixed. No board was touched and no real build ran.

## Verdict

One HIGH, one MED, five LOW, three INFO.

The MSys false OK is closed. `_run` is now a single choke point and strips
`MSYSTEM` for every child. The "exit 0, no bin" false OK is closed for
`target="build"`.

The gate has the opposite defect. It reports **FAILED on every incremental
build**: ninja does not rewrite `bootloader.bin` or `partition-table.bin` when
their inputs are unchanged (HIGH-1). The other false-OK paths still open are:

- Non-`build` targets have no backstop (MED-1).
- A concurrent same-dir build can supply "fresh" bins (LOW-1).

## Evidence

- **Root cause confirmed from the installed IDF.** In
  `C:\esp\v6.0.2\esp-idf\tools\idf.py:1178-1205`, `if 'MSYSTEM' in os.environ:`
  prints the "MSys/Mingw is no longer supported" warning, and `main()` is only
  called in the `else` branch. The process then exits 0 having done nothing.
  `MSYSTEM` is the only variable idf.py tests. CMake's `utilities.cmake:122` reads
  `ENV{MSYSTEM}` only to pick line endings.
- **PowerShell wrapper exit code.** These two commands both exit **0**:
  - `powershell -NoProfile -Command 'nonexistent_cmd_xyz; exit $LASTEXITCODE'`
  - the same with a missing profile, `& 'C:\nope\missing_profile.ps1' *>&1 | Out-Null; nonexistent_idf_xyz ...; exit $LASTEXITCODE`

  A missing `idf.py` alias therefore yields OK (used in MED-1).
- **Incremental build mtimes, main tree** (`firmware/KilnFW/build`, read-only):

  | File | mtime |
  |---|---|
  | `KilnCtrl.bin` | 2026-09-25 12:24:54 |
  | `KilnCtrl.elf` | 2026-09-25 12:24:53 |
  | `bootloader/bootloader.bin` | 2026-09-25 12:23:49 |
  | `partition_table/partition-table.bin` | **2026-09-20 10:41:13** |

  `.ninja_log` holds a single `partition-table.bin` entry. Later successful
  builds never regenerated it, so under this gate each of them would have been
  reported FAILED.
- **Why `KilnCtrl.bin` is always fresh.** `App/drivers/CMakeLists.txt:740`
  declares `drivers_build_info` as an `ALL` target. It runs
  `gen_build_info.cmake`, which `file(RENAME)`s a new `build_info.h` on every
  build with `FW_BUILD_TIME` to the second, so the app always relinks.
  `bootloader.bin` and `partition-table.bin` have no such forcing input.
- **PcTools tests** (worktree, main-tree venv interpreter, `PYTHONPATH` set to the
  worktree `src`): run with `-k "build or workbench or msys"` plus the three build
  test files, 233 passed, 1 skipped.
- **Negtest** (`tools\negtest.ps1 -Preset pytest` over the three build test files,
  copy at origin/dev `5033604f37`, log `%TEMP%\negtest_logs\20261012_004827_v90l`):
  baseline PASS. 4 of 9 mutations CAUGHT, 5 MISSED. See the appendix.

## Findings

### HIGH-1: every incremental `build_kilnfw` is reported FAILED

`_enforce_fresh_artifacts` requires all three bins to have mtime >= job start - 2 s.
An incremental build (any build after the first in a build dir, unless
`partitions.csv`, bootloader sources or bootloader sdkconfig changed) rewrites
`KilnCtrl.bin` but leaves `bootloader.bin` and `partition-table.bin` alone. The
result is:

`kilnfw-build: FAILED (exit code OK but build artifacts not fresh): stale (older than job start): ...partition-table.bin`

The same happens on the `jobs > 0` (`ninja -j N`) path. This is a false FAIL, but
it is not harmless:

- Agents will learn to ignore `kilnfw-build: FAILED`, which defeats the gate.
- Or they will `fullclean` every time, about 2100 TUs per build.
- `_refresh_build_sdkconfig` is skipped on every incremental build, which
  re-opens the sdkconfig sibling drift that the 2026-09-23 fix closed.
- `build_job_status` classifies these jobs as failed.

`test_stale_bins_are_failed` encodes the bug: it treats stale bootloader and
partition-table bins as failure.

**Fix.** Require freshness only for `KilnCtrl.bin`, which always relinks (see
Evidence). For `bootloader.bin` and `partition-table.bin`, require existence and
non-zero size only. Stronger alternative for the app: read `KilnCtrl.bin`'s
`esp_app_desc_t` (`kilnctrl/esp_app_desc.py`) and require its build date/time to
be >= the job start in UTC. That ties freshness to the image content instead of
the filesystem mtime.

Split the test: stale `KilnCtrl.bin` FAILS. Fresh `KilnCtrl.bin` with month-old
bootloader and partition-table bins is OK.

### MED-1: non-`build` targets still report OK when idf.py never ran

The artifact gate and the MSys check are applied only when `target == "build"`.
Three paths still exit 0 with nothing done:

- A missing or broken `idf.py` alias. The profile is `Out-Null`'d, so a failure to
  define `Invoke-idfpy` is silent, and `exit $LASTEXITCODE` exits 0 (see Evidence).
- Any `fullclean` or `reconfigure` in that state.
- The ungated `kilnfw-set-target` step on the `kiln_fw_root` path.

In each case the tool returns `kilnfw-<target>: OK`. For `reconfigure`, the
sdkconfig refresh then runs on that false OK.

**Fix.** Harden the PowerShell wrapper for every target and for set-target:

```
$ErrorActionPreference='Stop'
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) { exit 97 }
<cmd>
if ($null -eq $LASTEXITCODE) { exit 98 }
exit $LASTEXITCODE
```

Apply the MSys-message check to every `kilnfw-*` report, not just `build`. Per
target:

- `reconfigure`: require a fresh `build/CMakeCache.txt` mtime.
- `fullclean`: require that `build/` is empty or gone.

### LOW-1: freshness baseline is taken before the build-lock wait

`job_start_wall = time.time()` is taken before `_run_locked` blocks on
`build_lock(build_dir)`. Suppose another `build_kilnfw` on the same dir holds the
lock and writes fresh bins, and then this call's `idf.py` no-ops at exit 0 (the
MED-1 shell failure). This call reports OK on bins it did not produce. The bins
match the same tree, so the impact is small.

**Fix.** Record the baseline after the lock is acquired. For example, have
`_run_locked` set a `started_wall` field on a holder object, as `GateWaitResult`
does. Or snapshot `(mtime_ns, size)` of `KilnCtrl.bin` after acquiring the lock
and require it to change.

### LOW-2: the env strip is pinned only as a pair; the ungated path is untested

`_clean_env` runs in two places: `_run`, and `_run_locked`'s gated branch.
`test_child_env_has_no_msystem` goes through the gated branch, so it fails only
when both strips are removed:

- M1 (drop the `_run` strip): MISSED.
- M2 (drop the gated strip): MISSED.
- M3 (drop both): CAUGHT.

The ungated callers are `kilnfw-set-target`, `kilnfw-submodule-init` and
`saftyfw-configure`. Their only protection is the `_run` strip, and no test
covers it.

**Fix.** Add a test that calls `_run_locked` without `gate_label` (and `_run`
directly) with `MSYSTEM` set and asserts that it is absent in the child. The
gated-branch strip is then redundant defense in depth. Keep it with a comment
that says so, or drop it.

### LOW-3: artifact list and tolerance are not pinned by any test

- M6 (drop `bootloader.bin` and `partition-table.bin` from `_KILNFW_ARTIFACTS`)
  was MISSED. Every failing test also lacks `KilnCtrl.bin`.
- M5 (tolerance 2 s -> 3000 s) was MISSED. The stale test ages its bins by 1 h.

**Fix.** Add a parametrized test where each artifact is missing on its own and the
others are fresh, and expect FAILED naming that artifact. Age the stale-app case
by about 10 s, not 1 h. Once HIGH-1 is fixed, the bootloader and partition-table
cases become missing-only.

### LOW-4: the gate checks the summarized report, not the full output, and can drop the log path

`_enforce_fresh_artifacts` greps the `_summarize` result: the last 20 lines plus
up to 20 "interesting" lines. Today that is enough, because an MSys no-op prints
almost nothing. The downgraded report keeps only `report.splitlines()[1:][-25:]`,
so on a long report the `full log: <path>` line (line 2) is dropped.

**Fix.** Do the MSys check as an `output_check` on `_run`. `_summarize` already
supports one: it sees the full output and produces the
`FAILED (exit 0, but output check failed)` header. Always carry the `full log:`
line into the downgraded report.

### LOW-5: a zero-byte or truncated bin passes

The gate checks only existence and mtime. A `KilnCtrl.bin` that was truncated or
zero-length after an interrupted `elf2image` write would pass.

**Fix.** Require `size > 0` for all three bins. For `KilnCtrl.bin`, also require
the `esp_app_desc_t` magic (`0xABCD5432` at offset 0x20). The HIGH-1 alternative
already reads this structure.

### LOW-6: the autouse bypass hides how the gate interacts with sdkconfig refresh

`test_build_kilnfw_saftyfw_order.py` and `test_build_kilnfw_sdkconfig_refresh.py`
replace `_enforce_fresh_artifacts` with an identity lambda for every test. That
has three effects:

1. No test shows that a gate-downgraded report suppresses `_refresh_build_sdkconfig`.
   The behaviour is correct today only because the downgraded text does not
   contain `kilnfw-build: OK`.
2. Several refresh tests model "ELF unchanged after `target=build`":
   - `test_live_differs_and_elf_unchanged_skips_refresh`
   - `..._identical_...`
   - `..._missing_sibling_...`
   - `..._comment_only_...`
   - `..._real_config_line_...`

   With the real gate that case is a stale `KilnCtrl.bin`, so the build reports
   FAILED and refresh is never reached. Those branches are now reachable only for
   `target="reconfigure"`, and the bypass hides this.
3. The lambda accepts any signature, so a signature drift in the real call would be
   invisible in those files. `test_build_kilnfw_artifact_gate.py` still catches it.

**Fix.** Replace the bypass with a shared fake that writes fresh bins into
`build_dir`, so the real gate runs. Add one test where the gate downgrades the
report, and assert that the sdkconfig sibling is untouched. Move the
"ELF unchanged" refresh tests to `target="reconfigure"` or document them as
gate-bypassed.

### INFO-1: completeness of the MSYS strip

`_MSYS_ENV_VARS` covers `MSYSTEM`, `MSYSTEM_PREFIX`, `MSYSTEM_CHOST` and
`MSYS2_PATH_TYPE`. That is sufficient for the observed failure, because idf.py and
IDF's CMake read only `MSYSTEM`. The match is on `k.upper()`. M9 (case-sensitive
match) is equivalent on Windows, where `os.environ` keys are already upper-case,
so MISSED is expected there.

This Git Bash also exports `SHELL`, `TERM`, `EXEPATH` and `PLINK_PROTOCOL`, and
puts `C:\Program Files\Git\mingw64\bin` and `\usr\bin` at the front of `PATH`.
`ORIGINAL_PATH`, `MINGW_PREFIX`, `MINGW_CHOST` and `MSYSTEM_CARCH` are absent here
but appear in some Git and MSYS2 builds.

For KilnFW, the IDF profile prepends its own `PATH`, so the Git bins are
shadowed. The SaftyFW `cmake --build` and configure run without the profile, so
Git's `usr\bin` (`link.exe`, `sh.exe`, `find.exe`) comes before system tools. With
the Ninja generator and arm-none-eabi-gcc that is harmless today.

**Optional hardening:** also strip `MSYSTEM_CARCH`, `MINGW_*` and
`ORIGINAL_PATH`, and remove `\Git\usr\bin` and `\Git\mingw64\bin` entries from the
child `PATH`.

### INFO-2: other build tools and background jobs

- `build_saftyfw` runs `cmake --build` directly with no PowerShell layer, so the
  exit code propagates and a ninja no-op is correctly OK. The slot bins it
  produces are cross-checked by KilnFW's configure step and by
  `check_embedded_pico_image_fresh.ps1`. No gate is needed.
- No MCP tool builds the recovery image or the fixture firmware. The recovery image
  is built only by `check_00_kilnfw_recovery_target_build.ps1`, which strips
  `MSYSTEM` itself, as do the other `check_00_*` and `check_01_*` build checks.
- `build_saftyfw_host_tests` uses `powershell -File`, which propagates `exit N`
  and returns 1 on an uncaught throw.
- `run_pctools_tests` has `pytest_output_problems`.
- `run_repo_checks` uses `-File`.
- `build_job_status`: `build_jobs.classify_report` is OK only when every
  `<tag>: VERDICT` line is OK. The downgraded `kilnfw-build: FAILED` head makes the
  job `failed`. The tail drops the original OK head line (`[1:]`), so it cannot
  re-introduce one. No false OK there.

### INFO-3: clock skew and OneDrive

File mtimes and `time.time()` come from the same host clock. NTFS resolution is
100 ns, and the 2 s tolerance covers FAT and SMB granularity. OneDrive does not
rewrite mtimes on sync. A backward clock step of more than 2 s during a build (an
NTP correction) would cause a false FAIL. That is rare, and an
`esp_app_desc_t`-based check (HIGH-1 alternative) has the same exposure. Not worth
more.

## Appendix: negtest mutations

Command (worktree `C:\wt\buildfxrev_w27lxr`):

```
powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset pytest `
  -PresetArg "tests/test_build_kilnfw_artifact_gate.py tests/test_build_kilnfw_saftyfw_order.py tests/test_build_kilnfw_sdkconfig_refresh.py" `
  -Mutations buildfx_mut.json
```

Results: baseline PASS (25.9 s).

| # | Mutation | Verdict | Failing test |
|---|---|---|---|
| M1 | drop `_clean_env` in `_run` | MISSED | (LOW-2) |
| M2 | drop `_clean_env` on gated path | MISSED | (LOW-2, redundant line) |
| M3 | drop both strips | CAUGHT | `test_child_env_has_no_msystem` |
| M4 | drop MSys message check | CAUGHT | `test_msys_message_is_failed` |
| M5 | tolerance 2 s -> 3000 s | MISSED | (LOW-3) |
| M6 | drop bootloader + partition-table from list | MISSED | (LOW-3) |
| M7 | never call the gate | CAUGHT | `test_exit0_but_no_bin_is_failed`, `test_msys_message_is_failed`, `test_stale_bins_are_failed` |
| M8 | presence only, no freshness | CAUGHT | `test_stale_bins_are_failed` |
| M9 | case-sensitive strip | MISSED | (INFO-1, equivalent on Windows) |

```json
{"mutations":[
 {"name":"M1 drop _clean_env in _run (ungated path)","file":"tools/PcTools/src/mcpkit/workbench.py","find":"    env = _clean_env(env)","replace":"    pass"},
 {"name":"M2 drop _clean_env on gated path","file":"tools/PcTools/src/mcpkit/workbench.py","find":"child_env(_clean_env(kwargs.get(\"env\")))","replace":"child_env(kwargs.get(\"env\"))"},
 {"name":"M3 drop both strips","edits":[
   {"file":"tools/PcTools/src/mcpkit/workbench.py","find":"    env = _clean_env(env)","replace":"    pass"},
   {"file":"tools/PcTools/src/mcpkit/workbench.py","find":"child_env(_clean_env(kwargs.get(\"env\")))","replace":"child_env(kwargs.get(\"env\"))"}]},
 {"name":"M4 drop MSys message check","file":"tools/PcTools/src/mcpkit/workbench.py","find":"    if \"MSys/Mingw is no longer supported\" in report:","replace":"    if False:"},
 {"name":"M5 loosen stale tolerance 2s -> 3000s","file":"tools/PcTools/src/mcpkit/workbench.py","find":"if mtime < since - 2.0:","replace":"if mtime < since - 3000.0:"},
 {"name":"M6 drop bootloader+partition-table from artifact list","edits":[
   {"file":"tools/PcTools/src/mcpkit/workbench.py","find":"    (\"bootloader\", \"bootloader.bin\"),","replace":""},
   {"file":"tools/PcTools/src/mcpkit/workbench.py","find":"    (\"partition_table\", \"partition-table.bin\"),","replace":""}]},
 {"name":"M7 never call the gate","file":"tools/PcTools/src/mcpkit/workbench.py","find":"kilnfw_report = _enforce_fresh_artifacts(kilnfw_report, \"kilnfw-build\", build_dir,","replace":"kilnfw_report = (lambda *a: a[0])(kilnfw_report, \"kilnfw-build\", build_dir,"},
 {"name":"M8 freshness check skipped (presence only)","file":"tools/PcTools/src/mcpkit/workbench.py","find":"if mtime < since - 2.0:","replace":"if False:"},
 {"name":"M9 MSYS strip case-sensitive","file":"tools/PcTools/src/mcpkit/workbench.py","find":"if k.upper() not in _MSYS_ENV_VARS}","replace":"if k not in _MSYS_ENV_VARS}"}
]}
```
