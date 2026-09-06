/* fake_spi.c -- host fake backend for hal_spi.h. See fake_spi.h. */
#include "fake_spi.h"

#include <string.h>

#define FAKE_SPI_BUS_MAGIC    0x53504255u /* "SPBU" */
#define FAKE_SPI_DEVICE_MAGIC 0x53504445u /* "SPDE" */

typedef struct {
    uint32_t magic;
    int      slot;
} fake_spi_tag_t;

_Static_assert(sizeof(fake_spi_tag_t) <= HAL_SPI_BUS_STORAGE_BYTES,
               "fake_spi_tag_t must fit hal_spi_bus_t storage");
_Static_assert(sizeof(fake_spi_tag_t) <= HAL_SPI_DEVICE_STORAGE_BYTES,
               "fake_spi_tag_t must fit hal_spi_device_t storage");

typedef struct {
    uint8_t data[FAKE_SPI_MAX_RX_BYTES];
    size_t  len;
} fake_spi_rx_item_t;

typedef struct {
    bool     in_use;
    int      bus_slot;
    hal_spi_device_cfg_t cfg;

    fake_spi_rx_item_t rx_queue[FAKE_SPI_RX_SCRIPT_QUEUE_CAP];
    size_t              rx_head;
    size_t              rx_count;
} fake_spi_device_slot_t;

typedef struct {
    void            *ctx;
    hal_spi_async_cb_t cb;
    hal_status_t     result;
    bool             in_use;
} fake_spi_pending_async_t;

typedef struct {
    bool     in_use;
    bool     wedged;
    int      enqueue_timeout_remaining;
    int      completion_timeout_remaining;
    fake_spi_pending_async_t pending[FAKE_SPI_MAX_PENDING_ASYNC];
} fake_spi_bus_slot_t;

static fake_spi_bus_slot_t   s_buses[FAKE_SPI_MAX_BUSES];
static fake_spi_device_slot_t s_devices[FAKE_SPI_MAX_DEVICES];

static fake_spi_transfer_record_t s_log[FAKE_SPI_TRANSFER_LOG_CAP];
static size_t                      s_log_count;

static void log_transfer(fake_spi_xfer_kind_t kind, int dev_slot,
                          const uint8_t *tx, size_t tx_len,
                          const uint8_t *rx, size_t rx_len,
                          uint32_t timeout_ms)
{
    if (s_log_count >= FAKE_SPI_TRANSFER_LOG_CAP) return;
    fake_spi_transfer_record_t *r = &s_log[s_log_count++];
    r->kind = kind;
    r->dev_slot = dev_slot;
    r->tx_len = (tx_len > FAKE_SPI_MAX_TX_BYTES) ? FAKE_SPI_MAX_TX_BYTES : tx_len;
    if (tx && r->tx_len) memcpy(r->tx, tx, r->tx_len);
    r->rx_len = (rx_len > FAKE_SPI_MAX_RX_BYTES) ? FAKE_SPI_MAX_RX_BYTES : rx_len;
    if (rx && r->rx_len) memcpy(r->rx, rx, r->rx_len);
    r->timeout_ms = timeout_ms;
}

static fake_spi_bus_slot_t *get_bus(const hal_spi_bus_t *bus)
{
    if (bus == NULL) return NULL;
    fake_spi_tag_t tag;
    memcpy(&tag, bus->storage, sizeof(tag));
    if (tag.magic != FAKE_SPI_BUS_MAGIC) return NULL;
    if (tag.slot < 0 || tag.slot >= FAKE_SPI_MAX_BUSES) return NULL;
    if (!s_buses[tag.slot].in_use) return NULL;
    return &s_buses[tag.slot];
}

static int get_bus_slot_index(const hal_spi_bus_t *bus)
{
    if (bus == NULL) return -1;
    fake_spi_tag_t tag;
    memcpy(&tag, bus->storage, sizeof(tag));
    if (tag.magic != FAKE_SPI_BUS_MAGIC) return -1;
    if (tag.slot < 0 || tag.slot >= FAKE_SPI_MAX_BUSES) return -1;
    if (!s_buses[tag.slot].in_use) return -1;
    return tag.slot;
}

