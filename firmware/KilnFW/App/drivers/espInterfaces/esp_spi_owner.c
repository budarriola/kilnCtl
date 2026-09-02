#include "esp_spi_owner.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"

#include "../owner_slot_pool.h"

static const char *TAG = "esp_spi_owner";

/* Bound on both halves of spi_owner_transfer()'s wait (queuing the request,
 * then waiting for the owner task to complete it). Generous on purpose: a
 * real transfer -- display flush or MAX31856 register poke alike -- is
 * microseconds to low single-digit milliseconds (DISPLAY_ST7796_PLAN.md
 * section 9: 20 MHz display SPI clock, max_transfer_sz bounded to
 * ILI9488_SCRATCH_BYTES = 1440 B, so no single transfer exceeds ~576us; queue
 * depth 8 gives ~4.6ms worst-case backlog -- 1000ms has roughly 1700x margin
 * on transfer time alone).
 *
 * That margin does NOT mean this timeout only ever fires on a genuinely dead
 * bus, though (opus review, commit f3a1600: the previous version of this
 * comment reasoned only about transfer duration and therefore overclaimed
 * "never a false alarm"). The owner task can also be starved rather than
 * stuck: it is priority 5 with tskNO_AFFINITY, so a long flash erase or an
 * OTA write burst running with the cache disabled on the same core can stall
 * it for real, non-trivial stretches with the SPI bus itself perfectly
 * healthy. That is scheduler/cache starvation, not a wedged transaction, and
 * it is the realistic way this timeout fires -- not the transfer time this
 * margin is computed against. Must not be tightened without re-checking both
 * the largest real flush chunk this bus ever pushes AND the longest realistic
 * starvation window (flash erase / OTA write) this owner task can be caught
 * behind. */
#define SPI_OWNER_TRANSFER_TIMEOUT_MS 1000u

/* ---- Module-owned result-slot pool -- see owner_slot_pool.h's top comment
 * and kiln_io_owner.c's/thermo_owner.c's identical pools for the full
 * invariant this rests on. Unlike those two (fixed compile-time
 * SLOT_COUNT), this owner's queue depth is a runtime spi_owner_init()
 * parameter, so the pool is heap-allocated there, sized to queue_len.
 *
 * This replaces the previous design, which handed the owner task a pointer
 * to a StaticSemaphore_t and an esp_err_t* living in spi_owner_transfer()'s
 * OWN stack frame (opus review, commit f3a1600, G1): on a completion-wait
 * timeout that frame returns while the request is still sitting in the
 * queue holding both pointers. If the owner task was only transiently
 * starved (see the timeout comment above) rather than permanently wedged, it
 * goes on to dequeue and finish that request -- spi_owner_task() would then
 * write `*request.result_out` into a stack slot the caller's next function
 * call has since reused, and xSemaphoreGive() on a StaticSemaphore_t backed
 * by that same reused memory walks garbage list pointers on real FreeRTOS.
 * The old "latch wedged so this is the last transfer that can ever be in
 * flight" mitigation only holds for a PERMANENT wedge; it does nothing for
 * the transient case, which is also the more likely trigger for a 1000ms
 * timeout in the first place -- so it protected against the wrong scenario.
 * Moving both the result and the semaphore into this heap-backed pool means
 * a late completion always lands somewhere still valid, whether or not the
 * caller gave up first. */
typedef struct spi_owner_slot {
    esp_err_t result;
    StaticSemaphore_t sem_storage;
    SemaphoreHandle_t sem;
} spi_owner_slot_t;

