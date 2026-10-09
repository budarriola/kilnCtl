# MAX31856 `~FAULT` pin: interrupt-driven fault handling — evaluation (closed, keep polling)

Status: **evaluation complete, recommendation is KEEP POLLING.** One documentation
defect fixed in this pass (§6). The interrupt work below is specified but
deliberately NOT implemented — see §4 for why the measured benefit is zero.

Owner proposal being answered: *"use the MAX31856's `~FAULT` pin to trigger a
status-register read and handle the fault — in addition to polling, or possibly
as a replacement."*

Prior art this builds on, not repeats: `docs/audits/max31856_pin_usage_review_2026-09-08.md`
(commit `9623ee2c`).

---

## 1. What `~FAULT` actually does, per mode — from the datasheet

`hardware/datasheets/ThermocoupleBoard_Sensor_Temperature/MAX31856.pdf`, p.19,
CR0 bit 2 (Fault Mode), verbatim:

> **Fault Mode**
> 0 = Comparator Mode. The FAULT output and respective fault bit reflects the
> state of any non-masked faults by asserting when the fault condition is true,
> and deasserting when the fault condition is no longer true. There is a 2°C
> hysteresis when in comparator mode for threshold fault conditions. (default)
> 1 = Interrupt Mode. The FAULT output and respective fault bit asserts when a
> non-masked fault condition is true and remain asserted until a 1 is written to
> the Fault Status Clear bit. This deasserts FAULT and respective fault bit until
> a new fault is detected (note that this may occur immediately if the fault
> condition is still in place).

Two further passages that bear directly on the design:

p.21, Fault Mask Register (02h):

> The Fault Mask Register allows the user to mask faults from causing the FAULT
> output from asserting. Masked faults will still result in fault bits being set
> in the Fault Status register (0Fh). Note that the FAULT output is never
> asserted by thermocouple and cold-junction out-of-range status.

p.26, OVUV bit:

> 1 = The input voltage is negative or greater than VDD. The FAULT output is
> asserted unless masked.
> Note: The presence of the OVUV fault will suspend conversions and the ability
> of the MAX31856 to detect other faults (or clear faults when in comparator
> mode) until the fault is no longer present.

p.14, open-circuit detection cadence in automatic conversion mode:

> When the device is in automatic conversion mode, open-circuit detection may be
> disabled, or it may be set to automatically test for open circuits every 16
> conversion cycles.

> Disabling the open fault detection when in comparator mode while there is an
> open fault present will not clear the fault bit or FAULT pin. If this happens,
> to subsequently clear the fault, the MAX31856 must be placed in interrupt mode
> and then the fault cleared.

### Read this carefully — it inverts the intuition

**Comparator mode (what all four channels use today) is the SAFE mode for an
interrupt, and interrupt mode is the DANGEROUS one.** The naming is backwards
from what a firmware engineer expects.

- In **comparator mode**, `~FAULT` *tracks the condition*. It is a level that
  follows reality: asserted while the fault is true, deasserted when it clears.
  There is no host action required to release it. An interrupt armed on a
  comparator-mode `~FAULT` still has to be level-aware (§5), but it cannot
  self-latch — the pin will deassert on its own, so a missed edge is
  self-healing the moment the fault recurs or clears.
- In **interrupt mode**, `~FAULT` *latches until the host writes FAULTCLR*. That
  is structurally identical to `~DRDY`: a latching level released only by host
  action. An edge-only watcher that misses the first assertion is stuck exactly
  the way the safety channel's `~DRDY` was stuck
  (`docs/audits/safety_tc_drdy_stall_2026-09-08.md`, `512d3c33`). **Do not switch
  to interrupt mode in order to make an interrupt work.** The temptation exists
  because "interrupt mode" sounds like the mode you want when adding an
  interrupt; it is the mode that reintroduces the bug class we spent an evening
  auditing.

