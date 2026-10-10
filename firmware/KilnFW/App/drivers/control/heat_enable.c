#include "heat_enable.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "heat_enable";

/* Passive-poll budget for he_flush_release_blocking(): HE_FLUSH_MAX_ATTEMPTS x
 * HE_FLUSH_RETRY_MS = 200 ms of lock-check-and-sleep, spent waiting for a
 * release that is ALREADY in flight (someone else's send) to land.
 *
 * 2026-09-15 review of 059a896e, HIGH-1: this comment used to claim 200 ms
 * was the function's TOTAL bound, which was wrong -- the old loop body called
 * heat_enable_service_pending_release() itself every iteration, and that call
 * runs the blocking link exchange, not a poll. The true worst case is ONE
 * such exchange, plus this 200 ms passive poll on top -- not 200 ms.
 *
 * DERIVATION, corrected 2026-09-15 (review of 8813bedd, finding on the
 * timing comment). The previous version of this text put the bound at
 * "~5.5 s = 5000 ms xact_lock + ~345 ms reply + 200 ms poll". The ~345 ms
 * reply term DOES NOT EXIST on this path, and the number was never
 * reachable. Traced through the source rather than taken on trust:
 *
 *   safety_link_request_enable()  (safety_link.c:652) ends with
 *       return safety_exchange(link, request, sizeof(request), false);
 *   -- note expect_status == false.
 *   safety_exchange()             (safety_link_inbox.c:745) takes xact_lock
 *   with SAFETY_XACT_LOCK_TIMEOUT_MS, drains the inbox with a 0 ms wait,
 *   calls uart_protocol_send_broadcast(), and enters the
 *   SAFETY_LINK_REPLY_TIMEOUT_MS wait ONLY inside `if (expect_status)`.
 *
 * SAFETY_CMD_REQUEST_ENABLE is a fire-and-forget BROADCAST: no ACK, no
 * reply, nothing to wait for. So the only blocking term is the xact_lock
 * acquisition, SAFETY_XACT_LOCK_TIMEOUT_MS = 5000 ms
 * (safety_link_internal.h:46). True worst case:
 *
 *      5000 ms (xact_lock timeout) + 200 ms (this passive poll) = ~5.2 s
 *
 * entirely lock contention. For scale, the longest holder that contention
 * can actually be waiting behind is the rollback burst in
 * safety_link_commands.c (SAFETY_LINK_ROLLBACK_SEND_REPEATS 4 x
 * SAFETY_LINK_ROLLBACK_SEND_REPEAT_GAP_MS 250, plus one reply wait, about
 * 1345 ms), not an ordinary exchange -- so 5.2 s is the TIMEOUT bound, not a
 * routinely-observed duration. Quote 5.2 s, not 5.5 s, and do not
 * reintroduce a reply term for a command that has no reply.
 *
 * TASK WDT DEPENDENCY, named here deliberately: a 5.2 s block is LONGER than
 * this project's Task WDT period (CONFIG_ESP_TASK_WDT_TIMEOUT_S = 5, with
 * CONFIG_ESP_TASK_WDT_PANIC=y -- expiry PANICS, it does not merely warn).
 * The only reason that is not a reset today is that there are currently ZERO
 * esp_task_wdt_add() call sites anywhere in this tree, so no task is
 * subscribed and no task's silence is being timed. That is a property of the
 * tree, not a safety argument. WHOEVER FIRST SUBSCRIBES A TASK TO THE TASK
 * WDT must check whether that task can reach this path (today: any caller of
 * heat_enable_acquire*() or heat_enable_reconcile() -- httpd, the UART
 * bridge, profile_exec_wdt, autotune) and either exclude it, feed the
 * watchdog across this wait, or move the blocking call off that task. Found
 * here is far cheaper than found from a panic. That whole worst case now runs
 * on whichever task calls heat_enable_acquire() (httpd, the UART bridge, or
 * profile_executor/autotune_engine's own tick), because he_flush_release_
 * blocking() is only ever allowed to DRIVE the blocking exchange once per
 * call -- see its own comment. This constant only bounds the passive-poll
 * remainder. */
#define HE_FLUSH_MAX_ATTEMPTS 20
#define HE_FLUSH_RETRY_MS     10
/* F1: K4 must read closed within this long of an ARMED, fresh status, else re-request. */
#define HE_K4_CONFIRM_MS      3000u
#define HE_K4_MAX_RESENDS     4
/* Safety-link fix batch 2, MED-1: a Pico reboot is classified from the DIAG
 * frame of the NEW boot. Until one arrives the cause is undecided and heat is
 * withheld (no timeout fallback: a late fatal DIAG must still hold). */

