/* spi_owner_stub.h -- host stub for pico/spi/spi_owner.h, used ONLY to
 * host-test hal_spi_pico.c's own adapter logic (pin/clock/mode/cs
 * validation, one-device-ever enforcement, tx_len/rx_len matching, error
 * pass-through) in isolation from spi_owner.c's real hardware/spi.h calls.
 *
 * This is deliberately NOT fake_spi.c: fake_spi.c is a host backend for
 * interface/hal_spi.h itself (used by max31856.c's own host test,
 * test_max31856_hal_spi.c, to test a REAL hal_spi CLIENT against a fake
 * hal_spi BACKEND). This stub sits one layer lower -- it fakes the thing
 * hal_spi_pico.c itself calls into (spi_owner_init()/spi_owner_transfer()),
 * so THIS test exercises the real hal_spi_pico.c adapter body, the one
 * piece of code fake_spi.c's own test never touches at all.
 *
 * spi_owner_stub.c implements spi_owner_init()/spi_owner_transfer()
 * (spi_owner.h's exact signatures) against these controls; hal_spi_pico.c
 * itself never includes this header, only spi_owner.h -- keeping the
 * production adapter unaware it is being tested against a stub. */
#ifndef SAFTYFW_SPI_OWNER_STUB_H
#define SAFTYFW_SPI_OWNER_STUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clears every counter/recording and resets both configurable results to
 * their default (success). Call between test cases. */
void spi_owner_stub_reset(void);

/* Next (and every subsequent, until reset) spi_owner_init() call returns
 * this value. Default true. */
void spi_owner_stub_set_init_result(bool ok);

/* Next (and every subsequent, until reset) spi_owner_transfer() call
 * returns this value. Default true. */
void spi_owner_stub_set_transfer_result(bool ok);

size_t spi_owner_stub_init_count(void);
size_t spi_owner_stub_transfer_count(void);

/* Records of the MOST RECENT spi_owner_transfer() call -- sufficient for
 * this test's purposes (hal_spi_pico.c issues at most one spi_owner_transfer()
 * per hal_spi_transfer*() call, so there is no multi-call sequence to log
 * the way fake_spi.c's ordered transfer log covers for a real client). */
size_t spi_owner_stub_last_tx_len(void);
const uint8_t *spi_owner_stub_last_tx(void);
bool spi_owner_stub_last_rx_was_null(void);
size_t spi_owner_stub_last_len_arg(void); /* the one `len` spi_owner_transfer() takes */

#ifdef __cplusplus
}
#endif

#endif /* SAFTYFW_SPI_OWNER_STUB_H */
