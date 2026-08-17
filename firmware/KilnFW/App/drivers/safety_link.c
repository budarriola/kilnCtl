#include "safety_link.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "settings.h"
#include "uart_task_ids.h"

static const char *TAG = "safety_link";

/* The safety link permanently owns UART1 (settings.h: SAFETY_UART_PORT_NUM).
 * If the PC link is also configured onto UART1 the second uart_driver_install
 * simply fails at runtime, which is a confusing way to discover a
 * menuconfig mistake -- so say it at compile time instead. */
#if CONFIG_KILNCTL_UART_PORT_1
#error "The PC link and the safety link cannot share UART1: set KILNCTL_UART_PORT_0 (see App/drivers/Kconfig)."
#endif

/* Inbox depth. The far side is expected to answer one request at a time; the
 * extra slots absorb an unsolicited push landing next to a poll reply. */
#define SAFETY_INBOX_LEN 4

#define SAFETY_POLL_TASK_STACK    4096
#define SAFETY_POLL_TASK_PRIORITY 5

/* Every fault source this driver recognizes. A caller may only set or clear
 * bits that appear here: an unknown bit set would latch the fault line with no
 * named owner able to release it, and -- far worse -- a wildcard clear such as
 * ~0u would release every *other* source's assertion as a side effect. The
 * whole point of the source mask is that no reason can drop another reason's
 * fault, so the mask itself has to be closed. */
#define SAFETY_FAULT_SRC_ALL                                                       \
    ((uint32_t)(SAFETY_FAULT_SRC_MANUAL | SAFETY_FAULT_SRC_PC_LINK |               \
                SAFETY_FAULT_SRC_THERMO | SAFETY_FAULT_SRC_SAFETY_LINK |           \
                SAFETY_FAULT_SRC_APP))

/* Ceiling on how long a caller waits to start its own request/reply exchange.
 * One exchange is bounded by the ACK timeout times uart_protocol's retries plus
 * the reply timeout (~750 ms with a dead peer), so anything past a few of those
 * means the holder is wedged rather than merely unlucky. Waiting forever here
 * would let a stuck safety link take out whatever task asked it a question --
 * including the bridge task that answers the PC. */
#define SAFETY_XACT_LOCK_TIMEOUT_MS 5000u

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

/* The ESP32-S3 is little-endian and so is every multi-byte field in this
 * protocol, so these are memcpy rather than byte assembly -- but they stay
 * functions so the wire layout is still stated once, in one place. */
