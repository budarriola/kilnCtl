# CT attribution verification — commissioning plan

Status: PLANNED. Nothing in this document is built yet.

## The problem

Nothing today checks that a current-transformer clamp is on the conductor the
configuration says it is on. The configuration is simply believed. Every
downstream current guard inherits that unverified assumption: presence detection
(`current_sense.c`'s `conducting = (amps > i_present_a)`), S3 stuck-load, S9
ineffective-heat, S11 frozen-sensor, and the two guards that depend on
attribution directly, S14 overcurrent and S15 open-heater deficit. A clamp on
the wrong conductor, or a channel configured as fitted with no clamp on it at
all, does not produce a fault. It produces a confident wrong answer.

There is a second, worse defect already live. Setup wizard step 9 is titled "CT
mapping verification" and it does energize zones, but its client logic in
`firmware/KilnFW/App/drivers/http/setup_wizard_page.html` finishes like this:

```js
if (st.state === 'done') {
  step9StopPoll();
  document.getElementById('step9Ok').innerHTML = '<p class="wok">Verification complete.</p>';
  postStepState(9, 'done').then(...)
```

`st.state === 'done'` means the sweep ran to completion. It does not mean the
sweep resolved anything. The firmware already knows the difference — the
accumulator in `zones_current_sweep_task.c` carries `derived_mask`,
`conflict_mask` and `unresolved_zone_mask`, and `zone_sweep_plan_k_ct()` refuses
with a note when zones were left unresolved — but none of that is inspected
before "Verification complete" is printed and DONE is persisted. On the bench
fixture nothing is ever resolved, and step 9 nonetheless reads green. That is
precisely the failure mode the owner named: a verification that silently passes
when it could not measure anything, converting an unknown into a false
assurance that the guards downstream then trust.

Step 9 also declares `readinessKeys: []`, so there is no readiness fact that can
ever regress it. Once DONE, it stays DONE across any later configuration change.

## Recommendation: fold into step 9, do not add a standalone step

Step 9 already is the standalone step. It applies heat, it requires the owner
physically present behind an explicit "I am present and the kiln is safe to heat
right now" acknowledgement, it refuses while a firing or an autotune is actually
running, it has an abort path, and it drives
`/api/zones/current_sweep/{start,status,abort}`, which is the mechanism that
energizes one zone at a time with every other zone off. The energizing sequence
this feature needs exists and is in the right place in the step order, after CT
installation and calibration in step 8 and before PID tuning in step 10.

What is missing from step 9 is not a procedure. It is a verdict.

PID tuning (step 10) is a poor host and should be rejected. Autotune runs one
zone at a time for hours, needs a rested kiln with no residual heat, feeds a
deliberately fake setpoint, and has an entirely different refusal set. Its
energizing pattern is long and single-zone, not the short cross-zone pattern
attribution needs — during an autotune of zone 0 you learn nothing about whether
zone 1's clamp is on zone 1. Folding attribution into tuning would also mean a
miswiring is not discovered until hours into the commissioning sequence, after
it has already been trusted by everything in step 9.

Folding into the sweep engine itself, rather than the wizard step, is where the
actual work goes: the decision logic belongs in
`zones_current_sweep_engine.c` beside `zone_sweep_derive_ct_channel()`, which is
already pure and host-testable, and the wizard step becomes the surface that
reads and refuses to launder the verdict.

## One energizing pass, two verdicts

Normal-current measurement and attribution verification share the same energizing
sequence and must not be run twice. They are one step. They are two verdicts,
stored and reported separately, because they fail independently: a zone can
produce a clean, believable `i_normal_a` while its clamp is on the wrong
conductor (the magnitude is right, the attribution is wrong), and a correctly
attributed channel can still be too noisy to yield a normal current worth
recording. Collapsing them into one pass/fail would let either one's success
speak for the other.

## Procedure

The sweep's existing shape is correct and should not be changed. Per zone, in
index order, with all other zones commanded off through `kiln_io_owner`:

- energize, settle `ZONE_SWEEP_SETTLE_MS` (1000 ms),
- sample `ZONE_SWEEP_SAMPLE_MS` (4000 ms), accumulating a per-channel mean over
  all three channels, not just the expected one,
- de-energize, and require the channels to fall back before the next zone.

The settle window is the one number that deserves scrutiny. The CT front end is
a rectified peak envelope with a time constant of about 1 s
(`firmware/SaftyFW/docs/CURRENT_SENSE.md`), so 1000 ms of settling is one time
constant — roughly 63 % of the step. A zone whose true current is at the decision
threshold can therefore be measured a third low. The sample window is long
enough that this mostly averages out, but the honest fix is to raise settling to
three time constants (3000 ms, about 95 %) for the verification pass and to say
so, rather than to rely on the sample window to hide an under-settled envelope.
That lengthens a three-zone pass from 15 s to 21 s, which is not a cost worth
arguing about. **Open question for the owner**, since it changes an existing
constant that the k_ct calibration path also uses.

Sample count against noise: publish is 20 Hz, so a 4000 ms window is about 80
samples. Measured idle standard deviation on the fitted channel is 4.678 counts
in the 2026-09-06 capture and 9.329 counts in a later larger one, against 0.180
and 0.223 on the two unfitted channels — the fitted channel is roughly 40 to 50
times noisier, which is lead pickup, not a defect. Taking the pessimistic 9.329
counts, the standard error of an 80-sample mean is about 1.04 counts, or roughly
0.8 mA at k_ct 1.0 V/A. That is comfortably below the existing
`ZONE_SWEEP_NORMAL_NOISE_FLOOR_A` of 0.045 A, so the sample window is not the
binding constraint. The binding constraint is the fixture, below.

Attribution decision, per zone: reuse `zone_sweep_derive_ct_channel()`
unchanged. It already requires a dominant channel at
`ZONE_SWEEP_CT_DOMINANCE` (4x), already treats a NaN entry as making the whole
call ambiguous rather than silently zero, and already refuses rather than
guesses when two channels read within 4x of each other — which is exactly the
shared-CT case or a foreign load, neither of which is derivable. Its refusal is
the correct behaviour and must be preserved, not softened.

The verdict then compares the derived channel against the configured one. Plan
against the concept, not today's encoding: the question is "which CT channel does
this zone's configuration say it uses, and did a clamp on that channel actually
respond when this zone and only this zone was energized". Today that is
`ct_channel_map` plus `ct_topology`; under `docs/CT_CHANNEL_MASK_PLAN.md` it
becomes per-zone `zone_ct_channel[z]` with `member(ch) = {z : zone_ct_channel[z] == ch}`.
The verification should read the configured channel through a single accessor so
that the comparison survives the schema move untouched. Where two or more zones
share a channel, per-zone attribution is not derivable by construction and the
correct verdict for those zones is INCONCLUSIVE with the shared-channel reason,
never PASS; what can still be verified there is that the shared channel responds
to each member and that no non-member zone drives it.

## Pass, fail, inconclusive

Three states, never two. Per zone for attribution, and separately per zone for
normal current:

- **PASS** — a dominant channel was resolved, it matches the configured channel,
  and the response was above the noise floor with all non-configured channels
  quiet. Only a measurement can produce this.
- **FAIL** — a dominant channel was resolved and it is not the configured
  channel, or the configured channel stayed quiet while another responded, or two
  zones both claimed the same channel where the configuration says they are
  distinct (`conflict_mask`).
- **INCONCLUSIVE** — anything else. Below the floor, no dominant channel, a
  channel in a shared group, a NaN, an aborted or refused pass, a link drop
  mid-pass.

The rule that makes this safe is structural, not a matter of care at each call
site: **INCONCLUSIVE is the initial value and the only value reachable without a
measurement.** PASS is written in exactly one place, and only from a branch that
has already established a resolved dominant channel above the floor. Absence of
evidence therefore cannot fall through to PASS; it lands on the value it started
at.

## What the physics permits here, honestly

This bench cannot produce a PASS, and the plan should stop pretending a future
run on it will.

The fixture draws about 70 mA total, roughly 23 mA per zone. The sweep refuses to
record a normal current below `ZONE_SWEEP_NORMAL_NOISE_FLOOR_A` of 0.045 A, so
every zone is below the floor by a factor of two. Worse for attribution,
`ZONE_SWEEP_CT_RESPOND_A` is 2.0 A: a zone must pull two amps before the sweep
treats a channel as having responded at all, which is about 87 times what this
fixture draws. No adjustment of settling or sample count reaches that. The front
end is a rectified peak envelope, not a waveform sampler, so per-cycle techniques
— phase comparison, correlation against relay switching edges, synchronous
detection — are unavailable in principle, not merely unimplemented. The `sqrt2`
in the conversion also assumes a sinusoidal resistive load, which is fine for
elements and wrong for anything else.

So: **on this bench, every zone must report INCONCLUSIVE, and a run that reports
anything else is a bug.** That is a useful property, not a disappointment — it
makes the inconclusive path the one the bench exercises by default, so the false-
pass defect cannot hide in a rarely-taken branch. On a real kiln, with elements
drawing tens of amps, every threshold above is met with a wide margin and the
verdict is a real measurement.

Splitting it explicitly:

- **Verifiable on this bench**: the decision logic in host tests; that the
  bench run yields INCONCLUSIVE for all three zones and does not mark the step
  done; that the verdict is persisted, surfaced, and invalidated on a
  configuration change; that the step refuses to start during a firing or an
  autotune; that relays are driven only through `kiln_io_owner`.
- **Requires a real kiln**: any PASS at all; any FAIL from a genuinely swapped
  clamp; the true settling behaviour of the envelope at real current; whether
  3000 ms of settling is in fact enough; whether the 4x dominance ratio is the
  right margin with real elements and real lead coupling.

## Reporting and blocking

A wrong clamp is safety-relevant miswiring, not a preference. **Recommendation: a
FAIL blocks step 9 from completing and blocks the firing interlock.** It should
be a readiness item, not merely a wizard note, so it reaches the same gate that
already refuses to fire on a latched trip or an unverified E-stop. The operator
text should name the zone, the configured channel, and the channel that actually
responded, because that triple is enough to walk to the panel and move the clamp.

INCONCLUSIVE must **warn and block step 9 from reading done, but not block
firing.** Blocking firing on inconclusive would brick this bench and every kiln
too small to reach 2 A, which is a worse outcome than the status quo and would
get the check disabled. It must be visibly distinct from PASS in every surface —
a distinct status word and colour in the readiness list, an explicit "could not
determine" in the step 9 text — and must never be summarized alongside PASS as
"no problems found". The wizard step's stored state for an all-inconclusive run
is PENDING or SKIPPED, never DONE.

Note that the wizard's own tri-state `SETUP_WIZ_STEP_{PENDING,DONE,SKIPPED}` has
no room for "inconclusive", which is structurally why the current code posts
`done`. The verdict must therefore live in its own store and be projected into
the wizard through a readiness key, not squeezed into the progress blob.

## What is stored, and staleness

A verdict that outlives the configuration it was taken against is a trap. Store,
per zone and per verdict kind: the verdict enum, the measured current, the
channel that responded, the Unix timestamp, and a **configuration fingerprint** —
a hash over the fields the verdict actually depends on: each zone's configured CT
channel, `ct_installed`, the per-channel k_ct calibration values and their
provenance, and the zone-to-relay mapping.

Invalidation is then not a matter of remembering to clear it. The readiness item
recomputes the fingerprint from live config on every read and reports
INCONCLUSIVE, with the reason "configuration changed since this was verified",
whenever it differs from the stored one. A stale verdict cannot be read as a pass
because it is never compared for equality with PASS without the fingerprint check
in the same expression. This deliberately mirrors the wizard's existing REGRESSED
precedence in `setup_wizard_progress_effective_state()`, where `/api/readiness`
is authoritative over stored progress — giving step 9 a readiness key is what
lets that existing, already-tested machinery do the regression for free.

The reset-one-side-of-a-pair class applies directly here: the verdict and the
configuration are two pieces of state joined by a semantic contract. The
fingerprint is what expresses that contract in one place instead of relying on
every config writer to remember a companion clear.

## Schema and storage cost

New NVS namespace and key, both under the 15-character limit enforced by
`NVS_KEY_LEN_CHECK()` in `firmware/KilnFW/App/drivers/persist/nvs_key_check.h`
and by `tools/check_nvs_key_length.ps1`:

- namespace `ct_verify` — 9 characters.
- key `verdict_v1` — 10 characters.

Both fit with margin. (`relay_names_cfg` already sits at exactly 15, so this was
checked rather than assumed.) The blob is small: three zones times two verdicts
times {enum, float, channel byte} plus one timestamp and one 32-bit fingerprint —
under 64 bytes with a version byte and room to grow. NVS only, not the `cfg`
LittleFS partition, matching `setup_wizard_progress.c`'s reasoning.

No `ZONES_CFG_VERSION` bump is needed for this feature: it adds a new store
rather than widening the zones blob. It must, however, be written against the
accessor that `docs/CT_CHANNEL_MASK_PLAN.md` introduces, and should land after
that plan's step 4, so the fingerprint hashes the new per-zone field rather than
a field that is about to be replaced.

New readiness item key `ct_attribution`, alongside the existing `autotune` item
in `readiness_http.c`. Step 9's `readinessKeys` gains it. `SETUP_WIZARD_STEP_COUNT`
is unchanged — no step is added — so the mirror-drift check is unaffected.

`SETUP_PROGRESS_JSON_CAP` is unaffected for the same reason, which matters
because that constant carries an explicit instruction not to grow it.

## Testing

Host tests, in `zones_current_sweep_engine.c`'s existing pure-function test
target. Note that `test_zones_http.c` builds its own separate executable because
it `#include`s the implementation files directly; these tests belong beside the
engine tests, not in that one.

Required cases:

1. **Correct attribution** — zone 0 drives channel 0 at 12 A with channels 1 and
   2 near zero, configured channel 0. Expect PASS.
2. **Swapped clamp** — zone 0 drives channel 1 at 12 A, configured channel 0.
   Expect FAIL, with channel 1 named in the result.
3. **Below floor (the bench case)** — all three channels at 0.023 A. Expect
   INCONCLUSIVE, and explicitly assert it is not PASS.
4. **No dominant channel** — two channels within 4x. Expect INCONCLUSIVE with the
   ambiguous reason, not a guess at the larger one.
5. **NaN channel** — expect INCONCLUSIVE, not a zero substitution.
6. **Shared channel** — two zones configured to the same channel, both drive it.
   Expect INCONCLUSIVE for per-zone attribution, not FAIL and not PASS.
7. **Stale fingerprint** — a stored PASS whose fingerprint does not match live
   config. Expect the readiness item to report INCONCLUSIVE.
8. **Conflict** — two zones configured distinct, both resolving to the same
   channel. Expect FAIL.

**The negative test.** The single line that must be broken is the floor
comparison that gates the PASS branch — the condition requiring the resolved
channel's measured current to exceed the noise floor before a verdict may be
written as PASS. Inverting or deleting it must make test 3 (below floor) fail by
reporting PASS where INCONCLUSIVE was expected. If test 3 still passes with that
line removed, the test is vacuous and the false-pass defect this whole plan
exists to close is not actually covered. Per repo practice the negative test ends
with a forced full rebuild, not a hand-restore and an empty diff — a poisoned
executable surviving in the build directory has already produced a committed
wrong verdict once in this repo.

A second negative test is worth having on the fingerprint: breaking the
fingerprint comparison must make test 7 fail. Otherwise staleness invalidation is
untested and the trap is open.

## Coexistence with the k_ct sweep

`zone_sweep_plan_k_ct()` skips a channel only when its `safety_cfg_store` cal
source is `SAFETY_CT_CAL_SOURCE_MANUAL`. On this board all three channels report
`uncalibrated`, so that skip never engages and the sweep remains entitled to
overwrite channel 2's correct calibration — already recorded in ROADMAP as a
latent exposure. The verification pass must not widen it. Concretely: the
verification reads per-channel averages and writes only its own verdict store; it
never writes `k_ct_v_per_a`, never writes `ct_channel_map` or `zone_ct_channel`,
and never writes `i_normal_a` for a zone whose measurement was below the floor.
Running the verification must be safe to repeat and must not be capable of
changing the configuration it is checking — a verifier that rewrites its own
subject cannot fail.

Conversely, a k_ct sweep that does change calibration changes the fingerprint,
which correctly invalidates any prior verdict.

## Open questions for the owner

1. Raise `ZONE_SWEEP_SETTLE_MS` from 1000 ms to 3000 ms (one envelope time
   constant to three)? It also affects the existing k_ct calibration path.
2. Confirm that INCONCLUSIVE should block the wizard step but not the firing
   interlock, while FAIL blocks both.
3. Is `ZONE_SWEEP_CT_RESPOND_A` at 2.0 A the right response threshold for a real
   kiln, or should the verification use its own, separate threshold derived from
   the configured `i_normal_a` rather than a fixed constant?
4. Land order against `docs/CT_CHANNEL_MASK_PLAN.md` — this plan assumes it lands
   after that plan's step 4, so the fingerprint hashes `zone_ct_channel`.