static esp_err_t spi_owner_slot_pool_init(spi_owner_t *owner, size_t count)
{
    owner->slots = calloc(count, sizeof(spi_owner_slot_t));
    owner->slot_refcount = calloc(count, sizeof(uint8_t));
    if (!owner->slots || !owner->slot_refcount) {
        free(owner->slots);
        free(owner->slot_refcount);
        owner->slots = NULL;
        owner->slot_refcount = NULL;
        return ESP_ERR_NO_MEM;
    }

    owner->slot_lock = xSemaphoreCreateMutex();
    if (!owner->slot_lock) {
        free(owner->slots);
        free(owner->slot_refcount);
        owner->slots = NULL;
        owner->slot_refcount = NULL;
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < count; i++) {
        owner->slots[i].sem = xSemaphoreCreateBinaryStatic(&owner->slots[i].sem_storage);
        if (!owner->slots[i].sem) {
            for (size_t j = 0; j < i; j++) {
                vSemaphoreDelete(owner->slots[j].sem);
            }
            vSemaphoreDelete(owner->slot_lock);
            owner->slot_lock = NULL;
            free(owner->slots);
            free(owner->slot_refcount);
            owner->slots = NULL;
            owner->slot_refcount = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    owner->slot_count = count;
    return ESP_OK;
}

static void spi_owner_slot_pool_deinit(spi_owner_t *owner)
{
    if (owner->slots) {
        for (size_t i = 0; i < owner->slot_count; i++) {
            if (owner->slots[i].sem) {
                vSemaphoreDelete(owner->slots[i].sem);
            }
        }
    }
    if (owner->slot_lock) {
        vSemaphoreDelete(owner->slot_lock);
    }
    free(owner->slots);
    free(owner->slot_refcount);
    owner->slots = NULL;
    owner->slot_refcount = NULL;
    owner->slot_lock = NULL;
    owner->slot_count = 0;
}

static void spi_owner_task(void *arg)
{
    spi_owner_t *owner = (spi_owner_t *)arg;
    spi_owner_request_t request;

    while (true) {
        if (xQueueReceive(owner->request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (request.shutdown) {
            break;
        }

        esp_err_t result = ESP_OK;
        if (request.tx_buffer && request.tx_length > 0) {
            gpio_set_level((gpio_num_t)request.cs_pin, 0);
            spi_transaction_t trans = {
                .length = request.tx_length * 8,
                .tx_buffer = request.tx_buffer,
                .rx_buffer = request.rx_buffer,
                .rxlength = request.rx_length * 8,
            };
            result = spi_device_transmit(request.device, &trans);
            gpio_set_level((gpio_num_t)request.cs_pin, 1);
        } else {
            result = ESP_ERR_INVALID_ARG;
        }

        /* Write into the module-owned slot (never the caller's stack -- see
         * the pool comment above) and give its semaphore first, exactly as
         * kiln_io_owner.c's/thermo_owner.c's owner_task() tails do; only
         * what happens AFTER differs between "the caller is still waiting"
         * and "the caller already gave up", and this order-independent
         * release makes that irrelevant. */
        spi_owner_slot_t *slot = &owner->slots[request.slot];
        slot->result = result;
        xSemaphoreGive(slot->sem);

        xSemaphoreTake(owner->slot_lock, portMAX_DELAY);
        bool free_now = owner_slot_pool_release(owner->slot_refcount, owner->slot_count, request.slot);
        if (free_now) {
            xSemaphoreTake(slot->sem, 0); /* drain a Give() nobody ever collected */
            slot->result = ESP_OK;
        }
        xSemaphoreGive(owner->slot_lock);
    }

    if (owner->shutdown_done) {
        xSemaphoreGive(owner->shutdown_done);
    }
    vTaskDelete(NULL);
}

esp_err_t spi_owner_init(spi_owner_t *owner,
                             spi_host_device_t host,
                             UBaseType_t queue_len,
                             UBaseType_t task_priority,
                             uint32_t stack_depth,
                             BaseType_t core_id)
{
    if (!owner) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(owner, 0, sizeof(*owner));
    owner->host = host;
    owner->request_queue = xQueueCreate(queue_len, sizeof(spi_owner_request_t));
    if (!owner->request_queue) {
        ESP_LOGE(TAG, "failed to create request queue");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t pool_err = spi_owner_slot_pool_init(owner, (size_t)queue_len);
    if (pool_err != ESP_OK) {
        vQueueDelete(owner->request_queue);
        owner->request_queue = NULL;
        ESP_LOGE(TAG, "failed to create result-slot pool");
        return pool_err;
    }

    owner->shutdown_done = xSemaphoreCreateBinary();
    if (!owner->shutdown_done) {
        spi_owner_slot_pool_deinit(owner);
        vQueueDelete(owner->request_queue);
        owner->request_queue = NULL;
        ESP_LOGE(TAG, "failed to create shutdown semaphore");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(spi_owner_task,
                                                      "spi_owner_task",
                                                      stack_depth,
                                                      owner,
                                                      task_priority,
                                                      &owner->task_handle,
                                                      core_id);
    if (task_created != pdPASS) {
        vQueueDelete(owner->request_queue);
        owner->request_queue = NULL;
        vSemaphoreDelete(owner->shutdown_done);
        owner->shutdown_done = NULL;
        spi_owner_slot_pool_deinit(owner);
        ESP_LOGE(TAG, "failed to create owner task");
        return ESP_ERR_NO_MEM;
    }

    owner->initialized = true;
    return ESP_OK;
}

esp_err_t spi_owner_deinit(spi_owner_t *owner)
{
    if (!owner || !owner->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_owner_request_t shutdown_request;
    memset(&shutdown_request, 0, sizeof(shutdown_request));
    shutdown_request.shutdown = true;

    if (owner->request_queue) {
        xQueueSend(owner->request_queue, &shutdown_request, portMAX_DELAY);
    }

    if (owner->shutdown_done) {
        if (xSemaphoreTake(owner->shutdown_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "worker did not confirm shutdown in time; deleting queue anyway");
        }
        vSemaphoreDelete(owner->shutdown_done);
    }

    if (owner->request_queue) {
        vQueueDelete(owner->request_queue);
    }

    /* opus review, commit f3a1600, G2(c): a re-init after this deinit must
     * start clean, not carry a permanently latched `wedged` forward -- this
     * is the recovery path spi_owner_t::wedged's header comment promises.
     * The slot pool itself is torn down and freed here too (owner->slots is
     * this owner's own heap allocation, sized at init time), so any request
     * still orphaned in it (owner-side release pending on a timed-out
     * transfer) is released along with everything else -- safe, because a
     * deinit means the caller has decided this owner is done for good, not
     * merely resetting between transfers. */
    spi_owner_slot_pool_deinit(owner);

    owner->request_queue = NULL;
    owner->task_handle = NULL;
    owner->shutdown_done = NULL;
    owner->initialized = false;
    owner->wedged = false;
    return ESP_OK;
}

bool spi_owner_is_wedged(const spi_owner_t *owner)
{
    return owner && owner->wedged;
}

esp_err_t spi_owner_transfer(spi_owner_t *owner,
                                 spi_device_handle_t device,
                                 const uint8_t *tx_buffer,
                                 size_t tx_length,
                                 uint8_t *rx_buffer,
                                 size_t rx_length,
                                 int cs_pin)
{
    if (!owner || !owner->initialized || !owner->request_queue || !device) {
        return ESP_ERR_INVALID_ARG;
    }

    if (owner->wedged) {
        /* Fail fast -- see the .h comment on spi_owner_t::wedged. Do not
         * touch the queue or reserve a slot for a request the owner task
         * will never get to. */
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(owner->slot_lock, portMAX_DELAY);
    int idx = owner_slot_pool_alloc(owner->slot_refcount, owner->slot_count);
    xSemaphoreGive(owner->slot_lock);
    if (idx < 0) {
        /* Pool is sized to queue_len, so this means every slot is still
         * held -- either genuinely queue_len transfers in flight at once
         * (shouldn't happen: the queue itself would already be full and
         * reject xQueueSend below) or every slot is orphaned behind a
         * previously timed-out, still-unfinished owner task. Either way,
         * queuing more work behind it is exactly the pile-up spi_owner_t's
         * wedged latch exists to prevent, so treat this the same as a
         * completion timeout rather than silently blocking. */
        ESP_LOGE(TAG, "result-slot pool exhausted -- treating as wedged");
        owner->wedged = true;
        return ESP_ERR_TIMEOUT;
    }

    spi_owner_request_t request;
    memset(&request, 0, sizeof(request));
    request.device = device;
    request.tx_buffer = tx_buffer;
    request.tx_length = tx_length;
    request.rx_buffer = rx_buffer;
    request.rx_length = rx_length;
    request.cs_pin = cs_pin;
    request.slot = idx;

    if (xQueueSend(owner->request_queue, &request, pdMS_TO_TICKS(SPI_OWNER_TRANSFER_TIMEOUT_MS)) !=
        pdTRUE) {
        /* Never reached the owner task -- nobody but this call ever held the
         * slot, so release both halves of it ourselves right now (same
         * "never queued" shape as thermo_owner.c's post_and_wait()). The
         * queue itself being unable to accept a request for a full second
         * means the owner is stuck on whatever it is currently processing;
         * latch wedged so the next caller (display or thermocouple,
         * whichever comes first) fails immediately instead of also
         * blocking. */
        xSemaphoreTake(owner->slot_lock, portMAX_DELAY);
        owner_slot_pool_release(owner->slot_refcount, owner->slot_count, idx);
        owner_slot_pool_release(owner->slot_refcount, owner->slot_count, idx);
        xSemaphoreGive(owner->slot_lock);
        owner->wedged = true;
        ESP_LOGE(TAG, "request queue did not accept a transfer within %ums -- "
                      "owner task presumed wedged, failing all transfers until spi_owner_deinit()",
                 (unsigned)SPI_OWNER_TRANSFER_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    spi_owner_slot_t *slot = &owner->slots[idx];
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(slot->sem, pdMS_TO_TICKS(SPI_OWNER_TRANSFER_TIMEOUT_MS)) == pdTRUE) {
        result = slot->result; /* safe: read before this side's own release below */

        xSemaphoreTake(owner->slot_lock, portMAX_DELAY);
        bool free_now = owner_slot_pool_release(owner->slot_refcount, owner->slot_count, idx);
        if (free_now) {
            xSemaphoreTake(slot->sem, 0); /* drain a Give() nobody ever collected */
            slot->result = ESP_OK;
        }
        xSemaphoreGive(owner->slot_lock);
        return result;
    }

    /* The request WAS handed to the owner task, which is presumably still
     * blocked on whatever it is currently doing -- possibly permanently
     * wedged, possibly only transiently starved (scheduler/cache contention
     * -- see SPI_OWNER_TRANSFER_TIMEOUT_MS's comment) and about to finish
     * after all. Either way, this call releases ONLY its own half of the
     * slot's refcount and walks away -- it never touches slot->sem or
     * slot->result again. If the owner task later does finish, its own
     * xSemaphoreGive()/owner_slot_pool_release() pair (spi_owner_task()
     * above) is the one that brings the refcount to zero and actually
     * recycles the slot; if it never finishes, the slot stays orphaned
     * (refcount 1, permanently) rather than being reused while something
     * might still write into it -- exactly the invariant owner_slot_pool.h
     * exists to guarantee, extended here to a slot that started life
     * assigned to a stack-free, heap-backed pool instead of the caller's own
     * frame. That is what makes this orphan safe where the old stack-backed
     * design (opus review, commit f3a1600, G1) was not: there is no return
     * address or reused local variable for a late completion to corrupt
     * anymore, only a permanently-held pool slot -- a bounded resource leak
     * of at most `queue_len` slots for the remaining life of this owner, not
     * a memory-safety bug. */
    xSemaphoreTake(owner->slot_lock, portMAX_DELAY);
    owner_slot_pool_release(owner->slot_refcount, owner->slot_count, idx);
    xSemaphoreGive(owner->slot_lock);

    owner->wedged = true;
    ESP_LOGE(TAG, "owner task did not complete a transfer within %ums -- "
                  "presumed wedged, failing all transfers until spi_owner_deinit()",
             (unsigned)SPI_OWNER_TRANSFER_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}