typedef struct {
    SemaphoreHandle_t lock;
    SafetyLinkClass  *safety;
    uint32_t          held_mask;   /* bit per heat_enable_claimant_t */
    bool              granted;     /* a REQUEST_ENABLE(true) was accepted and has not been released */
    bool              pending;     /* someone holds a claim but the request did not land */
    uint32_t          enable_sends;
    uint32_t          release_sends;
    /* F1 (safety link review 2026-10-09): K4 reconcile. REQUEST_ENABLE is a
     * fire-and-forget broadcast, so `granted` only means the local UART took
     * the bytes. These fields compare it with the Pico's reported K4 state. */
    uint32_t          k4_open_since_ms;   /* 0 = no mismatch being timed */
    uint32_t          k4_next_resend_ms;
    uint8_t           k4_resends;         /* re-requests spent on this episode */
    bool              k4_unconfirmed;     /* retries exhausted, Pico still reports K4 open */
    /* Safety-link fix batch 2, MED-1: Pico reboot handling. */
    bool              reboot_seq_known;      /* seen_reboot_seq is valid */
    uint32_t          seen_reboot_seq;       /* safety_link pico_reboot_seq last observed */
    bool              reboot_classify_pending; /* reboot seen with a claim held, cause not yet known */
    uint32_t          reboot_classify_since_ms;
    bool              reboot_hold;           /* fatal-cause reboot: do NOT re-request heat */
    bool              last_pico_tripped;     /* last fresh DIAG said TRIPPED (T3: the Pico's trip latch
                                              * does not survive a non-watchdog reboot) */
    bool              reboot_was_tripped;    /* the reboot being classified followed a TRIPPED DIAG */
    /* Firing audit 2026-10-10 MED-5: the reboot VERDICT outlives the claim. reboot_classify_pending is
     * per-claim (cleared by the last release); these two are per-reboot-seq. verdict_pending: a reboot
     * was seen and no DIAG of the new boot has arrived yet, claim or not. fatal_latched: that DIAG
     * arrived while nobody held a claim and reported a fatal cause; the next acquire starts in
     * reboot_hold. Cleared only by a decisive DIAG, a newer reboot, or init. */
    bool              reboot_verdict_pending;
    bool              reboot_fatal_latched;
    bool              warned_pending; /* throttles the reconcile-retry warning */
    bool              release_pending; /* a REQUEST_ENABLE(false) is owed to the wire -- set the
                                         * instant the last claimant lets go, cleared ONLY once a
                                         * servicer's send of it actually succeeds (2026-09-15
                                         * review fix: a failed attempt used to drop this
                                         * unconditionally -- see heat_enable_service_pending_
                                         * release()). */
    bool              release_inflight; /* a servicer is between "picked this up" and "the send
                                         * returned" -- the mutual-exclusion half of the same fix:
                                         * without it, two callers (safety_poll_task's drain and a
                                         * concurrent send_enable() flush) could both decide
                                         * nothing is pending and neither waits for the other. */
    uint32_t          release_epoch[HEAT_ENABLE_CLAIMANT_COUNT];
                                        /* STOP GENERATION, per claimant. Advanced by every real
                                         * stop/pause/abort transition (heat_enable_release()), and
                                         * by an idle-backstop tick
                                         * (heat_enable_release_backstop()) only when that tick was
                                         * itself the teardown.
                                         *
                                         * 2026-09-16: it used to advance ONLY when the claimant's
                                         * bit was actually set, which made the whole mechanism
                                         * inert -- at every acquire_since() call site the bit is
                                         * CLEAR when the epoch is sampled, so a stop landing in the
                                         * window advanced nothing and the stale claim was accepted.
                                         * See heat_enable.h's
                                         * heat_enable_release_backstop() comment.
                                         * Lets a caller that decided to acquire
                                         * while holding its OWN module lock detect that the claim
                                         * was released in the gap between that decision and the
                                         * unlocked acquire call -- see heat_enable_acquire_since().
                                         * Per-claimant, not global, deliberately: a global counter
                                         * would let an autotune release spuriously refuse a
                                         * legitimate firing start, which would leave a run heating
                                         * with K4 open -- strictly worse than the window it
                                         * closes. Free-running; wrap is harmless, only equality is
                                         * ever tested and a full 2^32 wrap inside one unlocked
                                         * call window is not physically reachable. */
} heat_enable_ctx_t;

static heat_enable_ctx_t s_he;

static uint32_t claim_bit(heat_enable_claimant_t who)
{
    return (who < HEAT_ENABLE_CLAIMANT_COUNT) ? (1u << (unsigned)who) : 0u;
}

/* Takes the internal lock, or reports that it could not.
 *
 * The failure branch exists for exactly one reason and it is not a real-
 * hardware one: on target this is a plain mutex taken with portMAX_DELAY and
 * the take always succeeds. In the host-test build App/test/stubs/freertos/
 * semphr.h's xSemaphoreTake() returns pdFALSE unconditionally (it models
 * "asserts on a NULL handle", nothing else), and host tests are single-
 * threaded, so proceeding without the lock there is correct rather than
 * merely tolerable. Callers below therefore treat "lock not taken" as
 * "proceed" -- never as "give up", because giving up here would be a path
 * that silently declines to request or, far worse, to release heat. */
static bool he_lock(void)
{
    if (!s_he.lock) {
        return false;
    }
    return xSemaphoreTake(s_he.lock, portMAX_DELAY) == pdTRUE;
}

static void he_unlock(bool taken)
{
    if (taken && s_he.lock) {
        xSemaphoreGive(s_he.lock);
    }
}

static void he_k4_reset_locked(void)
{
    s_he.k4_open_since_ms = 0u;
    s_he.k4_next_resend_ms = 0u;
    s_he.k4_resends = 0u;
    s_he.k4_unconfirmed = false;
}

void heat_enable_init(SafetyLinkClass *safety_or_null)
{
    if (!s_he.lock) {
        s_he.lock = xSemaphoreCreateMutex();
        if (!s_he.lock) {
            /* Not fatal: every function here works without the lock (see
             * he_lock()). Logged because losing it on a real board means the
             * bookkeeping is unserialized, which is worth knowing. */
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed -- heat-enable bookkeeping is unserialized "
                          "this boot (requests and releases still work)");
        }
    }
    s_he.safety = safety_or_null;
    s_he.held_mask = 0;
    s_he.granted = false;
    s_he.pending = false;
    s_he.warned_pending = false;
    s_he.release_pending = false;
    s_he.release_inflight = false;
    he_k4_reset_locked();
    s_he.reboot_seq_known = false;
    s_he.seen_reboot_seq = 0u;
    s_he.reboot_classify_pending = false;
    s_he.reboot_classify_since_ms = 0u;
    s_he.reboot_hold = false;
    s_he.last_pico_tripped = false;
    s_he.reboot_was_tripped = false;
    s_he.reboot_verdict_pending = false;
    s_he.reboot_fatal_latched = false;
    for (unsigned i = 0; i < (unsigned)HEAT_ENABLE_CLAIMANT_COUNT; i++) {
        s_he.release_epoch[i] = 0u;
    }
}

