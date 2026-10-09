/* hal_i2c_esp.c -- ESP-IDF backend for interface/hal_i2c.h.
 *
 * Phase 1b ("adapt"): thin adapter over i2c_owner.c (this directory),
 * exactly as docs/HW_ABSTRACTION.md Phase 1b intends -- the owner IS
 * the backend body. Earlier revisions of this file duplicated i2c_owner.c's
 * task/queue/retry logic directly against driver/i2c_master.h instead of
 * calling it; that duplication is gone. This file now only:
 *  1. Owns the i2c_master_bus_handle_t lifecycle (i2c_new_master_bus /
 *     i2c_del_master_bus), which i2c_owner.c does not do -- i2c_owner_init()
 *     takes an already-created bus handle.
 *  2. Maps each hal_i2c_* call onto the matching i2c_owner_* call.
 *  3. Owns i2c_master_bus_add_device()/i2c_master_probe(), which also sit
 *     outside i2c_owner.c's scope (device attach/probe are not
 *     queue-routed, matching every real caller today -- SX1509.c,
 *     i2c_scan.c).
 *
 * hal_i2c_bus_cfg_t (interface/hal_i2c.h) carries queue_len/task_priority/
 * stack_depth/core_id straight through to i2c_owner_init(), mirroring
 * hal_spi_bus_cfg_t, so each attached device's owner task can be sized
 * independently by its caller (SX1509.c:414's real call passes (8, 5, 4096,
 * tskNO_AFFINITY); FT6336U.c:113/NS2009.c:83 both pass (8, 5, 3072,
 * tskNO_AFFINITY)) and registered for stack-margin reporting via
 * hal_i2c_get_task_handle() below. 0 in any cfg field means "backend
 * default" -- HAL_I2C_ESP_DEFAULT_* below, matching SX1509's larger sizing
 * so a caller that leaves a field at 0 is never under-provisioned relative
 * to today.
 *
 * hal_i2c_bus_init() must NEVER be called on a port that is already open --
 * see its own comment below for the 2026-09-06 hardware bug (a failed
 * i2c_new_master_bus() on an already-open port leaves the port half
 * released, breaking every other consumer sharing it) that killed the old
 * ALREADY_INIT recovery path. A caller sharing a port another driver
 * already brought up (e.g. FT6336U/NS2009 sharing SX1509's I2C_NUM_0) must
 * use hal_i2c_esp_adopt() (hal_i2c_esp_owner.h) on the existing handle/
 * owner instead.
 *
 * i2c_owner.c already preserves the two hard-won behaviors this backend
 * must not regress (interface/hal_i2c.h's own contract comment, and
 * docs/HW_ABSTRACTION.md "hal_i2c"): a STATIC per-call completion
 * semaphore (2026-08-20 SRAM-starvation fix) and worker-enforced
 * timeout_ms with the caller waiting unbounded (use-after-free avoidance),
 * plus the bus-reset-then-retry-once recovery on a failed transfer. Nothing
 * further to preserve here -- i2c_owner_transfer() IS that logic now.
 *
 * hal_i2c_probe() is a direct, unqueued i2c_master_probe() call on the bus
 * handle -- matching every real caller (SX1509.c:532,585,625; i2c_scan.c)
 * exactly: none of them route a probe through the owner queue, since a
 * probe is not a `i2c_master_dev_handle_t`-scoped transfer.
 */
#include "hal_i2c.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "hal_esp_common.h"
#include "hal_i2c_esp_owner.h"
#include "i2c_owner.h"

static const char *TAG = "hal_i2c_esp";

/* Backend defaults used when hal_i2c_bus_cfg_t's corresponding field is 0
 * ("backend default" per hal_i2c.h) -- matches SX1509.c:414's call (the
 * larger of the two live stack_depth values in the tree today, so a caller
 * that leaves this at 0 is never under-provisioned relative to today). A
 * caller that needs FT6336U/NS2009's smaller 3072 sizing now passes it
 * explicitly via cfg->stack_depth instead of being stuck with a hardcoded
 * value -- this is the fix for the widened interface's whole point. */
#define HAL_I2C_ESP_DEFAULT_TASK_STACK    4096
#define HAL_I2C_ESP_DEFAULT_TASK_PRIORITY 5
#define HAL_I2C_ESP_DEFAULT_QUEUE_LEN     8

typedef struct {
    i2c_owner_t owner_storage; /* used only when this bus_t created its own owner */
    i2c_owner_t *owner;        /* &owner_storage normally; an adopted external
                                 * i2c_owner_t* when hal_i2c_esp_adopt() was used
                                 * instead of hal_i2c_bus_init() -- see
                                 * hal_i2c_esp_owner.h. NULL when not initialized. */
    bool bus_owned;   /* false when adopted: don't i2c_del_master_bus() a handle
                        * this hal_i2c_bus_t instance didn't create. */
    bool owner_owned; /* false when adopted: don't i2c_owner_deinit() an owner
                        * this hal_i2c_bus_t instance didn't create -- some other
                        * driver (e.g. SX1509.c) still uses it. */
} hal_i2c_esp_bus_impl_t;

