# `stats.timeouts`/`link_reply_us` reading ~100% miss: benign accounting artifact, not link loss or a today regression

**Status: diagnosed, not fixed. No flash performed. Board left idle throughout.**
Investigated 2026-09-10 while gating the joint coupling-identification capture
(`docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`) on link health. Flagged
before heating because `safety_get_link_stats`/`get_heap_status` showed
`timeouts` climbing in lockstep with `sent` (every poll, ~100%) with
`link_reply_us.count` frozen. Held per owner instruction rather than treated
as a stable baseline.

## Verdict

**Benign, and not caused by today's SaftyFW changes.** `SAFETY_CMD_GET_STATUS`
is not actually a request/reply protocol on this link — `firmware/CommonFW/docs/LINK_PROTOCOL.md`
says so explicitly:

> `SAFETY_CMD_GET_STATUS` = `0x01` (existing) — no longer a poll
>
> The Pico pushes status unsolicited every 500 ms (§6), so the ESP does not
> need to ask. ... It may still be sent as a `BROADCAST` to request an
> immediate extra push ... but nothing depends on it, and the 500 ms cadence
> is the real mechanism.

Confirmed directly against both sides' source, not inferred from the doc alone:

- **Pico side** (`firmware/SaftyFW/src/tasks/link_task.c`): the switch
  statement that dispatches every incoming command
  (`link_task_handle_raw_frame()`, cases starting ~line 2355) has **no case
  for `SAFETY_CMD_GET_STATUS`/`LINK_FRAME_GET_STATUS_CMD` at all** — a GET_STATUS
  broadcast from the ESP is silently ignored on arrival. `link_task_send_status()`
  (line 781) is instead called from the main loop's own free-running gate,
  `if ((now - last_status_tx) >= pdMS_TO_TICKS(LINK_STATUS_TX_PERIOD_MS))`
  (line 2623, `LINK_STATUS_TX_PERIOD_MS = 500`, line 182) — a periodic push
  driven entirely by the Pico's own clock, with no causal relationship to any
  ESP request.
- **ESP side** (`firmware/KilnFW/App/drivers/safety/safety_link_inbox.c`,
  `safety_exchange()` ~line 712): still sends GET_STATUS every
  `SAFETY_POLL_PERIOD_MS` (500 ms) and waits up to `SAFETY_LINK_REPLY_TIMEOUT_MS`
  (~345 ms at 230400 baud, `safety_link.h:586`) for a STATUS frame to land,
  incrementing `stats.timeouts` if none does inside that window
  (`safety_link_inbox.c:794` area) and `link_reply_us` if one does. The
  function's own comment (`safety_link_inbox.c:772-778`) asserts "the STATUS
  frame that just satisfied the wait ... cannot be anything other than the
  answer to the request sent a few lines up" — **that assumption is false**
  for this command once the Pico stopped answering it on request (which per
  the doc excerpt above predates today entirely): the STATUS frame that lands
  inside or outside the wait window is whichever one the Pico's independent
  500 ms clock happens to emit next, not a reply caused by the ESP's send.

## Why this produces a persistent ~100%, not a random ~69%

Both cadences are 500 ms, driven by independent free-running tick counts with
no resync mechanism between them. The ESP's wait window (345 ms) covers 69%
of its own 500 ms cycle, so if the two clocks' relative phase were random on
every cycle, roughly 69% of polls would coincidentally match. They do not
drift relative to each other tick-by-tick, though (no external interrupt
re-synchronizes them): once a phase offset is set at boot, it stays
essentially fixed for the life of that boot, up to task-scheduling jitter.
That makes this fundamentally bimodal, not a rate: a given boot's phase
offset either falls inside the 345 ms hit window (near-100% match for the
rest of that boot, absent jitter) or in the 155 ms miss gap (near-100% miss)
— exactly the frozen `link_reply_us.count = 696` observed for the entire
investigation window (multiple direct reads of `/api/diagnostics/timing`
several minutes apart, `count` unchanged each time) while `timeouts` climbed
at the same rate as `sent` (`safety_get_link_stats`: sent 1126→1142→1179→1216,
timeouts 406→422→459→496, i.e. every single poll in each interval missing).
This session's boot (following the recent flash-triggered dual reset) landed
in the miss half; that is plausibly why "today" is when this was noticed, but
nothing in `c27484a2` (the stack/heap commit) touches `LINK_STATUS_TX_PERIOD_MS`,
`SAFETY_POLL_PERIOD_MS`, or `SAFETY_LINK_REPLY_TIMEOUT_MS` — confirmed by
diffing that commit, which only changes `LINK_TASK_STACK_WORDS`,
`UPDATE_TASK_STACK_WORDS`, and `configTOTAL_HEAP_SIZE`. The phase-lock hazard
itself is a pre-existing, protocol-acknowledged property of a "push, not a
poll" design paired with a `safety_exchange()` implementation that was never
updated to stop treating it as a matched request/reply after that redesign —
not a new defect introduced today.

## Suspects checked and cleared

- **Today's seqlock (`b202fe56`, `cb1ba325`)**: guards `s_cached_record` in
  `firmware/SaftyFW/src/config_store_flash.c`, read by the trip path and
  written by `link_task` only on `SET_CONFIG`/`COMMIT_CONFIG`. Read
  `s_seq_counter` live via `debug_read_symbol(peer="pico", symbol="s_seq_counter")`:
  **value 0 (even/stable)**, not stuck mid-write. `cmd_config_page_count`
  stayed flat at 15 across the whole investigation (no active refetch
  churning). Not implicated.