Also note the OVUV clause: under OVUV in comparator mode the part *cannot clear
faults at all* until the input recovers. So during the single most serious fault
the pin is a stuck level by design — another reason any consumer must be
level-tolerant rather than edge-counting.

---

## 2. Current state (measured, not assumed)

| | ESP ch0/1/2 (KilnFW) | Safety channel (SaftyFW) |
|---|---|---|
| Conversion mode | auto, CMODE=1 (`sdkconfig:3733`) | auto, CMODE=1 |
| Fault mode | **comparator** (`interrupt_fault_mode = false`, no override) | **comparator** (hardcoded) |
| Fault mask (02h) | written, unmasks OPEN + OVUV | written, same |
| `~FAULT` pin | ESP GPIO, input + internal pull-up, `GPIO_INTR_DISABLE` (`MAX31856.c:587-606`) | Pico GPIO, input + pull-up, polled |
| Pin sampled | inside `MAX31856_read()` -> `fault_pin_asserted` | inside `max31856_read()` -> same field |
| Who consumes the pin | diagnostics HTTP, LCD diagnostics page, UART bridge — **no control, no guard** | **nothing** outside its own unit test |
| Who consumes the SR fault bits | `profile_executor.c:444`, `autotune_engine.c:142`, `zones_current_sweep_engine.c:466` | S5, via `s5_bad_read_now()` |

### Actual poll intervals

- **ESP:** the SR byte arrives inside the same six-register burst as the
  temperature, and that burst is driven by the control loop at
  `PROFILE_EXECUTOR_TICK_MS = 1000` — **1 Hz, i.e. a 1 s worst-case poll
  latency.** The pin level is sampled in the same call, so pin and SR have
  identical latency today.
- **Safety:** `thermo_task_fn()` is `~DRDY`-driven with a `conv_ms *
  THERMO_TASK_DRDY_SILENCE_MULTIPLIER` timeout; `max31856_conversion_time_ms()`
  reports 151 ms, so the loop runs at the conversion cadence (~140–151 ms) with a
  302 ms backstop. **~150 ms typical, 302 ms worst case.**

---

## 3. Latency genuinely available from an interrupt

An interrupt can only beat the poll by the poll interval minus the ISR latency,
and only if the *part* knew about the fault sooner. Both halves fail:

1. **The part's own detection cadence dominates.** Per p.14, in automatic
   conversion mode open-circuit detection runs **every 16 conversion cycles** —
   at ~140 ms/conversion that is roughly **2.2 s** between open-circuit tests.
   The `~FAULT` pin cannot assert before the part has run the test. So for OPEN,
   the dominant latency term is 0–2.2 s of detection jitter, against which the
   ESP's 1 s poll and the safety channel's 150 ms poll are already comparable or
   smaller. An interrupt removes none of it.
2. **Every downstream consumer is graduated over seconds.** S5 requires
   `bad_read_count_threshold` **10 consecutive bad reads AND**
   `bad_read_time_s` **5.0 s** of accumulated bad time before it will even WARN
   (`safety_guards.h:202-210`), and `blind_grace_s` is 60 s. The ESP's
   `ch_sensor_ok` term feeds the 1 Hz executor tick. Delivering the fault bit
   150 ms or 1 s earlier advances the trip instant by at most that much against a
   5-second bar — under 20% of one threshold, and **zero** change to whether a
   trip occurs.

**Honest headline number: best case an interrupt buys ~1 s on the ESP and
~150 ms on the safety channel, against guard bars measured in 5–60 s, and
against a part-level detection jitter of ~2.2 s that it cannot touch.** That is
not a safety improvement. It is a latency change below the noise floor of the
thing it would feed.

---

## 4. Recommendation: **KEEP POLLING** — on both processors

Not deference-driven; this is a recommendation against the proposal, with the
reasoning above as the basis. Restated as the trade:

**What we would gain:** ≤1 s (ESP) / ≤150 ms (safety) on a 5 s guard bar. Nothing
detectable in behaviour.

**What we would pay:**
- A new edge-armed consumer of a level signal, in the same firmware and the same
  week we found that exact pattern stalling the safety thermocouple. Even done
  correctly (§5), it is a new instance of the shape that has now bitten this
  repo once with real, silent, hours-long consequences.
- An ISR that wants to read a status register — i.e. wants the SPI bus — while
  the bus is owned by `spi_owner` and each channel by its own FreeRTOS mutex
  (§7). The ISR therefore cannot do the read; it must defer to a task, which
  reintroduces the very scheduling latency the interrupt was supposed to remove.
- Interrupt-storm exposure on a chattering line (§7).
- On the safety processor specifically: added ISR surface on the *independent*
  protection layer, whose whole value is that it is simple enough to be
  trustworthy.

The pin is not a second, independent measurement. It is a second *transport* for
a bit the SPI burst already carries, generated by the same on-die fault logic.
Buying a redundant transport for a bit we already read every tick, at the price
of a bug class, is the wrong trade.

**Reconsider only if** one of these changes: (a) the control tick moves much
slower than 1 Hz, (b) OPEN detection is reconfigured to on-demand so the part's
2.2 s jitter disappears, or (c) a fault type is unmasked whose response
requirement is genuinely sub-second. None is true today.

### The one change actually worth making instead

If earlier ESP-side fault visibility is ever wanted, the cheap, boring win is to
**raise the ESP poll rate**, not to add an interrupt: the SR byte is already free
inside a burst the code already performs. That is a scheduling parameter, not a
new bug class. It is not proposed here because §3 shows there is nothing to buy.

---

## 5. If it is built anyway: the mandatory level-aware design

Recorded so a future implementer does not have to re-derive it. **Do not
implement without an explicit owner decision that overrides §4.**

Non-negotiables:

1. **Stay in comparator mode.** Per §1, interrupt mode makes `~FAULT` a latching
   level released only by FAULTCLR — the `~DRDY` failure shape. Comparator mode's
   pin tracks the condition and is self-releasing. Do not "fix" an interrupt by
   switching modes.
2. **The interrupt is an accelerator, never the only path.** The existing polled
   read in `MAX31856_read()` / `max31856_read()` stays exactly as it is and keeps
   its current cadence. The ISR may only make a read happen *sooner*; it may never
   be the reason a read happens at all. This single rule makes a missed edge cost
   latency instead of correctness, and it is what makes the design compose with
   whatever `thermo_task.c`'s in-flight `~DRDY` fix settles on — the fault path
   would ride the same task loop the DRDY fix already owns, adding no second
   wake-up authority.
3. **Arm on BOTH edges, then read the level.** Never `GPIO_IRQ_EDGE_FALL` alone.
   The handler's job is "the pin may have changed" — the truth is always
   re-derived from `gpio_get()` plus the next SR read, never from the edge.
4. **Level check immediately after arming, before the first wait.** A fault
   asserted before the ISR exists produces no edge. Sample the level once right
   after `gpio_config()` / `gpio_set_irq_enabled()` and, if asserted, post the
   same notification the ISR would have. This is the identical correction the
   audit recommended for `~DRDY` (move the missed-edge check to just after
   arming, not only after the first timeout).
5. **Every polled read re-syncs.** At the end of each ordinary polled read,
   compare the sampled pin level against the interrupt path's belief. A
   disagreement means an edge was lost; log it on a counter and take the level as
   truth. Because the polled read never stops, this converges within one poll
   interval unconditionally — the property that makes latching impossible.
6. **No SPI in the ISR.** ISR does `gpio_get()` + `xTaskNotifyFromISR()` /
   `__sev()` and nothing else. All register access stays on the owning task,
   behind the existing mutex and `spi_owner`.
