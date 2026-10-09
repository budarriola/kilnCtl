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
 * INTERFACE MISMATCH notes (reported per docs/HW_ABSTRACTION.md rather
 * than silently widening hal_spi.h):
 *
 * 1. hal_spi_bus_init()'s cfg carries queue_len/task_priority/stack_depth/
 *    core_id/dma_use_psram/async_flush/max_transfer_sz/dma_chan -- all of
 *    these are ESP-owner-task sizing/DMA knobs (hal_spi.h's own field
 *    comments already say "Pico ignores" for every one of them), so this
 *    backend does not validate or use them. sck_pin/mosi_pin/miso_pin/cs0_pin
 *    are forwarded to spi_owner_init() as-is (HAL Phase 1b "close the
 *    upward include": this backend no longer has its own compile-time
 *    notion of the right pins to compare cfg against -- board_pins.h moved
 *    out of this file entirely, and the caller, which does still read it,
 *    is now the one source of truth). cs0_pin == HAL_CS_NONE is rejected
 *    (there is no default to fall back to).
 * 2. hal_spi_device_attach()'s cfg (clock_hz/mode/hw_cs/cs_pin/queue_size/
 *    input_delay_ns) describes a per-device configuration spi_owner.c has
 *    no way to honor: SPI0 runs at one hardwired clock/mode
 *    (SPI_OWNER_BAUDRATE_HZ, SPI_CPOL_0/SPI_CPHA_1) and CS is bit-banged
 *    internally on the one CS line cfg->cs0_pin named at hal_spi_bus_init()
 *    time -- there is no per-transfer cs_pin parameter to
 *    spi_owner_transfer() at all (unlike the ESP owner, which takes cs_pin
 *    per call because it serializes several devices through one queue).
 *    This backend therefore validates cfg matches that single hardwired
 *    device (clock_hz == SPI_OWNER_BAUDRATE_HZ, mode == HAL_SPI_MODE_1,
 *    cs_pin == the bus's own recorded cs0_pin, hw_cs == HAL_CS_NONE) and
 *    fails HAL_INVALID_ARG otherwise, and only ever supports ONE attached
 *    device (a second hal_spi_device_attach() call, even with matching cfg,
 *    returns HAL_BUSY -- spi_owner.c has exactly one CS line, so a second
 *    concurrent device would silently collide on the wire).
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

/* HAL Phase 1b, "close the upward include" (docs/HW_ABSTRACTION.md):
 * this used to #include "board_pins.h" (a SaftyFW header), same as
 * spi_owner.c's former top-of-file note. Pin values now arrive via
 * hal_spi_bus_cfg_t/hal_spi_device_cfg_t at init/attach time instead --
 * see the INTERFACE MISMATCH notes above. */

#define HAL_SPI_PICO_BAUDRATE_HZ 4000000u /* mirrors spi_owner.c's SPI_OWNER_BAUDRATE_HZ */

/* RP2040 GPIO count -- valid pin numbers are 0..29 inclusive. */
#define HAL_SPI_PICO_MAX_GPIO 29

/* The one-device-per-physical-bus fact belongs to the underlying spi_owner.c
 * singleton, not to any single hal_spi_bus_t handle: an OWNED bus and an
 * ADOPTED bus (hal_spi_bus_adopt()) both refer to the same real hardware, so
 * "is a device already attached" must be answered the same way through
 * either one. Making this a per-handle field (as it was before) let an
 * attach through an adopted handle race past an attach already done through
 * the owning handle, since each handle's memset()-zeroed storage started
 * device_attached false independently -- two devices, one bit-banged CS.
 * Named per CLAUDE.md's "reset one side of a pair" checklist: this is now
 * the single owning fact, module-static so every handle onto the one
 * physical bus shares it. hal_spi_bus_deinit() is HAL_NOT_SUPPORTED today
 * for an OWNED bus (note 6 below) so this is never cleared at runtime yet --
 * if a real deinit is ever added, it MUST clear this alongside the bus impl
 * state it is now decoupled from. */
static bool s_spi_owner_device_attached = false;

typedef struct {
    uint32_t magic;
    bool     initialized;
    /* The one CS0 GPIO this bus's spi_owner_init() brought up -- recorded so
     * hal_spi_device_attach() can validate a device cfg's cs_pin against
     * what THIS bus actually initialized, instead of a compile-time
     * board_pins.h constant (HAL Phase 1b, "close the upward include"). */
    uint8_t  cs0_pin;
    /* False when adopted via hal_spi_bus_adopt(): this hal_spi_bus_t
     * instance shares the singleton spi_owner.c bus another instance
     * already brought up, and must not re-run spi_owner_init() or claim any
     * teardown rights over it. Mirrors hal_spi_esp.c's owner_owned. */
    bool     owner_owned;
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
    if (cfg->cs0_pin == HAL_CS_NONE) {
        /* See INTERFACE MISMATCH note 1 -- spi_owner_init() has no default
         * CS0 pin to fall back to; the caller must name one. */
        return HAL_INVALID_ARG;
    }
    if (cfg->sck_pin < 0 || cfg->sck_pin > HAL_SPI_PICO_MAX_GPIO ||
        cfg->mosi_pin < 0 || cfg->mosi_pin > HAL_SPI_PICO_MAX_GPIO ||
        cfg->miso_pin < 0 || cfg->miso_pin > HAL_SPI_PICO_MAX_GPIO ||
        cfg->cs0_pin < 0 || cfg->cs0_pin > HAL_SPI_PICO_MAX_GPIO) {
        /* Out-of-range pin numbers would otherwise be silently truncated by
         * the int-to-uint8_t narrowing below. */
        return HAL_INVALID_ARG;
    }
    if (cfg->sck_pin == cfg->mosi_pin || cfg->sck_pin == cfg->miso_pin ||
        cfg->sck_pin == cfg->cs0_pin || cfg->mosi_pin == cfg->miso_pin ||
        cfg->mosi_pin == cfg->cs0_pin || cfg->miso_pin == cfg->cs0_pin) {
        /* A zeroed (or otherwise degenerate) cfg must not drive multiple
         * SPI0 roles off the same GPIO -- e.g. an all-zero cfg would
         * otherwise pass with sck/mosi/miso/cs0 all on GPIO0. */
        return HAL_INVALID_ARG;
    }

    hal_spi_pico_bus_impl_t *impl = bus_impl_of(bus);
    memset(impl, 0, sizeof(*impl));
    impl->magic = HAL_SPI_PICO_BUS_MAGIC;

    spi_owner_pins_t pins = {
        .sck_pin = (uint8_t)cfg->sck_pin,
        .mosi_pin = (uint8_t)cfg->mosi_pin,
        .miso_pin = (uint8_t)cfg->miso_pin,
        .cs0_pin = (uint8_t)cfg->cs0_pin,
    };
    if (!spi_owner_init(&pins)) {
        return HAL_IO;
    }
    impl->initialized = true;
    impl->owner_owned = true;
    impl->cs0_pin = pins.cs0_pin;
    /* A fresh spi_owner_init() call means a fresh bring-up of the singleton
     * bus -- nothing can already be attached to it. This also gives host
     * tests, which run many bus_init() calls in one process, a real reset
     * point for s_spi_owner_device_attached instead of it leaking state
     * across otherwise-independent test cases. */
    s_spi_owner_device_attached = false;
    return HAL_OK;
}

hal_status_t hal_spi_bus_adopt(hal_spi_bus_t *bus, int bus_id, hal_spi_bus_t *existing)
{
    (void)bus_id; /* spi_owner.c is hardwired to SPI0; nothing else exists on this board */
    if (!bus || !existing) {
        return HAL_INVALID_ARG;
    }
    hal_spi_pico_bus_impl_t *existing_impl = bus_impl_of(existing);
    if (existing_impl->magic != HAL_SPI_PICO_BUS_MAGIC || !existing_impl->initialized) {
        return HAL_NOT_READY;
    }

    hal_spi_pico_bus_impl_t *impl = bus_impl_of(bus);
    memset(impl, 0, sizeof(*impl));
    impl->magic = HAL_SPI_PICO_BUS_MAGIC;
    impl->initialized = true;
    impl->cs0_pin = existing_impl->cs0_pin;
    impl->owner_owned = false; /* adopted -- never re-init or tear down the singleton */
    return HAL_OK;
}

hal_status_t hal_spi_bus_deinit(hal_spi_bus_t *bus)
{
    if (!bus) {
        return HAL_INVALID_ARG;
    }
    hal_spi_pico_bus_impl_t *impl = bus_impl_of(bus);
    if (!impl->owner_owned) {
        /* Adopted bus: this instance never created spi_owner.c's singleton
         * state, so it must not claim any teardown rights over it -- some
         * other hal_spi_bus_t (whoever's hal_spi_bus_init() actually ran)
         * still uses it. Only this instance's own local storage is cleared. */
        memset(impl, 0, sizeof(*impl));
        return HAL_OK;
    }
    /* See INTERFACE MISMATCH note 6 -- spi_owner.h has no teardown entry
     * point at all for the real, owning instance. */
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
        cfg->cs_pin != bus_impl->cs0_pin || cfg->hw_cs != HAL_CS_NONE) {
        /* See INTERFACE MISMATCH note 2 -- spi_owner.c has exactly one
         * hardwired clock/mode/CS; nothing else can be honored. */
        return HAL_INVALID_ARG;
    }
    if (s_spi_owner_device_attached) {
        /* See INTERFACE MISMATCH note 2 -- one CS line, one device, ever.
         * This is a module-level fact about the single spi_owner.c
         * singleton, not a per-handle one, so an attach through an ADOPTED
         * bus is refused just as surely as a second attach through the
         * owning bus -- see s_spi_owner_device_attached's own comment. */
        return HAL_BUSY;
    }

    hal_spi_pico_device_impl_t *dev_impl = device_impl_of(dev);
    memset(dev_impl, 0, sizeof(*dev_impl));
    dev_impl->magic = HAL_SPI_PICO_DEVICE_MAGIC;
    dev_impl->attached = true;
    s_spi_owner_device_attached = true;
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
