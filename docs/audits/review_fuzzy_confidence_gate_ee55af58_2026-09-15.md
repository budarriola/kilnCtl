# Adversarial review: `ee55af58`, the fuzzy confidence gate (2026-09-15)

Scope: commit `ee55af58` ("adaptive fuzzy: sec 3 confidence gate"), which
implements `docs/ADAPTIVE_FUZZY_EVALUATION.md` (`5387ff52`) section 3
only. Reviewed against that plan, against the fixed-arm verdict the feature
must overturn (`scenario_factorial_results_2026-09-14.md` + the opus review
`e2245b8d`), and against this repo's own standing bug classes.

No board was flashed. No heating run was performed. No `debug_*` tool was
called. No `.kicad_*` file and no builtin schedule value was touched.
`sim_factorial_driver.c`, the `adaptive_fuzzy_evaluation_progress_2026-09-14.md`
progress doc, `kiln_cfg_store.c` and `kiln_package.c` are other sessions'
live work and were not edited.

**Verdict: the gate is correctly wired, correctly locked, and bit-exact at
zero authority — and its two load-bearing mechanisms are both broken in ways
that make the section 8 campaign uninterpretable if it runs on this as-is.**
Four findings are blocking. Nothing here is a reason to keep or remove fuzzy;
they are reasons the instrument cannot currently measure it.

---

## What was actually verified, and how

| Check | Result |
|---|---|
| `tools/run_all_checks.ps1 -ExecutionPolicy Bypass` (foreground) | **94 passed, 0 skipped, 0 failed**, exit 0 |
| KilnFW **target** build (`build_kilnfw`, the implementer relied only on the check script for this) | **OK in 76.4 s** |
| Host tests, fresh output directory, restored source | **45/45 built and passed** |
| Negative tests NT1/NT3/NT4/NT5 | all four fail loudly; details below |
| Restoration | by hand (reverse edit), **empty `git diff`**, then a forced full rebuild from a fresh output directory before trusting green |

A transient `test_zones_http.c` failure (`nvs_save()` must dispatch
`kiln_cfg_store_autosave_from_live()`) appeared mid-session and is **another
agent's in-flight `kiln_cfg_store.c` work**, not attributable to `ee55af58`;
it was absent from this session's baseline and absent again from the final
fresh rebuild.

---

## BLOCKING findings

### F1. The confidence counter is a band-pass on model **disagreement**, and is guaranteed LOW on a converged model

This is the most consequential defect and it is not visible from
`adaptive_tune.c` alone. `c` rises only when

```c
if (model_refined && fabsf(z->last_delta_pct) <= 10.0f)   // adaptive_tune.c:770
```

Both halves have to be read against `adaptive_tune_model.c`:

- `last_delta_pct` is **the blended move, not the fit's disagreement**:
  `k_blended = k_dc + 0.15f * (k_fit - k_dc)` (`ADAPTIVE_TUNE_BLEND_ALPHA`,
  `adaptive_tune_internal.h:210`), and
  `last_delta_pct = (k_blended - k_dc)/k_dc * 100` (`adaptive_tune_model.c:244`).
  So `|delta| <= 10 %` admits a raw fit that disagrees with the standing
  model by up to **66.7 %**. It is a property of the smoother, not of the fit.
- `model_refined` is **false** when the move is immaterial:
  `material_move < k_dc * 0.005f` refuses the refinement outright
  (`adaptive_tune_model.c:183-187`). Undoing the same 0.15 blend, that is a
  raw fit agreeing with the standing model to within **3.33 %**.

Combined, `c` increments only for raw-fit relative error in
**[3.33 %, 66.7 %]**, and floors to 0 on both sides of that window. The
consequences:

- **A perfectly converged zone floors `c` to 0 and turns fuzzy off, permanently.**
  Once a well-identified zone settles into sub-3.33 % fits, `model_refined`
  is false every run, `c` is floored every run, and fuzzy never runs again on
  that zone — for exactly the reason it was supposed to be trusted.
- A persistently, *consistently* wrong model — a systematic identification
  bias that reproduces every firing — sits comfortably inside the window and
  earns `c = 4`, i.e. maximum confidence.

This directly inverts plan §3.1's signal 2 ("coefficient of variation of the
last K accepted raw `K_fit` values **narrowing rather than wandering**"). The
commit discloses the substitution as "a scope-limited proxy"; it is not
scope-limited, it is **anti-correlated with model quality at the good end**.

