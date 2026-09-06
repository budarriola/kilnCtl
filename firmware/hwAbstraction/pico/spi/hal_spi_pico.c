/* hal_spi_pico.c -- pico-sdk backend for interface/hal_spi.h.
 *
 * HAL Phase 1b ("adapt"), matching the ESP side's hal_spi_esp.c collapse
 * (that file's own top comment, corrected 2026-09-05): this is a thin
 * adapter over the REAL SaftyFW SPI owner (spi_owner.c/h, moved into this
 * same directory by Phase 1a, body byte-identical). It holds no request
 * state of its own and starts no second task/queue -- spi_owner.c already
 * documents (spi_owner.h's own top comment) why this board's single-device
 * SPI0 bus does not need one.
 *
 * spi_owner.c is a SINGLETON: spi_owner_init()/spi_owner_transfer() operate
 * on module-static state, not a caller-supplied handle, and the transport is
 * hardwired to SPI0 + the fixed board_pins.h pins/CS0 + 4 MHz mode 1 (see
 * spi_owner.c's own top-of-file comments for why: exactly one real device on
 * this bus today). Every hal_spi_bus_t/hal_spi_device_t this backend hands
 * out therefore refers to that same singleton -- this file's storage is a
 * thin "have I been initialized/attached" tag, not per-instance state, the
 * same shape hal_uart_pico.c uses for uart_owner.c's equally-singleton ring.
 *
 * INTERFACE MISMATCH notes (reported per docs/HW_ABSTRACTION_PLAN.md rather
 * than silently widening hal_spi.h):
 *
 * 1. hal_spi_bus_init()'s cfg carries queue_len/task_priority/stack_depth/
 *    core_id/dma_use_psram/async_flush/max_transfer_sz/dma_chan -- all of
 *    these are ESP-owner-task sizing/DMA knobs (hal_spi.h's own field
 *    comments already say "Pico ignores" for every one of them), so this
 *    backend does not validate or use them. sck_pin/mosi_pin/miso_pin ARE
 *    checked against board_pins.h's SAFTYFW_PIN_SPI0_{SCK,MOSI,MISO}
 *    constants -- spi_owner_init() has no parameter to route a different
 *    pin set to, so a caller asking for different pins would silently get
 *    the wrong ones without this check.
 * 2. hal_spi_device_attach()'s cfg (clock_hz/mode/hw_cs/cs_pin/queue_size/
 *    input_delay_ns) describes a per-device configuration spi_owner.c has
 *    no way to honor: SPI0 runs at one hardwired clock/mode
 *    (SPI_OWNER_BAUDRATE_HZ, SPI_CPOL_0/SPI_CPHA_1) and CS is bit-banged
 *    internally on the one fixed SAFTYFW_PIN_SPI0_CS0 line -- there is no
 *    per-transfer cs_pin parameter to spi_owner_transfer() at all (unlike
 *    the ESP owner, which takes cs_pin per call because it serializes
 *    several devices through one queue). This backend therefore validates
 *    cfg matches that single hardwired device (clock_hz ==
 *    SPI_OWNER_BAUDRATE_HZ, mode == HAL_SPI_MODE_1, cs_pin ==
 *    SAFTYFW_PIN_SPI0_CS0, hw_cs == HAL_CS_NONE) and fails HAL_INVALID_ARG
 *    otherwise, and only ever supports ONE attached device (a second
 *    hal_spi_device_attach() call, even with matching cfg, returns HAL_BUSY
 *    -- spi_owner.c has exactly one CS line, so a second concurrent device
 *    would silently collide on the wire).
 * 3. hal_spi_transfer()/_polling() take independent tx_len/rx_len;
 *    spi_owner_transfer(tx, rx, len) takes ONE len applied to both buffers
 *    (rx may be NULL to skip read-back, matching spi_write_read_blocking's
 *    own contract). Every real caller (max31856.c) already calls with
 *    tx_len == rx_len whenever rx is non-NULL, so this is not widened here;
 *    a caller passing rx != NULL with rx_len != tx_len gets HAL_INVALID_ARG
 *    rather than a silently truncated/overrun transfer.
 * 4. hal_spi_transfer_polling() has no distinct polling entry point on this
 *    backend -- spi_owner_transfer() already does one blocking
 *    spi_write_read_blocking()/spi_write_blocking() call with no queued/ISR
 *    path to choose between (hal_spi.h's own comment: "pico: identical to
 *    hal_spi_transfer"). This backend dispatches both to the same helper.
 * 5. hal_spi_transfer_async() is HAL_NOT_SUPPORTED: spi_owner.c has no
 *    async/queued transfer of any kind (mutex + direct blocking call only,
 *    per spi_owner.h's own design-rationale comment), matching hal_spi.h's
 *    documented default for a backend that does not enable this feature.
 * 6. hal_spi_bus_deinit() is HAL_NOT_SUPPORTED: spi_owner.h exports no
 *    deinit/teardown function (nothing in SaftyFW ever tears this bus down;
 *    it is brought up once at boot in main.c and lives for the process).
 * 7. hal_spi_get_task_handle() returns NULL unconditionally -- spi_owner.c
 *    has no owner task (hal_spi.h's own doc comment explicitly allows this
 *    for "a backend with no owner task at all, e.g. host/pico today").
 * 8. hal_spi_bus_is_wedged() returns false unconditionally -- spi_owner.c
 *    has no wedge latch (hal_spi.h's own doc comment: "pico/host backends
 *    have no wedge concept and always report false").
 */
