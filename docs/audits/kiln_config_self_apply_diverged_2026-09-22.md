# `POST /api/kiln_configs/apply` reports `diverged` on a no-op self-apply — 2026-09-22

Source-only audit. HEAD `3534c1a6`. No board was touched.

## The observation

On the bench (ESP `63a48ab3`, Pico `05f1ab1f`), `POST /api/kiln_configs/apply`
for `id=1` — the only saved kiln config, already active, so a self-apply whose
blob is byte-identical to what is live — returned 202 and then settled at

    state=done_failed, diverged=true,
    reason="both halves committed and matched, but the post-swap ceiling/arming
    check failed (Pico reports no config_crc after the swap) -- heaters disabled
    and alarmed, config left pending for retry"

Afterwards `active_id` was still 1, zones unchanged, the safety link up / armed /
not tripped, no reboot, readiness unchanged, and nothing alarmed on the safety
side. Earlier the same day the W42 row (a kiln-config apply driven through the
web page) PASSED live against this same firmware.

## Verdict

**The `config_crc` half of the post-swap check is broken by construction: the
swap itself sets the cached CRC to 0 a few steps earlier, and then treats that
same 0 as evidence that the Pico is unconfigured.** The check can only pass when
`safety_poll_task` happens to land a concurrent refetch inside the window
between the swap's own readback and the check — a ~500 ms-period coin flip. The
`diverged=true` result is a false alarm; the swap itself completed correctly on
both halves, which is exactly what the bench state showed. Confidence: high for
the mechanism (it is a direct data-flow read), medium-high for "a concurrent
poll refetch is what makes the lucky runs pass", since that part is inferred
from the code plus the fact that the two opposite outcomes came from
byte-identical firmware.

## 1. What the check actually tests

`kiln_cfg_swap_apply_impl()` runs the swap. Step 10/11, after both halves have
committed and been read back, is:

    firmware/KilnFW/App/drivers/persist/kiln_cfg_swap.c:738
        safety_ceiling_sync_reconcile_on_link_up(link);
    firmware/KilnFW/App/drivers/persist/kiln_cfg_swap.c:741
        bool diverged = safety_ceiling_sync_is_diverged(div_reason, sizeof(div_reason));
    firmware/KilnFW/App/drivers/persist/kiln_cfg_swap.c:747-750
        if (!diverged && safety_cfg_store_cached_crc() == 0) {
            diverged = true;
            snprintf(div_reason, sizeof(div_reason), "Pico reports no config_crc after the swap");
        }

So the message names the *second*, separate condition, not the ceiling/arming
comparator: the first test (`safety_ceiling_sync_is_diverged()`, the
abs_max_temp_c identity latch) passed; what fired is the `cached_crc == 0`
approximation, whose own comment at `kiln_cfg_swap.c:742-746` calls it
"defensive, not the primary signal" for "a Pico that has never reported ANY
config_crc this boot".

The value it reads is not a live frame field. `safety_cfg_store_cached_crc()`
returns `s_store.config_crc`
(`firmware/KilnFW/App/drivers/safety/safety_cfg_store.c:1294-1297`), the ESP's
own cache tag. "No config_crc" means literally `== 0`, the sentinel set by
`reset_to_defaults()` at `safety_cfg_store.c:503` (`/* never fetched */`).

The Pico's real, live CRC is a different variable entirely: it arrives on
FW_VERSION frames as `link->peer_config_crc` and is fed to the cache-freshness
comparator once per poll tick at
`firmware/KilnFW/App/drivers/safety/safety_link_poll.c:155-169`
(`safety_sync_cfg_cache()` → `safety_cfg_store_maybe_refetch(link, live_crc)`).
The check at `kiln_cfg_swap.c:747` never looks at it.

## 2. Why it fails on a no-op self-apply — and on any apply

Candidate (e), timing, is the one that survives; the rest are ruled out.

The decisive coupling is that the cache's CRC tag is **whatever the caller
passed in**, not what the Pico reported:

    firmware/KilnFW/App/drivers/safety/safety_cfg_store.c:1524
        scratch->config_crc = config_crc;
    firmware/KilnFW/App/drivers/safety/safety_cfg_store.c:1674
        s_store = *scratch;