- **Today's stack bumps (`c27484a2`)**: `LINK_TASK_STACK_WORDS` and
  `UPDATE_TASK_STACK_WORDS` are both expressed as
  `configMINIMAL_STACK_SIZE * N`, and this port's `configMINIMAL_STACK_SIZE`
  (256, `FreeRTOSConfig.h:60`) is itself in words, matching every other task
  in this file (`current_task.c`, `safety_core.c`, `thermo_task.c` all use
  the identical pattern and are documented as 256 words = 1 KB) — **not** a
  bytes-vs-words units bug of the kind flagged as a risk. ESP-side task
  stack margins (`get_stack_margin()`) are all healthy (36-81% headroom,
  including `safety_poll` 57.3% and `safety_proto_rx` 42.6%, the two ESP
  tasks that own this link), so ESP-side scheduling starvation is not the
  cause either.
- **Genuine frame loss**: `crc/framing errors` stayed flat at 3 across the
  entire session; `frames_deframed`/`dequeued_total` and
  `cmd_status_count`/`diag_applied`/`power_applied` all climbed at healthy,
  steady rates throughout. A link dropping frames would show these stalling
  or CRC errors climbing; neither happened.
- **Safety-guard reliability**: `safety_link_up_locked()`
  (`firmware/KilnFW/App/drivers/safety/safety_link.c:259`) — the function
  S6a/S6b actually key off — is computed from `safety_age_ms_locked()`
  (time since the last successfully-applied STATUS/DIAG/POWER frame,
  regardless of the request/reply pairing) compared against
  `poll_period_ms * SAFETY_LINK_UP_PERIODS` (3, i.e. 1500 ms), **not** from
  `stats.timeouts` or `link_reply_us` at all. Every valid decoded frame
  updates `s_last_valid_frame_tick`/`s_valid_frame_seen`
  (`firmware/SaftyFW/src/tasks/link_task.c:2347-2348`) independent of frame
  type. `trip_mask` read `0x0000` throughout, `safety_get_diag` reported
  "link up" the whole time, and status data (thermocouples, `ct_counts`)
  stayed fresh (`age_ms` 220 ms at one sample) — consistent with the guard
  path being unaffected by this counter's misbehavior.

## What is not fully closed

I did not capture a raw UART trace (the `saleae` MCP server was unavailable
this session — connection refused) to directly observe the two clocks'
relative phase and prove the bimodal mechanism beyond the frozen-counter/
climbing-timeout signature and the source-level design. The explanation
above is strongly supported by code and live counters but stops short of an
oscilloscope-level confirmation.

## Follow-up defect, not fixed here — for whoever picks this up

**This is not merely a stale/unhelpful counter; the ESP-side accounting is
factually wrong and says so out loud.** `safety_exchange()`'s comment at
`firmware/KilnFW/App/drivers/safety/safety_link_inbox.c:772-778` asserts "the
STATUS frame that just satisfied the wait ... cannot be anything other than
the answer to the request sent a few lines up." That is false for this
command: per `LINK_PROTOCOL.md`'s own "no longer a poll" note and
`link_task.c`'s dispatch switch (no `GET_STATUS` case at all), the Pico never
answers this request; any STATUS frame the ESP sees is its independent
500 ms push, not a reply. The practical consequence: **this counter is
guaranteed to read either ~0% or ~100% "failure" for the entire life of every
boot**, permanently, with a false comment sitting next to it explaining why
that can't be a bug. That is exactly the shape that costs the next
investigator hours re-deriving what this pass already found — do not let
this be re-discovered from scratch.

**The fix, deliberately not made in this pass** (board stays idle, no flash,
per the instruction that started this investigation): either stop
incrementing `stats.timeouts`/`link_reply_us` for `SAFETY_CMD_GET_STATUS`
entirely, or send it with `expect_status=false`, the same way the
`!peer_version_known` `FW_VERSION` request already does
(`safety_link_poll.c:432-446`) — and correct the now-disproven comment at
`safety_link_inbox.c:772-778` in the same change. This belongs in its own
pass with its own build/flash/verification, not folded into a capture that
was only gated on this counter, not caused by it.

## Recommendation

- **This link is safe to run the coupling capture on.** The counters worth
  watching are the ones the safety guard path itself already uses:
  `crc/framing errors` (should stay flat), `cmd_status_count`/`diag_applied`/
  `power_applied` (should keep climbing at a steady rate), and
  `link_reply_us.count`/`max` (an erratic *change* — new CRC errors, a stall
  in the climbing counters, or `link_reply_us.count` suddenly moving — is the
  real signal). **`stats.timeouts` climbing at the `sent` rate is this link's
  ordinary, already-present behavior this boot and should not itself gate or
  abort a run.** `docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md` is updated
  with this corrected watch-list.
- **Worth an owner decision, not fixed here per the no-flash constraint on
  this pass:** `safety_exchange()`'s GET_STATUS path
  (`safety_link_inbox.c`/`safety_link_poll.c`) still carries request/reply
  timing logic and a misleading doc comment for a command the protocol
  itself says is push-only. Either stop counting GET_STATUS-specific
  timeouts at all (matching the "nothing depends on it" framing in
  `LINK_PROTOCOL.md`), or send it as `expect_status=false` the way the
  `!peer_version_known` FW_VERSION request already is (`safety_link_poll.c:432-446`),
  so this stat stops reading an artifact of protocol-vestigial bookkeeping as
  if it measured real link health.
