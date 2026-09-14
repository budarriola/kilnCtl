# adaptive_tune_ki.c refusal-message truncation + check_pid_fuzzy_drift.ps1 build race (2026-09-13)

Follow-up to the two defects named in `docs/audits/adaptive_tune_ki_effective_reference_loop_2026-09-13.md`'s
appended opus-review section (commit `a380305793487eeb88499dc768cea34e38727d5c`). Both are fixed here.

## Defect 1 -- PID_FUZZY refusal reason truncated

`z->ki_refusal_reason` (`firmware/KilnFW/App/drivers/control/adaptive_tune_internal.h:322`) is
`char[96]`, written via `adaptive_tune_set_reason()` -> `vsnprintf()`
(`firmware/KilnFW/App/drivers/control/adaptive_tune.c:138`), which truncates silently -- no return-code
check, nothing that flags an overflow.

The message `e78fbc5b` added at `adaptive_tune_ki.c`'s effective-vs-reference fuzzy guard
formatted to **163 bytes** at `fuzzy_pct=100` (measured directly with Python's `%` formatting
against the literal format string), well past the 96-byte buffer. `vsnprintf` truncated it at
"...not the stored ", discarding the entire actionable half: what was withheld and why applying it
would ratchet the reference. The existing regression test
(`test_ki_diagnosis_withholds_correction_when_zone_is_pid_fuzzy`,
`firmware/KilnFW/App/test/test_adaptive_tune_ki_bounds.c`) only asserted `strstr(reason, "fuzzy")`,
which matches at character 57 -- inside the surviving prefix -- so it passed straight through the
truncation.

