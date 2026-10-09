# `link_reply_us` composition audit, 2026-09-14

**Trigger.** The `f2bb46bb` hardware measurement closing the link-latency item
in `docs/HW_ABSTRACTION.md` (n=2139, min 6.5 ms, mean 135 ms, median ~142 ms,
p90 ~222 ms, max 321 ms against a 345 ms budget) has a shape that does not
look like transport latency: a minimum near zero with a mean/median sitting
near half the observed range is the signature of waiting on the phase of a
periodic producer, not of a round trip clustered near a floor. This audit
checks that hypothesis against source and against the live board.

## 1. What `safety_exchange()` actually waits for

`firmware/KilnFW/App/drivers/safety/safety_link_inbox.c:751-884`, the
`expect_status` branch (lines 810-822):

> "A STATUS frame landed inside the wait window. Note what this is NOT proof
> of: xact_lock only rules out a second *exchange* racing this one, it does
> not make the frame a caused reply -- SAFETY_CMD_GET_STATUS has no dispatch
> case on the Pico (LINK_PROTOCOL.md "no longer a poll"; SaftyFW's
> link_task.c never answers this request), so any STATUS frame seen here is
> that peer's own free-running 500 ms push, which may or may not have
> anything to do with the send a few lines up. What is timed below is
> therefore 'how long until a status push happened to land after we asked',
> not a measured reply latency."

This is not a new finding produced by this audit -- it is already stated,
verbatim, in the production comment above the exact code that increments
`link_reply_us_count`/`_last`/`_mean` (`safety_link_inbox.c:823-836`). The
match is `safety_drain_inbox_for_status()` (`safety_link_inbox.c:739-742`,
implemented via `safety_drain_inbox_ex(..., true, ...)`), which accepts the
**next STATUS-typed frame of any kind**, not a frame keyed to this request --
`SAFETY_CMD_GET_STATUS` carries no wire sequence/message id for a reply to
key on (see `safety_link_stats_t::link_reply_us_count`'s own doc comment,
`safety_link.h:1147-1153`, for the same point). `safety_poll_task()`'s own
header comment (`firmware/KilnFW/App/drivers/safety/safety_link_poll.c:372-`
`388`) confirms the send is a deliberate ESP->Pico liveness heartbeat that the
Pico's `link_task.c` dispatch switch has no case for and never answers --
"what genuinely IS dead on the ESP side is only the 'wait for a matched
reply' half of this exchange."

**Verdict on Q1: confirmed by source, not inferred.** `safety_exchange()`
does not match a specific reply command for `GET_STATUS`; it accepts the
next inbound STATUS-typed frame of any origin, which on this link is always
the Pico's independent 500 ms push, never a caused reply.

## 2. Per-command decomposition

There is no decomposition to perform, because there is only one contributor.
`link_reply_us` is written from exactly one call site
(`safety_link_inbox.c:797-836`, inside `safety_exchange()`'s
`expect_status` branch), and `expect_status=true` has exactly two callers in
the whole tree:

- `safety_link_poll.c:439` -- the steady-state poll loop's own
  `SAFETY_CMD_GET_STATUS` send (`request[]` at line 389).
- `safety_link.c:732-738` -- `safety_link_ping()` ("Same GET_STATUS send as
  `safety_poll_task()`"), sending the identical `SAFETY_CMD_GET_STATUS`
  request.

Every other `safety_exchange()` call in the tree passes `expect_status=false`
(e.g. the FW_VERSION request at `safety_link_poll.c:507`; the send at
`safety_link.c:721`), and those calls never touch `link_reply_us` at all --
`reply_start_us` is only set (`safety_link_inbox.c:797`) and only consumed
under `if (expect_status)`. So the composition is **100% `GET_STATUS`
phase-wait samples, 0% anything else** -- not a mix, and not something a
finer-grained board counter could split further, because the counter
literally has no other source.

Genuinely reply-matched commands exist elsewhere on this link (`GET_CONFIG_
PAGE`, matched by page id in `safety_link_commands.c`) but they run through
different code paths that were never wired into `link_reply_us`. The
strongest same-repo evidence for what a genuine matched reply actually costs
is `safety_link.h:591-596`'s note on SaftyFW's own `s_page_reply_us_max`
(decode-to-send-return on the Pico side): "never exceeded ~900 us live."
Combined with the wire-time note two paragraphs above it in the same file
(`safety_link.h:562`, "a frame is ~2 ms on the wire at 115200"), a genuinely
matched exchange should cost roughly 1-5 ms end to end -- consistent with
`link_reply_us`'s own observed **minimum** of 6.5 ms (the lucky case where
the periodic push happens to land almost immediately after the send), and
nowhere near explaining a mean of 135 ms or a max of 321 ms.

## 3. Live board check (read-only)

Board at the time of this check: `reset_reason='software (esp_restart)'`,
uptime 2729-2780 s, link up, SaftyFW armed, no trip, no unacknowledged crash
(`get_heap_status`), `safety_get_status()`: "link up; SaftyFW armed (relay_
owner not tripped); safety thermocouple valid ... 60 ms old". Baseline
`safety_get_link_stats()`: `sent 2687 ... timeouts 3 ... cmd histogram
[status=5479 fw_version=1 ... config_page=3 ...]`.