/* Reset-one-side guard (2026-09-15 review of 1c8d7f6e, finding HIGH-1): a
 * re-enable must never overtake a release that is still owed to the wire OR
 * currently being sent by someone else. The old fix here just called
 * heat_enable_service_pending_release() once, trusting that if nothing was
 * pending there was nothing to wait for -- but that flag is cleared the
 * instant a servicer PICKS UP the release, before the (blocking) send
 * actually completes. A safety_poll_task drain that cleared the flag and
 * then blocked on xact_lock left this exact window open: send_enable() would
 * see release_pending==false, send enable=true first, and the drain's
 * enable=false would land after it -- Pico disabled, ESP believing it is
 * granted, nothing left to retry.
 *
 * Fixed with release_inflight as the second half of the same guard: it is
 * set the moment a servicer commits to the send and cleared only once that
 * send returns. This function waits, bounded, for BOTH release_pending and
 * release_inflight to read false -- driving the drain itself each iteration
 * so it does not depend on some other task's schedule to make progress.
 * Returns false (without having sent enable=true) if the wait times out;
 * the caller must treat that exactly like a down link -- do not send, mark
 * pending, let heat_enable_reconcile() retry. Never holds s_he.lock across
 * the wait or the blocking send it drives.
 *
 * 2026-09-15 review of 059a896e, HIGH-1: the retry loop used to call
 * heat_enable_service_pending_release() -- the BLOCKING send -- on every one
 * of its up to HE_FLUSH_MAX_ATTEMPTS iterations. Each of those can itself
 * take a full link exchange, so the real worst case was attempts x exchange
 * time, not the "200 ms" the old comment claimed; it also meant a caller
 * could stack several ~5.2 s blocking exchanges back to back on its own
 * stack. Fixed by driving the exchange AT MOST ONCE per call: if nothing is
 * already in flight, drive it ourselves up front, then only PASSIVELY poll
 * (lock-check-and-sleep, no send) for up to HE_FLUSH_MAX_ATTEMPTS x
 * HE_FLUSH_RETRY_MS for it (or a concurrent servicer's own attempt) to clear.
 * True worst case is now one exchange plus the 200 ms poll budget, ~5.2 s in
 * total and all of it xact_lock contention (the "~5.3 s" this comment used to
 * quote came from a reply-timeout term that does not exist on a fire-and-
 * forget broadcast) --
 * see HE_FLUSH_MAX_ATTEMPTS's own comment -- never attempts x exchange. */
static bool he_flush_release_blocking(void)
{
    bool taken = he_lock();
    bool outstanding = s_he.release_pending || s_he.release_inflight;
    bool other_inflight = s_he.release_inflight;
    he_unlock(taken);
    if (!outstanding) {
        return true;
    }

    if (!other_inflight) {
        /* Nobody else has committed to this send -- drive it ourselves,
         * exactly once. If a concurrent servicer already owns it
         * (other_inflight), driving it again here would just be a second
         * caller racing the same exchange for no benefit, so skip straight
         * to polling for their result instead. */
        heat_enable_service_pending_release();
    }

    for (int attempt = 0; attempt < HE_FLUSH_MAX_ATTEMPTS; attempt++) {
        taken = he_lock();
        outstanding = s_he.release_pending || s_he.release_inflight;
        he_unlock(taken);
        if (!outstanding) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(HE_FLUSH_RETRY_MS));
    }
    taken = he_lock();
    outstanding = s_he.release_pending || s_he.release_inflight;
    he_unlock(taken);
    return !outstanding;
}

/* Sends REQUEST_ENABLE(true) and folds the result back into the state.
 * Called with the lock NOT held (the exchange blocks; danger_mode.c sends
 * outside its own lock for the same reason). Returns whether the request
 * landed. */
