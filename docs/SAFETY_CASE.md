# Safety Case — the argument that this kiln controller is safe enough

> **Status:** stub · **Last reviewed:** 2026-08-16
> **Keep this file current.** A safety case that lags the code is worse than no
> safety case, because it invites trust it has not earned. If this file cannot
> be kept honest, delete it rather than let it drift.

## Why this file exists

`firmware/SaftyFW/docs/SAFETY_MODEL.md` says what each guard does and why its
thresholds are what they are. That is a description of a mechanism. A safety
case is a different claim: that the mechanisms, taken together and given
everything known to be missing, reduce the risk of an unattended kiln to
something acceptable.

Nobody can make that claim from inside one firmware's documentation, because the
interesting failures are the ones that cross the boundary — a main processor
that has stopped commanding, a safety processor with a welded contactor
downstream of it, a thermocouple that both processors read through the same
broken wire.

## What belongs here

- The hazards, named: uncontrolled overheat, fire, electric shock during
  service, a trip that fails to interrupt power.
- For each hazard, which guard or physical measure addresses it, and — more
  usefully — the residual risk that nothing addresses.
- The single points of failure that survive having two processors, particularly
  anything downstream of both relays.
- What the system does **not** protect against, stated plainly. This is the part
  most likely to be true and least likely to get written.
- The evidence: which claims are argued, which are host-tested, and which have
  been demonstrated on real hardware. These are not interchangeable.

## Preconditions

This cannot be written honestly until `SaftyFW` exists and its guards have been
exercised on hardware — roadmap milestone M4 at the earliest. Writing it sooner
would produce a document that argues from intent rather than from evidence,
which is exactly the failure mode a safety case exists to prevent.

## Completion checklist

- [ ] Hazard list written
- [ ] Each hazard mapped to a guard, a physical measure, or an accepted risk
- [ ] Residual risks and non-protections stated explicitly
- [ ] Evidence classified: argued / host-tested / hardware-verified
- [ ] Reviewed against [`../firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`](../firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md)