static float safety_read_f32_le(const uint8_t *bytes)
{
    float value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void safety_put_f32_le(uint8_t *out, float value)
{
    memcpy(out, &value, sizeof(value));
}

static void safety_put_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void safety_put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t safety_elapsed_ms(TickType_t since)
{
    /* Unsigned tick subtraction, so the 32-bit tick counter's eventual wrap
     * (~497 days at the default 100 Hz) produces the right delta rather than
     * a nonsense one. */
    TickType_t delta = xTaskGetTickCount() - since;
    return (uint32_t)delta * portTICK_PERIOD_MS;
}

static inline bool safety_lock(SafetyLinkClass *link)
{
    return xSemaphoreTake(link->state_lock, portMAX_DELAY) == pdTRUE;
}

static inline void safety_unlock(SafetyLinkClass *link)
{
    xSemaphoreGive(link->state_lock);
}

/* ------------------------------------------------------------------------ */
/* Cache / staleness                                                        */
/* ------------------------------------------------------------------------ */

/* Age of the cached status in ms, saturating just below the reserved
 * "never received" value so the two can never be confused. */
static uint16_t safety_age_ms_locked(const SafetyLinkClass *link)
{
    if (!link->ever_received) {
        return SAFETY_LINK_AGE_NEVER;
    }
    uint32_t elapsed = safety_elapsed_ms(link->cached_tick);
    if (elapsed >= SAFETY_LINK_AGE_NEVER) {
        return (uint16_t)(SAFETY_LINK_AGE_NEVER - 1u);
    }
    return (uint16_t)elapsed;
}

/* link_up = a valid reply within SAFETY_LINK_UP_PERIODS poll periods. With
 * polling switched off there is no period to measure against, so the
 * configured default is used -- otherwise turning polling off would make the
 * link look permanently up (nothing ever goes stale) or permanently down
 * (window of zero), and both are lies. */
static bool safety_link_up_locked(const SafetyLinkClass *link)
{
    uint16_t age = safety_age_ms_locked(link);
    if (age == SAFETY_LINK_AGE_NEVER) {
        return false;
    }
    uint32_t base = link->poll_period_ms;
    if (base == 0u) {
        base = (SAFETY_POLL_PERIOD_MS > 0) ? (uint32_t)SAFETY_POLL_PERIOD_MS : 500u;
    }
    return (uint32_t)age <= base * SAFETY_LINK_UP_PERIODS;
}

/* Drives GPIO6 from the current source mask. High = fault asserted = U1's LED
 * lit = the Pico's mainFault input pulled low (docs/HARDWARE.md). Called with
 * state_lock held so the pin and the mask can't disagree. */
static void safety_apply_fault_locked(SafetyLinkClass *link)
{
    gpio_set_level((gpio_num_t)link->fault_io, link->fault_sources != 0u ? 1 : 0);
}

/* ------------------------------------------------------------------------ */
/* Frame handling                                                           */
/* ------------------------------------------------------------------------ */

/* Accepts one status frame from the Pico (see the contract in safety_link.h)
 * and replaces the cache with it. Returns false, and counts a frame error, if
 * the payload isn't a status frame of the right length -- a short or unknown
 * payload is dropped rather than partially applied, because half a temperature
 * reading is worse than none. */
static bool safety_apply_status(SafetyLinkClass *link, const uart_proto_message_t *msg)
{
    /* Length before payload[0]: a zero-length frame has no subcommand byte to
     * read, and the || below short-circuits in the right order only if the
     * length test comes first. */
    if (msg->length != SAFETY_LINK_STATUS_FRAME_LEN || msg->payload[0] != SAFETY_CMD_GET_STATUS) {
        if (safety_lock(link)) {
            link->stats.frame_errors++;
            safety_unlock(link);
        }
        ESP_LOGW(TAG, "unexpected frame from dev%u/task%u: subcmd 0x%02X, %u bytes",
                 msg->device, msg->task_id, msg->payload[0], msg->length);
        return false;
    }

    const uint8_t *p = msg->payload;
    if (!safety_lock(link)) {
        return false;
    }
    /* Bits 0/1 describe *our* view of the link and *our* fault output; they
     * are filled in at read time, so whatever the peer put there is dropped
     * rather than trusted (see safety_link.h). */
    link->cached.flags = (uint8_t)(p[1] & ~(SAFETY_FLAG_LINK_UP | SAFETY_FLAG_FAULT));
    link->cached.tc_temp_c = safety_read_f32_le(&p[2]);
    link->cached.cj_temp_c = safety_read_f32_le(&p[6]);
    /* The contract says the Pico sends NaN when SAFETY_FLAG_TEMP_VALID is
     * clear, but a peer that sends 0.0 instead -- or a firmware that forgets --
     * must not have it forwarded to the PC as a real reading of a stone-cold
     * kiln. The flag is the authority; enforce it here rather than trusting
     * the far side to have been careful. */
    if (!(link->cached.flags & SAFETY_FLAG_TEMP_VALID)) {
        link->cached.tc_temp_c = NAN;
        link->cached.cj_temp_c = NAN;
    }
    link->cached.tc_fault = p[10];
    link->cached.current_a[0] = safety_read_f32_le(&p[11]);
    link->cached.current_a[1] = safety_read_f32_le(&p[15]);
    link->cached.current_a[2] = safety_read_f32_le(&p[19]);
    link->cached_tick = xTaskGetTickCount();
    link->ever_received = true;
    link->stats.frames_received++;
    safety_unlock(link);
    return true;
}

/* Drains the inbox for up to wait_ms, applying every status frame found.
 * Returns true if at least one was applied. Waiting on the *first* message
 * only -- once something has arrived the rest of the queue is taken without
 * blocking, so a burst is absorbed in one pass. */
static bool safety_drain_inbox(SafetyLinkClass *link, uint32_t wait_ms)
{
    uart_proto_message_t msg;
    bool got_status = false;
    TickType_t wait = pdMS_TO_TICKS(wait_ms);

    while (uart_protocol_receive(link->inbox, &msg, wait) == ESP_OK) {
        if (msg.length >= 1 && safety_apply_status(link, &msg)) {
            got_status = true;
        }
        wait = 0; /* only the first receive is allowed to block */
    }
    return got_status;
}

/* One complete request/reply exchange, serialized against every other one on
 * this link (see SafetyLinkClass::xact_lock).
 *
 * Note what is *not* here: no bespoke framing, retry or CRC logic. Retries,
 * de-duplication and the CRC all belong to uart_protocol, which is the same
 * code the PC link runs -- this function is only the request/reply pairing on
 * top of it. */
static esp_err_t safety_exchange(SafetyLinkClass *link, const uint8_t *request, size_t length,
                                  bool expect_status)
{
    if (!request || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(link->xact_lock, pdMS_TO_TICKS(SAFETY_XACT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "timed out after %ums waiting for the safety link transaction lock",
                 (unsigned)SAFETY_XACT_LOCK_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    /* Anything already queued is a previous reply or an unsolicited push: fold
     * it into the cache now, so the wait below can only see a frame that our
     * own request produced. */
    (void)safety_drain_inbox(link, 0);

    if (safety_lock(link)) {
        link->stats.frames_sent++;
        safety_unlock(link);
    }

    esp_err_t err = uart_protocol_send(&link->proto, UART_PROTO_DEVICE_SAFETY, UART_TASK_ID_SAFETY,
                                        UART_TASK_ID_SAFETY, request, length,
                                        SAFETY_LINK_ACK_TIMEOUT_MS);
    if (err != ESP_OK) {
        if (safety_lock(link)) {
            link->stats.timeouts++;
            safety_unlock(link);
        }
        /* NOT logged here: with no Pico attached this fires on every single
         * poll. The poll task logs the *state* (link down) at most once per
         * SAFETY_LINK_DOWN_LOG_PERIOD_MS instead -- see safety_update_health. */
        xSemaphoreGive(link->xact_lock);
        return err;
    }

    if (expect_status) {
        if (!safety_drain_inbox(link, SAFETY_LINK_REPLY_TIMEOUT_MS)) {
            /* ACKed but no answer: the peer's protocol layer is alive and its
             * application layer is not. Counted as a timeout, since the result
             * for the caller is the same -- no fresh data. */
            if (safety_lock(link)) {
                link->stats.timeouts++;
                safety_unlock(link);
            }
            err = ESP_ERR_TIMEOUT;
        }
    } else {
        /* A peer that volunteers a status right after (e.g. after
         * REQUEST_ENABLE) gets it folded into the cache for free. */
        (void)safety_drain_inbox(link, SAFETY_LINK_ACK_TIMEOUT_MS);
    }

    xSemaphoreGive(link->xact_lock);
    return err;
}

/* ------------------------------------------------------------------------ */
/* Poll task                                                                */
/* ------------------------------------------------------------------------ */

/* Rate-limited link-state logging plus the one fault source this driver
 * raises on its own. Only ever called from the poll task, which is why
 * down_logged/down_log_tick need no locking. */
static void safety_update_health(SafetyLinkClass *link)
{
    bool up = false;
    bool policy = false;
    uint16_t age = SAFETY_LINK_AGE_NEVER;

    if (safety_lock(link)) {
        up = safety_link_up_locked(link);
        age = safety_age_ms_locked(link);
        policy = link->fault_on_link_loss;
        safety_unlock(link);
    }

    if (up) {
        if (link->down_logged) {
            ESP_LOGI(TAG, "safety processor link is up again");
            link->down_logged = false;
        }
    } else if (!link->down_logged ||
               safety_elapsed_ms(link->down_log_tick) >= SAFETY_LINK_DOWN_LOG_PERIOD_MS) {
        if (age == SAFETY_LINK_AGE_NEVER) {
            ESP_LOGW(TAG, "no reply from the safety processor (never seen one). Expected while "
                          "the RP2040 firmware does not exist; status reports link_up=0.");
        } else {
            ESP_LOGW(TAG, "safety processor link is down (last status %u ms ago)", age);
        }
        link->down_logged = true;
        link->down_log_tick = xTaskGetTickCount();
    }

    if (policy) {
        safety_link_set_fault_source(link, SAFETY_FAULT_SRC_SAFETY_LINK, !up);
    }
}

static void safety_poll_task(void *arg)
{
    SafetyLinkClass *link = (SafetyLinkClass *)arg;
    const uint8_t request[] = { SAFETY_CMD_GET_STATUS };

    while (true) {
        uint16_t period = 0;
        if (safety_lock(link)) {
            period = link->poll_period_ms;
            safety_unlock(link);
        }

        if (period == 0u) {
            /* Polling off: still drain anything the peer pushes unsolicited,
             * so an enabled-but-unpolled link isn't blind. */
            (void)safety_drain_inbox(link, SAFETY_LINK_IDLE_TICK_MS);
            continue;
        }

        TickType_t started = xTaskGetTickCount();
        (void)safety_exchange(link, request, sizeof(request), true);
        safety_update_health(link);

        /* Measure the sleep from the start of the attempt, so the poll rate
         * stays at the requested period rather than period + however long a
         * dead peer took to time out. Floored so a period shorter than the
         * exchange itself still yields. */
        uint32_t spent = safety_elapsed_ms(started);
        uint32_t sleep_ms = (spent >= period) ? SAFETY_LINK_MIN_POLL_GAP_MS : (period - spent);
        if (sleep_ms < SAFETY_LINK_MIN_POLL_GAP_MS) {
            sleep_ms = SAFETY_LINK_MIN_POLL_GAP_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

/* ------------------------------------------------------------------------ */
/* Bring-up                                                                 */
/* ------------------------------------------------------------------------ */

esp_err_t safety_link_start(SafetyLinkClass *link)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (link->initialized) {
        return ESP_OK;
    }

    /* Validated before it is shifted into pin_bit_mask below: a bad Kconfig
     * value would otherwise be a shift past the width of the type, and the
     * fault line is the last thing on this board that should be driven by
     * accident. */
    if (!GPIO_IS_VALID_OUTPUT_GPIO(SAFETY_FAULT_IO)) {
        ESP_LOGE(TAG, "SAFETY_FAULT_IO (%d) is not a usable output", (int)SAFETY_FAULT_IO);
        return ESP_ERR_INVALID_ARG;
    }

    memset(link, 0, sizeof(*link));
    link->fault_io = SAFETY_FAULT_IO;
    link->poll_period_ms = (uint16_t)SAFETY_POLL_PERIOD_MS;
    link->fault_on_link_loss = true; /* fail-safe; see safety_link.h */
    /* No reading has ever arrived, and NaN is the only honest value for that.
     * Zero would read as a stone-cold kiln, which is exactly the wrong
     * direction to be wrong in. */
    link->cached.tc_temp_c = NAN;
    link->cached.cj_temp_c = NAN;
    link->cached.current_a[0] = NAN;
    link->cached.current_a[1] = NAN;
    link->cached.current_a[2] = NAN;

    /* Declared up here, not at first use: every failure below lands on the
     * shared cleanup labels, which report it. */
    esp_err_t err = ESP_ERR_NO_MEM;

    link->state_lock = xSemaphoreCreateMutex();
    link->xact_lock = xSemaphoreCreateMutex();
    if (!link->state_lock || !link->xact_lock) {
        ESP_LOGE(TAG, "failed to create link mutexes");
        goto fail_locks;
    }

    /* The fault line first, and de-asserted, before anything else can fail:
     * the pin powers up as a floating input, and a floating gate on U1 is an
     * undefined fault state at the safety processor. Driving it low (LED off,
     * Pico's mainFault released) is the known-good starting point, and it is
     * the poll task's job -- not bring-up's -- to raise it. */
    gpio_config_t fault_cfg = {
        .pin_bit_mask = 1ULL << (uint32_t)link->fault_io,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&fault_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fault line gpio_config(%d) failed: %s", link->fault_io, esp_err_to_name(err));
        goto fail_locks;
    }
    gpio_set_level((gpio_num_t)link->fault_io, 0);

    err = uart_owner_init(&link->owner, SAFETY_UART_PORT_NUM, SAFETY_TX_IO, SAFETY_RX_IO,
                           SAFETY_UART_BAUD_RATE, UART_OWNER_QUEUE_LEN, UART_OWNER_TASK_PRIORITY,
                           UART_OWNER_STACK_SIZE, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_owner_init(uart%d) failed: %s", SAFETY_UART_PORT_NUM, esp_err_to_name(err));
        goto fail_locks;
    }

    /* The one thing that makes this link different from the PC link. Each
     * TCMT1109 inverts: the driver's high lights the LED, which pulls the
     * receiver's collector low, so an idle-high UART line arrives idle-low in
     * both directions. Inverting both signals in the UART peripheral puts the
     * bits back the right way up for free; doing it in software would mean
     * hand-decoding the line. Must be after uart_param_config (inside
     * uart_owner_init), which rewrites the same register block.
     *
     * This also makes the barrier transparent to the far end: TXD_INV and U2
     * are two inversions in series, so the Pico's RX sees ordinary polarity,
     * and U3's inversion of the Pico's ordinary TX is undone by RXD_INV. The
     * RP2040 therefore needs no PIO UART and no external inverter -- exactly
     * one end inverts, and it is this one. */
    err = uart_set_line_inverse(SAFETY_UART_PORT_NUM, UART_SIGNAL_TXD_INV | UART_SIGNAL_RXD_INV);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_line_inverse failed: %s", esp_err_to_name(err));
        goto fail_owner;
    }

    /* GPIO5 is the bare collector of U3, on net DataFromSafty -- R15's 1k
     * pull-up to 3.3V_Main is already fitted on this net, and nothing else
     * sits on it. With the phototransistor off the pin would float, so the
     * internal pull-up is what
     * defines the LED-off level (high at the pad = low after RXD_INV = the
     * space/break level). uart_set_pin already asks for this, but it is
     * restated because it is load-bearing rather than incidental: without it
     * the link doesn't merely get noisy, it has no defined idle at all. */
    err = gpio_set_pull_mode((gpio_num_t)SAFETY_RX_IO, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rx pull-up on gpio%d failed: %s", SAFETY_RX_IO, esp_err_to_name(err));
        goto fail_owner;
    }

    err = uart_protocol_init(&link->proto, &link->owner, UART_PROTO_DEVICE_ESP,
                              UART_PROTOCOL_TASK_PRIORITY, UART_PROTOCOL_STACK_SIZE, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_protocol_init(uart%d) failed: %s", SAFETY_UART_PORT_NUM,
                 esp_err_to_name(err));
        goto fail_owner;
    }

    /* Registered on *this* protocol instance (the isolated link), not on the
     * PC link's -- same task_id, two separate address spaces. This is the
     * inbox the Pico's replies land in. */
    err = uart_protocol_register_task(&link->proto, UART_TASK_ID_SAFETY, SAFETY_INBOX_LEN,
                                       &link->inbox);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register safety task failed: %s", esp_err_to_name(err));
        goto fail_proto;
    }

    /* Set before the task exists, not after: the poll task calls back into the
     * public API (safety_link_set_fault_source), which refuses to touch an
     * uninitialized link -- so the flag has to be true by the time it runs its
     * first iteration, not merely by the time start() returns. */
    link->initialized = true;
    if (xTaskCreatePinnedToCore(safety_poll_task, "safety_poll", SAFETY_POLL_TASK_STACK, link,
                                 SAFETY_POLL_TASK_PRIORITY, &link->poll_task,
                                 tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "failed to create safety poll task");
        link->initialized = false;
        err = ESP_ERR_NO_MEM;
        goto fail_task;
    }

    ESP_LOGI(TAG, "safety link up on uart%d (tx=%d rx=%d, inverted), fault out=gpio%d, poll=%ums",
             SAFETY_UART_PORT_NUM, SAFETY_TX_IO, SAFETY_RX_IO, link->fault_io,
             link->poll_period_ms);
    return ESP_OK;

fail_task:
    uart_protocol_unregister_task(&link->proto, UART_TASK_ID_SAFETY);
fail_proto:
    uart_protocol_deinit(&link->proto);
fail_owner:
    uart_owner_deinit(&link->owner);
fail_locks:
    if (link->state_lock) {
        vSemaphoreDelete(link->state_lock);
        link->state_lock = NULL;
    }
    if (link->xact_lock) {
        vSemaphoreDelete(link->xact_lock);
        link->xact_lock = NULL;
    }
    return (err != ESP_OK) ? err : ESP_ERR_NO_MEM;
}

esp_err_t safety_link_stop(SafetyLinkClass *link)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Take BOTH locks before deleting the poll task, in the documented order
     * (xact outside state), so it can only be killed between exchanges *and*
     * outside safety_update_health's state-lock section. Taking only xact_lock
     * left the other window open: the task could be deleted while holding
     * state_lock, and the vSemaphoreDelete below would then destroy a mutex
     * that is still held -- after which every remaining caller blocks forever.
     * `initialized` is cleared under the locks so late callers bounce off the
     * state check instead of racing the teardown. */
    xSemaphoreTake(link->xact_lock, portMAX_DELAY);
    xSemaphoreTake(link->state_lock, portMAX_DELAY);
    link->initialized = false;
    if (link->poll_task) {
        vTaskDelete(link->poll_task);
        link->poll_task = NULL;
    }
    xSemaphoreGive(link->state_lock);
    xSemaphoreGive(link->xact_lock);

    uart_protocol_unregister_task(&link->proto, UART_TASK_ID_SAFETY);
    uart_protocol_deinit(&link->proto);
    uart_owner_deinit(&link->owner);

    /* The fault line is deliberately left in whatever state it was in. Tearing
     * the link down is not evidence that the controller is healthy, and
     * releasing the safety processor's fault input on the way out would be the
     * opposite of fail-safe. */
    vSemaphoreDelete(link->state_lock);
    vSemaphoreDelete(link->xact_lock);
    link->state_lock = NULL;
    link->xact_lock = NULL;
    link->inbox = NULL;
    link->initialized = false;
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out)
{
    if (!link || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    *out = link->cached;
    out->age_ms = safety_age_ms_locked(link);
    out->link_up = safety_link_up_locked(link);
    out->fault_asserted = (link->fault_sources != 0u);
    safety_unlock(link);
    return ESP_OK;
}

esp_err_t safety_link_request_enable(SafetyLinkClass *link, bool enable)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t request[] = { SAFETY_CMD_REQUEST_ENABLE, (uint8_t)(enable ? 1u : 0u) };
    return safety_exchange(link, request, sizeof(request), false);
}

esp_err_t safety_link_ping(SafetyLinkClass *link)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t request[] = { SAFETY_CMD_GET_STATUS };
    return safety_exchange(link, request, sizeof(request), true);
}

esp_err_t safety_link_set_poll_period(SafetyLinkClass *link, uint16_t period_ms)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    link->poll_period_ms = period_ms;
    safety_unlock(link);
    ESP_LOGI(TAG, "poll period set to %u ms%s", period_ms, period_ms == 0 ? " (polling off)" : "");
    return ESP_OK;
}

