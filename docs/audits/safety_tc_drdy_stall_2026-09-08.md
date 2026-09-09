# Safety MAX31856 "TC invalid" — register-level verdict — 2026-09-08

## Why this audit exists

Owner challenged the standing verdict ("chip dead / not converting") because
the safety thermocouple looks properly attached. Two hypotheses were live:
(1) a dead/unresponsive MAX31856, or (2) `thermo_task.c`'s CR1-verify
downgrade discarding a good, finite reading post-hoc (the documented
`saftyfw_tc_invalid_is_one_cr1_byte` failure mode). Settled at the register
level over SWD. **Neither hypothesis is what's happening; a third, more
specific cause was found.**

## First SWD attempt was unreliable — stale ELF, now flagged explicitly

The running Pico firmware is build `36394fbd` (`safety_get_fw_version`,
built 2026-09-09 02:26:43Z). `firmware/SaftyFW/build/SaftyFW.elf` on disk
predates that (embeds `109b0b9c`, a later commit, built 03:51:51Z that same
session from a HEAD two commits ahead) — same "stale ELF" trap CLAUDE.md
already documents for KilnFW crash symbolization, not previously known to
apply to `debug_read_symbol`'s address resolution on SaftyFW too.

Reading `s_reconfig_gave_up` / `s_reconfig_retries` against that mismatched
ELF gave `gave_up=true, retries=0` — internally impossible per
`max31856_reconfig_retry.c` (`gave_up` can only be set inside the same
statement that sets `retry_count>=20`, and both fields are written together
in `thermo_task.c`'s reconfig block). This is almost certainly the same kind
of contradictory read flagged in the task brief from a previous session.

**Fix applied for this session**: `git worktree add --detach` at commit
`36394fbd` (main tree's `git status --porcelain` showed only unrelated
untracked test-artifact directories, nothing touching SaftyFW source, so
this reproduces the exact running binary), rebuilt `SaftyFW.elf` there
(confirmed by grepping the built ELF for the literal string `36394fbd`
next to its embedded build timestamp), and pointed `debug_read_symbol`'s
`elf_path` at that ELF. All values below are from that matched-ELF build,
read three times each, identical every time.

## Verbatim register/state values (matched ELF, 3 consistent samples)

```
s_reconfig_gave_up   = 0x00        (false)
s_reconfig_retries   = 0x00000000  (0)
s_snapshot (thermo_task.c, 0x20003f78, 20 bytes):
  timestamp_ms = 0x004e3804 (5,128,196 ms uptime)
  valid        = 0x00 (false)
  tc_c         = 0x7fc00000 (NaN)
  cj_c         = 0x7fc00000 (NaN)
  fault_bits   = 0x00
  spi_failed   = 0x00 (false)
  cj_valid     = 0x00 (false)

pico_gpio_read(12)  [~DRDY]   = LOW, 3 consecutive samples, no toggling
pico_gpio_read(11)  [~FAULT]  = HIGH (no fault asserted)
pico_gpio_read(6)   [relay]   = LOW (de-energized, confirmed before any
                                 further action)

safety_get_diag: trip_reason 5, warn_mask/trip_mask 0x0010 (S5), latched
safety_get_status: "safety TC invalid (no MAX31856 fault bits set)"
```

Reads are reliable: once resolved against the correct ELF, every value is
internally consistent (no impossible `gave_up`/`retries` combination) and
reproduces identically across repeated reads.

## Verdict: NOT chip-dead, NOT CR1-verify-discard — a missed boot-time DRDY edge

- **CR1-verify-discard is ruled out.** `s_reconfig_gave_up=false` and
  `s_reconfig_retries=0` mean `max31856_tc_type_verified()` has been true
  since the very first check this boot — the retry loop never needed to run
  even once. The commissioned tc_type stuck on the one-shot `main.c`
  `max31856_configure()` call. This directly contradicts the leading
  hypothesis from `project_saftyfw_tc_invalid_is_one_cr1_byte` for the
  *current* boot.
- **Chip-dead is also not supported.** `~FAULT` (GPIO11) reads HIGH — a real
  hardware signal driven by the MAX31856 itself, saying it has no fault
  condition latched. A genuinely dead/unpowered part reads that pin however
  the pull-up leaves it, but combined with the pattern below, the more
  specific explanation fits better than "dead chip" and is one we can name
  precisely.
- **What the evidence actually shows**: `s_snapshot` is exactly the
  DRDY-silence branch's output (`thermo_task.c:411-425` — `valid=false`,
  `cj_c=NaN`, `cj_valid=false`, `fault_bits=0`, `spi_failed=false`, set when
  `ulTaskNotifyTake()` times out with zero notifications). That branch never
  attempts an SPI read at all. And `~DRDY` (GPIO12) is stuck LOW, sampled
  three times with no toggling — consistent with "a conversion completed and
  asserted DRDY, and nothing has ever read the registers to release it,"
  which for the MAX31856 (DRDY clears only on a host read of the data
  registers, not on its own) is a self-sustaining stall: no falling edge can
  occur again because the pin never returns high.