7. **Rate-limit at the source.** Disable the pin's interrupt inside the ISR and
   re-enable it only from the task after the deferred read completes
   (interrupt-disarm-until-serviced). This bounds a chattering line to one ISR per
   serviced read by construction, rather than relying on a debounce timer.

Testability: items 3–5 are a pure truth table over (edge seen, pin level,
last-known state) and must be extracted into a host-testable predicate — the same
split `max31856_fault_pin_policy.c` already demonstrates — with a negative test
proving each branch can fail. Do not land the ISR before that predicate is pinned;
`thermo_task_drdy_missed_edge()` shipped without one and that gap is still open.

---

## 6. The real defect, and its resolution — **DONE in this pass**

`firmware/SaftyFW/src/max31856_fault_pin_policy.h` claimed:

> This output feeds max31856_reading_t.fault_pin_asserted, which safety_guards.c's
> S5 (thermocouple fault) reads. A flipped polarity here would make S5 either
> never trip on a genuine MAX31856 fault … or trip permanently on a healthy, idle
> part.

**Both sentences are false.** S5's `s5_bad_read_now()` (`safety_guards.c`) reads
`in->fault_bits` — the Fault Status register byte — plus `spi_failed`,
`tc_valid`, `isnan(tc_c)`. Grepping all of SaftyFW, `fault_pin_asserted` is
written at `max31856.c:317` and read **nowhere** but its own unit test. It is not
copied into `thermo_snapshot_t` and reaches no `safety_guard_input_t` field.

Two candidate fixes were considered:

- **(a) Wire the pin into S5** as an additional bad-read term.
- **(b) Correct the comment.**

**Chose (b), and (b) is the honest fix.** The pin carries no information the SR
byte does not already carry — same on-die fault latch, same mask register, read in
the same burst. The only thing it adds is transport diversity: an SPI read that
returned plausible-looking garbage could show a clean SR while the pin sat low.
That is a real but narrow gap, and it is already largely covered by S5's
`spi_failed` and `tc_valid` terms. Against that, (a) is a **behaviour change to
the independent protection layer** — it adds a new trip input to a latched,
graduated safety guard — and the brief for this pass is explicit that S5's
latch/debounce must not change and that such a change belongs to the owner.
Adding an unproven trip input to S5 to justify a comment is the wrong direction.

Applied: the header's claim now states what actually consumes the field, says
plainly that no guard reads it, keeps the polarity contract (LOW = asserted)
intact, and instructs anyone who later *does* wire it into a guard to update the
comment in the same commit. The same false claim was duplicated in
`firmware/SaftyFW/test/test_max31856_fault_pin_policy.c`'s header comment and was
corrected identically — a stale rationale in the test file would have re-seeded
the error.

**No behavioural change. No guard logic touched. Polarity unchanged.** The
existing accept/refuse host-test pair for the predicate still passes unmodified,
which is the correct outcome for a comment-only fix — see §9 on why a negative
test is not applicable here.

**Left open for the owner:** whether to wire `fault_pin_asserted` into S5 as an
extra bad-read term. If it is ever taken up, note that the pull-up makes an
absent or unpowered daughterboard read HIGH = healthy, so the failure direction
is "no false trip", not "spurious trip" — the safe direction, which is what makes
(a) a defensible future option rather than a hazard. It would still need its own
justification, its own host tests, and bench confirmation before firing.

---

## 7. What could go wrong (assessed for the design in §5)

- **Chattering fault line -> interrupt storm.** Real. OVUV in particular sits at a
  threshold; a marginal input can toggle. §5.7's disarm-until-serviced bounds it
  structurally. Note comparator mode's 2 °C hysteresis applies only to *threshold*
  faults, not to OPEN or OVUV — the two we actually unmask — so hysteresis is not
  the protection here.
- **Fault asserting during an SPI access.** Harmless in comparator mode: the pin
  is a level, so it is still asserted when the transaction ends and the deferred
  read sees it. Under the §5 rules there is no state to corrupt, because no
  decision is derived from the edge.
