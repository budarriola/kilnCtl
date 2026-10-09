# On/off device zones — owner decisions, with recommendations (2026-09-14)

> **Superseded in part, 2026-10-04:** D1 (do not decouple an on/off device from the zone array) was overturned by the owner requirement recorded in `docs/SPARE_RELAY_ONOFF_PLAN.md`: spare relays bind to on/off devices through a separate aux-outputs store, with no zone-array widening. The rest of this document is unchanged.

**Purpose.** `docs/ON_OFF_ZONE.md` is carried on `ROADMAP.md` as an L-sized
item "blocked on four owner questions". It is not. **Steps 1–8 are shipped and
host-tested; the only open work is one supervised bench session (step 9).** The
four questions at the bottom of that plan were written *before* step 1 and three
of them have since been answered by the implementation or dissolved by it.

This document exists so the owner can dispose of the whole item in a couple of
minutes. Every decision below leads with a recommended answer, not a menu.

Constraints applied throughout: single-zone reasoning only; the bench is a ~4 W
fixture capped near 40 °C above ambient, so nothing kiln-scale is validatable on
it and no bench value ships; the 0.5 °C materiality rule is per objective.

---

## 0. What actually changed since the plan was written

| Plan premise | Status today | Consequence |
|---|---|---|
| "Design only, nothing implemented" (`ROADMAP.md` L row) | **False.** `d58492c9` (typing + guard exclusions), `3d740f78` (trigger core), `dd1d6ada` (rule storage), `172e3081` + `b46c120c` (UI/wizard), `bf1db47f` (relay actuation), `83c8b28b`/`e8e32c7a` (UART trace), `be27d461` (FAULTED-aggregation fix) | The roadmap row is stale and overstates the remaining work by ~8 steps. Fix the row. |
| Coupling row/column zeroing is the feature's hardest safety argument | Implemented **getter-side**, so no write path can bypass it (`zones_config_accessors.c:505-525`). Independent of whether the multi-zone coupling model itself survives: a refuted/retired coupling model only *removes* a risk here, it never creates work. | No owner decision. Closed. |
| Fuzzy layer / per-kiln derivation | An on/off zone runs no PID and no fuzzy at all (plan §1). The fuzzy programme cannot reach it. | Unaffected. No decision. |
| A zone with no identified model now runs plain PID (`233ded79`) | Strictly reduces the blast radius of an on/off zone that somehow slipped the type check — it would fall back to plain PID rather than to garbage identified gains. | Unaffected. No decision. |
| "`iter_tune`/`adaptive_tune` likewise skip it" (plan §1) | **Not true today.** `adaptive_tune.c` and `firing_score.c` contain no `zone_type`/`zone_is_on_off` reference, and `profile_executor_firing_stats.c:200` snapshots a zone on `z->active` alone. With `adaptive_tune` now the sole automatic gain writer (`88bb4333`, `e5cd7145`), an on/off zone that is *also* opted into adaptive tuning would feed meaningless IAE/lag statistics in as training data. | **Engineering gap, not an owner question** — see §2 below. |
| `docs/SAFETY_CASE.md` must record the guard-3 coverage gap and the `max_temp_c == 0` relaxation | Neither is present; `SAFETY_CASE.md` contains no on/off material at all. | **Engineering gap.** See §2. |

---

## 1. The decisions the owner actually has to make

### D1 — A zone slot is a scarce, hard-compiled resource. Recommend: accept it; do not widen it.

**Recommendation: keep the zone array at 3 and accept that an on/off device
costs a heater zone. On the bench, convert zone 2 for the step-9 dry-contact
test and convert it straight back.**

Why it wins: the zone count is not configurable — `MAX31856_CHANNEL_COUNT` is
`THERMO_CHANNEL_COUNT`, hard-defined as `3u` in
`firmware/KilnFW/App/drivers/common/uart_task_ids.h:289`, and it sizes every
per-zone array in the executor, the guards, the coupling matrix, the firing
record and the persisted `zone_cfg_t` blob. Widening it is a schema bump plus a
sweep of every one of those consumers — **L, and it touches the safety-relevant
config-migration code the flash guard specifically protects.** Nothing today
justifies that.

Strongest argument against: a 3-element kiln that also wants a vent has *no*
spare slot, so the feature is unusable on exactly the configuration this bench
represents. That is real. Note the board reports **3 thermocouples but 4
relays** — the hardware for a vent exists (relay 4, mask `0x08`); only the zone
slot does not.

What would flip it: the owner saying the production kiln needs all three zones
heating *and* a scheduled vent. Then the right answer is a small number of
**TC-less on/off zone slots appended past the thermocouple-backed ones** (the
plan already makes `thermo_mask == 0` legal for this zone type, §2) rather than
a blanket widening — still M–L, but bounded and worth doing on a real
requirement.

### D2 — Fail-safe state. Recommend: leave it OFF for every on/off zone, permanently.

**Recommendation: `failsafe_state = OFF` everywhere; treat fail-safe-ON as
unsupported until a device arrives whose de-energised state is genuinely
unsafe.**

Why it wins: on a safety trip the Pico opens K4 and de-energises the whole
heating chain. If the device is fed from that interlocked supply — the default
assumption for anything wired into this cabinet — `failsafe_state = ON` is a
request the hardware cannot honour, and `apply_relay()` correctly forces OFF
under an authority block regardless. Configuring ON would therefore be a
*setting that lies*, which is worse than not offering it. OFF costs nothing and
is already the schema default and the zero-init value.

Strongest argument against: a damper whose real safe state is *open* (dump heat
on a runaway) is a legitimate device and would be left shut on a trip.

