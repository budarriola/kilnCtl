# CT sampling rate / mains-cycle filtering review — 2026-09-18

**Verdict: the requested change was NOT implemented.** The premise it rests
on — that the RP2040's 16-sample back-to-back ADC burst is aliasing a raw
60 Hz (or 50 Hz) sine wave at a random phase — is refuted, with numbers, by
the circuit this firmware actually samples. Implementing "100 Hz sampling,
300 ms mains-cycle-locked averaging" would add real complexity (and, per the
owner's follow-up message, real non-blocking/ISR-accumulator complexity) to
solve a problem this hardware's analog front end does not have, while
leaving the actual, still-unexplained anomaly untouched.

## 1. The task's hypothesis, and why the hardware contradicts it

The task frames the ~9.3-count idle std on the fitted channel as **phase
aliasing of a raw AC current waveform**: "16 back-to-back RP2040 conversions
complete in tens of microseconds — a tiny slice of a 16.67 ms mains cycle.
Each pass therefore samples the CT's AC waveform at an essentially random
phase."

`firmware/SaftyFW/docs/CURRENT_SENSE.md` §1 and §3 describe the actual
front end (`hardware/mainBoard/output/kiln.pdf` p.4, U8 AD8542, channels 1-3
identical):

- **U8A is an inverting precision half-wave rectifier** ("superdiode"): the
  CT's AC input is rectified before it ever reaches the ADC.
- **R77 (1 MΩ) ∥ C57 (1 µF) is a peak hold, τ = R·C = 1.0 s.** It recharges
  fast (through D14, on every negative half-cycle) and decays slowly
  (only through R77) between recharges.
- §3's own dynamics table: "Rise... < 10 ms. Fall... τ = 1 s." §4: "Because
  the front end has already done the demodulation, the sampler is simple...
  **The ADC does not see a current waveform. It sees a rectified peak
  envelope.**"

