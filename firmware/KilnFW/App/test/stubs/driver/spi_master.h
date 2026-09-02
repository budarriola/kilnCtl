// Host-test stub -- see stubs/esp_err.h for why these exist.
#ifndef TEST_STUB_SPI_MASTER_H
#define TEST_STUB_SPI_MASTER_H

#include <stddef.h>

#include "esp_err.h"

typedef int spi_host_device_t;
typedef struct spi_device_t *spi_device_handle_t;

/* Added 2026-09-01 for esp_spi_owner.c's host test (test_esp_spi_owner.c),
 * which #includes esp_spi_owner.c directly (same convention as
 * test_kiln_cfg_store.c etc.) -- spi_owner_task() references these types
 * and spi_device_transmit() so the translation unit compiles and links,
 * even though the FreeRTOS task stub (freertos/task.h) never actually runs
 * spi_owner_task() on host, so this function body is never reached by any
 * test. Values/behavior here are never asserted against -- only "the call
 * compiles" matters, same convention as the rest of this directory. */
typedef struct {
    unsigned long length;
    const void *tx_buffer;
    void *rx_buffer;
    unsigned long rxlength;
    unsigned int flags;
} spi_transaction_t;

/* Real value is IDF-internal and irrelevant on host -- only "this bit is set
 * or not" is ever asserted against (test_esp_spi_owner.c, DISPLAY_ST7796_PLAN.md
 * 9.3). Any nonzero, distinct-from-other-flags value is fine here. */
#define SPI_TRANS_DMA_USE_PSRAM (1u << 8)

extern int g_stub_spi_transmit_calls;
/* DISPLAY_ST7796_PLAN.md 9.3: last trans.flags seen by spi_device_transmit(),
 * so test_esp_spi_owner.c can prove owner->dma_use_psram actually reaches
 * the transaction on the queued (display flush) path -- and does NOT reach
 * the polling (MAX31856) path, which never carries a PSRAM buffer. */
extern unsigned int g_stub_spi_transmit_last_flags;

static inline esp_err_t spi_device_transmit(spi_device_handle_t device, spi_transaction_t *trans)
{
    (void)device;
    g_stub_spi_transmit_calls++;
    g_stub_spi_transmit_last_flags = trans ? trans->flags : 0;
    return ESP_OK;
}

/* Added 2026-09-01 for DISPLAY_ST7796_PLAN.md 9.5 (MAX31856 transfers move to
 * spi_device_polling_transmit -- 11us measured vs 26us for the queued/ISR
 * path spi_device_transmit() goes through). Test-visible call counters let
 * test_esp_spi_owner.c prove spi_owner_task() actually dispatches to the
 * polling variant when a request asks for it, and to the queued variant
 * when it does not -- without either function's *behavior* differing here
 * (both just return ESP_OK, same as spi_device_transmit() above; the point
 * under test is which one gets called, not what it does). Declared extern,
 * defined once in test_esp_spi_owner.c, same convention as
 * g_stub_queue_send_calls in stubs/freertos/queue.h. */
extern int g_stub_spi_polling_transmit_calls;
extern unsigned int g_stub_spi_polling_transmit_last_flags;

static inline esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *trans)
{
    (void)device;
    g_stub_spi_polling_transmit_calls++;
    g_stub_spi_polling_transmit_last_flags = trans ? trans->flags : 0;
    return ESP_OK;
}

#endif // TEST_STUB_SPI_MASTER_H