static bool send_enable(const char *why)
{
    if (!he_flush_release_blocking()) {
        /* A release is still stuck in flight or pending after the bounded
         * wait above -- sending enable=true now would be the exact reorder
         * this guard exists to prevent. Defer instead: mark pending (same
         * shape as a down link) so heat_enable_reconcile() retries this
         * claim once the release has actually cleared. */
        bool taken = he_lock();
        bool warn = !s_he.warned_pending;
        s_he.pending = true;
        s_he.warned_pending = true;
        he_unlock(taken);
        if (warn) {
            ESP_LOGE(TAG, "heat-enable request (%s) DEFERRED: a release is still in flight on the "
                          "wire and did not clear after a driven exchange plus %d ms of polling -- "
                          "retried once it does (or by the watchdog's reconcile)",
                     why, HE_FLUSH_MAX_ATTEMPTS * HE_FLUSH_RETRY_MS);
        }
        return false;
    }

    esp_err_t err = safety_link_request_enable(s_he.safety, true);

    bool taken = he_lock();
    bool warn_failure = false;
    if (err == ESP_OK && s_he.reboot_hold) {
        /* Review-2 LOW-1: a fatal-reboot hold was set while this send was in flight; the Pico now holds
         * an enable we must not keep. Record nothing as granted and owe the wire a release. */
        s_he.granted = false;
        s_he.pending = false;
        s_he.warned_pending = false;
        s_he.release_pending = true;
        s_he.enable_sends++;
        ESP_LOGW(TAG, "heat-enable request (%s) landed under a reboot hold -- release queued", why);
    } else if (err == ESP_OK) {
        s_he.granted = true;
        s_he.pending = false;
        s_he.warned_pending = false;
        s_he.enable_sends++;
    } else {
        s_he.granted = false;
        s_he.pending = true;
        /* Once per failing episode, not once per reconcile tick: this runs
         * every WATCHDOG_CHECK_PERIOD_MS while a link is down, and an
         * unthrottled ESP_LOGE there floods the UART log bridge for the whole
         * duration of the very fault the operator is trying to read about
         * (the same reasoning profile_executor.c's LOG_PRESTART_ONCE
         * documents). Cleared again by a successful send or a release. */
        warn_failure = !s_he.warned_pending;
        s_he.warned_pending = true;
    }
    /* The claim can have been dropped while the exchange was in flight (an
     * operator halting a firing in the same millisecond it started). Leaving
     * a granted request standing with nobody holding it is the one outcome
     * this module must never produce, so undo it right here rather than
     * waiting for a release that has already been and gone.
     *
     * WHAT THIS BRANCH DOES AND DOES NOT COVER (corrected 2026-09-15, review
     * of 8813bedd, finding MEDIUM-5 -- the comments here previously described
     * a guarantee this code does not provide):
     *
     *   COVERED: a release that arrives after this send began and is not
     *   followed by a re-acquire. held_mask reads 0 here, the branch fires,
     *   the orphaned grant is undone and compensated on the wire.
     *
     *   NOT COVERED, BY CONSTRUCTION: the release-then-re-acquire window.
     *   heat_enable_acquire*() sets held_mask |= bit BEFORE calling into
     *   here, so from this evaluation's point of view the mask is non-zero
     *   for the whole of that window -- the test cannot fire, no matter how
     *   long the exchange blocks. This is not a race that sometimes loses;
     *   it is unreachable. Note the mask CANNOT simply be written after the
     *   send instead: this send is on behalf of a caller that does hold the
     *   claim, and a mask that reads 0 during its own exchange would make
     *   every ordinary acquire look orphaned and immediately self-release.
     *
     *   CONSEQUENCE while that window is open: heat_owner_active_decide()
     *   (safety_link_frames.c) derives KILNLINK_CONTEXT_FLAG_HEAT_OWNER_
     *   ACTIVE from this SAME held_mask, so the Pico is told "a heat owner is
     *   active" throughout -- both sides agree, so nothing alarms, and it
     *   self-heals silently one tick later. No relay is energized by this
     *   (zone relays are gated separately, via kiln_io_owner/relay_authority);
     *   what is briefly lost is the second interlock pole, not heat.
     *
     * The window itself is closed one level up rather than here, by
     * heat_enable_acquire_since(): a caller that committed to acquiring while
     * holding its own lock passes the epoch it saw, and an intervening
     * release makes the acquire REFUSE instead of resurrecting the claim.
     * See that function and heat_enable_claim_epoch(). */
    bool orphaned = (err == ESP_OK) && (s_he.held_mask == 0);
    if (orphaned) {
        s_he.granted = false;
        s_he.pending = false;
    }
    he_unlock(taken);

    if (orphaned) {
        esp_err_t rel_err = safety_link_request_enable(s_he.safety, false);
        bool t2 = he_lock();
        s_he.release_sends++;
        /* 2026-09-15 review of 059a896e, HIGH-3: this used to drop a failed
         * compensating release on the floor -- exactly the LOW-5 defect the
         * commit fixed for heat_enable_service_pending_release(), left
         * unfixed here in the direction that matters most (heat left
         * ENABLED, ESP believing it released). Queue it the same way:
         * release_pending stays/becomes true, so safety_poll_task's next
         * loop retries via heat_enable_service_pending_release() instead of
         * this failure being final. (This used to add "or heat_enable_
         * reconcile()'s own drain, once HIGH-2 restores it" -- corrected
         * 2026-09-15 (review of 8813bedd): HIGH-2 deliberately did NOT
         * restore that drain and is not going to, so safety_poll_task is the
         * only driver of an owed release.) This was also a fire-and-forget (void)
         * cast that logged the same "released again" line unconditionally --
         * exactly the shape danger_mode.c's seed bug had; the relay is
         * already off by this point (heat_enable.h's ordering rule), but the
         * safety processor's own enable line can still be standing if this
         * send failed, so name the failure rather than asserting it landed. */
        if (rel_err != ESP_OK) {
            s_he.release_pending = true;
        }
        he_unlock(t2);
        if (rel_err == ESP_OK) {
            ESP_LOGW(TAG, "heat-enable request (%s) landed after its last claimant let go -- released again",
                     why);
        } else {
            ESP_LOGE(TAG, "heat-enable request (%s) landed after its last claimant let go -- "
                          "release FAILED: %s -- the safety processor may still believe heating "
                          "is permitted -- queued for retry",
                     why, esp_err_to_name(rel_err));
        }
        return false;
    }
    if (err != ESP_OK) {
        if (warn_failure) {
            ESP_LOGE(TAG, "heat-enable request (%s) NOT sent: %s -- the safety processor was never "
                          "asked to close K4, so no element current will flow. Retried every "
                          "watchdog tick; further copies suppressed until it lands or is released.",
                     why, esp_err_to_name(err));
        }
        return false;
    }
    ESP_LOGW(TAG, "heat-enable requested (%s): safety processor asked to permit heating (K4)", why);
    return true;
}

uint32_t heat_enable_claim_epoch(heat_enable_claimant_t who)
{
    if (claim_bit(who) == 0u) {
        return 0u;
    }
    bool taken = he_lock();
    uint32_t epoch = s_he.release_epoch[who];
    he_unlock(taken);
    return epoch;
}

