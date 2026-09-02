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
} spi_transaction_t;

static inline esp_err_t spi_device_transmit(spi_device_handle_t device, spi_transaction_t *trans)
{
    (void)device;
    (void)trans;
    return ESP_OK;
}

#endif // TEST_STUB_SPI_MASTER_H
