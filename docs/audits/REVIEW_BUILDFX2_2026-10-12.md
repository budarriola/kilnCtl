# Review: build_kilnfw artifact gate fix (a69614d69), 2026-10-12

Scope: origin/dev `a69614d69` ("build_kilnfw: gate on KilnCtrl.bin only, harden
wrapper, post-lock baseline, tests"), which fixes REVIEW_BUILDFX_2026-10-12
HIGH-1 (incremental builds reported FAILED), MED-1 and LOW-1..6.
Files: `tools/PcTools/src/mcpkit/workbench.py`, `tests/_kilnfw_fakes.py`,
`tests/test_build_kilnfw_artifact_gate.py`, the sdkconfig-refresh and
saftyfw-order tests, and the prior review's fix-status table.

Method: code read; a real clean and a real no-op incremental build through
`build_kilnfw()` running the a69614d69 code (worktree `C:\wt\bfx2bld_*`,
inside the build gate, launched from a Git Bash parent with `MSYSTEM=MINGW64`);
a PowerShell 5.1 experiment of the wrapper's exit-code semantics; and 12
mutations through `tools/negtest.ps1 -Preset pytest` (appendix).

## Verdict

**a69614d69 does not block promotion.** No HIGH or MED finding. HIGH-1 is fixed
and confirmed on a real incremental build. MED-1's wrapper hardening is correct.
Four LOW items are test gaps or stale text, none of which lets a broken build
report OK today.

## Real builds (a69614d69 code, MSYSTEM=MINGW64 in the parent)

| Run | Result | Notes |
|---|---|---|
| clean (`set-target` + `build`) | `kilnfw-build: OK in 835.2s` | MSYSTEM stripped: idf.py ran, no MSys refusal |
| no-op incremental (`build`, no source change) | `kilnfw-build: OK in 159.7s` | 28 ninja steps: `build_info.h` regenerated, `[26/28] Linking CXX executable KilnCtrl.elf`, `[27/28] Generating binary image` |

No-op run timestamps (epoch seconds): job start (post-lock) 1791797168.6;
`KilnCtrl.bin` mtime 1791797353 (fresh, after start); `bootloader.bin`
1791796226 and `partition-table.bin` 1791795520 (both older than start, left
alone by ninja). The gate reported OK, which is the HIGH-1 case. It also
confirms that a true ninja no-op cannot happen for KilnFW:
`drivers_build_info` is an ALL target that rewrites `build_info.h` on every
build, so the app always relinks and `KilnCtrl.bin` is always rewritten on success.

## Findings

### LOW-1: four guards have no test that fails when they are removed

The mutations N2, N3, N7 and N8 were MISSED (appendix):

- **N2**: the env strip is made case-sensitive (`k not in` instead of
  `k.upper() not in`). The tests only use uppercase keys. A caller dict
  carrying `msystem` would reach the child as `MSYSTEM`, because Windows
  environment names are case-insensitive. No current caller builds such a dict.
- **N3**: the real `_run_locked` stops recording `lock_info["start_wall"]`.
  The harness replaces `_run_locked` with a fake that sets `start_wall` itself,
  so the placement in the real function is untested. If it regressed, the
  `time.time()` fallback (taken before the lock wait) would apply, which is the
  prior LOW-1 again.
- **N7**: `$global:LASTEXITCODE=$null;` is removed from the wrapper. This reset is
  load-bearing. The IDF profile ends with `Register-IdfCompletions`, which runs
  `python.exe` and leaves `$LASTEXITCODE=0`. Without the reset, a wrapper whose
  inner command never ran a native process would exit 0 instead of 98. The
  `Get-Command` check (exit 97) and `output_check` still cover the main
  no-op causes, so this is LOW. `test_wrapper_fails_closed` asserts only
  on the command string, and no test runs PowerShell.
- **N8**: `_downgrade` drops its `full log:` line. The test still passes because
  the tail (`lines[1:]`) usually carries the original `full log:` line too.

Fix: add a lowercase-key strip case, a test that drives the real
`_run_locked` (ungated, monkeypatched `build_lock`) and checks `start_wall`
is taken after acquisition, an assertion on the reset text in the command
string (or a Windows-only test that runs the wrapper against a fake
profile), and an assertion that the downgraded head line block contains
`full log:` before `--`.

### LOW-2: dead desc-time parse and stale descriptions

- `_app_bin_build_wall()` still parses the esp_app_desc time with
  `strptime`/`mktime`, but its time result is discarded. Only its "bad image"
  problem is used (`built, bad = ...`; `stamp = app_mtime`).
- The `_enforce_fresh_artifacts` docstring still says "its embedded build
  time, else its mtime".
- In `REVIEW_BUILDFX_2026-10-12.md`, the HIGH-1 row of the fix-status table still
  says "esp_app_desc build time, mtime fallback". The real-build note below that
  table describes the change correctly.

Fix: rename the function to an image-validity check, drop the time parse, and
correct both texts. A later reader might otherwise put the desc-time check back,
which is mutation N10, the observed false FAILED.

### LOW-3: no cross-check that static bins match this tree's configuration

`bootloader.bin` and `partition-table.bin` are checked for existence and a
non-zero size only. That is adequate for the build gate. The IDF partition-table
custom command depends on `partitions.csv` and carries sdkconfig-derived flags
on its command line, and the bootloader is an ExternalProject with
`BUILD_ALWAYS 1`. A successful ninja run therefore leaves both current
for the tree's own sdkconfig.

The remaining hole is a `build/` copied or published from elsewhere, such as
`check_00_kilnfw_target_build.ps1`'s publish step. That step publishes from the
same tree state, so it is not a new risk. If a cross-check is wanted, it belongs
in `flash_firmware`, which is what ships the bins. One option is to compare
`partition-table.bin` against `gen_esp32part.py` output from `partitions.csv`.
Not required for this commit.

### INFO

- **Clock.** The 2 s tolerance is relative to `time.time()` taken right after
  `build_lock` is acquired, before the gate wait and before the profile loads.
  NTFS mtimes and `time.time()` use the same host clock. The build dirs are under
  `C:\wt` or the OneDrive tree, and OneDrive does not rewrite local mtimes. A stale
  `KilnCtrl.bin` can pass only if the wall clock steps backward by more than
  2 s during the job, for example an NTP correction. That would also need the
  wrapper to exit 0 without linking, and the wrapper hardening now prevents that.
- **Failure after link.** A failure after the link step (for example
  `gen_esp32part`, or the post-build size check) exits nonzero. It is reported
  FAILED before the gate is consulted.
- **flash_firmware is independent of the report.** It re-checks the three bins
  with `os.path.isfile`, so a 0-byte file passes there. It then runs
  `check_kilnfw_stale`, which compares sources, including `partitions.csv`,
  against `KilnCtrl.bin` and `build_info.h`. This is consistent with the
  app-only freshness design. Requiring size > 0 in flash too would remove the
  asymmetry.
- **Env strip.** `_MSYS_ENV_VARS` is stripped case-insensitively at the
  `_run` choke point and in the gated branch of `_run_locked`. `MINGW_PREFIX`,
  `ORIGINAL_PATH` and Git's `PATH` entries pass through. idf.py's MSys refusal
  keys on `MSYSTEM` alone, so the strip is complete for its purpose. The
  `output_check` on every kilnfw step backs it up. Confirmed on the real build:
  the parent had `MSYSTEM=MINGW64` and idf.py built normally.
- **Wrapper semantics** (PowerShell 5.1, measured): `$ErrorActionPreference='Stop'`
  does not turn a native command's stderr into a terminating error. stderr with
  rc 0 gave 0, and stderr with rc 3 gave 3, also through the profile's `idf.py`
  alias function. A missing `cd` target gave 1, a missing tool gave 97, and no
  native command run gave 98. The wrapper is correct.
- **fullclean effect check.** The build lock lives in
  `%TEMP%\kilnctl-builds\locks`, not in `build/`, so `_enforce_target_effect`'s
  "build/ empty or absent" check is not tripped by the lock.
- **Pre-existing, not in this commit: `build_lock` reclaims only by age.**
  A caller killed while holding the lock (observed here, when an outer
  timeout killed the python process) leaves the lock file in place. Every later
  build of that directory then waits up to `wait_timeout` (1200 s), and the lock
  is only reclaimed after `stale_after` (2100 s). Checking whether the
  recorded PID is still alive would reclaim it at once.

## Answers to the review questions

1. **Can a stale `KilnCtrl.bin` pass?**
   - A clock skew or jump passes only with a backward step of more than 2 s (INFO above).
   - A file touched without a relink passes in principle. Only a process
     outside the build could do that, within the 2 s window, while the build exits 0.
   - A failure after the link is FAILED from the exit code.
   - A no-op build cannot happen for KilnFW; the real run shows a relink every
     time, and it is reported OK, not stale.
2. **Can a stale or wrong static bin ship?**
   - After a clean, a missing or empty bin is FAILED.
   - A bin left over from another sdkconfig is regenerated by ninja on any
     successful build.
   - A cross-check is optional and belongs in `flash_firmware` (LOW-3).
3. **What is the 2 s tolerance relative to?** The post-lock `time.time()`.
   The clock is the same one NTFS uses, and OneDrive does not affect it.
4. **Does `flash_firmware` consume these results consistently?** It does not
   consume them at all. It runs its own checks, which agree with this design apart
   from the 0-byte asymmetry (INFO).
5. **M3 and M9.**
   - M3 (no recorded result) has its analogue N1, which is CAUGHT; the env-strip tests now bite.
   - M9 has its analogue N2 (case sensitivity), which is still MISSED (LOW-1).
6. **`_clean_env`.** It is correct and sufficient. It is case-insensitive and sits
   at a single choke point. The unstripped `MINGW_*`/`PATH` variables are irrelevant
   to idf.py's check (INFO).
7. **Negtests and real build.**
   - Negtests: 8 of 12 CAUGHT. The baseline passed and the real tree was unchanged.
   - The real incremental no-op build reported OK.

## Appendix: mutations

Command (from `C:\wt\bfx2rv_*` at a69614d69):

```
powershell -ExecutionPolicy Bypass -File tools/negtest.ps1 -Preset pytest `
  -PresetArg "tests/test_build_kilnfw_artifact_gate.py tests/test_build_kilnfw_saftyfw_order.py tests/test_build_kilnfw_sdkconfig_refresh.py tests/test_build_jobs.py" `
  -Mutations bfx2_mut.json
```

Baseline PASS (46.5 s). Overall verdict MISSED. `real_tree_unchanged: true`, `copies_removed: true`.

| Mutation | Result |
|---|---|
| N1 drop both env strips (M3 analogue) | CAUGHT |
| N2 case-sensitive strip (M9 analogue) | MISSED |
| N3 real `_run_locked` never records post-lock start_wall | MISSED |
| N4 tolerance 2 s -> 3000 s | CAUGHT |
| N5 static bins: drop size > 0 check | CAUGHT |
| N6 drop app-desc magic check | CAUGHT |
| N7 wrapper: no LASTEXITCODE reset | MISSED |
| N8 downgrade drops full log line | MISSED |
| N9 ignore post-lock baseline | CAUGHT |
| N10 freshness from desc time | CAUGHT |
| N11 gate never applied to build | CAUGHT |
| N12 wrapper: no Get-Command check | CAUGHT |

```json
{"mutations":[
 {"name":"N1 drop both env strips (M3 analogue)","edits":[
   {"file":"tools/PcTools/src/mcpkit/workbench.py","find":"    env = _clean_env(env)","replace":"    pass"},
   {"file":"tools/PcTools/src/mcpkit/workbench.py","find":"child_env(_clean_env(kwargs.get(\"env\")))","replace":"child_env(kwargs.get(\"env\"))"}]},
 {"name":"N2 case-sensitive strip (M9 analogue)","file":"tools/PcTools/src/mcpkit/workbench.py","find":"if k.upper() not in _MSYS_ENV_VARS}","replace":"if k not in _MSYS_ENV_VARS}"},
 {"name":"N3 real _run_locked never records post-lock start_wall","file":"tools/PcTools/src/mcpkit/workbench.py","find":"                lock_info[\"start_wall\"] = time.time()","replace":"                pass"},
 {"name":"N4 tolerance 2 s -> 3000 s","file":"tools/PcTools/src/mcpkit/workbench.py","find":"if stamp < since - _FRESH_TOLERANCE_S:","replace":"if stamp < since - 3000.0:"},
 {"name":"N5 static bins: drop size>0 check","file":"tools/PcTools/src/mcpkit/workbench.py","find":"        if size <= 0:","replace":"        if False:"},
 {"name":"N6 drop app-desc magic check","file":"tools/PcTools/src/mcpkit/workbench.py","find":"            if bad:","replace":"            if False:"},
 {"name":"N7 wrapper: no LASTEXITCODE reset","file":"tools/PcTools/src/mcpkit/workbench.py","find":"$global:LASTEXITCODE=$null; ","replace":""},
 {"name":"N8 downgrade drops full log line","file":"tools/PcTools/src/mcpkit/workbench.py","find":"+ f\"\\n{log_line}\\n--\\noutput tail:\\n{tail}\")","replace":"+ f\"\\n--\\noutput tail:\\n{tail}\")"},
 {"name":"N9 ignore post-lock baseline","file":"tools/PcTools/src/mcpkit/workbench.py","find":"job_start_wall = lock_info.get(\"start_wall\", job_start_wall)","replace":"pass"},
 {"name":"N10 freshness from desc time","file":"tools/PcTools/src/mcpkit/workbench.py","find":"                stamp = app_mtime","replace":"                stamp = built if built is not None else app_mtime"},
 {"name":"N11 gate never applied to build","file":"tools/PcTools/src/mcpkit/workbench.py","find":"    if target == \"build\":","replace":"    if target == \"never\":"},
 {"name":"N12 wrapper: no Get-Command check","file":"tools/PcTools/src/mcpkit/workbench.py","find":"if (-not (Get-Command {tool} -ErrorAction SilentlyContinue)) {{ exit 97 }}; ","replace":""}
]}
```

## Fix status

| Finding | Status | Fix |
|---|---|---|
| LOW-1 N2/N3/N7/N8 untested guards | fixed | four new tests in `test_build_kilnfw_artifact_gate.py` (lowercase env key; real `_run_locked` with a fake lock and fake `_run`; wrapper reset text and order; `full log:` in the downgraded head); each re-negative-tested CAUGHT |
| LOW-2 dead desc-time parse, stale texts | fixed | `_app_bin_build_wall` replaced by `_app_image_problem` (magic/validity only); docstring corrected; HIGH-1 row of REVIEW_BUILDFX now says mtime-only and why the desc time must not return |
| LOW-3 static bins vs tree config | accepted, not changed | reviewed as optional ("Not required"): a successful ninja run leaves both bins current, and the publish step uses the same tree state; a `gen_esp32part.py` cross-check belongs in `flash_firmware` if ever wanted (re-verified 2026-10-12, no defect) |
| INFO: flash accepts 0-byte bins | fixed | `flash_firmware` refuses an empty bootloader/partition-table/app bin; tests in `test_flash_firmware_erase_partitions.py` |
| INFO: build_lock reclaims only by age | fixed | `build_lock` also reclaims when the recorded PID is dead or the live process started after the lock was written (PID reuse); tests in `test_buildlock.py` |