_Static_assert(sizeof(hal_i2c_esp_bus_impl_t) <= sizeof(((hal_i2c_bus_t *)0)->storage),
               "hal_i2c_esp_bus_impl_t exceeds HAL_I2C_BUS_STORAGE_BYTES reservation");

typedef struct {
    i2c_master_dev_handle_t device;
    /* Back-pointer to the owning bus's owner, captured at attach time --
     * hal_i2c_transfer()'s signature (interface/hal_i2c.h) takes only the
     * device, not the bus, so this is the only way a transfer can reach the
     * SAME i2c_owner_t that device_attach() used. */
    i2c_owner_t *owner;
} hal_i2c_esp_device_impl_t;

_Static_assert(sizeof(hal_i2c_esp_device_impl_t) <= sizeof(((hal_i2c_device_t *)0)->storage),
               "hal_i2c_esp_device_impl_t exceeds HAL_I2C_DEVICE_STORAGE_BYTES reservation");

static hal_i2c_esp_bus_impl_t *bus_impl_of(hal_i2c_bus_t *bus) {
    return (hal_i2c_esp_bus_impl_t *)(void *)bus->storage;
}

static hal_i2c_esp_device_impl_t *device_impl_of(hal_i2c_device_t *dev) {
    return (hal_i2c_esp_device_impl_t *)(void *)dev->storage;
}

hal_status_t hal_i2c_bus_init(hal_i2c_bus_t *bus, int bus_id, const hal_i2c_bus_cfg_t *cfg) {
    if (!bus || !cfg) {
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_bus_impl_t *impl = bus_impl_of(bus);
    memset(impl, 0, sizeof(*impl));

    uint32_t queue_len = cfg->queue_len ? cfg->queue_len : HAL_I2C_ESP_DEFAULT_QUEUE_LEN;
    int task_priority = cfg->task_priority ? cfg->task_priority : HAL_I2C_ESP_DEFAULT_TASK_PRIORITY;
    uint32_t stack_depth = cfg->stack_depth ? cfg->stack_depth : HAL_I2C_ESP_DEFAULT_TASK_STACK;
    BaseType_t core_id = (cfg->core_id == HAL_CORE_ANY) ? tskNO_AFFINITY : (BaseType_t)cfg->core_id;

    i2c_master_bus_handle_t bus_handle = NULL;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = (i2c_port_num_t)bus_id,
        .sda_io_num = (gpio_num_t)cfg->sda_pin,
        .scl_io_num = (gpio_num_t)cfg->scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK) {
        /* hal_i2c_bus_init() must NEVER be called on a port that is already
         * open -- this used to recover via i2c_master_get_bus_handle() on
         * ESP_ERR_INVALID_STATE ("ALREADY_INIT"), but IDF's cleanup on the
         * FAILED i2c_new_master_bus() call above leaves the port half
         * released ("acquire bus failed" / "Bus not freed entirely"), so
         * every later transfer on the port -- including by whoever already
         * owned it -- started failing with ESP_ERR_INVALID_RESPONSE. A
         * caller sharing an already-open port must use hal_i2c_esp_adopt()
         * (hal_i2c_esp_owner.h) on the existing handle/owner instead of
         * calling this function at all. No recovery here; just fail loud. */
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s -- if bus %d is already open, use "
                      "hal_i2c_esp_adopt() instead of hal_i2c_bus_init()",
                 esp_err_to_name(err), bus_id);
        memset(impl, 0, sizeof(*impl));
        return hal_esp_err_to_status(err);
    }
    impl->bus_owned = true;

    esp_err_t owner_err = i2c_owner_init(&impl->owner_storage, bus_handle, queue_len, task_priority,
                                          stack_depth, core_id);
    if (owner_err != ESP_OK) {
        i2c_del_master_bus(bus_handle);
        memset(impl, 0, sizeof(*impl));
        return hal_esp_err_to_status(owner_err);
    }
    impl->owner = &impl->owner_storage;
    impl->owner_owned = true;

    return HAL_OK;
}