bool heat_enable_acquire_since(heat_enable_claimant_t who, uint32_t epoch)
{
    uint32_t bit = claim_bit(who);
    if (bit == 0u) {
        return false;
    }

    bool taken = he_lock();
    uint32_t now_epoch = s_he.release_epoch[who];
    bool stale = (now_epoch != epoch);
    bool withheld = false;
    if (!stale) {
        bool was_empty = (s_he.held_mask == 0u);
        s_he.held_mask |= bit;
        if (was_empty) {
            /* MED-5: a verdict that was pending (or fatal) when the previous claim ended is
             * re-armed for this one, so resume/start cannot launder an undecided reboot. */
            if (s_he.reboot_verdict_pending) {
                s_he.reboot_classify_pending = true;
                if (s_he.reboot_classify_since_ms == 0u) {
                    s_he.reboot_classify_since_ms = 1u;
                }
            }
            if (s_he.reboot_fatal_latched) {
                s_he.reboot_fatal_latched = false;
                s_he.reboot_hold = true;
            }
        }
        withheld = s_he.reboot_classify_pending || s_he.reboot_hold;
        if (withheld && !s_he.granted) {
            s_he.pending = true; /* reconcile re-requests once the verdict is benign */
        }
    }
    bool already_granted = s_he.granted;
    he_unlock(taken);

    if (stale) {
        /* The claim was released between the caller committing to this
         * acquire and this call actually running. Recording it now would
         * resurrect a claim for a run that no longer exists -- K4 requested
         * on behalf of nobody, with heat_owner_active_decide() reporting a
         * heat owner to the Pico to match. Refuse instead; the caller's own
         * exit path has already run, so there is nothing left to tear down. */
        ESP_LOGE(TAG, "heat-enable acquire (%s) REFUSED: the claim was released between the caller "
                      "committing to it and this call (epoch %u -> %u) -- the run that asked for "
                      "heat no longer exists, so the safety processor was NOT asked to close K4",
                 who == HEAT_ENABLE_CLAIMANT_PROFILE ? "firing" : "autotune",
                 (unsigned)epoch, (unsigned)now_epoch);
        return false;
    }

    if (withheld && !already_granted) {
        ESP_LOGW(TAG, "heat-enable acquire (%s) WITHHELD: Pico reboot verdict pending or fatal",
                 who == HEAT_ENABLE_CLAIMANT_PROFILE ? "firing" : "autotune");
        return false;
    }
    if (already_granted) {
        /* Nothing to send: the request is standing. This is the branch that
         * keeps a control loop calling acquire() every tick from putting a
         * REQUEST_ENABLE on the wire every tick. */
        return true;
    }
    return send_enable(who == HEAT_ENABLE_CLAIMANT_PROFILE ? "firing" : "autotune");
}

bool heat_enable_acquire(heat_enable_claimant_t who)
{
    /* Thin wrapper: sample the epoch and immediately spend it, i.e. opt out
     * of the staleness check. Correct for any caller that is not holding a
     * decision made earlier under some other lock. */
    return heat_enable_acquire_since(who, heat_enable_claim_epoch(who));
}

/* Shared body of heat_enable_release() and heat_enable_release_backstop().
 * `bit` is already validated non-zero by both wrappers, so `who` is in range
 * and indexing release_epoch[] is safe. `stop_transition` is the
 * discriminator: true for a real stop/pause/abort, false for the
 * unconditional per-tick idle backstop. heat_enable.h's
 * heat_enable_release_backstop() comment explains why that distinction, and
 * not "was it held", is the one that matters.
 *
 * Kept as ONE body rather than duplicated so the source-text scans in
 * test_heat_enable.c keep covering the real code, and so a later edit cannot
 * fix one path and silently miss the other.
 *
 * 2026-09-15 review of 1c8d7f6e, finding HIGH-1's "second, smaller
     * window": this used to be two separate lock sections -- the bookkeeping
     * above, then an unlock, then a second lock just to set release_pending.
     * An acquire arriving in the gap between them saw had_request already
     * cleared and release_pending not yet set, and could send enable=true
     * with nothing yet flagged to flush it against. Folded into one section
     * so release_pending becomes true in the SAME critical section that
     * clears granted/pending -- there is no window left for a concurrent
     * acquire to observe. */