Fix: reworded the message (`adaptive_tune_ki.c`, the `ZONE_CONTROL_MODE_PID_FUZZY` branch) to
`"zone is PID_FUZZY %.0f%%: withholding correction -- trace is fuzzy's effective Ki, not reference"`,
which formats to **94 bytes** at the worst case (`fuzzy_pct=100`) -- comfortably inside the 96-byte
buffer, no truncation at all -- while keeping both halves (withheld: the correction; why: the trace
reflects fuzzy's effect, not the stored reference). The buffer itself was NOT enlarged, per this
repo's standing rule against growing stack/struct buffers in this kind of path (two prior panics
from oversized locals, see CLAUDE.md's "httpd stack near-overflow" and "PSRAM stack + NVS" notes).

Test hardened (same file): added a length assertion (`strlen(reason) < sizeof(buffer) - 1`) and a
check for `"reference"`, a token that only appears in the actionable, previously-truncated tail --
the old 163-byte wording did NOT contain a surviving `"reference"` after truncation (it cut off
right before that word), so this new check would have caught the original defect. Confirmed the
full `check_pid_fuzzy_drift.ps1`-adjacent host-test suite (`build_host_tests.ps1`, 38/38
executables) still builds and passes with both changes.

**Other refusal reasons in the same buffer**, checked by measuring each format string's expansion
at its own worst-case argument values:

| call site (verdict) | worst-case length | fits in 96? |
|---|---|---|
| insufficient samples | 36 | yes |
| no-correction-indicated | 38 | yes |
| FLOORED (`-ff_hold` signature) | 92 | yes (tight, but no truncation) |
| PID_FUZZY guard | 163 (old) / 94 (new) | **no (old, fixed)** / yes (new) |
| no existing positive Ki | 33 | yes |
| corrected Ki not a valid gain | 41 | yes |
| cumulative ceiling bound | 80 | yes |
| cumulative floor bound | 81 | yes |
| `zones_config_set_pid()` rejected | 48 | yes |

Only the PID_FUZZY message truncated. The FLOORED message is the next-closest at 92/96 -- worth a
glance if its wording ever grows, but it does not truncate today and is out of this task's scope
(not the identified defect).

## Defect 2 -- `check_pid_fuzzy_drift.ps1` / `pid_fuzzy_drift_check.py` shared-build-dir race

The review established the check's math is correct (804 vectors, worst |diff| = 6.1e-09) and that a
captured `'vswhere.exe' is not recognized ...` line is harmless vcvarsall.bat stderr noise (it tries
vswhere first, falls back, still succeeds) -- `pid_fuzzy_drift_check.py` only echoes captured
build output when the build itself fails, so that line is a symptom of investigating a real failure
elsewhere, never the cause by itself. `check_pid_fuzzy_drift.ps1` now states this explicitly in a
header comment so a future reader does not re-chase it.

The real likely cause of intermittent failures: `pid_fuzzy_drift_check.py`'s `BUILD_DIR` was
`firmware/KilnFW/App/test/build` -- the **same** directory `build_host_tests.ps1` builds the whole
host-test tree into. `build_host_tests.ps1` serializes on `tools/build_lock.ps1`'s named
`Global\` Mutex before touching that tree; `pid_fuzzy_drift_check.py` took no lock at all. Two
concurrent `run_all_checks.ps1` runs (or one overlapping a manual host-test build) could have their
`cl.exe` invocations and `.obj`/build-log writes collide in that shared directory -- the same class
of corruption `build_lock.ps1`'s own header comment documents for `check_bootloader_builds.ps1`.

Fix chosen: **give the check its own private build directory**
(`firmware/KilnFW/App/test/build_pid_fuzzy_drift/`) rather than teaching it to take
`tools/build_lock.ps1`'s Mutex. Rationale: `build_lock.ps1` is a PowerShell-only helper (a .NET
named `Mutex` dot-sourced into a `.ps1`); `pid_fuzzy_drift_check.py` is a standalone Python script
with no PowerShell parent to dot-source it from and no existing Python binding for that specific
Mutex. Re-implementing a second, parallel named-Mutex primitive in Python just to take the *same*
named lock as the host-test build would add complexity without benefit, since this check's own
tiny two-file build (`pid_fuzzy_drift_harness.c` + the unmodified `pid_fuzzy.c`) never actually
needs to interleave with the host-test build it would otherwise be contending with -- a private
directory removes the collision surface entirely rather than serializing around it. This matches
the same reasoning `build_lock.ps1`'s comment gives for *not* using a fresh directory per run for
the builds it *does* lock: those are large incremental CMake/ninja trees where a fresh directory
would pay a real reconfigure cost every run; this harness is two files compiled directly with `cl`,
so a dedicated directory costs nothing extra.

Also labelled the vswhere non-cause directly in `check_pid_fuzzy_drift.ps1`'s header comment (see
above).

### Negative test (drift check can still fail)

Injected a genuine, real drift into the Python mirror
(`tools/PcTools/src/kilnctrl/fuzzy_band_probe.py`'s `pid_fuzzy_adjust()`), adding `band_e = band_e +
1.0` right after `band_e` is computed, then reran `check_pid_fuzzy_drift.ps1`:

```
PID_FUZZY DRIFT CHECK: FAILED at vector #49
  inputs: error_c=-7.4 error_rate=-1.0 error_band_c=20.0 rate_band_c_per_s=0.5 base=(0.06,0.0003,0.01) strength_pct=25
  C kp=0.0580499955, Python kp=0.05778571428571429, |diff|=0.00026428121428571233 > tolerance 0.0001
```

Exit code 1, confirmed. Restored `fuzzy_band_probe.py` by hand (removed the single injected line;
`git status`/`git diff` on that file show no changes afterward), deleted
`build_pid_fuzzy_drift/`, and reran from a clean build:

```
PID_FUZZY DRIFT CHECK: 804 vectors agreed within tolerance (worst |diff| = 6.100000005115902e-09, tolerance = 0.0001).
```

Exit 0. The check both passes on real code and fails on a genuine drift -- not vacuous.

## `run_all_checks.ps1` / host tests

`build_host_tests.ps1`: 38/38 host test executables built and passed (no adaptive_tune failures).

`tools/run_all_checks.ps1`: 4 of 94 checks FAILED. Three were named in the review as pre-existing
cross-agent collisions, confirmed still failing here (owner files named so those sessions can
clear them):

- `tools\check_coil_power_w_sentinel_guard.ps1` -- `test_zones_http.c` not in its `ALLOWED_FILES`
  (owner: whoever maintains `check_coil_power_w_sentinel_guard.ps1`'s allowlist / `test_zones_http.c`)
- `tools\PcTools\check_zones_per_zone_field_drift.ps1` -- `autotune_baseline_k_dc` missing from the
  PcTools client dict (owner: `tools/PcTools`'s zones HTTP client, per this task's exclusion list)
- `tools\PcTools\selfcheck.py` -- FAILED (owner: `tools/PcTools`)

A **fourth**, not named in the review, is also currently red:
`firmware\KilnFW\App\test\check_sim_iter_tune_bars.ps1` -- `sim_iter_tune.exe`'s A1 bar reports a
pinned known-failure ceiling of <=2.0% design target not met (3.6364% observed, 24/660). This is
unrelated to adaptive_tune_ki.c/the drift check and outside this task's owned files
(`pid_fuzzy.c`/`.h`, `firing_score.*`, `firing_compare.*` and friends are explicitly out of scope
here) -- named so its owning session can pick it up.

None of the four are caused by, or related to, the two fixes in this document.

## Structural findings NOT acted on (per task scope -- recorded here as open items)

- The `ZONE_CONTROL_MODE_PID_FUZZY` guard in `adaptive_tune_refine_ki_locked()` reads
  `control_mode`/`fuzzy_strength_pct` at refine time rather than snapshotting them at capture time.
  Safe only because both writers currently sit behind `ota_http_check_interlocks()`; nothing
  structurally links the two. Not fixed here.
- The same guard fails OPEN if `zones_config_get_control_mode()` returns `false` -- a config-read
  failure lets the correction through rather than blocking it. Not fixed here.

## Files changed

- `firmware/KilnFW/App/drivers/control/adaptive_tune_ki.c` -- reworded the PID_FUZZY refusal
  message to fit the 96-byte buffer.
- `firmware/KilnFW/App/test/test_adaptive_tune_ki_bounds.c` -- strengthened the regression test
  (length assertion + end-of-message token check).
- `firmware/KilnFW/App/test/pid_fuzzy_drift_check.py` -- private build directory instead of the
  shared host-test build tree.
- `firmware/KilnFW/App/test/check_pid_fuzzy_drift.ps1` -- header comment naming the vswhere
  non-cause and the build-dir fix.