What would flip it: the owner fitting such a device **on its own supply, ahead
of K4**. That is a wiring decision, not a firmware one — the ON path is already
built and confirm-gated in the UI (`172e3081`); it just cannot be honoured
behind the interlock. Work either way: **zero**.

*This answers plan question 1 ("is it behind K4?") without needing the answer:
the recommendation is correct under both.*

### D3 — Switching-frequency budget. Recommend: take this off the owner's plate.

**Recommendation: an engineer sets it. Ship the existing defaults — `hyst_c`
2.0 °C, `min_on_s` = `min_off_s` = 30 s — which bound the worst case at 60
switches/hour, and record that bound as a row in `docs/RELAY_LIFE_BUDGET.md`.**

Why it wins: the plan asked the owner for an expected switching frequency so the
relay budget could be sized. That question is now moot in the direction that
matters — the firmware *enforces* a ceiling rather than depending on an
estimate. Two independent min-on/min-off layers exist (decision core and
actuation gate, `bf1db47f`), the zones page already shows a live projected
cycles/hr, and `relay_cycles_add()` now counts on/off transitions for real. The
owner does not have to predict anything.

Strongest argument against: 30 s holds may be too sluggish for a fast damper.
That is a per-device tuning knob already exposed in the UI (1–3600 s), not a
design question.

What would flip it: a device the owner wants cycling faster than ~120
switches/hour, which would put a mechanical relay's rated life inside the
firing's own duration and make contactor choice the real constraint. Work:
**S** (one doc row), versus **zero** if left undocumented.

*This answers plan question 4.*

### D4 — Schedule the step-9 bench session. Recommend: yes, next bench slot, dry contacts.

**Recommendation: one ~30-minute owner-present session, dry contacts only (LED
jig or meter on the relay terminals — no load), following
`docs/audits/on_off_zone_bench_readiness_2026-09-08.md` §1–2. No heat needed.**

Why it wins: this is the only gap in the feature. No on/off zone has ever
actuated a physical relay; everything else is host-tested. The readiness pack
already carries the pre-flight checklist, the procedure, and — since
`83c8b28b`/`e8e32c7a` — an edge-triggered UART trace with an annotated correct
sequence and four named failure signatures, so the session produces a durable
record rather than a verbal "it worked". The 4 W bench cap is irrelevant here:
the test proves relay logic, not thermal behaviour.

Strongest argument against: it consumes a supervised bench slot for a feature
with no device fitted yet.

What would flip it: deciding under D1 that the feature is shelved — then skip
it. Work: **S**, owner present.

### Dissolved — plan questions 2 and 3, no owner input needed

- **Q2, "is the device near a safety thermocouple?"** S8 is a *rise*-rate guard
  on the Pico's own thermocouples; a vent or fan produces a *fall*. The
  interaction the question worried about cannot occur in the direction S8
  watches. This is a commissioning siting note, not a decision, and per standing
  rule no Pico guard is made conditional on ESP zone type regardless.
- **Q3, "confirm the interpretation, and that `RELAY_IO` isn't what you wanted."**
  Answered by the owner on 2026-09-07 (recorded verbatim in the plan's OWNER
  DECISIONS block) and then by eight shipped steps built on that
  interpretation. Re-asking it now would be asking the owner to re-authorise
  work already done.

---

## 2. Two engineering gaps — no owner decision, just work

1. **`adaptive_tune`/`firing_score` do not know about zone types.** An on/off
   zone is snapshotted into the firing record on `z->active` alone
   (`profile_executor_firing_stats.c:200`) and neither `adaptive_tune.c` nor
   `firing_score.c` references `zone_is_on_off()`. With `adaptive_tune` now the
   sole automatic gain writer and running concurrently with fuzzy (`88bb4333`,
   `e5cd7145`), and `firing_score` voting across four objectives, an on/off zone
   opted into adaptive tuning would contribute nonsense statistics. It cannot
   corrupt a *heater's* gains — the skip decision is per zone — but the plan's
   §1 contract says it must be skipped, and today it is not. **S**: one
   `skip_reason` branch alongside the existing `!z->enabled` / `!zr->active`
   ones, plus the corresponding skip in the stats snapshot, negative-tested.
   Do this before step 9, not after.
2. **`docs/SAFETY_CASE.md` has no on/off content.** The plan requires two
   entries there: the guard-3 (welded-contact) coverage gap accepted for on/off
   outputs, and the `max_temp_c == 0` relaxation for a TC-less on/off zone —
   the only relaxation ever made to that settled semantics. **S**, doc-only,
   but it is the record that makes the guard disabling defensible later.

---

## 3. Is the feature still worth doing?

**Yes — because it is almost entirely already done.** This is not a feature
awaiting a build decision; it is a finished feature awaiting a 30-minute
validation and two small cleanups. The premise it was written under still
holds: none of the fuzzy, adaptive-tune, plain-PID-fallback or coupling changes
since 2026-09-07 touch an on/off zone, because an on/off zone deliberately
participates in none of those paths. Dropping it now would discard eight
shipped, tested steps to save nothing.

The one thing that genuinely dents its value is **D1**: on this hardware an
on/off device costs a heater zone, so a 3-zone kiln cannot have both. If the
owner's real kiln needs three heating zones plus a vent, the feature as built is
demonstrable but not deployable, and the follow-on work (TC-less zone slots past
the thermocouple-backed three) is M–L. That is the question worth the owner's
two minutes; the rest are already answered.

**Total work implied by the recommended set:** two S engineering items (§2), one
S doc row (D3), one S bench session (D4), one stale `ROADMAP.md` row to correct.
No M or L work unless D1 flips.
