# S6a startup grace (914205f8) reverted — re-diagnosis

An opus review of `914205f8` (S6a startup grace, `s6a_startup_grace_active`,
`S6A_STARTUP_GRACE_MS` 5 s) found it NOT SAFE. All four findings were
verified against the code/docs and confirmed. The commit is reverted.

## Review findings, verified

1. **HARDWARE.md's fail-danger characterization contradicts the commit's
   premise.** Confirmed. `firmware/SaftyFW/docs/HARDWARE.md` section 1 step C
   and section 4 both say the fault line **fails de-asserted**: ESP absent,
   unpowered, or held in reset (GPIO6 undriven/high-Z) reads `mainFault`
   **HIGH** = healthy. GPIO6 is not an ESP32-S3 strapping pin, so "mid-boot,
   not yet initialized" is electrically the same undriven/high-Z state as
   "in reset" — it should also read healthy, not asserted. `914205f8`'s
   premise ("mid-boot leaves GPIO6 undriven-healthy... latching a trip") is
   self-contradictory: an undriven-healthy line cannot latch a trip. The
   commit extended HARDWARE.md's documented behavior into a new case
   ("present but not yet initialized") without justification, and the
   extension is backwards.

2. **Grace does not gate on `clock_stalled`.** Confirmed.
   `firmware/SaftyFW/src/tasks/safety_core.c:824`:
   `bool s6a_startup_grace_active = now_ms < S6A_STARTUP_GRACE_MS;` — no
   `clock_stalled` check, unlike `context_valid` (`:846`,
   `!clock_stalled && ...`) and `reboot_grace_active` (`:800`,
   `if (!clock_stalled && ...)`). A clock pinned below 5000 ms leaves
   suppression on indefinitely.

3. **Re-arms every Pico boot.** Confirmed, same line: `now_ms` is the Pico's
   own free-running `to_ms_since_boot()`, reset to 0 on every boot. A
   sub-5-second reset loop keeps the window open on essentially every tick.

4. **Suppression is invisible.** Confirmed. `firmware/SaftyFW/src/
   safety_guards.c:474-480`: the suppressed branch is a comment
   ("Suppressed: still inside the Pico's own post-reset startup window...
   Nothing else about this tick changes") with no log line, no status bit,
   no diag counter. A live S6a-worthy condition during this window leaves no
   trace anywhere.

## What actually happened (re-diagnosis)

`914205f8`'s own commit message says the trip was "real by the guard's own
rules" and was cleared once with `safety_clear_trip()`. Re-checking why GPIO6
would actually go high (asserted) during ESP boot rather than staying
undriven/healthy:

`firmware/KilnFW/App/drivers/safety/safety_link.c:415-419` initializes the
fault GPIO output-low (de-asserted/healthy) as the very first thing in
`safety_link_init()`. But `safety_link_poll.c:348`:

```c
safety_link_set_fault_source(link, SAFETY_FAULT_SRC_SAFETY_LINK, !up || version_mismatch);
```

asserts `SAFETY_FAULT_SRC_SAFETY_LINK` — driving GPIO6 **high**, a real
fault — whenever the safety link is not yet up or its peer's protocol
version has not been confirmed. This is intentional, documented behavior
(`LINK_PROTOCOL.md` sec 4, "no FW_VERSION from the safety processor... link
as down and blocking heat").

`fc6d30f8` reset both processors together (a dual ESP+Pico reflash). The
Pico reboots and starts polling `mainFault` almost immediately. The ESP
takes longer to reach the point where `safety_link_init()` runs and the
handshake with the Pico completes (`FW_VERSION` exchange). During that
window the ESP's own link layer treats "link not up yet" as a real fault and
**correctly drives GPIO6 high** — this is not an artifact of GPIO6 being
undriven; it is the ESP actively asserting fault per its own link-down
policy.

**Conclusion: the original S6a trip was correct, not spurious.** The safety
processor saw a genuine main-controller fault line asserted (link not yet
up) and tripped exactly as designed. There is no bug on the SaftyFW side
that needs a guard change. The "just clear it" pattern flagged as a smell in
the commit message was, in this instance, actually appropriate for what
happened — a boot-race trip, not a latent defect — though it should still be
logged/investigated per-incident rather than becoming routine.

## Disposition

- `914205f8` reverted in full (`safety_guards.c`, `safety_guards.h`,
  `tasks/safety_core.c`, `docs/SAFETY_MODEL.md`, and
  `test/test_safety_guards.c`'s `test_s6a_startup_grace()`, which only made
  sense paired with the feature and is removed with it, not kept disabled).
- No new guard-side change is implemented by this task. If a change is
  wanted at all, it belongs on the KilnFW side: either accept that a dual
  ESP+Pico reset legitimately produces a brief, correct S6a trip during boot
  (and make the ESP-side reflash/reset procedure sequence the two resets so
  the Pico's poll starts only after the ESP's link is already up), or teach
  the *operator procedure* (not the safety guard) to expect and clear this
  specific trip after a dual reset. A Pico-side suppression of a
  correctly-asserted fault line is the wrong layer for this and should not
  be reintroduced.