esp_err_t safety_link_get_stats(SafetyLinkClass *link, safety_link_stats_t *out)
{
    if (!link || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    *out = link->stats;
    out->poll_period_ms = link->poll_period_ms;
    safety_unlock(link);
    /* uart_owner counts the physical line errors (break/parity/frame and ring
     * overflows) for this port; they are the same class of problem as a
     * payload this driver had to reject, so the PC sees one number. Read
     * outside the lock -- it's a plain volatile counter owned by the UART
     * event task. */
    out->frame_errors += uart_owner_get_rx_error_count(&link->owner);
    return ESP_OK;
}

esp_err_t safety_link_set_fault_source(SafetyLinkClass *link, uint32_t source_mask,
                                        bool assert_fault)
{
    if (!link || source_mask == 0u || (source_mask & ~SAFETY_FAULT_SRC_ALL) != 0u) {
        /* An unrecognized bit is refused outright rather than masked down to
         * the known ones: silently narrowing a clear request would release
         * sources the caller never named. */
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    uint32_t before = link->fault_sources;
    if (assert_fault) {
        link->fault_sources |= source_mask;
    } else {
        link->fault_sources &= ~source_mask;
    }
    uint32_t after = link->fault_sources;
    safety_apply_fault_locked(link);
    safety_unlock(link);

    if (after != before) {
        /* Logged on change only: this is called from the poll task on every
         * iteration once the link-loss policy is active, so logging every call
         * would be one line per poll. */
        ESP_LOGW(TAG, "isolated fault line %s (sources 0x%02X -> 0x%02X)",
                 (after != 0u) ? "ASSERTED" : "released", (unsigned)before, (unsigned)after);
    }
    return ESP_OK;
}

esp_err_t safety_link_set_fault(SafetyLinkClass *link, bool assert_fault)
{
    return safety_link_set_fault_source(link, SAFETY_FAULT_SRC_MANUAL, assert_fault);
}

bool safety_link_get_fault(SafetyLinkClass *link)
{
    if (!link || !link->initialized) {
        return false;
    }
    if (!safety_lock(link)) {
        return false;
    }
    bool asserted = (link->fault_sources != 0u);
    safety_unlock(link);
    return asserted;
}

uint32_t safety_link_get_fault_sources(SafetyLinkClass *link)
{
    if (!link || !link->initialized) {
        return 0u;
    }
    if (!safety_lock(link)) {
        return 0u;
    }
    uint32_t sources = link->fault_sources;
    safety_unlock(link);
    return sources;
}

esp_err_t safety_link_fault_on_link_loss(SafetyLinkClass *link, bool enable)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!link->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_lock(link)) {
        return ESP_FAIL;
    }
    link->fault_on_link_loss = enable;
    safety_unlock(link);

    if (!enable) {
        /* Drop the source this policy owns; leaving it latched would make
         * "stop asserting on link loss" a no-op until something else happened
         * to clear it. Other sources are untouched. */
        safety_link_set_fault_source(link, SAFETY_FAULT_SRC_SAFETY_LINK, false);
        ESP_LOGW(TAG, "fault-on-link-loss DISABLED: a dead safety link will no longer assert "
                      "the isolated fault line");
    } else {
        ESP_LOGI(TAG, "fault-on-link-loss enabled (default, fail-safe)");
    }
    return ESP_OK;
}