static void he_release_common(heat_enable_claimant_t who, uint32_t bit, bool stop_transition)
{
    bool taken = he_lock();
    bool was_held = (s_he.held_mask & bit) != 0u;
    s_he.held_mask &= ~bit;
    if (stop_transition || was_held) {
        /* Advanced in the SAME critical section that clears the bit, so a
         * concurrent heat_enable_acquire_since() can never observe the bit
         * gone and the epoch unchanged (which would let it re-acquire on
         * behalf of the run this release just ended).
         *
         * `stop_transition` is what makes this mechanism work at all
         * (2026-09-16): a real stop must advance the generation whether or not
         * the claim was held, because the claimant's bit is CLEAR at every
         * acquire_since() call site at the moment the epoch is sampled -- a
         * start has not acquired yet, a resume's pause already released.
         * `was_held` is kept alongside it so an idle-backstop tick that IS the
         * teardown still counts. A backstop tick with nothing held advances
         * nothing, which is what keeps a passing idle tick from refusing a
         * legitimate in-flight acquire. */
        s_he.release_epoch[who]++;
    }
    bool last_out = (s_he.held_mask == 0u);
    bool had_request = s_he.granted || s_he.pending;
    if (last_out) {
        s_he.granted = false;
        s_he.pending = false;
        s_he.warned_pending = false;
        he_k4_reset_locked();
        s_he.reboot_classify_pending = false;
        s_he.reboot_hold = false;
        s_he.reboot_was_tripped = false;
        /* reboot_verdict_pending / reboot_fatal_latched deliberately survive (MED-5). */
        if (had_request) {
            s_he.release_pending = true;
        }
    }
    he_unlock(taken);

    if (!was_held || !last_out || !had_request) {
        /* Never claimed, someone else still holds it, or nothing was ever
         * asked for -- nothing to undo. This is what makes the per-tick
         * backstop callers (profile_executor's "state is not RUNNING" branch)
         * free rather than a release frame every tick. */
        return;
    }

    /* 2026-09-15 fix (docs/audits/profile_executor_coredump_2026-09-15.md):
     * the actual REQUEST_ENABLE(false) safety-link exchange used to happen
     * right here, synchronously, on WHATEVER task called heat_enable_release()
     * -- for profile_executor_status.c's halt()/pause() and
     * profile_executor.c's escalate_guard_trip() path, that is
     * profile_executor's own 4096 B task stack, and the measured worst-case
     * depth through this exact chain (safety_exchange -> uart_protocol_send_
     * broadcast -> ... -> uart_enable_tx_write_fifo) was the single deepest
     * contributor to four recurring stack-smash panics on that task. The
     * bookkeeping above (granted/pending/held_mask, all under s_he.lock) is
     * unaffected -- heat_enable_is_granted()/is_held() already flip
     * synchronously, right here (release_pending was already set in the
     * SAME critical section above), before any frame goes on the wire,
     * exactly as before. Only the deep UART call itself is deferred,
     * mirroring safety_link.h's existing reannounce_pending/boot_clear_
     * pending pattern: drained by safety_link_poll.c's safety_poll_task
     * (8192 B, ample headroom) once per loop iteration via heat_enable_
     * service_pending_release() below. A missed or failed release can never
     * be lost -- release_pending is now cleared ONLY on a successful send
     * (2026-09-15 review fix, finding LOW-5) -- and a re-enable can never
     * race ahead of a release that is still owed or in flight either --
     * send_enable() waits, bounded, for both to clear before it will send
     * enable=true (finding HIGH-1; see he_flush_release_blocking()). No
     * module lock is held across any deferred/blocking call in either
     * place. */
}

void heat_enable_release(heat_enable_claimant_t who)
{
    uint32_t bit = claim_bit(who);
    if (bit == 0u) {
        return;
    }
    /* A REAL stop/pause/abort transition: always advances the stop generation,
     * so an acquire that sampled it before this call refuses instead of
     * resurrecting the claim. heat_enable.h explains why this, and not the
     * backstop form, is the default spelling. */
    he_release_common(who, bit, true);
}

void heat_enable_release_backstop(heat_enable_claimant_t who)
{
    uint32_t bit = claim_bit(who);
    if (bit == 0u) {
        return;
    }
    /* The unconditional per-tick idle backstop (profile_executor.c's and
     * autotune_engine.c's "not running" branches). Advances the stop
     * generation only if this tick was itself the teardown -- an idle tick
     * with nothing held must not invalidate an acquire that is merely in
     * flight. */
    he_release_common(who, bit, false);
}

/* Drains a release owed to the wire, if any. Never called with s_he.lock (or
 * any other module's lock) held by the caller -- takes and releases it only
 * for the small bookkeeping steps, exactly like send_enable() does for the
 * enable=true side. Safe and cheap to call unconditionally every tick from a
 * task with real stack headroom (safety_poll_task); also called from
 * he_flush_release_blocking() (driven by send_enable(), to make progress on
 * its own wait rather than depending on some other task's schedule), and
 * directly by host tests in place of a real safety_poll_task.
 *
 * 2026-09-15 review of 1c8d7f6e: this used to clear release_pending BEFORE
 * attempting the send, unconditionally -- a failed send was logged and then
 * forgotten (finding LOW-5), and the pre-clear-then-block-on-the-wire shape
 * was exactly what let a concurrent send_enable() decide nothing was
 * pending while this call was still in flight (finding HIGH-1). Both are
 * fixed together: release_inflight now covers the in-flight window (so a
 * concurrent caller waits instead of racing ahead), and release_pending is
 * cleared ONLY once the send actually succeeds, so a failed attempt stays
 * queued and gets retried the next time anything calls this -- on target,
 * that is every safety_poll_task loop iteration, so a transient failure
 * self-heals within one poll period without any caller having to notice. */
void heat_enable_service_pending_release(void)
{
    bool taken = he_lock();
    bool go = s_he.release_pending && !s_he.release_inflight;
    if (go) {
        s_he.release_inflight = true;
    }
    he_unlock(taken);
    if (!go) {
        return;
    }

    /* Unconditional attempt, exactly like danger_mode_stop()'s: enable=false
     * is the fail-safe direction and safety_link.c attempts it whether or
     * not the link looks up. The relays are already off by the time any
     * caller reaches heat_enable_release() -- see heat_enable.h's ordering
     * rule -- but this used to cast the result to (void) and print
     * "released" regardless, which is the exact seed bug (danger_mode.c,
     * commit 2bcdc2d): an owner-queue/link failure looked identical in the
     * log to a confirmed release. Surface the disagreement instead. */
    esp_err_t rel_err = safety_link_request_enable(s_he.safety, false);
    bool t2 = he_lock();
    s_he.release_sends++;
    s_he.release_inflight = false;
    if (rel_err == ESP_OK) {
        s_he.release_pending = false;
    }
    he_unlock(t2);
    if (rel_err != ESP_OK) {
        ESP_LOGE(TAG, "heat-enable release FAILED: %s -- the safety processor may still believe "
                      "heating is permitted (K4); relays are already off, but do not assume the "
                      "enable line dropped -- will retry",
                 esp_err_to_name(rel_err));
    } else {
        ESP_LOGW(TAG, "heat-enable released: safety processor asked to drop heating (K4)");
    }
}