static fake_spi_device_slot_t *get_device(const hal_spi_device_t *dev)
{
    if (dev == NULL) return NULL;
    fake_spi_tag_t tag;
    memcpy(&tag, dev->storage, sizeof(tag));
    if (tag.magic != FAKE_SPI_DEVICE_MAGIC) return NULL;
    if (tag.slot < 0 || tag.slot >= FAKE_SPI_MAX_DEVICES) return NULL;
    if (!s_devices[tag.slot].in_use) return NULL;
    return &s_devices[tag.slot];
}

static int get_device_slot_index(const hal_spi_device_t *dev)
{
    if (dev == NULL) return -1;
    fake_spi_tag_t tag;
    memcpy(&tag, dev->storage, sizeof(tag));
    if (tag.magic != FAKE_SPI_DEVICE_MAGIC) return -1;
    if (tag.slot < 0 || tag.slot >= FAKE_SPI_MAX_DEVICES) return -1;
    if (!s_devices[tag.slot].in_use) return -1;
    return tag.slot;
}

void fake_spi_reset_all(void)
{
    memset(s_buses, 0, sizeof(s_buses));
    memset(s_devices, 0, sizeof(s_devices));
    memset(s_log, 0, sizeof(s_log));
    s_log_count = 0;
}

bool fake_spi_bus_is_live(const hal_spi_bus_t *bus)
{
    return get_bus(bus) != NULL;
}

bool fake_spi_device_is_live(const hal_spi_device_t *dev)
{
    return get_device(dev) != NULL;
}

size_t fake_spi_transfer_count(void)
{
    return s_log_count;
}

const fake_spi_transfer_record_t *fake_spi_transfer(size_t index)
{
    if (index >= s_log_count) return NULL;
    return &s_log[index];
}

hal_status_t fake_spi_script_rx(hal_spi_device_t *dev, const uint8_t *data, size_t len)
{
    fake_spi_device_slot_t *d = get_device(dev);
    if (!d) return HAL_NOT_READY;
    if (data == NULL && len > 0) return HAL_INVALID_ARG;
    if (len > FAKE_SPI_MAX_RX_BYTES) return HAL_INVALID_SIZE;
    if (d->rx_count >= FAKE_SPI_RX_SCRIPT_QUEUE_CAP) return HAL_NO_MEM;
    size_t tail = (d->rx_head + d->rx_count) % FAKE_SPI_RX_SCRIPT_QUEUE_CAP;
    fake_spi_rx_item_t *item = &d->rx_queue[tail];
    item->len = len;
    if (len) memcpy(item->data, data, len);
    d->rx_count++;
    return HAL_OK;
}

/* Consumes the oldest scripted rx response (if any) into out[0..out_len),
 * zero-filling anything beyond the scripted length or when nothing is
 * queued. Does nothing (leaves out untouched) if out is NULL. */
static void consume_rx_script(fake_spi_device_slot_t *d, uint8_t *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return;
    if (d->rx_count == 0) {
        memset(out, 0, out_len);
        return;
    }
    fake_spi_rx_item_t *item = &d->rx_queue[d->rx_head];
    size_t n = (item->len < out_len) ? item->len : out_len;
    memcpy(out, item->data, n);
    if (n < out_len) memset(out + n, 0, out_len - n);
    d->rx_head = (d->rx_head + 1) % FAKE_SPI_RX_SCRIPT_QUEUE_CAP;
    d->rx_count--;
}

void fake_spi_inject_enqueue_timeout(hal_spi_bus_t *bus, int count)
{
    fake_spi_bus_slot_t *b = get_bus(bus);
    if (!b || count < 0) return;
    b->enqueue_timeout_remaining = count;
}

void fake_spi_inject_completion_timeout(hal_spi_bus_t *bus, int count)
{
    fake_spi_bus_slot_t *b = get_bus(bus);
    if (!b || count < 0) return;
    b->completion_timeout_remaining = count;
}