#include "hal_spi.h"

#include <string.h>

#include "spi_owner.h"

/* TEMPORARY (HAL Phase 1b), same as spi_owner.c's own top-of-file note:
 * board_pins.h is a SaftyFW header, resolved via the private include dir
 * SaftyFW's CMakeLists.txt gives hwabstraction_pico. */
#include "board_pins.h"

#define HAL_SPI_PICO_BAUDRATE_HZ 4000000u /* mirrors spi_owner.c's SPI_OWNER_BAUDRATE_HZ */

typedef struct {
    uint32_t magic;
    bool     initialized;
    /* Reset-one-side hazard, named per CLAUDE.md's checklist: this flag and
     * hal_spi_pico_device_impl_t::attached below are two sides of the same
     * fact (spi_owner.c has exactly one CS line, so at most one hal_spi
     * device may ever be attached to it). Owning it HERE, on the bus impl
     * that hal_spi_bus_init()'s memset() already zeroes on every (re-)init,
     * means it cannot go stale independently of the bus the way a
     * file-scope global could -- there is no second reset path that clears
     * the bus but forgets this flag, because there is no other reset path
     * at all. hal_spi_bus_deinit() is HAL_NOT_SUPPORTED today (note 6 below)
     * so this is never exercised at runtime yet -- if a real deinit is ever
     * added, it MUST clear this alongside `initialized`, exactly the class
     * of pairing CLAUDE.md's "reset one side of a pair" section warns about. */
    bool     device_attached;
} hal_spi_pico_bus_impl_t;

#define HAL_SPI_PICO_BUS_MAGIC 0x53504942u /* "SPIB" */

_Static_assert(sizeof(hal_spi_pico_bus_impl_t) <= sizeof(((hal_spi_bus_t *)0)->storage),
               "hal_spi_pico_bus_impl_t exceeds HAL_SPI_BUS_STORAGE_BYTES reservation");

typedef struct {
    uint32_t magic;
    bool     attached;
} hal_spi_pico_device_impl_t;

#define HAL_SPI_PICO_DEVICE_MAGIC 0x53504944u /* "SPID" */

_Static_assert(sizeof(hal_spi_pico_device_impl_t) <= sizeof(((hal_spi_device_t *)0)->storage),
               "hal_spi_pico_device_impl_t exceeds HAL_SPI_DEVICE_STORAGE_BYTES reservation");

static hal_spi_pico_bus_impl_t *bus_impl_of(hal_spi_bus_t *bus)
{
    return (hal_spi_pico_bus_impl_t *)(void *)bus->storage;
}

static hal_spi_pico_device_impl_t *device_impl_of(hal_spi_device_t *dev)
{
    return (hal_spi_pico_device_impl_t *)(void *)dev->storage;
}

hal_status_t hal_spi_bus_init(hal_spi_bus_t *bus, int bus_id, const hal_spi_bus_cfg_t *cfg)
{
    (void)bus_id; /* spi_owner.c is hardwired to SPI0; nothing else exists on this board */
    if (!bus || !cfg) {
        return HAL_INVALID_ARG;
    }
    if (cfg->sck_pin != SAFTYFW_PIN_SPI0_SCK || cfg->mosi_pin != SAFTYFW_PIN_SPI0_MOSI ||
        cfg->miso_pin != SAFTYFW_PIN_SPI0_MISO) {
        /* See INTERFACE MISMATCH note 1 -- spi_owner_init() has no parameter
         * to route a different pin set to. */
        return HAL_INVALID_ARG;
    }

    hal_spi_pico_bus_impl_t *impl = bus_impl_of(bus);
    memset(impl, 0, sizeof(*impl));
    impl->magic = HAL_SPI_PICO_BUS_MAGIC;

    if (!spi_owner_init()) {
        return HAL_IO;
    }
    impl->initialized = true;
    return HAL_OK;
}