- **ISR running while the bus is held — yes, it can, and that is the point of
  §5.6.** On the ESP the SPI host is shared with the ST7796 panel under
  `spi_owner`, and each channel has its own FreeRTOS mutex with a bounded
  `THERMO_OWNER_WAIT_MS = 200` timeout. A GPIO ISR is not blocked by either — it
  will fire mid-blit and mid-burst. Any ISR that tried to touch SPI would either
  corrupt a display transaction or deadlock on a mutex it cannot take from
  interrupt context. Hence: ISR does `gpio_get()` and a notify, full stop.
- **Priority inversion via the notify.** The deferred read still queues behind
  `thermo_owner`'s existing serialization, so under bus contention the "fast"
  interrupt path degrades to the poll latency it was meant to beat. This is not a
  bug, it is the ceiling — and it is a further argument for §4.
- **Two firmwares, two different exposures.** On the ESP the fault path feeds the
  thermal guards, which already fail safe via `ch_sensor_ok`. On the RP2040 it
  would feed S5, the independent protection layer, whose value is its simplicity
  and auditability. A change there needs its own justification, its own bench
  confirmation, and should not ride along on an ESP-side change. **They are not
  one change; do not implement them as one.**
- **The failure that would not be noticed.** If an interrupt path were ever
  allowed to *replace* the poll (§5.2 violated), a stuck-high pin or a dead ISR
  would present as "no faults ever" — clean counters, healthy-looking status,
  identical to a genuinely healthy part. That is the reset-one-side signature
  CLAUDE.md catalogues, and it is why "replace" is rejected outright rather than
  merely disfavoured.

---

## 8. Ordered, testable steps

Only step 1 is done. Steps 2–3 are optional follow-ups; steps 4+ are gated on an
owner decision that overrides §4.

1. **[DONE]** Correct the false S5 claim in `max31856_fault_pin_policy.h` and its
   test's header comment. *Test:* existing host tests still pass unchanged;
   `grep -rn fault_pin_asserted firmware/SaftyFW` shows no consumer outside the
   unit test, matching what the comment now says.
2. **[open, small]** Add the one-line comment at `kiln_io.c:218` recording that
   the DRDY falling-edge sense is diagnostic only and the read path is
   deliberately level-based. Carried over from the audit's item 4; not done here
   to keep this pass to a single concern. *Test:* comment-only.
3. **[open, owner]** Decide whether `fault_pin_asserted` should become an S5 input
   (§6). *Test:* if yes — a host test per new S5 branch, each with a negative
   case, plus a bench confirmation that an absent daughterboard does not trip.
4. **[gated on owner override of §4]** Extract the level-aware arm/recover
   predicate (§5.3–5.5) as a pure function alongside `max31856_fault_pin_policy.c`.
   *Test:* full truth table, every branch negative-tested, before any ISR exists.
5. **[gated]** ESP only, one channel first: arm both-edge interrupt, level-check
   after arming, notify-only ISR, disarm-until-serviced. Poll path untouched.
   *Test:* re-sync counter from §5.5 stays at zero over a long run; forced missed
   edge recovers within one poll interval.
6. **[gated, separate decision]** Safety channel, only after step 5 has run on the
   bench for a sustained period, and only with its own justification. Must compose
   with, not duplicate, `thermo_task.c`'s `~DRDY` wake-up authority.

---

## 9. Negative-testing note

Step 1 is a comment-only change: there is no predicate whose behaviour changed, so
there is no production function that can be broken to prove a test fails. Writing
a test that "fails when the comment is wrong" would be a test of a string literal,
not of behaviour — the vacuous shape this repo has explicitly rejected. The
verification that the *claim* is now true is the grep in step 1, which is
falsifiable and was run. Steps 3–6 all involve real behaviour and each carries its
own negative-test requirement above.