Issued one bounded `kiln_batch` of 5 calls: `safety_get_commissioning` x3
interleaved with `safety_ping` x2 (read-only; `safety_get_commissioning`
reads `GET /api/safety/commissioning`, which triggers a live `GET_CONFIG_
PAGE` fetch only when its cached config CRC has gone stale). Result: all 5
calls `ok`. Post-check `safety_get_link_stats()`: `sent 2696 ... timeouts 3
... cmd histogram [status=5550 ... config_page=3 ...]` -- `config_page`
unchanged, because the cached config CRC already matched live (`config CRC
17058 (matches live, not stale)` in every `safety_get_commissioning` reply),
so this run did not need to issue a fresh `GET_CONFIG_PAGE` round trip.
Forcing a stale-config refetch just to sample that path was judged out of
scope for a read-only, low-risk pass (it would require perturbing config
state), so no fresh genuinely-replied-command timing sample was collected
live this session beyond the historical `s_page_reply_us_max` figure in
Section 2. `link_reply_us` moved `2139 -> 2140` (only 1 of 2 `safety_ping`
calls landed a fresh phase-matched sample within its own call; timeouts
stayed at 3), `min`/`max` unchanged at 6507/321187 us -- consistent with a
handful of additional phase-wait draws from the same underlying process
rather than any new extreme.

Board left exactly as found: link up, Pico armed, no trip, no unacknowledged
crash, relays off (`currents not fitted, not fitted, 0.02 A`), timeouts
counter unchanged by this session's calls (still 3, all attributable to the
2026-09-14 `f2bb46bb` measurement's own back-to-back bursts per that entry).

## 4. Verdict

**Confirmed, not refuted.** `link_reply_us` is measuring wait-for-next-
periodic-push (phase wait against the Pico's free-running 500 ms `STATUS`
push), not transport/round-trip latency, for 100% of its samples -- there is
no mixed population to separate, because `GET_STATUS` is the only command
that ever feeds this counter. The near-uniform spread from ~6.5 ms to ~321 ms
around a mean/median near half the 500 ms period is exactly the shape phase-
wait against a 500 ms producer produces, and is corroborated by the same
file's own evidence that a genuinely matched reply on this link costs on the
order of 1-5 ms (`~900 us` Pico-side dispatch + `~2 ms` wire time), nowhere
near the observed mean or max.

Consequences, as anticipated:

- **The 345 ms budget was written for something else and should not be
  graded against this counter.** `SAFETY_LINK_REPLY_TIMEOUT_MS`'s derivation
  (`safety_link.h:565-596`) is explicitly about *transport* margin -- wire
  time doubled plus jitter margin -- for commands that DO get a matched
  reply (its own worked example is `GET_CONFIG_PAGE`). `link_reply_us` in
  its current form never measures that quantity for `GET_STATUS`, so
  "93% of budget" in the `f2bb46bb` closure is not a transport-margin
  finding -- it is an observation about how late a 500 ms push can land
  after an unrelated `ping`, which is bounded by the poll period and
  `safety_drain_inbox_for_status()`'s own wait budget, not by wire speed.
- **The number will not respond to transport improvements at all.** Raising
  baud, shrinking frames, or tightening framing overhead cannot move a
  phase-wait distribution whose ceiling is set by the Pico's 500 ms push
  cadence and the ESP's own poll period, not by anything on the wire.
- **A consumer alarming on `link_reply_us` (mean or max) is alarming on poll
  phase, not link health.** A perfectly healthy link where the periodic push
  happens to land late relative to an `expect_status` send reads exactly
  like a slow/degraded link by this metric, and a link silently losing every
  other push could -- depending on when the surviving pushes land -- still
  produce ordinary-looking phase-wait numbers. This is the same
  distinction the existing `safety_link_inbox.c:838-874` comment already
  draws for why a single-window timeout here is deliberately NOT folded into
  `stats.timeouts`: this window is "exactly as likely on a healthy link ...
  as it is on a dead one." The same reasoning applies one level up, to the
  distribution itself, not just to a single miss.

## 5. What a consumer should alarm on instead

Not `link_reply_us` at all, in its current form, for link-health purposes.
The number that already measures the property a health alarm actually wants
-- "is the peer's periodic push arriving on schedule" -- is
`safety_get_link_stats()`'s `timeouts`, which per the existing
`safety_link_inbox.c:857-874` comment is written exclusively by
`safety_poll_task()`'s own elapsed-time-based push-gap check
(`safety_link_poll.c`), independent of this request-phase-dependent window.
That is the metric that is actually blind to phase and sensitive to real
loss. `link_reply_us` remains useful only for what it undisputedly does
measure -- "how promptly does a forced `safety_ping()` see the next push" --
which is a diagnostic of poll/scheduling behavior, not of transport or of
link health, and should be labelled that way wherever it is surfaced
(`GET /api/diagnostics/timing`, `get_heap_status`'s printed block). The
`f2bb46bb` recommendation to prefer `max` over `mean` "if this is ever used
as a live health signal" should be read in light of this: neither statistic
of this particular counter is a sound link-health signal, `timeouts` is.

**This audit does not change the instrumentation, the 345 ms budget, or the
`f2bb46bb` prose in `docs/HW_ABSTRACTION.md`'s closure paragraph itself** --
seeing `link_reply_us` retargeted to a real matched-reply command (e.g.
timing `GET_CONFIG_PAGE`/`GET_DIAG`-class exchanges instead of `GET_STATUS`,
or being renamed/documented as "poll phase-wait" rather than "link reply
latency") is a design decision for whoever owns that counter next, not a
cleanup folded into this read-only pass. What this audit adds is a pointer
from the closure paragraph (see `docs/HW_ABSTRACTION.md`) to this file, so
the next reader does not take "item closes" to mean "this measures
transport latency."

Related: `c8f7506b` (board git state at the time of the `f2bb46bb`
measurement), `firmware/CommonFW/docs/LINK_PROTOCOL.md` ("SAFETY_CMD_
GET_STATUS ... no longer a poll"), `docs/audits/
safety_link_get_status_timeout_counter_2026-09-10.md` (the prior audit that
established the no-dispatch-case fact this one builds on).