bool safety_link_get_fault_on_link_loss(SafetyLinkClass *link)
{
    if (!link || !link->initialized) {
        return false;
    }
    if (!safety_lock(link)) {
        return false;
    }
    bool enabled = link->fault_on_link_loss;
    safety_unlock(link);
    return enabled;
}

/* ------------------------------------------------------------------------ */
/* PC-facing payload builders (layout defined in uart_task_ids.h)           */
/* ------------------------------------------------------------------------ */

size_t safety_link_build_status_payload(SafetyLinkClass *link, uint8_t *out)
{
    safety_link_status_t status;
    if (!out || safety_link_get_status(link, &status) != ESP_OK) {
        return 0;
    }

    uint8_t flags = status.flags; /* bits 2..5 come from the Pico */
    if (status.link_up) {
        flags |= SAFETY_FLAG_LINK_UP;
    }
    if (status.fault_asserted) {
        flags |= SAFETY_FLAG_FAULT;
    }

    out[0] = SAFETY_CMD_GET_STATUS;
    out[1] = flags;
    safety_put_f32_le(&out[2], status.tc_temp_c);
    safety_put_f32_le(&out[6], status.cj_temp_c);
    out[10] = status.tc_fault;
    safety_put_f32_le(&out[11], status.current_a[0]);
    safety_put_f32_le(&out[15], status.current_a[1]);
    safety_put_f32_le(&out[19], status.current_a[2]);
    safety_put_u16_le(&out[23], status.age_ms);
    return SAFETY_LINK_STATUS_PAYLOAD_LEN;
}

size_t safety_link_build_stats_payload(SafetyLinkClass *link, uint8_t *out)
{
    safety_link_stats_t stats;
    if (!out || safety_link_get_stats(link, &stats) != ESP_OK) {
        return 0;
    }

    out[0] = SAFETY_CMD_GET_LINK_STATS;
    safety_put_u32_le(&out[1], stats.frames_sent);
    safety_put_u32_le(&out[5], stats.frames_received);
    safety_put_u32_le(&out[9], stats.frame_errors);
    safety_put_u32_le(&out[13], stats.timeouts);
    safety_put_u16_le(&out[17], stats.poll_period_ms);
    return SAFETY_LINK_STATS_PAYLOAD_LEN;
}
