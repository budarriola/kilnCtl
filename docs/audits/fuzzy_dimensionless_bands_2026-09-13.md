# Dimensionless (autotune-derived) fuzzy membership bands (2026-09-13)

## 0. What this serves, and a correction mid-task

**Owner requirement:** the fuzzy controller must not ship with parameters
guessed for, or trained on, anything other than the kiln it is installed on
-- it must derive them from that kiln's own PID autotune result.

`pid_fuzzy.c`'s `ERROR_BAND_C_DEFAULT = 20.0f` and
`RATE_BAND_C_PER_S_DEFAULT = 0.5f` are absolute degC/degC-per-s constants the
file's own header comment admits were sized by desk reasoning about "a
mid-size kiln," never measured or derived from any plant. That fact stands
regardless of anything below.

**What does NOT stand, and was corrected mid-task:** this task's original
brief (and the doc audits it cited) argued the rate band was reachable only
under disturbance because it is "~6x wider than this plant's fastest ramp
(~0.083 degC/s)". An independent opus review
(`docs/audits/review_sim_fuzzy_commits_2026-09-13.md`, `8a12521b`) found
that argument unsupported:

- 0.083 degC/s is `300 degC/hr / 3600` -- an **incidental unit conversion**
  of a commanded profile rate, not a measured plant limit.
- `pid_fuzzy.c`'s own header comment records the **measured** peak rate from
  the one real mode-3 hardware capture
  (`fuzzy_ab_20260904d_s50_run1.jsonl`) as **0.110 degC/s**, 33% higher.
  `ZONE_MAX_RAMP_C_PER_HR_MAX` (`zones_config_json.h`) allows commanded
  ramps up to 1000 degC/hr = 0.278 degC/s, higher still.
- `fuzzy_nine_cell_probe.c`'s reachability test was literally
  `0.5 > 6.0 * 0.083` = `0.5 > 0.498` -- true by a **0.4% margin**.
  Substituting the actual measured 0.110 makes that inequality **false**:
  even the unmodified absolute default bands were never as
  reachability-starved as originally claimed.