hal_status_t hal_spi_bus_deinit(hal_spi_bus_t *bus)
{
    (void)bus;
    /* See INTERFACE MISMATCH note 6 -- spi_owner.h has no teardown entry
     * point at all. */
    return HAL_NOT_SUPPORTED;
}

bool hal_spi_bus_is_wedged(const hal_spi_bus_t *bus)
{
    (void)bus;
    return false; /* INTERFACE MISMATCH note 8 */
}

void *hal_spi_get_task_handle(const hal_spi_bus_t *bus)
{
    (void)bus;
    return NULL; /* INTERFACE MISMATCH note 7 */
}

hal_status_t hal_spi_device_attach(hal_spi_bus_t *bus, hal_spi_device_t *dev,
                                    const hal_spi_device_cfg_t *cfg)
{
    if (!bus || !dev || !cfg) {
        return HAL_INVALID_ARG;
    }
    hal_spi_pico_bus_impl_t *bus_impl = bus_impl_of(bus);
    if (bus_impl->magic != HAL_SPI_PICO_BUS_MAGIC || !bus_impl->initialized) {
        return HAL_NOT_READY;
    }
    if (cfg->clock_hz != HAL_SPI_PICO_BAUDRATE_HZ || cfg->mode != HAL_SPI_MODE_1 ||
        cfg->cs_pin != SAFTYFW_PIN_SPI0_CS0 || cfg->hw_cs != HAL_CS_NONE) {
        /* See INTERFACE MISMATCH note 2 -- spi_owner.c has exactly one
         * hardwired clock/mode/CS; nothing else can be honored. */
        return HAL_INVALID_ARG;
    }
    if (bus_impl->device_attached) {
        /* See INTERFACE MISMATCH note 2 -- one CS line, one device, ever. */
        return HAL_BUSY;
    }

    hal_spi_pico_device_impl_t *dev_impl = device_impl_of(dev);
    memset(dev_impl, 0, sizeof(*dev_impl));
    dev_impl->magic = HAL_SPI_PICO_DEVICE_MAGIC;
    dev_impl->attached = true;
    bus_impl->device_attached = true;
    return HAL_OK;
}

static hal_status_t hal_spi_pico_transfer_common(hal_spi_device_t *dev, const uint8_t *tx,
                                                  size_t tx_len, uint8_t *rx, size_t rx_len,
                                                  uint32_t timeout_ms)
{
    (void)timeout_ms; /* spi_owner_transfer() takes no caller-supplied timeout;
                        * its own fixed SPI_OWNER_LOCK_TIMEOUT_MS margin applies. */
    if (!dev) {
        return HAL_INVALID_ARG;
    }
    hal_spi_pico_device_impl_t *dev_impl = device_impl_of(dev);
    if (dev_impl->magic != HAL_SPI_PICO_DEVICE_MAGIC || !dev_impl->attached) {
        return HAL_NOT_READY;
    }
    if (rx != NULL && rx_len != tx_len) {
        /* See INTERFACE MISMATCH note 3 -- spi_owner_transfer() has one
         * length for both buffers. */
        return HAL_INVALID_ARG;
    }

    bool ok = spi_owner_transfer(tx, rx, tx_len);
    return ok ? HAL_OK : HAL_IO;
}

hal_status_t hal_spi_transfer(hal_spi_device_t *dev, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                               size_t rx_len, uint32_t timeout_ms)
{
    return hal_spi_pico_transfer_common(dev, tx, tx_len, rx, rx_len, timeout_ms);
}

hal_status_t hal_spi_transfer_polling(hal_spi_device_t *dev, const uint8_t *tx, size_t tx_len,
                                       uint8_t *rx, size_t rx_len, uint32_t timeout_ms)
{
    /* See INTERFACE MISMATCH note 4 -- identical to hal_spi_transfer() on
     * this backend. */
    return hal_spi_pico_transfer_common(dev, tx, tx_len, rx, rx_len, timeout_ms);
}

hal_status_t hal_spi_transfer_async(hal_spi_device_t *dev, const uint8_t *tx, size_t len,
                                     uint32_t timeout_ms, hal_spi_async_cb_t cb, void *ctx)
{
    (void)dev;
    (void)tx;
    (void)len;
    (void)timeout_ms;
    (void)cb;
    (void)ctx;
    return HAL_NOT_SUPPORTED; /* INTERFACE MISMATCH note 5 */
}