So the ADC input by design is *not* the 60 Hz sinusoid the task's hypothesis
assumes. A 16-sample burst lasting tens of microseconds samples one point on
a slowly-decaying (τ = 1 s) envelope, not an instant of a fast AC waveform —
there is no sine-wave phase for a microsecond-scale burst to alias against.
This closes the specific mechanism named in the task ("samples the CT's AC
waveform at an essentially random phase").

## 2. The next candidate — envelope recharge-phase aliasing — also fails, quantitatively

A more charitable reading of the hypothesis: even though the ADC sees an
envelope rather than a sine, the envelope still re-peaks once per
half-mains-cycle (8.333 ms at 60 Hz, 10 ms at 50 Hz) and decays in between.
If the 50 ms task period (`SAFTYFW_PERIOD_CURRENT_TASK_MS`,
`firmware/SaftyFW/src/task_priorities.h:78`) isn't phase-locked to the mains
half-cycle, each pass's burst could land at a different point in that
decay, and *that* phase could show up as noise.

Bound the maximum size of that effect from the RC values alone, no
measurement needed:

```
ΔV/V_peak ≈ Δt / τ           (small-Δt approximation of 1 - e^(-Δt/τ))
Δt = 8.333 ms (60 Hz half-cycle, the shorter/worse case)
τ  = 1.0 s
ΔV/V_peak ≈ 8.333e-3 / 1.0 = 0.833 %
```

That is the *entire* decay budget available between recharges, as a
fraction of whatever the channel's peak level is *at that moment* — not of
full scale. Channel 2's measured idle level today is mean 105 counts
(std 9.329, min 89, max 138 — the numbers this task supplied). Applying the
0.833 % bound to that actual signal level:

```
0.00833 * 105 counts ≈ 0.87 counts
```

Even taking the most generous possible case — the channel pinned at full
scale (4095 counts, which it plainly is not) — the bound is
`0.00833 * 4095 ≈ 34 counts`, still short of the measured 49-count
peak-to-peak spread (min 89, max 138), and the realistic bound at the
*actual* observed signal level is under 1 count: **roughly a factor of 10
below the measured std, and more than 50× below it at the actual (not
full-scale) signal level.**

So the envelope-recharge-phase mechanism cannot produce the observed
9.3-count std either. Both readings of the task's aliasing hypothesis are
refuted by the RC time constant that is already in the circuit, independent
of any new measurement — this follows from R77/C57's values alone (§1) and
the numbers already supplied in the task.

## 3. What the 9.3-count anomaly actually is (and isn't)

For comparison, the two channels with no CT fitted read essentially flat:
ch0 std 0.180, ch1 std 0.223 (744 samples / 180.2 s, all three channels,
relays off) — consistent with ordinary ADC/op-amp noise on the unregulated
`3.3v_Safty` rail (CURRENT_SENSE.md §4's "RP2040 ADC errata" section already
flags this rail as "unregulated-for-precision"). Channel 2's std is **~40-50×**
that floor, on the one channel with a CT lead physically connected.

That points at the CT lead itself (an unshielded ~3.5 mm-jack cable) as the
noise entry point — capacitive/inductive pickup ahead of the rectifier,
which the rectifier+peak-hold then faithfully processes as if it were a real
CT signal — rather than at anything about *when* within a mains cycle the
ADC burst happens to land. That is a broadband-noise problem, not a
periodic-aliasing problem, and it does not respond specially to an
integer-mains-cycle averaging window the way true aliasing would; it
responds to averaging over more independent samples in general (ordinary
`1/√N` variance reduction), whatever the window boundary happens to be.
Root-causing *that* needs a bench measurement (an oscilloscope on the CT
lead, or a controlled load test) that this review — no board access, no
flashing, per the task's own constraints — cannot perform, and it is a
hardware/wiring question, not a sampling-algorithm one.

## 4. Why no code change follows from this

The task's own instruction: *"VERIFY this hypothesis before building on it;
if the evidence says otherwise, report that instead and stop."* Section 2
above is that verification, using only the RC values already in
`CURRENT_SENSE.md` §1 and the counts already measured and supplied with the
task — no new hardware access was needed or used.

Implementing "100 Hz sampling averaged over an integer number of mains
cycles" regardless would:

- Contradict `CURRENT_SENSE.md` §4's explicit, reasoned "No DMA, no
  free-running capture, no FFT, no RMS accumulator" for a benefit that the
  math in §2 shows does not exist for this front end — exactly the "written
  spec, deliberately violated and left stale" failure mode this repo's own
  CLAUDE.md calls out.
- Add real complexity for nothing: per the owner's follow-up message, doing
  this properly would require a non-blocking, interrupt/timer-driven
  accumulator (a long busy-wait burst is explicitly disallowed), a
  bounded/timed-out ADC read path, and a validity/staleness flag on a
  partial window — all real safety-processor engineering cost, spent on a
  mechanism that section 2 shows cannot be responsible for the measured
  noise.
- Not actually fix the ~23 mA / noise-floor problem this task cites as the
  reason for the change (`ZONE_SWEEP_NORMAL_NOISE_FLOOR_A` = 0.045 A vs. a
  ~23 mA per-zone bench signal): if the noise is broadband pickup on the CT
  lead rather than mains-cycle aliasing, a mains-cycle-locked window helps
  by ordinary averaging (same as any other averaging window of the same
  length would), and does *not* uniquely "cancel" anything the way the task
  describes — so the 100 Hz/300 ms design is not even the right *general*
  averaging window to reach for once the aliasing story is gone; a plain
  longer low-pass (e.g. a larger `CS_FILTER_TAU_S`, unchanged sample method)
  would be the more honestly-justified next step, and even that requires a
  real hardware noise measurement to size correctly, which is explicitly
  the kind of thing this review must not fabricate.

No firmware, guard, config-schema, or NVS-key change was made.
`ZONE_SWEEP_NORMAL_NOISE_FLOOR_A` and all guard thresholds are untouched, as
required. `current_sense.c`, `current_sense.h`, `task_priorities.h`, and
this doc's own §4 sampling section are all unchanged.

## 5. On the owner's non-blocking follow-up

The owner's follow-up message (received while this review was in progress)
added hard non-blocking/bounded-read/degrade-safely acceptance criteria for
*if* the sampling scheme changed. Those constraints were not exercised
here, because §2's refutation means the change they were guarding against
was never made. They are recorded here so a future session that *does* find
a real, numerically-justified reason to change this module's sampling
(e.g. a bench measurement that pins the CT-lead-pickup hypothesis in §3 and
motivates a real fix) does not have to re-derive them:

- No busy-wait/spin across a mains cycle or an averaging window; any
  multi-sample accumulation must be spread across scheduler ticks or driven
  by a timer/interrupt, not a blocking loop in `current_task`.
- No blocking call while holding `current_sense.c`'s module state lock;
  accumulate under a short critical section, do arithmetic outside it.
- Every ADC read must be bounded/timed-out, with an explicit, stated bound,
  and a defined degraded-reading behavior on timeout (not an indefinite
  wait).
- A partial/aborted window must publish a validity/staleness signal, never
  be indistinguishable from a complete one, and guard-path callers must not
  be able to mistake one for the other.
- The worst-case time in the acquisition path and worst-case
  interrupts-disabled time must be stated with numbers.

## 6. What would actually move this forward

1. A real bench measurement of the CT lead's pickup (scope on the lead with
   the CT disconnected from the ADC input but still wired, or a controlled
   short/open-circuit test) to test the §3 hypothesis directly, rather than
   inferring it from the two existing sample sets.
2. If §3's hypothesis holds, the fix is most likely physical (lead
   shielding/routing, grounding) or a straightforward longer low-pass
   filter (larger `CS_FILTER_TAU_S`) — not a mains-cycle-synchronized
   sampler, since there is no periodic aliasing here to specifically
   cancel.
3. Either way, `ZONE_SWEEP_NORMAL_NOISE_FLOOR_A` stays a separate, later
   decision gated on a real measured post-fix noise floor, per the task's
   own out-of-scope note — nothing here changes that.

No code, test, or threshold changes accompany this review; it is
documentation only.