This document was rewritten to use the measured 0.110 degC/s figure
throughout and to report reachability as a continuous membership fraction
rather than a brittle "reachable / not reachable" binary. The reachability
motivation is **weaker** than originally stated. The owner requirement
(derive from this kiln's own autotune result, not from guessed constants)
is **unaffected** and is what this pass still serves.

## 1. Derivation

Autotune already produces, per zone, a first-order-plus-dead-time (FOPDT)
model: `model_k_dc` (degC at duty=1.0, steady state), `model_tau_s` (s),
`model_dead_time_s` (s) -- `zone_model_at()`
(`firmware/KilnFW/App/drivers/persist/zones_config_accessors.c:1592`),
already used by the feedforward path
(`profile_executor_feedforward.c`'s `zone_load_model()`).

`pid_fuzzy_derive_bands()` (`pid_fuzzy.c`/`pid_fuzzy.h`) derives both
membership-band half-widths from that model:

```
rate_band_c_per_s = model_k_dc / model_tau_s
error_band_c      = model_k_dc * 0.5
```

**Rate band.** For a FOPDT step response, `dT/dt|t=0+ = k/tau` at a full
(duty=1.0) step -- the fastest rate this plant's own thermal response can
physically produce under full actuation. Units check: degC / s. A genuine
disturbance (stuck-open lid, runaway element) can plausibly approach this
bound; an ordinary commanded ramp, run at well under full duty, should sit
meaningfully below it -- so "large rate" keeps meaning "not a normal
firing", the same intent the absolute default's header comment states, just
derived instead of guessed.

**Error band.** Half of the plant's own full-duty steady-state temperature
rise -- a "large" error scaled to what this zone's own actuator can
correct, rather than an arbitrary degC figure that means something
different on a 40 degC bench rig than on a 1200 degC kiln.

Both fall back to the absolute `ERROR_BAND_C_DEFAULT`/
`RATE_BAND_C_PER_S_DEFAULT` (20.0/0.5) when `model_k_dc`/`model_tau_s` are
not both finite and positive (a zone that has never been autotuned, or a
pathological fit) -- `pid_fuzzy_derive_bands()` returns `false` in that case
so the caller can log it, explicitly, rather than silently running the
unmeasured constants indistinguishably from a deliberate configuration.

Implementation: `pid_fuzzy_derive_bands()` in `pid_fuzzy.c`/`.h`. Wired into
production at `resolve_fuzzy_bands()`
(`firmware/KilnFW/App/drivers/control/profile_executor_pid_tick.c`), called
from `pid_fuzzy_prepare_gains()`. Computed **at use time**, not persisted --
no `ZONES_CFG_VERSION` bump needed, per this task's own scope note.

## 2. Per-zone numbers (live bench board, as supplied for this task)

| zone | model_k_dc | model_tau_s | model_dead_time_s | derived error_band_c | derived rate_band_c_per_s |
|---|---|---|---|---|---|
| z0 | 42.731 | 255.6 | 40.3 | 21.37 | 0.1672 |
| z1 | 32.397 | 258.9 | 31.3 | 16.20 | 0.1251 |
| z2 | 33.849 | 247.1 | 26.0 | 16.92 | 0.1370 |

**Provenance note:** these are the live values supplied in this task's
brief, not independently re-read from the board this session (no board
access was performed for this pass -- sim-first, single-zone, per the
owner's standing constraint).

**Sanity check against the measured envelope.** The derived rate bands
(0.125-0.167 degC/s) sit at 1.1-1.5x the measured peak rate (0.110 degC/s)
-- same order of magnitude, not 6x it the way the absolute default (0.5) is.
This is the honest form of the sanity check this task asked for: the
derived band is close to, and slightly above, the largest rate this plant
has actually been observed to produce -- large enough that an ordinary
ramp does not saturate the membership function, small enough that an
ordinary ramp still moves it meaningfully, which is what a "large rate"
band is supposed to do.

## 3. Reachability, corrected

`fuzzy_nine_cell_probe.c` was corrected to report reachability as a
continuous membership fraction (outer-bucket degree at the measured peak
rate, 0.110 degC/s) instead of a binary threshold, and its printed
reachability verdicts were re-derived against that figure rather than the
incidental 0.083 conversion.

**Against the absolute default (0.5 degC/s):** measured peak rate gives
**22%** outer-bucket membership (`0.110 / 0.5`) -- this number was already
in `pid_fuzzy.c`'s own header comment before this pass, confirming 0.110 is
the right figure to use. Nonzero, partial -- not the "0 of 6, needs a
disturbance" claim originally made.

**Against the derived, per-zone bands:**

| zone | derived rate_band_c_per_s | membership at measured 0.110 degC/s peak |
|---|---|---|
| z0 | 0.1672 | 66% |
| z1 | 0.1251 | 88% |
| z2 | 0.1370 | 80% |

This is the honest headline number: an ordinary firing's own measured peak
rate moves from **22% outer-bucket membership** (absolute default) to
**66-88%** (derived, per zone) -- a real, substantial increase in how much
the rule table actually engages during normal operation, not a binary
"unreachable to reachable" flip, because the rate axis was already
partially engaged under the old bands.

**Per-cell count, both before and after, using this document's own
reachability criterion (>=50% membership at the measured peak = reachable
in normal operation; 10-50% = partially reachable; <10% = disturbance-only):**

- Before (absolute default, 22% membership on the rate axis): the 3 cells
  with `rate_bucket == STEADY` are reachable outright (rate membership is
  effectively 100% near rate=0); the 6 cells with `rate_bucket in
  {FALLING, RISING}` sit at 22% -- **partially reachable**, not fully
  disturbance-only as originally claimed, but not dominantly reachable
  either.
- After (derived bands, 66-88% membership): the same 3 STEADY cells stay
  reachable; the 6 FALLING/RISING cells now cross the 50% threshold on all
  three zones -- **reachable in normal operation** by this criterion.

**Physical-sign caveat (unchanged by the correction):** `error_rate_c_per_s`
is signed as `-d(measurement)/dt`, so a normal climb toward setpoint pairs
POS error with the FALLING bucket (not RISING), and a normal cool-down pairs
NEG error with RISING. Of the 6 non-STEADY-rate cells, only the 3 that pair
physically-correctly with an ordinary approach (POS/FALLING, ZERO/FALLING or
ZERO/RISING depending on direction, NEG/RISING) actually co-occur during
normal tracking; the other 3 (error growing away from target, e.g.
POS/RISING) represent a genuinely worsening trajectory and remain
disturbance/fault-indicating regardless of band width -- which is correct
control-design behaviour, not a shortfall of the derivation.

## 4. Sim comparison (single-zone, sim-first, per the owner's standing
constraint)

`sim_fuzzy_closedloop.c`'s tracking scenario (ordinary ramp/dwell/ramp-down,
zone 0's own measured model, no injected disturbances), rebuilt from a
clean host-test build after this pass's negative test (see §6):

| strength_pct | bands | IAE (degC*s) | MAE (degC) |
|---|---|---|---|
| 0 (control) | n/a | 13749.7 | 3.3052 |
| 25 | derived (19.62 / 0.1488, zone 0) | 13198.3 | 3.1727 |
| 50 | derived (19.62 / 0.1488, zone 0) | 12867.0 | 3.0930 |
| 25 | absolute default (20/0.5), `fbdc5bd0` | 12716.0 | not recorded |
| 50 | absolute default (20/0.5), `fbdc5bd0` | 11796.0 | 2.8356 |

Control (strength 0) reproduces the required baseline exactly
(13749.7 / 3.3052), confirming this build is not stale.

**Derived bands underperform the previously-measured absolute-band
improvement on this metric**, roughly by half: strength 50's MAE
improvement over control is -0.2122 degC with derived bands versus
-0.4696 degC with the absolute default; strength 25's IAE improvement is
-551.4 versus -1033.7. Both bands still improve over control monotonically
with strength; neither beats the other on every measure -- derived bands
lose on this single-zone tracking metric.

**Materiality, stated explicitly per this task's own instruction:** every
one of these differences is far under this project's 0.5 degC MAE
materiality line --

- control vs. absolute-default strength 50: 0.4696 degC (already close to,
  not clearly over, the line)
- control vs. derived strength 50: 0.2122 degC
- derived vs. absolute-default at strength 50: 0.1122 degC

None of these numbers settles anything about hardware behaviour by
themselves (single-zone sim, refuted multi-zone coupling model not
exercised, no hardware A/B exists for either band configuration). The
IAE/MAE comparison is a ranking/regression tool only, exactly as
`sim_fuzzy_closedloop.c`'s own header states.

## 5. Judgement: is the derivation worth making?

**On the reachability motivation alone: no longer strong.** The corrected
analysis in §3 shows the rate axis was already partially engaged (22%
membership) under the absolute default, not fully starved -- the change is
a meaningful but incremental increase (22% -> 66-88%), not an
unreachable-to-reachable fix.

**On the tracking-quality motivation: negative.** §4 shows derived bands
measurably underperform the absolute default on this project's own
single-zone sim ranking tool, though both remain within the sub-0.5 degC
materiality band.

**On the owner's stated requirement, taken on its own terms: yes.**
`ERROR_BAND_C_DEFAULT`/`RATE_BAND_C_PER_S_DEFAULT` remain constants "sized
by desk reasoning about a mid-size kiln," never measured on any plant, and
the owner's requirement is specifically that no shipped parameter be
guessed for, or trained on, a kiln other than the one it runs on. A
derivation from this zone's own autotune-identified `model_k_dc`/
`model_tau_s` satisfies that requirement directly, is dimensionally sound
(both formulas check out in degC and degC/s respectively), falls back
explicitly (logged) rather than silently for a never-autotuned zone, and
preserves every existing safety/multiplier contract exactly (§6). It is
shipped as a preference over the absolute constants, which remain in place,
unremoved, as the documented last-resort fallback.

**Conclusion:** the constants were unjustified and are now derived from a
sound, dimensionally-checked formula tied to this kiln's own autotune
result -- satisfying the owner's requirement on its own terms. The
originally-claimed reachability improvement was overstated (corrected in
§3) and the measurable tracking-quality effect is a small **regression**
on this project's own sim ranking tool, not an improvement -- both facts
are reported here plainly rather than the overstated version this task
started with. Recommend keeping the change (it is what the owner asked
for and does not make anything worse outside noise/materiality), while not
representing it as a tracking-quality win.

## 6. Contracts preserved

- `strength_pct == 0` still reproduces base PID gains bit-for-bit --
  `pid_fuzzy_derive_bands()` changes what bands are computed, never how
  `pid_fuzzy_adjust()`'s own strength=0 short-circuit behaves. Verified by
  the existing `test_fuzzy_prepare_gains_zero_strength_is_base_gains_bit_
  exact` (`test_profile_executor_prestart.c`) and `sim_fuzzy_closedloop.c`'s
  own bit-for-bit gate, both still passing.
- Centre-cell multiplier contract (`strength_pct=50` gives kp x0.75, ki
  x1.25, kd x0.75) is a property of `RULE_TABLE`/`MAX_NUDGE_FRACTION`, not
  of the band width -- unaffected by this change, still asserted exactly
  by `fuzzy_nine_cell_probe.c`.
- `test_profile_executor_prestart.c`'s `zones_config_get_model()` stub
  returns `false` (no model) by default, so every pre-existing test in that
  file that never explicitly configures a model sees the unchanged
  absolute-default-via-config path -- including
  `test_fuzzy_prepare_gains_matches_pid_fuzzy_adjust_directly`, which
  asserts production output equals a direct `pid_fuzzy_adjust()` call with
  literal 20.0/0.5 bands. Confirmed unchanged (all 37/38 -- see below for
  the 38th -- host-test executables still pass).
- `fuzzy_gain_mirror_drift_check.py` (compares `pid_fuzzy_prepare_gains()`
  statement-for-statement against `test_closed_loop.c`'s hand-written
  mirror `fuzzy_tick()`): the new band-resolution logic was pulled into a
  separate function, `resolve_fuzzy_bands()`, called as a single statement
  from `pid_fuzzy_prepare_gains()`, and the check's fold/strip rules
  updated to drop that one call (the mirror has no `zone_runtime_t`/model
  concept, the same structurally-required difference the check already
  handles for `strength_pct`'s config-getter resolution). Confirmed passing
  (`FUZZY-GAIN MIRROR DRIFT CHECK: OK`).

## 7. Negative test

Broke production code: `pid_fuzzy.c`'s `ERROR_BAND_K_FRACTION` changed from
`0.5f` to `5.0f` (a 10x error). Rebuilt from a **clean** host-test build
(`rm -rf firmware/KilnFW/App/test/build` first, per the corrected-process
point below) and confirmed `test_pid_fuzzy.c`'s new assertion failed:

```
FAIL C:\...\test_pid_fuzzy.c:492: error band == k_dc * 0.5 exactly
```

Restored by hand (reversed the one-line edit, not `git checkout --`/
`git restore`). `git diff` on `pid_fuzzy.c` after restoration shows only
this pass's additions (no leftover sabotage). **Then rebuilt from a clean
build directory a second time** (not merely re-checked an empty diff) and
confirmed 37/37 (later 38/38 once `check_fuzzy_gain_mirror_drift.ps1`'s
companion fix landed) host-test executables build and pass, and
`tools/run_all_checks.ps1` reports 94/94.

This rebuild-after-restore step matters specifically because of a
documented process failure this same task area hit earlier the same day
(`docs/audits/review_sim_fuzzy_commits_2026-09-13.md`): a prior pass's
negative test left a stale, still-sabotaged `.exe` in place after an
empty-`git-diff` restoration, and that stale binary produced several wrong
downstream numbers before the mistake was caught. Every number in §4 and
§3 of this document was captured after a `rm -rf` + full rebuild, not
merely a re-run of a possibly-stale binary.

## 8. Checks

- Host tests: `firmware/KilnFW/App/test/build_host_tests.ps1` -- 38/38
  executables built and passed (via PowerShell, ESP-IDF/MSVC toolchain).
- Target build: `idf.py -C firmware/KilnFW build` (via the ESP-IDF
  PowerShell profile) -- succeeds, `KilnCtrl.bin` 0x227730 bytes, 28% free
  in the smallest app partition.
- `tools/run_all_checks.ps1`: 94/94 passed, 0 failed.
- No board was flashed; no heating run was performed.

## Files touched

- `firmware/KilnFW/App/drivers/control/pid_fuzzy.h` /
  `pid_fuzzy.c` -- `pid_fuzzy_derive_bands()`.
- `firmware/KilnFW/App/drivers/control/profile_executor_pid_tick.c` --
  `resolve_fuzzy_bands()`, wired into `pid_fuzzy_prepare_gains()`.
- `firmware/KilnFW/App/test/test_pid_fuzzy.c` -- unit tests for
  `pid_fuzzy_derive_bands()` (valid model, three live zones, no-model
  fallback, non-finite/negative/zero-tau defence).
- `firmware/KilnFW/App/test/fuzzy_nine_cell_probe.c` -- corrected
  reachability figure (0.110 measured, not 0.083 incidental), per-zone
  derived-band reachability report.
- `firmware/KilnFW/App/test/sim_fuzzy_closedloop.c` -- bands derived from
  zone 0's own model instead of hardcoded absolute defaults.
- `firmware/KilnFW/App/test/fuzzy_gain_mirror_drift_check.py` -- fold rule
  for the new `resolve_fuzzy_bands()` call site.
