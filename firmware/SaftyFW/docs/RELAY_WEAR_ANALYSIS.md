# K4 relay wear -- assessed, not built

Prompted by KilnFW's `relay_cycles.c` (lifetime contact-cycle accounting for
the ESP's four relays, now surfaced in the web UI against a documented
~1e5-operation contact-life budget, `docs/HARDWARE.md`) and `kiln_io.h`'s
note that K4 -- the relay the safety processor energizes to permit heating --
"belongs to the RP2040 safety processor... and is not reachable from here at
all." KilnFW counts its own four relays and reads zero for index 4 because
that slot is simply unused with three zones; K4's wear is not counted
anywhere. This doc is the assessment of whether it should be.

## How K4 is actually driven

Traced end to end, on both processors:

- **SaftyFW**, `src/tasks/relay_owner.c`: the *only* code in the build that
  writes GPIO6 (the K4 drive pin). It only ever changes state in response to
  a queued command: `RELAY_OWNER_CMD_ENERGIZE`, `_TRIP`, `_CLEAR_TRIP`. There
  is no per-tick or per-guard-evaluation write -- `relay_owner_task`'s main
  loop calls `relay_grace_tick()` every iteration (a state-machine check,
  not a GPIO write) and re-checks in with the watchdog, but only touches
  GPIO6 inside the `switch` on an actual received command.
- The energize command reaches `relay_owner` from exactly one place:
  `safety_core_request_enable()` (`src/tasks/safety_core.c:1516`), itself a
  thin forward with two ON-direction interlock checks (update-transfer,
  commissioning/tc-installed) and no periodic re-assertion logic.
- `safety_core_request_enable()` has exactly one caller:
  `link_task_handle_request_enable()` (`src/tasks/link_task.c:1252`),
  which decodes `SAFETY_CMD_REQUEST_ENABLE` (0x02) off the wire -- one frame
  per ESP-initiated request, not a poll.
- On the ESP side (KilnFW, read-only for this pass), that frame is sent by
  `safety_link_request_enable()`, called from `heat_enable.c`'s
  `send_enable()`/`send_release()`, themselves driven only by
  `heat_enable_claim()`/`heat_enable_release()` -- called at firing start,
  firing stop, autotune start/finish, and safety halts
  (`profile_executor.c`, `autotune_engine.c`, `danger_mode.c`). The one
  thing that runs continuously, `heat_enable_reconcile()` (from
  `profile_executor.c`'s watchdog tick), only **retries a still-pending
  failed** request -- `s_he.pending && !s_he.granted` -- it never re-sends
  once a request has landed. A granted enable is never re-asserted.

This is a structurally different mechanism from what makes KilnFW's own
relay counters matter. `relay_cycles.h`'s own header comment names the real
driver of wear on *that* board: "A 60 s time-proportioning window can spend
[the 1e5 budget] in a few hundred hours of firing" -- the PWM zone relays
cycle on a duty-cycle timer, many times per firing. K4 has no duty-cycle
role. It is a permit relay: one transition to enable at the start of a
firing (or an autotune run), one to disable at the end, and rare extra
pairs around a TRIP/CLEAR_TRIP or a danger-mode halt.

## Numbers

Call it generously 10 K4 transitions per firing (enable, disable, plus a
couple of trip/clear/pause-resume cycles on a bad day). KilnFW's own PWM
relay counters -- accumulating far faster, every duty window, across
whatever real bench/dev use has produced this board's history -- sit at
1712/1899/1938 lifetime operations today. K4, cycling roughly two orders of
magnitude less often per firing than a duty-cycled zone relay, would be
nowhere near comparable after the same usage history -- worth double-digit
operations, not thousands. At 10 cycles/firing, the 1e5 contact-life budget
is ~10,000 firings away: even at an implausible one firing a day that is
decades, and this board does not fire anywhere near daily.

## Decision: do not build lifetime cycle accounting for K4

The wear is not material. Adding a counter, a persistence path, and (per the
task) eventually a link field and UI consumer would be real surface area --
another flash writer to get right on a safety processor, another producer
with no consumer until a second pass wires one up (this repo has shipped
that gap repeatedly, see `project_consumer_without_producer_class` in
project memory) -- in exchange for a number that will read single or
low-double digits for the operational life of the board. That trade is not
worth making.

## If this is ever revisited

Something would have to change first: K4 being cycled far more often than
today's enable/disable-per-firing shape (e.g. a future design that re-polls
or re-asserts the permit on some fast timer), or a policy decision that even
a small count is worth surfacing for a different reason (audit trail,
correlating trips with relay age). Neither holds today. If it ever does,
the shape to mirror is KilnFW's `relay_cycles.c`:

- Accumulate the K4 transition count in RAM on every `relay_owner_task`
  GPIO6 write (one `uint32_t`, incremented in the same `switch` cases that
  already call `gpio_put()` -- no new task, no hot-path cost).
- Persist through the same flash-write path `config_store_flash.c` already
  uses (`flash_safe_execute()`, called from a context that is not itself
  the flash-owning worker/handler, avoiding this repo's documented
  reentrancy deadlock and PSRAM-stack-vs-flash-write hazards), at a bounded
  interval (mirror `RELAY_CYCLES_PERSIST_INTERVAL_S` = 600 s) plus an
  explicit flush at a natural end point (GRACE exit, a TRIP), never on
  every transition.
- Expose the count for a diagnostics/status frame (a new field on the
  existing link status broadcast, `CommonFW/docs/LINK_PROTOCOL.md`) and a
  KilnFW-side UI consumer -- both out of scope here, and both real,
  scoped follow-up work if the decision above ever flips.

No source files changed in `src/` for this pass; `safety_guards.c` and
`safety_core.c` were read, not edited.

## Update, 2026-09-06: `docs/RELAY_LIFE_BUDGET_PLAN.md`

The decision above (no counter, no persistence, no link field on the Pico)
still holds and this doc's analysis is unaffected. What changed is *where*
K4 gets counted: `RELAY_LIFE_BUDGET_PLAN.md` has the **ESP** count K4 edges
by observing K4's reported state on the existing safety-status frame, into
a fifth `relay_cycles.c` slot, rather than adding a Pico-side counter or a
new link field — see that plan's "Design" section and
`firmware/KilnFW/docs/HARDWARE.md`'s "Relay type and contact-life budget"
section for the resulting type/threshold/indication behavior. The K4
edge-counting call site on the ESP is a separate, still-open step of that
plan as of this writing.
