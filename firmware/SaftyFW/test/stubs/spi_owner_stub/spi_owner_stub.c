/* spi_owner_stub.c -- see spi_owner_stub.h. Implements spi_owner.h's exact
 * public API against controllable test state; hal_spi_pico.c links against
 * this instead of the real pico-sdk-backed spi_owner.c in the host build. */
#include "spi_owner.h"

#include <string.h>

#include "spi_owner_stub.h"

#define STUB_MAX_TX_BYTES 32

static bool s_init_result = true;
static bool s_transfer_result = true;
static size_t s_init_count = 0;
static size_t s_transfer_count = 0;

static uint8_t s_last_tx[STUB_MAX_TX_BYTES];
static size_t s_last_tx_len = 0;
static bool s_last_rx_was_null = true;
static size_t s_last_len_arg = 0;

void spi_owner_stub_reset(void)
{
    s_init_result = true;
    s_transfer_result = true;
    s_init_count = 0;
    s_transfer_count = 0;
    memset(s_last_tx, 0, sizeof(s_last_tx));
    s_last_tx_len = 0;
    s_last_rx_was_null = true;
    s_last_len_arg = 0;
}

void spi_owner_stub_set_init_result(bool ok) { s_init_result = ok; }
void spi_owner_stub_set_transfer_result(bool ok) { s_transfer_result = ok; }

size_t spi_owner_stub_init_count(void) { return s_init_count; }
size_t spi_owner_stub_transfer_count(void) { return s_transfer_count; }
size_t spi_owner_stub_last_tx_len(void) { return s_last_tx_len; }
const uint8_t *spi_owner_stub_last_tx(void) { return s_last_tx; }
bool spi_owner_stub_last_rx_was_null(void) { return s_last_rx_was_null; }
size_t spi_owner_stub_last_len_arg(void) { return s_last_len_arg; }

bool spi_owner_init(void)
{
    s_init_count++;
    return s_init_result;
}

bool spi_owner_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    s_transfer_count++;
    s_last_len_arg = len;
    s_last_rx_was_null = (rx == NULL);
    s_last_tx_len = (len > STUB_MAX_TX_BYTES) ? STUB_MAX_TX_BYTES : len;
    if (tx && s_last_tx_len) {
        memcpy(s_last_tx, tx, s_last_tx_len);
    }
    return s_transfer_result;
}