bool heat_enable_is_held(heat_enable_claimant_t who)
{
    uint32_t bit = claim_bit(who);
    if (bit == 0u) {
        return false;
    }
    bool taken = he_lock();
    bool held = (s_he.held_mask & bit) != 0u;
    he_unlock(taken);
    return held;
}

bool heat_enable_is_granted(void)
{
    bool taken = he_lock();
    /* Never claim heat is enabled while the Pico reports K4 open past the
     * bounded re-request budget (F1). */
    bool granted = s_he.granted && s_he.held_mask != 0u && !s_he.k4_unconfirmed;
    he_unlock(taken);
    return granted;
}

bool heat_enable_retry_pending(void)
{
    bool taken = he_lock();
    bool pending = (s_he.pending || s_he.k4_unconfirmed) && s_he.held_mask != 0u;
    he_unlock(taken);
    return pending;
}

bool heat_enable_grant_unconfirmed(void)
{
    bool taken = he_lock();
    bool u = s_he.k4_unconfirmed && s_he.held_mask != 0u;
    he_unlock(taken);
    return u;
}

void heat_enable_note_pico_boot(uint32_t reboot_seq, bool diag_since_reboot, uint8_t boot_reason,
                                uint32_t now_ms)
{
    bool taken = he_lock();
    bool log_hold = false;
    bool log_benign = false;
    if (!s_he.reboot_seq_known) {
        s_he.reboot_seq_known = true;
        s_he.seen_reboot_seq = reboot_seq;
    } else if (reboot_seq != s_he.seen_reboot_seq) {
        s_he.seen_reboot_seq = reboot_seq;
        /* The new boot starts a new episode: counters from the old boot say
         * nothing about it (reset-one-side class: the Pico side restarted). */
        he_k4_reset_locked();
        s_he.reboot_verdict_pending = true;  /* MED-5: survives release/pause */
        s_he.reboot_fatal_latched = false;   /* a newer boot supersedes an older verdict */
        s_he.reboot_classify_since_ms = now_ms ? now_ms : 1u;
        if (s_he.held_mask != 0u) {
            s_he.reboot_was_tripped = s_he.last_pico_tripped;
            s_he.reboot_classify_pending = true;
        }
    }
    if (s_he.held_mask == 0u) {
        s_he.reboot_classify_pending = false;
        s_he.reboot_hold = false;
        s_he.reboot_was_tripped = false;
        if (s_he.reboot_verdict_pending && diag_since_reboot) {
            const uint8_t fatal_u = SAFETY_LINK_DIAG_BOOT_WATCHDOG | SAFETY_LINK_DIAG_BOOT_BROWNOUT |
                                    SAFETY_LINK_DIAG_BOOT_STACK_OVERFLOW |
                                    SAFETY_LINK_DIAG_BOOT_MALLOC_FAILED |
                                    SAFETY_LINK_DIAG_BOOT_ASSERT_FAILED;
            s_he.reboot_verdict_pending = false;
            s_he.reboot_fatal_latched = (boot_reason & fatal_u) != 0u;
        }
    } else if (s_he.reboot_classify_pending) {
        if (diag_since_reboot) {
            s_he.reboot_classify_pending = false;
            s_he.reboot_verdict_pending = false;
            const uint8_t fatal = SAFETY_LINK_DIAG_BOOT_WATCHDOG | SAFETY_LINK_DIAG_BOOT_BROWNOUT |
                                  SAFETY_LINK_DIAG_BOOT_STACK_OVERFLOW |
                                  SAFETY_LINK_DIAG_BOOT_MALLOC_FAILED |
                                  SAFETY_LINK_DIAG_BOOT_ASSERT_FAILED;
            /* T3 (REVIEW_SAFTYFW_TRIP_PATH_2026-10-10): the Pico's trip latch is RAM-only
             * and does not survive a non-watchdog reset, so a reboot that followed a
             * TRIPPED DIAG is treated as fatal whatever its boot reason says: the
             * latched trip was lost, not cleared by an operator. Withholds heat only. */
            if ((boot_reason & fatal) != 0u || s_he.reboot_was_tripped) {
                s_he.reboot_hold = true;
                /* Withdraw any queued or standing re-request. K4 stays open. */
                if (s_he.granted || s_he.pending) {
                    /* A false reboot detection must not leave the Pico grant standing:
                     * owe the wire a REQUEST_ENABLE(false). */
                    s_he.release_pending = true;
                }
                s_he.granted = false;
                s_he.pending = false;
                s_he.warned_pending = false;
                log_hold = true;
            } else {
                log_benign = true;
            }
        }
        /* No timeout fallback (Opus review MED): until a DIAG of the NEW boot arrives the
         * cause is unknown, and a late WATCHDOG/BROWNOUT DIAG must still hold. Staying
         * undecided only withholds heat (fail safe); the profile executor shows the
         * condition and the operator resumes or stops. */
    }
    he_unlock(taken);
    if (log_hold) {
        ESP_LOGE(TAG, "Pico rebooted with a fatal cause (boot_reason 0x%02x) during a heat claim -- "
                      "NOT re-requesting heat; K4 stays open until an operator resumes",
                 (unsigned)boot_reason);
    }
    if (log_benign) {
        /* Owner decision 2026-10-10: a benign Pico reboot (power-on / unknown
         * cause) mid-firing auto-resumes heat -- F1 re-requests once the Pico is
         * ARMED. A fatal cause (handled above) pauses instead. */
        ESP_LOGW(TAG, "Pico rebooted during a heat claim (benign cause) -- F1 will re-request heat");
    }
}

