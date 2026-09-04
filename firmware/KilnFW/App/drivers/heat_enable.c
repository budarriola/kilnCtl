#include "heat_enable.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "heat_enable";

typedef struct {
    SemaphoreHandle_t lock;
    SafetyLinkClass  *safety;
    uint32_t          held_mask;   /* bit per heat_enable_claimant_t */
    bool              granted;     /* a REQUEST_ENABLE(true) was accepted and has not been released */
    bool              pending;     /* someone holds a claim but the request did not land */
    uint32_t          enable_sends;
    uint32_t          release_sends;
    bool              warned_pending; /* throttles the reconcile-retry warning */
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
}

/* Sends REQUEST_ENABLE(true) and folds the result back into the state.
 * Called with the lock NOT held (the exchange blocks; danger_mode.c sends
 * outside its own lock for the same reason). Returns whether the request
 * landed. */
static bool send_enable(const char *why)
{
    esp_err_t err = safety_link_request_enable(s_he.safety, true);

    bool taken = he_lock();
    bool warn_failure = false;
    if (err == ESP_OK) {
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
     * waiting for a release that has already been and gone. */
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
        he_unlock(t2);
        if (rel_err != ESP_OK) {
            /* This was a fire-and-forget (void) cast that logged the same
             * "released again" line unconditionally -- exactly the shape
             * danger_mode.c's seed bug had. The relay is already off by this
             * point (heat_enable.h's ordering rule), but the safety
             * processor's own enable line can still be standing if this send
             * failed, so name the failure rather than asserting it landed. */
            ESP_LOGE(TAG, "heat-enable request (%s) landed after its last claimant let go -- "
                          "release FAILED: %s -- the safety processor may still believe heating "
                          "is permitted",
                     why, esp_err_to_name(rel_err));
        } else {
            ESP_LOGW(TAG, "heat-enable request (%s) landed after its last claimant let go -- released again",
                     why);
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

bool heat_enable_acquire(heat_enable_claimant_t who)
{
    uint32_t bit = claim_bit(who);
    if (bit == 0u) {
        return false;
    }

    bool taken = he_lock();
    s_he.held_mask |= bit;
    bool already_granted = s_he.granted;
    he_unlock(taken);

    if (already_granted) {
        /* Nothing to send: the request is standing. This is the branch that
         * keeps a control loop calling acquire() every tick from putting a
         * REQUEST_ENABLE on the wire every tick. */
        return true;
    }
    return send_enable(who == HEAT_ENABLE_CLAIMANT_PROFILE ? "firing" : "autotune");
}

void heat_enable_release(heat_enable_claimant_t who)
{
    uint32_t bit = claim_bit(who);
    if (bit == 0u) {
        return;
    }

    bool taken = he_lock();
    bool was_held = (s_he.held_mask & bit) != 0u;
    s_he.held_mask &= ~bit;
    bool last_out = (s_he.held_mask == 0u);
    bool had_request = s_he.granted || s_he.pending;
    if (last_out) {
        s_he.granted = false;
        s_he.pending = false;
        s_he.warned_pending = false;
    }
    he_unlock(taken);

    if (!was_held || !last_out || !had_request) {
        /* Never claimed, someone else still holds it, or nothing was ever
         * asked for -- nothing to undo. This is what makes the per-tick
         * backstop callers (profile_executor's "state is not RUNNING" branch)
         * free rather than a release frame every tick. */
        return;
    }

    /* Unconditional attempt, exactly like danger_mode_stop()'s: enable=false
     * is the fail-safe direction and safety_link.c attempts it whether or
     * not the link looks up. The relays are already off by the time any
     * caller reaches here -- see heat_enable.h's ordering rule -- but this
     * used to cast the result to (void) and print "released" regardless,
     * which is the exact seed bug (danger_mode.c, commit 2bcdc2d): an
     * owner-queue/link failure looked identical in the log to a confirmed
     * release. Surface the disagreement instead. */
    esp_err_t rel_err = safety_link_request_enable(s_he.safety, false);
    bool t2 = he_lock();
    s_he.release_sends++;
    he_unlock(t2);
    if (rel_err != ESP_OK) {
        ESP_LOGE(TAG, "heat-enable release FAILED: %s -- the safety processor may still believe "
                      "heating is permitted (K4); relays are already off, but do not assume the "
                      "enable line dropped",
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
    bool granted = s_he.granted && s_he.held_mask != 0u;
    he_unlock(taken);
    return granted;
}

bool heat_enable_retry_pending(void)
{
    bool taken = he_lock();
    bool pending = s_he.pending && s_he.held_mask != 0u;
    he_unlock(taken);
    return pending;
}

void heat_enable_reconcile(void)
{
    bool taken = he_lock();
    bool want_retry = s_he.pending && s_he.held_mask != 0u && !s_he.granted;
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
