/* fake_i2c.c -- host fake backend for hal_i2c.h. See fake_i2c.h. */
#include "fake_i2c.h"

#include <string.h>

#define FAKE_I2C_BUS_MAGIC    0x49324342u /* "I2CB" */
#define FAKE_I2C_DEVICE_MAGIC 0x49324344u /* "I2CD" */
#define FAKE_I2C_RX_QUEUE_CAP 4

typedef struct {
    uint32_t magic;
    int      slot;
} fake_i2c_tag_t;

_Static_assert(sizeof(fake_i2c_tag_t) <= HAL_I2C_BUS_STORAGE_BYTES,
               "fake_i2c_tag_t must fit hal_i2c_bus_t storage");
_Static_assert(sizeof(fake_i2c_tag_t) <= HAL_I2C_DEVICE_STORAGE_BYTES,
               "fake_i2c_tag_t must fit hal_i2c_device_t storage");

typedef struct {
    uint8_t data[FAKE_I2C_MAX_RX_BYTES];
    size_t  len;
} fake_i2c_rx_item_t;

typedef struct {
    bool    in_use;
    uint8_t addr;
    bool    nack;
    bool    timeout_once;

    fake_i2c_rx_item_t rx_queue[FAKE_I2C_RX_QUEUE_CAP];
    size_t              rx_head;
    size_t              rx_count;
} fake_i2c_addr_script_t;

typedef struct {
    bool in_use;
    fake_i2c_addr_script_t scripts[FAKE_I2C_MAX_ADDR_SCRIPTS];
} fake_i2c_bus_slot_t;

typedef struct {
    bool     in_use;
    int      bus_slot;
    uint8_t  addr;
    uint32_t clock_hz;
} fake_i2c_device_slot_t;

static fake_i2c_bus_slot_t    s_buses[FAKE_I2C_MAX_BUSES];
static fake_i2c_device_slot_t s_devices[FAKE_I2C_MAX_DEVICES];

static fake_i2c_transfer_record_t s_log[FAKE_I2C_TRANSFER_LOG_CAP];
static size_t                      s_log_count;

static fake_i2c_bus_slot_t *get_bus(const hal_i2c_bus_t *bus)
{
    if (bus == NULL) return NULL;
    fake_i2c_tag_t tag;
    memcpy(&tag, bus->storage, sizeof(tag));
    if (tag.magic != FAKE_I2C_BUS_MAGIC) return NULL;
    if (tag.slot < 0 || tag.slot >= FAKE_I2C_MAX_BUSES) return NULL;
    if (!s_buses[tag.slot].in_use) return NULL;
    return &s_buses[tag.slot];
}

static int get_bus_slot_index(const hal_i2c_bus_t *bus)
{
    if (bus == NULL) return -1;
    fake_i2c_tag_t tag;
    memcpy(&tag, bus->storage, sizeof(tag));
    if (tag.magic != FAKE_I2C_BUS_MAGIC) return -1;
    if (tag.slot < 0 || tag.slot >= FAKE_I2C_MAX_BUSES) return -1;
    if (!s_buses[tag.slot].in_use) return -1;
    return tag.slot;
}

static fake_i2c_device_slot_t *get_device(const hal_i2c_device_t *dev)
{
    if (dev == NULL) return NULL;
    fake_i2c_tag_t tag;
    memcpy(&tag, dev->storage, sizeof(tag));
    if (tag.magic != FAKE_I2C_DEVICE_MAGIC) return NULL;
    if (tag.slot < 0 || tag.slot >= FAKE_I2C_MAX_DEVICES) return NULL;
    if (!s_devices[tag.slot].in_use) return NULL;
    return &s_devices[tag.slot];
}

/* Finds addr's script entry on this bus, creating one (defaulted to ack, no
 * timeout, empty rx queue) if none exists yet and room remains. Returns
 * NULL only if the table is full and addr is not already present. */