bool heat_enable_reboot_undecided(void)
{
    bool taken = he_lock();
    bool u = s_he.reboot_classify_pending && s_he.held_mask != 0u;
    he_unlock(taken);
    return u;
}

bool heat_enable_reboot_hold(void)
{
    bool taken = he_lock();
    bool h = s_he.reboot_hold && s_he.held_mask != 0u;
    he_unlock(taken);
    return h;
}

void heat_enable_note_pico_state(bool fresh, uint8_t diag_state, bool k4_closed, uint32_t now_ms)
{
    bool taken = he_lock();
    bool log_giveup = false;
    bool log_resend = false;
    bool armed_or_warn = diag_state == SAFETY_LINK_DIAG_STATE_ARMED ||
                         diag_state == SAFETY_LINK_DIAG_STATE_WARN;
    if (fresh) {
        s_he.last_pico_tripped = (diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED);
    }
    if (s_he.held_mask == 0u) {
        he_k4_reset_locked();
    } else if (s_he.reboot_hold) {
        /* MED-1: a fatal-cause Pico reboot -- never re-request; nothing to time. */
        s_he.k4_open_since_ms = 0u;
    } else if (!s_he.granted) {
        /* Nothing to compare (a send or a re-request is outstanding). */
    } else if (!fresh) {
        /* Stale: keep the episode counters, just stop timing. */
        s_he.k4_open_since_ms = 0u;
    } else if (!armed_or_warn) {
        /* GRACE/INIT/TRIPPED: K4 cannot be expected closed, so do not time it
         * AND (LOW-1) end the episode: the retry budget and the unconfirmed
         * latch belong to one ARMED stretch, so they restart with the next. A
         * GRACE->ARMED transition restarts the clock below. */
        he_k4_reset_locked();
    } else if (k4_closed) {
        he_k4_reset_locked();
    } else if (s_he.k4_open_since_ms == 0u) {
        s_he.k4_open_since_ms = now_ms ? now_ms : 1u;
        s_he.k4_next_resend_ms = s_he.k4_open_since_ms + HE_K4_CONFIRM_MS;
    } else if ((int32_t)(now_ms - s_he.k4_next_resend_ms) >= 0 && !s_he.k4_unconfirmed) {
        if (s_he.k4_resends >= HE_K4_MAX_RESENDS) {
            s_he.k4_unconfirmed = true;
            log_giveup = true;
        } else {
            /* Exponential backoff: first re-request 3 s after K4 first read
             * open, then 6, 12, 24 and 48 s between the following checks. */
            s_he.k4_resends++;
            s_he.k4_next_resend_ms = now_ms + (HE_K4_CONFIRM_MS << s_he.k4_resends);
            s_he.granted = false;  /* not claimed until the re-request lands */
            s_he.pending = true;
            s_he.warned_pending = false;
            log_resend = true;
        }
    }
    uint8_t n = s_he.k4_resends;
    he_unlock(taken);
    if (log_resend) {
        ESP_LOGW(TAG, "Pico is ARMED but reports K4 open while a heat grant is held -- "
                      "re-requesting (attempt %u of %d)", (unsigned)n, HE_K4_MAX_RESENDS);
    }
    if (log_giveup) {
        ESP_LOGE(TAG, "heat grant could not be obtained: Pico ARMED, K4 still open after %d "
                      "re-requests -- heat is NOT enabled (reported as not granted)", HE_K4_MAX_RESENDS);
    }
}

void heat_enable_reconcile(void)
{
    /* 2026-09-15 review of 059a896e, HIGH-2: this function used to call
     * heat_enable_service_pending_release() here as a "second, independent
     * driver" for a stuck release (see the prior review's MEDIUM-4). That
     * puts the full blocking UART release chain -- safety_exchange() and
     * everything under it -- on profile_exec_wdt's stack, which is the SAME
     * 4096 B size as profile_executor's, the exact task whose suspected
     * stack overflow 1c8d7f6e existed to stop putting that chain on. The
     * static checker cannot clear this: profile_exec_wdt is INDETERMINATE
     * (unresolved indirect calls), so there was no headroom argument to fall
     * back on, only the fact that nothing had blown up yet.
     *
     * Removed rather than kept "because it hasn't paniced" -- reconcile()
     * goes back to what it did before 059a896e: retrying a STUCK ENABLE via
     * send_enable() below (which is the shallow, mostly-bookkeeping path
     * unless it actually has to drive a release flush -- see he_flush_
     * release_blocking()'s own comment on why that is now bounded to at most
     * one exchange). The gap this reopens -- no fallback driver for a stuck
     * *release* if safety_poll_task itself wedges -- is the SAME gap that
     * existed before 059a896e (prior review's MEDIUM-4); it is not made
     * worse by this revert, and closing it belongs on a task with real
     * headroom (safety_poll_task's own 8192 B, which already drains
     * release_pending every loop) or a dedicated low-priority task, not on
     * the watchdog. See docs/audits/review_executor_race_fix_059a896e_
     * 2026-09-15.md finding HIGH-2. */

    bool taken = he_lock();
    bool want_retry = s_he.pending && s_he.held_mask != 0u && !s_he.granted &&
                      !s_he.reboot_hold && !s_he.reboot_classify_pending;
    he_unlock(taken);

    if (!want_retry) {
        return;
    }
    /* Logging (and its throttle) lives in send_enable() -- see its failure
     * branch. */
    (void)send_enable("retry");
}

uint32_t heat_enable_enable_send_count(void)
{
    bool taken = he_lock();
    uint32_t n = s_he.enable_sends;
    he_unlock(taken);
    return n;
}

uint32_t heat_enable_release_send_count(void)
{
    bool taken = he_lock();
    uint32_t n = s_he.release_sends;
    he_unlock(taken);
    return n;
}