size_t fake_spi_pending_async_count(const hal_spi_bus_t *bus)
{
    fake_spi_bus_slot_t *b = get_bus(bus);
    if (!b) return 0;
    size_t n = 0;
    for (int i = 0; i < FAKE_SPI_MAX_PENDING_ASYNC; i++) {
        if (b->pending[i].in_use) n++;
    }
    return n;
}

bool fake_spi_pump(hal_spi_bus_t *bus)
{
    fake_spi_bus_slot_t *b = get_bus(bus);
    if (!b) return false;
    /* Oldest-enqueued-first: pending[] is a fixed array populated
     * append-at-first-free-slot, so the lowest-index in-use entry is also
     * the oldest -- FIFO without a separate ring, matching the ESP queue's
     * order for this bus's shared owner queue. */
    for (int i = 0; i < FAKE_SPI_MAX_PENDING_ASYNC; i++) {
        if (b->pending[i].in_use) {
            fake_spi_pending_async_t p = b->pending[i];
            b->pending[i].in_use = false;
            if (p.cb) p.cb(p.ctx, p.result);
            return true;
        }
    }
    return false;
}

hal_status_t hal_spi_bus_init(hal_spi_bus_t *bus, int bus_id,
                               const hal_spi_bus_cfg_t *cfg)
{
    (void)bus_id;
    if (bus == NULL || cfg == NULL) return HAL_INVALID_ARG;

    int free_slot = -1;
    for (int i = 0; i < FAKE_SPI_MAX_BUSES; i++) {
        if (!s_buses[i].in_use) { free_slot = i; break; }
    }
    if (free_slot < 0) return HAL_NO_MEM;

    fake_spi_bus_slot_t *b = &s_buses[free_slot];
    memset(b, 0, sizeof(*b));
    b->in_use = true;

    fake_spi_tag_t tag;
    tag.magic = FAKE_SPI_BUS_MAGIC;
    tag.slot = free_slot;
    memset(bus->storage, 0, sizeof(bus->storage));
    memcpy(bus->storage, &tag, sizeof(tag));
    return HAL_OK;
}

hal_status_t hal_spi_bus_deinit(hal_spi_bus_t *bus)
{
    fake_spi_bus_slot_t *b = get_bus(bus);
    if (!b) return HAL_NOT_READY;
    /* Deinit orphans any devices still attached to this bus -- their calls
     * will read as HAL_NOT_READY via the bus slot below, matching real
     * hardware where a torn-down bus cannot serve an attached device. */
    b->in_use = false;
    memset(bus->storage, 0, sizeof(bus->storage));
    return HAL_OK;
}

bool hal_spi_bus_is_wedged(const hal_spi_bus_t *bus)
{
    fake_spi_bus_slot_t *b = get_bus(bus);
    return b ? b->wedged : false;
}

void *hal_spi_get_task_handle(const hal_spi_bus_t *bus)
{
    /* Host fake has no owner task (single-threaded, synchronously pumped). */
    (void)bus;
    return NULL;
}

hal_status_t hal_spi_device_attach(hal_spi_bus_t *bus, hal_spi_device_t *dev,
                                    const hal_spi_device_cfg_t *cfg)
{
    if (dev == NULL || cfg == NULL) return HAL_INVALID_ARG;
    int bus_slot = get_bus_slot_index(bus);
    if (bus_slot < 0) return HAL_NOT_READY;

    int free_slot = -1;
    for (int i = 0; i < FAKE_SPI_MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) { free_slot = i; break; }
    }
    if (free_slot < 0) return HAL_NO_MEM;

    fake_spi_device_slot_t *d = &s_devices[free_slot];
    memset(d, 0, sizeof(*d));
    d->in_use = true;
    d->bus_slot = bus_slot;
    d->cfg = *cfg;

    fake_spi_tag_t tag;
    tag.magic = FAKE_SPI_DEVICE_MAGIC;
    tag.slot = free_slot;
    memset(dev->storage, 0, sizeof(dev->storage));
    memcpy(dev->storage, &tag, sizeof(tag));
    return HAL_OK;
}

/* Shared body for hal_spi_transfer / hal_spi_transfer_polling: both are
 * synchronous and differ only in the logged kind (ESP: polling_transmit vs
 * the queued path; pico: identical either way, per hal_spi.h). */