**Answering the review question as asked:** the proxy has *not* reintroduced a
model-quality dependence into `cap_L` — the two are genuinely multiplied,
`cap_L` reads `z->ff_dead_time_s`/`ff_tau_s` and nothing else, and
`pid_fuzzy_confidence_strength_pct(4, 0.0f) == 0` is tested. The plan's
central structural claim survives. But the proxy *does* decide **where** fuzzy
runs, and it selects for the model-matched-but-still-moving population — a
subset of the `A6 = MATCHED` cells that contain the seven known limit cycles.
That is a weaker version of the same hazard, arriving by a different door.

*Recommended fix (a plan-level decision, so reported not applied):* treat the
`MIN_MATERIAL_MOVE_FRAC` refusal **specifically** as agreement (a good run),
distinct from every other refusal reason, rather than as a floor. That needs
`adaptive_tune_refine_zone_locked()` to return a reason, not a bool. Anything
less leaves the inversion in place.

### F2. The N3 oscillation threshold is not defensible, and the measured data says so

The §5 agent's factorial run (`84430e35`) reports `oscillation_tripped = yes`
on **2314 of 4680** adaptive firings (~49 %), while only **84** firings ever
had a tick at non-zero strength. Those two numbers together are decisive:
**essentially every trip occurred in a firing where fuzzy was never active.**
The detector is not detecting fuzzy-induced limit cycles; it is firing on
ordinary PID control, in a **noiseless** simulator. Since the detector ticks
in both adaptive arms (which is what makes firing 1 of `A_FUZZY_AT`
bit-identical to `A_PID_AT`), a trip is by construction not evidence about
fuzzy at all.

Three root causes, all in `pid_fuzzy_confidence.c`:

1. **No amplitude deadband.** A ±0.001 °C sign flip counts exactly as much as
   a ±5 °C one. On hardware, a zone holding setpoint *well* sits with error
   hovering across zero; with MAX31856 noise at 1 Hz, four crossings in
   600 samples is not a limit cycle, it is a **successful dwell**. The
   file's own comment — "settling dwells commonly cross zero 0-1 times from
   measurement noise near the setpoint" — is wrong, and it is the load-bearing
   justification for the number 4.
2. **Undisclosed divergence: the dwell restriction was dropped.** Plan §3.2
   says "count error zero-crossings over a rolling window **during a dwell**".
   The implementation ticks on every `pid_fuzzy_prepare_gains()` call — ramps,
   approach, and dwell alike. Approach overshoot legitimately crosses zero
   1-3 times, and a tumbling window happily accumulates those alongside
   unrelated noise crossings from an adjacent phase.
3. **4 in 600 samples is very tight** for a 1 Hz tick. The plan's 29-vs-0
   margin was measured on the *whole oscillating episode*, not per 600 s
   window, and in a noiseless plant. The margin the header cites does not
   exist at this granularity.

The tumbling-vs-sliding disclosure is fine in the direction it argues (a real
limit cycle cannot hide by straddling a boundary). It is worse in the
direction that actually matters here: tumbling lets unrelated crossings from
different control phases pool into one count.

*Recommended fix:* require `|error_c|` to exceed a fraction of the zone's own
`error_band_c` on **each** side before a crossing counts, restrict counting to
dwell, and then re-derive the threshold from the measured per-window
distribution rather than choosing it.

### F3. Plan §7 gate 3 is structurally unreachable, and criterion 4 will remove the feature for the wrong reason

Gate 3 requires **zero** dwell error zero-crossings in `A_FUZZY_AT` across all
nine firings on the seven pinned cells; criterion 4 makes any crossing
removal-triggering *independently of every tally*.

`PID_FUZZY_OSCILLATION_TRIP_CROSSINGS` is **4**. A backstop that works
perfectly must therefore record four crossings before it can act — failing
gate 3 by construction. And per F2, crossings are already being recorded in
the plain-PID arm, where there is no fuzzy to blame.

As written, §7 gate 3 cannot distinguish "the backstop worked" from "the
backstop was never needed" from "the plant crossed zero". **This must be
restated before §8 runs** — e.g. "no *sustained* limit cycle: crossings
bounded and the backstop tripped, measured against `A_PID_AT`'s own crossing
count on the same cell as the control". Escalated, not fixed: §7/§8 are the
plan owner's.

### F4. `cap_L` does not close in the cells it exists to close

Plan §6 scales sensor dead time and tau **together** for kiln-span cells
(40.3 → 76.9 s, 255.6 → 488 s). Both regimes therefore land at the same
ratio:

```
bench: 40.3 / 255.6 = 0.158       kiln: 76.9 / 488 = 0.158
cap_L(0.158) = 1 - (0.158-0.10)/0.20 = 0.71
gated strength = round(50 * 1.0 * 0.71) = 35
```

So in the seven known limit-cycling cells `cap_L` reduces authority from 50 to
**35** — about 30 % — rather than to zero. The limit cycle was measured
*monotone in dead time* at strength 50; nothing establishes that 35 stops it.
The plan's own 0.083 figure mixes a bench dead time with a kiln tau and is
superseded by its §6 correction.

Consequence: the anti-limit-cycle claim rests **entirely on N3**, which F2 and
F3 undermine. `cap_L`'s 0.10/0.30 breakpoints are honestly labelled chosen-not-
measured throughout the code, and the factorial is the right instrument to
place them — but they are currently placed such that the primary mechanism
does not engage where the plan says it must.

---

## MEDIUM findings

### F5. Enabling fuzzy now silently requires adaptive tune to be enabled

`c` is sourced **only** from `adaptive_tune_run_end()`, which floors it for any
zone hitting a skip reason — the first of which is `!z->enabled`, "zone not
opted into adaptive tuning" (`adaptive_tune.c:660`). On a zone with adaptive
tune off, `c` is 0 forever, so `gated_strength_pct` is 0 forever, so **fuzzy
never runs at all**, regardless of model, configuration, or operator intent.