static fake_i2c_addr_script_t *find_or_create_script(fake_i2c_bus_slot_t *b, uint8_t addr)
{
    int free_slot = -1;
    for (int i = 0; i < FAKE_I2C_MAX_ADDR_SCRIPTS; i++) {
        if (b->scripts[i].in_use && b->scripts[i].addr == addr) return &b->scripts[i];
        if (!b->scripts[i].in_use && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;
    memset(&b->scripts[free_slot], 0, sizeof(b->scripts[free_slot]));
    b->scripts[free_slot].in_use = true;
    b->scripts[free_slot].addr = addr;
    return &b->scripts[free_slot];
}

static fake_i2c_addr_script_t *find_script(fake_i2c_bus_slot_t *b, uint8_t addr)
{
    for (int i = 0; i < FAKE_I2C_MAX_ADDR_SCRIPTS; i++) {
        if (b->scripts[i].in_use && b->scripts[i].addr == addr) return &b->scripts[i];
    }
    return NULL;
}

static void log_transfer(int dev_slot, uint8_t addr,
                          const uint8_t *tx, size_t tx_len,
                          const uint8_t *rx, size_t rx_len,
                          uint32_t timeout_ms)
{
    if (s_log_count >= FAKE_I2C_TRANSFER_LOG_CAP) return;
    fake_i2c_transfer_record_t *r = &s_log[s_log_count++];
    r->addr = addr;
    r->dev_slot = dev_slot;
    r->tx_len = (tx_len > FAKE_I2C_MAX_TX_BYTES) ? FAKE_I2C_MAX_TX_BYTES : tx_len;
    if (tx && r->tx_len) memcpy(r->tx, tx, r->tx_len);
    r->rx_len = (rx_len > FAKE_I2C_MAX_RX_BYTES) ? FAKE_I2C_MAX_RX_BYTES : rx_len;
    if (rx && r->rx_len) memcpy(r->rx, rx, r->rx_len);
    r->timeout_ms = timeout_ms;
}

void fake_i2c_reset_all(void)
{
    memset(s_buses, 0, sizeof(s_buses));
    memset(s_devices, 0, sizeof(s_devices));
    memset(s_log, 0, sizeof(s_log));
    s_log_count = 0;
}

bool fake_i2c_bus_is_live(const hal_i2c_bus_t *bus)
{
    return get_bus(bus) != NULL;
}

bool fake_i2c_device_is_live(const hal_i2c_device_t *dev)
{
    return get_device(dev) != NULL;
}

size_t fake_i2c_transfer_count(void)
{
    return s_log_count;
}

const fake_i2c_transfer_record_t *fake_i2c_transfer(size_t index)
{
    if (index >= s_log_count) return NULL;
    return &s_log[index];
}

void fake_i2c_script_nack(hal_i2c_bus_t *bus, uint8_t addr)
{
    fake_i2c_bus_slot_t *b = get_bus(bus);
    if (!b) return;
    fake_i2c_addr_script_t *s = find_or_create_script(b, addr);
    if (!s) return;
    s->nack = true;
    s->timeout_once = false;
}

void fake_i2c_script_ack(hal_i2c_bus_t *bus, uint8_t addr)
{
    fake_i2c_bus_slot_t *b = get_bus(bus);
    if (!b) return;
    fake_i2c_addr_script_t *s = find_or_create_script(b, addr);
    if (!s) return;
    s->nack = false;
    s->timeout_once = false;
}

void fake_i2c_script_transfer_timeout(hal_i2c_bus_t *bus, uint8_t addr)
{
    fake_i2c_bus_slot_t *b = get_bus(bus);
    if (!b) return;
    fake_i2c_addr_script_t *s = find_or_create_script(b, addr);
    if (!s) return;
    s->timeout_once = true;
}

hal_status_t fake_i2c_script_rx(hal_i2c_bus_t *bus, uint8_t addr,
                                 const uint8_t *data, size_t len)
{
    fake_i2c_bus_slot_t *b = get_bus(bus);
    if (!b) return HAL_NOT_READY;
    if (data == NULL && len > 0) return HAL_INVALID_ARG;
    if (len > FAKE_I2C_MAX_RX_BYTES) return HAL_INVALID_SIZE;
    fake_i2c_addr_script_t *s = find_or_create_script(b, addr);
    if (!s) return HAL_NO_MEM;
    if (s->rx_count >= FAKE_I2C_RX_QUEUE_CAP) return HAL_NO_MEM;
    size_t tail = (s->rx_head + s->rx_count) % FAKE_I2C_RX_QUEUE_CAP;
    s->rx_queue[tail].len = len;
    if (len) memcpy(s->rx_queue[tail].data, data, len);
    s->rx_count++;
    return HAL_OK;
}

static void consume_rx_script(fake_i2c_addr_script_t *s, uint8_t *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return;
    if (!s || s->rx_count == 0) {
        memset(out, 0, out_len);
        return;
    }
    fake_i2c_rx_item_t *item = &s->rx_queue[s->rx_head];
    size_t n = (item->len < out_len) ? item->len : out_len;
    memcpy(out, item->data, n);
    if (n < out_len) memset(out + n, 0, out_len - n);
    s->rx_head = (s->rx_head + 1) % FAKE_I2C_RX_QUEUE_CAP;
    s->rx_count--;
}

hal_status_t hal_i2c_bus_init(hal_i2c_bus_t *bus, int bus_id,
                               int scl_pin, int sda_pin)
{
    (void)bus_id; (void)scl_pin; (void)sda_pin;
    if (bus == NULL) return HAL_INVALID_ARG;

    int free_slot = -1;
    for (int i = 0; i < FAKE_I2C_MAX_BUSES; i++) {
        if (!s_buses[i].in_use) { free_slot = i; break; }
    }
    if (free_slot < 0) return HAL_NO_MEM;

    fake_i2c_bus_slot_t *b = &s_buses[free_slot];
    memset(b, 0, sizeof(*b));
    b->in_use = true;

    fake_i2c_tag_t tag;
    tag.magic = FAKE_I2C_BUS_MAGIC;
    tag.slot = free_slot;
    memset(bus->storage, 0, sizeof(bus->storage));
    memcpy(bus->storage, &tag, sizeof(tag));
    return HAL_OK;
}

hal_status_t hal_i2c_bus_deinit(hal_i2c_bus_t *bus)
{
    fake_i2c_bus_slot_t *b = get_bus(bus);
    if (!b) return HAL_NOT_READY;
    b->in_use = false;
    memset(bus->storage, 0, sizeof(bus->storage));
    return HAL_OK;
}

hal_status_t hal_i2c_device_attach(hal_i2c_bus_t *bus, hal_i2c_device_t *dev,
                                    uint8_t addr, uint32_t clock_hz)
{
    if (dev == NULL) return HAL_INVALID_ARG;
    int bus_slot = get_bus_slot_index(bus);
    if (bus_slot < 0) return HAL_NOT_READY;

    int free_slot = -1;
    for (int i = 0; i < FAKE_I2C_MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) { free_slot = i; break; }
    }
    if (free_slot < 0) return HAL_NO_MEM;

    fake_i2c_device_slot_t *d = &s_devices[free_slot];
    memset(d, 0, sizeof(*d));
    d->in_use = true;
    d->bus_slot = bus_slot;
    d->addr = addr;
    d->clock_hz = clock_hz;

    fake_i2c_tag_t tag;
    tag.magic = FAKE_I2C_DEVICE_MAGIC;
    tag.slot = free_slot;
    memset(dev->storage, 0, sizeof(dev->storage));
    memcpy(dev->storage, &tag, sizeof(tag));
    return HAL_OK;
}

hal_status_t hal_i2c_transfer(hal_i2c_device_t *dev,
                               const uint8_t *tx, size_t tx_len,
                               uint8_t *rx, size_t rx_len,
                               uint32_t timeout_ms)
{
    fake_i2c_device_slot_t *d = get_device(dev);
    if (!d) return HAL_NOT_READY;
    if (tx == NULL && tx_len > 0) return HAL_INVALID_ARG;
    if (rx == NULL && rx_len > 0) return HAL_INVALID_ARG;
    if (tx_len > FAKE_I2C_MAX_TX_BYTES || rx_len > FAKE_I2C_MAX_RX_BYTES) return HAL_INVALID_SIZE;

    fake_i2c_bus_slot_t *b = &s_buses[d->bus_slot];
    fake_i2c_addr_script_t *s = find_script(b, d->addr);

    if (s && s->timeout_once) {
        s->timeout_once = false;
        return HAL_TIMEOUT;
    }
    if (s && s->nack) {
        return HAL_IO; /* NACK on a transfer, not a probe -- see hal_i2c.h */
    }

    consume_rx_script(s, rx, rx_len);
    /* dev_slot logged via the tag lookup below to keep the record
     * self-describing even if the device is later detached. */
    int dev_slot = -1;
    {
        fake_i2c_tag_t tag;
        memcpy(&tag, dev->storage, sizeof(tag));
        dev_slot = tag.slot;
    }
    log_transfer(dev_slot, d->addr, tx, tx_len, rx, rx_len, timeout_ms);
    return HAL_OK;
}

hal_status_t hal_i2c_probe(hal_i2c_bus_t *bus, uint8_t addr, uint32_t timeout_ms)
{
    (void)timeout_ms;
    fake_i2c_bus_slot_t *b = get_bus(bus);
    if (!b) return HAL_NOT_READY;
    fake_i2c_addr_script_t *s = find_script(b, addr);
    if (s && s->nack) return HAL_NOT_FOUND;
    return HAL_OK;
}