static hal_status_t do_sync_transfer(fake_spi_xfer_kind_t kind,
                                      hal_spi_device_t *dev,
                                      const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len,
                                      uint32_t timeout_ms)
{
    fake_spi_device_slot_t *d = get_device(dev);
    if (!d) return HAL_NOT_READY;
    if (tx == NULL && tx_len > 0) return HAL_INVALID_ARG;
    if (rx == NULL && rx_len > 0) return HAL_INVALID_ARG;
    if (tx_len > FAKE_SPI_MAX_TX_BYTES || rx_len > FAKE_SPI_MAX_RX_BYTES) return HAL_INVALID_SIZE;

    fake_spi_bus_slot_t *b = &s_buses[d->bus_slot];
    if (b->enqueue_timeout_remaining > 0) {
        b->enqueue_timeout_remaining--;
        /* Enqueue-timeout: the request was never accepted, so it must not
         * appear in the ordered transfer log (see fake_spi.h). */
        return HAL_TIMEOUT;
    }

    consume_rx_script(d, rx, rx_len);
    log_transfer(kind, get_device_slot_index(dev), tx, tx_len, rx, rx_len, timeout_ms);
    return HAL_OK;
}

hal_status_t hal_spi_transfer(hal_spi_device_t *dev,
                               const uint8_t *tx, size_t tx_len,
                               uint8_t *rx, size_t rx_len,
                               uint32_t timeout_ms)
{
    return do_sync_transfer(FAKE_SPI_XFER_NORMAL, dev, tx, tx_len, rx, rx_len, timeout_ms);
}

hal_status_t hal_spi_transfer_polling(hal_spi_device_t *dev,
                                       const uint8_t *tx, size_t tx_len,
                                       uint8_t *rx, size_t rx_len,
                                       uint32_t timeout_ms)
{
    return do_sync_transfer(FAKE_SPI_XFER_POLLING, dev, tx, tx_len, rx, rx_len, timeout_ms);
}

hal_status_t hal_spi_transfer_async(hal_spi_device_t *dev,
                                     const uint8_t *tx, size_t len,
                                     uint32_t timeout_ms,
                                     hal_spi_async_cb_t cb, void *ctx)
{
    fake_spi_device_slot_t *d = get_device(dev);
    if (!d) return HAL_NOT_READY;
    if (tx == NULL && len > 0) return HAL_INVALID_ARG;
    if (len > FAKE_SPI_MAX_TX_BYTES) return HAL_INVALID_SIZE;

    fake_spi_bus_slot_t *b = &s_buses[d->bus_slot];
    if (b->enqueue_timeout_remaining > 0) {
        b->enqueue_timeout_remaining--;
        return HAL_TIMEOUT;
    }

    int free_pending = -1;
    for (int i = 0; i < FAKE_SPI_MAX_PENDING_ASYNC; i++) {
        if (!b->pending[i].in_use) { free_pending = i; break; }
    }
    if (free_pending < 0) {
        /* Pool exhaustion, distinct from a timeout: latches the wedge --
         * see hal_spi_bus_is_wedged's contract in fake_spi.h. */
        b->wedged = true;
        return HAL_NO_MEM;
    }

    hal_status_t result = HAL_OK;
    if (b->completion_timeout_remaining > 0) {
        b->completion_timeout_remaining--;
        result = HAL_TIMEOUT;
    }

    b->pending[free_pending].in_use = true;
    b->pending[free_pending].cb = cb;
    b->pending[free_pending].ctx = ctx;
    b->pending[free_pending].result = result;

    /* The request itself IS accepted (unlike an enqueue-timeout), so it is
     * logged now, at enqueue time -- fake_spi_pump() only fires the
     * callback later, mirroring the ESP owner queue building exactly one
     * request per call at enqueue time (hal_spi.h's "one request build per
     * call"). No rx buffer exists for async (hal_spi.h has no rx param
     * here), so nothing is scripted/consumed for this kind. */
    log_transfer(FAKE_SPI_XFER_ASYNC, get_device_slot_index(dev), tx, len, NULL, 0, timeout_ms);
    return HAL_OK;
}