Before this commit, fuzzy ran on any zone with an identified model. This is an
owner-visible behavioural change that the commit message does not name. It may
well be the intended posture (§2's bootstrap-at-zero), but it should be a
stated decision, not a side effect of where `c` happens to live.

### F6. `c` is RAM-only per boot — firing 1 after any reboot always runs plain PID

Correct and deliberate per plan §2 (documented at
`adaptive_tune_internal.h:515`). Worth stating as an operational consequence:
a power cycle mid-campaign silently resets earned authority to zero, and
nothing surfaces that.

### F7. `S_MAX_PCT = 50` is both correct-per-plan **and** an unannounced operator regression

Plan §3.2 fixes `S_MAX = 50`, so 50 as a ceiling is right. But the composition
is `min(configured, gated)` and `gated <= 50` always, so an operator who
configures 100 gets **50**, with no log line, no status field, and no API
surface saying so. The implementer flagged the ceiling; nothing flags it to the
operator. Both halves of the review question are true.

### F8. The gate is completely unobservable on hardware

The pre-existing no-model path logs once per zone per boot
(`log_fuzzy_disabled_no_model_once()`) precisely so an operator can tell fuzzy
is inactive and why. The new gate — which can zero fuzzy for three separate
reasons (`cap_L == 0`, `c == 0`, oscillation tripped) — logs nothing and
exposes nothing. Plan §7 requires `strength_pct_realised` / `confidence_c` /
`cap_L` / `oscillation_tripped` per row, but that is harness-side; on a real
board there is no way to answer "why did fuzzy go quiet". A one-shot log on
each transition plus a `/api` field would close it. Not added here: the tick
path is being exercised right now by another session's campaign.

---

## LOW findings / notes

- **F9.** `if (c == 0 || cl <= 0.0f) return 0;` is arithmetically redundant —
  the formula below already returns 0 in both cases. Flipping `||` to `&&`
  (NT2) changed no behaviour and failed no test. Harmless belt-and-braces, but
  its "deliberate AND" comment implies a load-bearing guard that it is not.
- **F10.** No test covers the tumbling-window reset (crossings cleared at
  600 s), and none covers `pid_fuzzy_oscillation_reset()`'s **call site** in
  `profile_executor_run.c` — only the module function is tested. Given that
  the file's own header names the reset-one-side-of-a-pair class as the reason
  that call exists, the call site deserves a test.
- **F11.** `fuzzy_gain_mirror_drift_check.py` drops all seven gate statements
  via `PROD_ONLY_STMT_RES`. That is the correct treatment (the mirror has no
  `zone_runtime_t`/`adaptive_tune` concept), but it should be said plainly:
  **that check provides zero coverage of the gate.** The host tests do (NT4).

---

## Verified sound

### V1. Lock ordering and producer-under-lock (review item 6) — clean

- `pid_fuzzy_prepare_gains()` runs **inside** `s_exec.lock` (taken
  `profile_executor.c:332`, released `:1560`) and takes `adaptive_tune_lock`.
  That is `s_exec.lock` → `s_at.lock`, the sanctioned order.
- The precedent is pre-existing and unchanged:
  `adaptive_tune_get_enabled(zi)` is already called two lines earlier in the
  same tick and takes the same lock.
- Both new accessors bounds-check, take the lock, read/write **one `uint8_t`**,
  release. No producer call, no blocking call, no logging, no allocation under
  the lock. `dashboard_get_status()`-class hazards do not apply.
- Both `adaptive_tune_run_end()` call sites are **outside** `s_exec.lock`
  (`profile_executor.c:380`, after the give at `:371`; and
  `profile_executor_status.c:92`, after the give at `:82`). No reverse order
  exists anywhere.

**Correction to the prior review `59fce1ce`.** Its premise — that
`adaptive_tune_run_end` is "NOT in the executor path" — does not hold:
`profile_executor.c:380` calls it directly from `executor_task_entry()`, and
`profile_executor_halt()` (which `profiles_stop()` reaches) calls it too. That
is a pre-existing correction to the record, not something `ee55af58`
introduced. **Its conclusion nevertheless still holds for this commit:** the
delta inside `run_end` is two plain `uint8_t` assignments — no new stack, no
new call, no new lock, no new allocation — so it adds nothing to the open
`profile_executor` / `profiles_stop()` panic's risk surface.

### V2. Bit-identity at zero authority (review item 5) — holds through the real production path

- `pid_fuzzy_adjust()` **early-returns** at `strength_pct == 0`
  (`pid_fuzzy.c:215`), assigning the sanitized base gains directly. There is no
  multiply-by-1.0 and no add-of-0.0 anywhere on that path, so this is exact,
  not merely close. (`sanitize_base()` is the identity for every legitimate
  gain; it exists to stop a NaN base gain leaking through the *safest* setting.)
- The one thing the fuzzy path does that `ZONE_CONTROL_MODE_PID` does not is
  call `pid_rescale_integral_for_new_ki()`. That is a **hard no-op** here:
  it early-returns on `old_ki == new_ki` and on `!(old_ki > 0.0f)`
  (`pid.c:65`), which covers both the first tick (`fuzzy_prev_effective_ki`
  starts at `0.0f`) and every tick thereafter (`adj_ki == base_ki` exactly).

So plan §7 gate 2 (`A_FUZZY_AT` firing 1 bit-identical to `A_PID_AT` firing 1)
is structurally sound, and the §5 agent's observation that it holds in the
harness is consistent with the code rather than coincidental.

### V3. Consumer-without-producer (review item 7) — producer confirmed, default is the safe one

`z->ff_dead_time_s` and `z->ff_tau_s` have exactly one writer,
`zone_load_model()` (`profile_executor_feedforward.c:99-102`), fed by
`zone_model_at()` → `zones_config_get_model()` → `autotune_engine_step_identify.c`.
That writer requires **all three** of `k_dc`/`tau_s`/`dead_time_s` to be finite
and `> 0.0f`, and otherwise writes `0.0f` to all three.

On a board that has never autotuned: `ff_tau_s == 0.0f` →
`pid_fuzzy_confidence_cap_l()` hits `!(tau_s > 0.0f)` → returns `0.0f` →
gated strength 0 → **fuzzy fully off**. That is the safe default, and it is
belt-and-braces with the pre-existing no-model path, which independently
forces `strength_pct = 0` and logs once. The "cap 0 vs cap full" question has
the safe answer, and by two independent mechanisms.

The commit's claim that the ramp-transient-identification module's
start-temperature-as-ambient defect cannot feed this gate checks out: that
module is not in this write path.

### V4. The five modified pre-existing tests (review item 8) — not blunted

`grant_full_fuzzy_confidence()` sets `ff_dead_time_s = 1.0f` / `ff_tau_s = 500.0f`
(L/tau = 0.002) and `fuzzy_confidence_c = MAX_C`. It grants the gate's two new
preconditions and **nothing else** — it does not bypass `S_MAX_PCT`, and every
affected assertion was correspondingly re-pointed from a direct
`pid_fuzzy_adjust(..., 100, ...)` comparison to `..., PID_FUZZY_CONFIDENCE_S_MAX_PCT, ...`.
All comparisons remain exact equality. The discriminating one
(`..._uses_zone_commanded_setpoint_when_capped`) still separates `expect` from
`wrong` by `> 0.01` at strength 50, so it still tests what its name claims.
`reset_fuzzy_gain_test_state()` zeroing `adaptive_tune_zones[]` is the right
default (bootstrap-at-zero) rather than a convenient one.

### V5. Ceiling-only composition (review item 1) — the right reading

The plan's literal `strength = round(S_MAX * (c/4) * cap_L)` is an
**assignment**, which would override an operator's deliberate `0` *upward* and
would fight `harvest_freeze`, the no-model gate and the NaN-strength gate, all
of which zero `strength_pct` immediately above it. `min()` is strictly safer
and composes correctly with all three. Ceiling-only is correct. Its
unanticipated side effects are F5, F7 and F8.

### V6. Negative tests — every new test's coverage is real

Each break was made in **production** code, one at a time, then hand-restored
(reverse edit; never `git checkout --`/`restore`/`stash`, since other sessions
hold live work in this tree).

| # | Production break | Result |
|---|---|---|
| NT1 | `PID_FUZZY_OSCILLATION_TRIP_CROSSINGS` 4 → 100 | 5 assertions fail in `test_pid_fuzzy_confidence.c` **and 4 in `test_profile_executor_prestart.c`** (the real production path) |
| NT2 | `c == 0 \|\| cl <= 0` → `&&` | **no failure** — arithmetically redundant, see F9 |
| NT3 | `cap_l()` returns `1.0f` instead of `0.0f` above `L/tau = 0.30` (the central design point) | 2 assertions fail |
| NT4 | delete the production `min()` in `pid_fuzzy_prepare_gains()` | 7 assertions fail across 3 prestart tests |
| NT5 | delete `pid_rescale_integral_for_new_ki()` from the production function | `fuzzy_gain_mirror_drift_check.py` exits **1** with a correct first-divergent-line diagnosis |

NT5 is the important one for the "a negative test on a mirror is vacuous"
rule: the check was broken from the **production** side, not from the
test-local mirror, and it failed. The check is non-vacuous — within the
statement set it compares, which per F11 excludes the gate itself.

After restoration: `git diff` empty on all three touched production files,
then a **full rebuild into a fresh output directory** — 45/45 built and passed,
and the mirror check back to OK. No prebuilt binary was measured.

---

## Is the gate inert? (review item 11) — yes, and it is the gate, not only the geometry

The §5 agent measures `confidence_c == 0` on **4486 of 4680** adaptive
firings, with only 84 firings ever reaching non-zero strength. Separating the
causes, because they are separable:

- **Geometry (the harness).** Factorial dwell is `6*tau` where
  `sim_scenarios_adaptive.c` needed `16*tau` before `adaptive_tune` harvests,
  so only 218/4680 firings refined at all. That is the §5 agent's harness and
  was deliberately left unchanged to preserve the byte-identical inertness
  proof for the three original arms.
- **The gate itself.** Even given a refinement, F1's band-pass floors `c` on
  any converged zone, and F5 means a zone without adaptive tune enabled can
  never earn any confidence at all. Both are properties of `ee55af58` and
  would persist under **any** dwell geometry.
- **Not `cap_L`.** At the factorial's `L/tau ≈ 0.158`, `cap_L ≈ 0.71` →
  strength 35, non-zero across essentially the whole cell space. `cap_L` is
  not what makes the feature inert — and per F4, that is its own problem.

So, plainly, per the plan's own §9 escalation rule rather than as a
celebration: **if §8 runs on `ee55af58` as committed, adaptive fuzzy will pass
its removal criteria mainly by never engaging** — and it will do so for a
reason (F1) that is a defect, not a design choice. Reporting it as "survives,
but indistinguishable from plain adaptive PID" would credit the feature for
inertness it did not earn honestly.

**Recommendation: settle F1, F2 and F3 before running §8.** F4 should be
re-decided with them, since it determines whether `cap_L` or N3 is the
mechanism the campaign is actually testing.

---

## What I could not verify

- **Whether 35 % authority limit-cycles** in the seven pinned cells (F4). That
  needs the factorial, which is §8 and not mine.
- **The hardware false-positive rate for N3** (F2). The 49 % figure is
  simulation. The hardware argument — that a settled dwell with real
  thermocouple noise crosses zero far more than "0-1 times" — is reasoning from
  the sensor, not a measurement; no heating run was permitted or performed.
- **Which span the seven pinned cells sit in.** F4's arithmetic gives the same
  0.158 either way under §6 scaling, so the conclusion does not depend on it,
  but I did not confirm the cells' factor assignments directly.
- **The `build_kilnfw` result carried a `[STALE MCP SERVER]` banner** (4 files
  changed on disk since that server process started). The banner is about the
  server's own source freshness; the build itself ran `idf.py` against the
  current tree and returned OK. Not independently re-run after a restart.