- **How this happens with a healthy chip**: `thermo_task.c`'s own comments
  (lines 309-341) already document that CMODE auto-conversion starts the
  instant `main.c`'s pre-scheduler `max31856_configure()` returns — before
  `vTaskStartScheduler()` even runs — while the DRDY GPIO IRQ is deliberately
  armed only once `thermo_task_fn()` itself starts running, specifically to
  avoid touching FreeRTOS ISR-safe APIs before the scheduler exists. That
  ordering leaves a window: if the MAX31856's first automatic conversion
  finishes and asserts `~DRDY` low *before* `gpio_set_irq_enabled_with_
  callback(..., GPIO_IRQ_EDGE_FALL, ...)` executes, the falling edge that
  would have woken the task already happened with nobody watching. The task
  then arms an edge-triggered IRQ on a pin that is *already* low — no future
  edge is possible until something reads the chip's registers, and nothing
  ever will, because the only code that reads them is gated behind that same
  notification. Boot-time race, not a hardware defect.

## Differential requested by the owner: SaftyFW vs KilnFW CR1 sequence

Not the active cause here (CR1 verified fine, see above), but checked as
requested since it bears on hardware-vs-firmware confidence generally:

- **Clock**: both cap at 4 MHz (`max31856.c:46` `MAX31856_SPI_CLOCK_HZ
  4000000u`; KilnFW's `MAX31856.c:69` `MAX31856_SPI_MAX_CLOCK_HZ 4000000`,
  Kconfig-backed but capped the same).
- **Sequence**: both write CR1 while conversions are stopped, then read CR1
  back once and compare against the expected encoded byte
  (SaftyFW: `max31856.c:246-287`, `max31856_cr1_readback_check()`; KilnFW:
  `MAX31856.c:901-848`, inline `0x%02X` expected/actual compare log). Same
  shape: write-then-read-back, no extra inter-write delay in either, no
  retry inside the single `configure()` call in either (SaftyFW's retry is
  a separate periodic re-probe layered outside this function, per
  `max31856_reconfig_retry.h`; KilnFW has no equivalent because ESP-side
  channels are configured once at boot with the IC reliably powered by
  then).
- **Conclusion**: the two implementations are structurally equivalent for
  CR1 write/verify, and SaftyFW's own CR1 verification is currently
  succeeding on this boot anyway, so this comparison does not implicate a
  CR1 difference as today's cause.

## Three ESP zone channels (owner's differential, corrected count: 3, not 5)

```
thermo_read(channel=0): CH0: 35.01 C (CJ 36.69 C)
thermo_read(channel=1): CH1: 35.08 C (CJ 36.58 C)
thermo_read(channel=2): CH2: 35.06 C (CJ 36.52 C)
```

All three report sane, plausible cold-junction values (mid-30s °C, consistent
with each other and with room-adjacent temperature). This confirms the part
type, the general MAX31856/SPI approach, and CR1 verification all work
reliably on this hardware family — the fault is isolated to the safety
processor's one channel, and specifically to the DRDY-edge race above, not
to a part-family or approach problem.

## Owner action

**Firmware fix, not a hardware fix.** The chip is very likely healthy — do
not start desoldering or reseating the probe on this evidence. The fix is a
timing/ordering change in `thermo_task.c`/`main.c`: after arming the DRDY IRQ
in `thermo_task_fn()`, explicitly check whether the pin is already low
(`gpio_get(SAFTYFW_PIN_THERMO_DRDY)`) and, if so, treat that as an immediate
"data ready" condition — read the burst once right there to release DRDY —
before entering the normal `ulTaskNotifyTake()` wait loop. That closes the
race without changing the CMODE/IRQ-arming order that the existing comments
say is required for scheduler-safety reasons.

**Not implemented in this pass.** This is a real firmware bug but touches
boot-ordering / ISR-arming code that the existing comments mark as having
already caused a hardware double-fault once (see `thermo_task_fn()`'s header
comment) when gotten wrong; it deserves its own careful pass with a host
test for the "pin already low at arm time" branch, not a quick patch bundled
into a diagnostic session. Recommend a Pico reset as the immediate unblock
(clears the stuck DRDY latch by restarting the race with new timing — not
guaranteed to avoid the race a second time, but likely, since the exact
timing that caused it was one specific boot) and separately queuing the
ordering fix above. No guard behavior was changed and the S5 trip was not
cleared in this session.

## Reflash needed?

No firmware change was made. No SaftyFW rebuild/reflash required for this
audit; the `SaftyFW.elf` rebuilt here was only to get a matching ELF for SWD
symbol resolution, done in a separate worktree (`git worktree add --detach`
at commit `36394fbd`), never touching the main tree or the running board.