and the swap's own readback deliberately passes 0 in order to force an
unconditional refetch:

    firmware/KilnFW/App/drivers/persist/kiln_cfg_swap.c:329-336
        /* Force a FRESH GET_CONFIG_PAGE round trip ... config_crc=0 forces the
         * unconditional-refetch path (safety_cfg_store_refetch()'s own
         * contract: 0 never matches a real config_crc). */
        if (!safety_cfg_store_refetch(link, 0)) {

That call is inside `push_and_verify_pico()`, which step 6/7 runs
unconditionally (`kiln_cfg_swap.c:619`). A **successful** refetch therefore
leaves `s_store.config_crc == 0` — the very state the step-10 check reads as
"the Pico has never reported a config_crc this boot". Between the refetch and
the check nothing else refreshes it: the intervening work
(`persist_marker(PICO_DONE)` at `:634`, the generation check, the ESP import at
`:664`, `persist_marker(ESP_DONE)` at `:683`, the export readback,
`kiln_cfg_store_set_active_id_raw()` at `:723`,
`safety_ceiling_sync_reconcile_on_link_up()` at `:738`) is ESP-local NVS work
plus a ceiling raise attempt; none of them call a refetch.

The only writer that can restore a non-zero tag in that window is the other
task: `safety_poll_task`, every `CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS`
(default 500, `firmware/KilnFW/App/drivers/Kconfig:843-846`), calls
`safety_cfg_store_maybe_refetch()`. With the cache at 0 and the peer's CRC
non-zero the mismatch branch is taken
(`safety_cfg_store.c:1806-1830`), the backoff is not in force (a successful
refetch left `s_retry_not_before_us = 0`), and
`safety_cfg_store_refetch_nonblocking()` performs the whole multi-page fetch
under a `xSemaphoreTake(..., 0)` try-lock (`safety_cfg_store.c:1758-1776`),
this time tagging the cache with the *real* live CRC. If that tick lands inside
the swap's window, `cached_crc != 0` at `:747` and the swap reports `done_ok`.
If it does not, the swap reports `diverged`.

The swap's post-readback window is dominated by three NVS writes and a possible
UART ceiling round trip, so it is plausibly of the same order as the 500 ms poll
period — which is precisely the shape that produces both outcomes from one
firmware image. That W42 passed and this run failed on **byte-identical code**
is itself evidence: `git diff 7dcde0dd 63a48ab3 -- firmware/KilnFW/App/drivers/persist/
firmware/KilnFW/App/drivers/safety/ firmware/KilnFW/App/drivers/http/kiln_cfg_http.c`
is empty. Nothing about content, identity, or the ARMED window distinguishes the
two runs in this code path.

Ruling out the other candidates:

- **(a) "the Pico only re-sends config_crc after a write, and an identical write
  is skipped".** Not the mechanism. The check does not wait for a fresh peer
  CRC at all; it reads the ESP's own cache tag. And the swap's Pico push is a
  *volatile* install (`kiln_cfg_swap.c:619`, `volatile_install=true`), which by
  design does not go through the flash-writing COMMIT_CONFIG path, so the
  Pico's reported `config_crc` is not expected to change for any apply,
  identical content or not. The check would be equally satisfied by the stale
  pre-swap CRC — it only wants "non-zero".
- **(b) "the ESP clears its cached crc before the swap and waits for a status
  frame that carries crc only in some states".** Half right and worth stating
  precisely: the ESP does clear its cached CRC — but *during* the swap, at
  `kiln_cfg_swap.c:334`, not before it, and it is not waiting for anything. The
  peer CRC rides every FW_VERSION frame (`safety_link_poll.c:143-152`), with no
  ARMED/GRACE gating.
- **(c) "the Pico's config-write window was closed, so the Pico half was refused
  or skipped".** Ruled out by the reason string itself: it is emitted only
  after `push_and_verify_pico()` returned true, which requires the field-by-field
  `pico_readback_matches()` comparison against a fresh `GET_CONFIG_PAGE` to
  have succeeded (`kiln_cfg_swap.c:338`). A refused or skipped Pico half exits
  at `:620-631` with a different message ("swap refused, rolled back cleanly").
  The volatile path exists specifically so this never needs the disarmed
  flash-write window.
- **(d) a reset-one-side pairing.** This *is* one, structurally — it is the
  cleanest instance of the class in `CLAUDE.md`'s list. Two pieces of state in
  different modules joined by an unexpressed contract: `safety_cfg_store`'s
  `config_crc` doubles as both "the tag identifying which Pico config this cache
  mirrors" and "have we ever fetched anything" (`:365`, `/* 0 = never fetched */`).
  `kiln_cfg_swap.c` legitimately overloads the first meaning (0 as "match
  nothing, force a fetch") and a second consumer in the same file then reads the
  second meaning off the same field. Nothing forces the two to be revisited
  together.

## 3. "heaters disabled and alarmed" is not true on this path

The observed bench state — no alarm, readiness unchanged, link armed — is
consistent with the code; the message overstates what happened.

All heat-off enforcement lives in `enforce_ceiling_divergence()`
(`firmware/KilnFW/App/drivers/safety/safety_ceiling_sync.c:339`) and fires only
in the `ceiling_diverged` branch:

    firmware/KilnFW/App/drivers/safety/safety_ceiling_sync.c:516-521
        if (s_disable_all_relays_off) { s_disable_all_relays_off(); }
        if (s_disable_halt_run)       { s_disable_halt_run(); }

and it is that branch alone that sets the latch `s_divergence_active` (`:529`)
which `safety_ceiling_sync_is_diverged()` (`:180-189`) publishes to the status
surfaces. In this incident that comparator returned **false** — the ceiling
matched — which is exactly why control reached the `cached_crc == 0` clause at
`:747`. That clause sets only a local `bool diverged` and formats a string
(`kiln_cfg_swap.c:751-762`). It calls no disable-heat hook, sets no latch, and
raises nothing any status route can see. The only trace it leaves outside the
HTTP reply is a log line that repeats the same false claim:

    firmware/KilnFW/App/drivers/persist/kiln_cfg_swap_worker.c:121
        ESP_LOGE(TAG, "kiln config swap target_id=%ld left the board DIVERGED (heaters disabled): %s", ...)

"Config left pending for retry" *is* accurate: `clear_pending()` at
`kiln_cfg_swap.c:770` is skipped on this exit, so the pending record stays at
marker `ESP_DONE` for boot recovery. Note that `active_id` was already moved to
the target at `:723`, before this check, deliberately (see the step-12 comment
at `:706-719`), so nothing about the store is wrong either.

So the operator-visible message is wrong in both directions: it reports a
safety action that did not happen, and it reports a failure for a swap that
succeeded.

## Why the host tests did not catch this

`firmware/KilnFW/App/test/test_kiln_cfg_swap.c` fakes both halves of the
coupling apart. Its `safety_cfg_store_refetch()` stub discards the argument
entirely:

    firmware/KilnFW/App/test/test_kiln_cfg_swap.c:326-333
        bool safety_cfg_store_refetch(SafetyLinkClass *link, uint16_t config_crc)
        { (void)config_crc; ... return true; }

while the cached CRC is an independent variable pinned non-zero:

    firmware/KilnFW/App/test/test_kiln_cfg_swap.c:372-373
        static uint16_t s_cached_crc = 1; // non-zero = "configured" ...
        uint16_t safety_cfg_store_cached_crc(void) { return s_cached_crc; }

The one line of production behaviour that matters here — `refetch(link, 0)`
sets the cached CRC to 0 — is the line the fake deletes. This is the
"idealized test input" class: the stub models the success/failure axis the test
author was thinking about and silently drops the data flow that the defect
lives in.

## 4. Recommended fix

Smallest correct change, firmware, one file:

**Delete the `cached_crc == 0` clause at
`firmware/KilnFW/App/drivers/persist/kiln_cfg_swap.c:747-750`** (and its
comment at `:742-746`).

Rationale, in the clause's own words: it is an "approximation pending item 16
(the UNCONFIGURED flag is not on the wire yet)" and explicitly "defensive, not
the primary signal — the field-by-field readback above is what actually proves
the Pico is configured with P." That readback
(`pico_readback_matches()`, reached from `kiln_cfg_swap.c:338`) has already run
and passed before this clause is evaluated, against a cache filled by a fresh
`GET_CONFIG_PAGE` round trip in the same breath. The clause adds no information;
it can only report the swap's own bookkeeping back at itself. Removing it leaves
the real cross-check — `safety_ceiling_sync_is_diverged()` at `:741`, which does
compare the ESP's target ceiling against the Pico's own fetched value and does
enforce heat-off — fully in place.

If the defensive intent is to be kept rather than dropped, the only honest form
is to test the peer's live value instead of the ESP's cache tag: read
`link->peer_config_crc` (via the `safety_lock()`-guarded pattern at
`safety_link_poll.c:158-165`) together with `peer_build_known`, and treat
"`peer_build_known` and `peer_config_crc == 0`" as the unconfigured signal. That
is strictly more code for the same coverage, so the deletion is preferred until
item 16's real UNCONFIGURED flag lands.

Separately, and independently of which option is taken:

- **Fix the message.** The `diverged` exit at `kiln_cfg_swap.c:759-761` and the
  log at `kiln_cfg_swap_worker.c:121` should state what actually happens on that
  path. Heat-off is `enforce_ceiling_divergence()`'s to claim, not this one's;
  the swap exit should say the config is left pending for retry and that the
  divergence latch (if any) is what governs heat.

**Negative test.** Add to `firmware/KilnFW/App/test/test_kiln_cfg_swap.c` a case
that reproduces the coupling rather than faking around it: make the
`safety_cfg_store_refetch()` stub honour its contract —
`s_cached_crc = config_crc;` on success — and assert that a successful apply
still returns true with `diverged == false`. Against the current source that
test fails with exactly the observed reason string (the stub's `refetch(link, 0)`
zeroes `s_cached_crc`, the step-10 clause fires); against the fix it passes. To
negative-test the fix itself, re-introduce the deleted clause by hand, rebuild
the host-test executable from scratch (do not measure a prebuilt `.exe`), and
confirm the new case fails.

### Does "short-circuit a self-apply to done_ok" hide a real defect?

Yes — do not do that. It is the wrong fix for two reasons. First, it does not
describe this defect: the failure is not specific to identical content or to
`target_id == active_id`; the same clause fires on any apply whose window misses
a poll tick, and W42's differing-identity apply is subject to it too. Second, a
self-apply is the one case where the Pico's half is most worth re-pushing and
re-verifying — a board whose Pico rebooted, was reflashed, or was volatile-
written by the commissioning page can be genuinely out of sync with an unchanged
ESP config, and re-applying the active kiln is the operator's normal remedy.
Short-circuiting it would turn that remedy into a no-op while reporting success,
which is a worse failure than the current false alarm.

## Secondary finding (not the cause, worth a separate pass)

`enforce_ceiling_divergence()` uses three file-scope `static` arrays
(`safety_ceiling_sync.c:391-393`, ~1.5 KB) and justifies them at `:385-387` with
"this function is provably only ever called from safety_poll_task ... so there
is no reentrancy hazard". That premise is no longer true: `kiln_cfg_swap.c:738`
calls `safety_ceiling_sync_reconcile_on_link_up()` — which calls
`enforce_ceiling_divergence()` unconditionally at `:673` — from the dedicated
`kiln_cfg_swap_worker` task. The two tasks can therefore be inside those shared
buffers at once, and the same call also mutates the rate-limit statics
`now_us`/`s_last_log_us`/`s_reconcile_backoff`. The observable damage is bounded
(a garbled reason string, a mis-scheduled backoff), but the comment's claim
should be corrected and the arrays made safe, or the swap's call moved onto the
poll task.