hal_status_t hal_i2c_esp_adopt(hal_i2c_bus_t *bus, i2c_master_bus_handle_t handle,
                                i2c_owner_t *owner) {
    if (!bus || !owner) {
        return HAL_INVALID_ARG;
    }
    if (!owner->initialized || (handle != NULL && owner->bus != handle)) {
        ESP_LOGE(TAG, "hal_i2c_esp_adopt: owner not initialized or handle mismatch");
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_bus_impl_t *impl = bus_impl_of(bus);
    memset(impl, 0, sizeof(*impl));
    impl->owner = owner;
    impl->bus_owned = false;
    impl->owner_owned = false;
    return HAL_OK;
}

void *hal_i2c_get_task_handle(const hal_i2c_bus_t *bus) {
    if (!bus) {
        return NULL;
    }
    const hal_i2c_esp_bus_impl_t *impl = (const hal_i2c_esp_bus_impl_t *)(const void *)bus->storage;
    if (!impl->owner || !impl->owner->initialized) {
        return NULL;
    }
    return (void *)impl->owner->task_handle;
}

hal_status_t hal_i2c_bus_deinit(hal_i2c_bus_t *bus) {
    if (!bus) {
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_bus_impl_t *impl = bus_impl_of(bus);
    if (!impl->owner || !impl->owner->initialized) {
        return HAL_NOT_READY;
    }

    i2c_master_bus_handle_t bus_handle = impl->owner->bus;
    bool bus_owned = impl->bus_owned;
    bool owner_owned = impl->owner_owned;

    /* Adopted bus (hal_i2c_esp_adopt(), owner_owned == false): this bus_t
     * did not create the owner or the bus handle, so it must not tear
     * either down -- some other driver (e.g. SX1509.c) still uses them.
     * Only this instance's own local storage is cleared. */
    esp_err_t err = ESP_OK;
    if (owner_owned) {
        /* i2c_owner_deinit() only fails today on !owner->initialized, which
         * the check above already rules out -- but treat a failure
         * defensively rather than assume that stays true forever: an
         * earlier return here used to leave `impl` exactly as it was
         * (owner.initialized still true, bus handle still live) on any
         * error, which reads as "still a valid, usable bus" to every other
         * hal_i2c_* call even though the caller was just told deinit
         * happened. Whatever i2c_owner_deinit() returns, still release the
         * bus handle this instance owns (if any) and zero `impl` so a later
         * hal_i2c_transfer/probe/device_attach on this bus_t correctly sees
         * HAL_NOT_READY instead of a half-torn-down owner that reads as
         * initialized. The error itself is still reported to the caller. */
        err = i2c_owner_deinit(impl->owner);
    }

    if (bus_owned && bus_handle) {
        i2c_del_master_bus(bus_handle);
    }

    memset(impl, 0, sizeof(*impl));

    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }
    return HAL_OK;
}

hal_status_t hal_i2c_device_attach(hal_i2c_bus_t *bus, hal_i2c_device_t *dev, uint8_t addr,
                                    uint32_t clock_hz) {
    if (!bus || !dev) {
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_bus_impl_t *bus_impl = bus_impl_of(bus);
    if (!bus_impl->owner || !bus_impl->owner->initialized) {
        return HAL_NOT_READY;
    }
    hal_i2c_esp_device_impl_t *dev_impl = device_impl_of(dev);
    memset(dev_impl, 0, sizeof(*dev_impl));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = clock_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_impl->owner->bus, &dev_config, &dev_impl->device);
    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }
    dev_impl->owner = bus_impl->owner;
    return HAL_OK;
}

hal_status_t hal_i2c_device_detach(hal_i2c_device_t *dev) {
    if (!dev) {
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_device_impl_t *dev_impl = device_impl_of(dev);
    if (!dev_impl->device) {
        return HAL_NOT_READY;
    }
    /* i2c_master_bus_rm_device() is not queue-routed, matching
     * i2c_master_bus_add_device() above -- attach/detach both sit outside
     * i2c_owner.c's transfer queue (device management, not a transfer). */
    esp_err_t err = i2c_master_bus_rm_device(dev_impl->device);
    memset(dev_impl, 0, sizeof(*dev_impl));
    return hal_esp_err_to_status(err);
}

hal_status_t hal_i2c_transfer(hal_i2c_device_t *dev, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                               size_t rx_len, uint32_t timeout_ms) {
    if (!dev) {
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_device_impl_t *dev_impl = device_impl_of(dev);
    if (!dev_impl->device || !dev_impl->owner) {
        return HAL_NOT_READY;
    }

    esp_err_t result = i2c_owner_transfer(dev_impl->owner, dev_impl->device, tx, tx_len, rx, rx_len,
                                           timeout_ms);
    return hal_esp_err_to_status(result);
}

hal_status_t hal_i2c_probe(hal_i2c_bus_t *bus, uint8_t addr, uint32_t timeout_ms) {
    if (!bus) {
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_bus_impl_t *impl = bus_impl_of(bus);
    if (!impl->owner || !impl->owner->initialized) {
        return HAL_NOT_READY;
    }
    esp_err_t err = i2c_master_probe(impl->owner->bus, addr, timeout_ms);
    return hal_esp_err_to_status(err);
}
