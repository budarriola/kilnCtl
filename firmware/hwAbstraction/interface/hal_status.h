/* hal_status.h -- portable HAL status codes.
 *
 * Backend-independent. See docs/HW_ABSTRACTION_PLAN.md "hal_status_t" for
 * the derivation of this table against every consumer that branches on a
 * specific error code (not just OK/fail).
 */
#ifndef KILNCTL_HAL_STATUS_H
#define KILNCTL_HAL_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

/* `_Alignas` is C11-only, `alignas` is the C++ spelling. Hoisted here so
 * hal_uart.h/hal_i2c.h/hal_spi.h (all of which include this header) share a
 * single definition instead of three copies that could drift. */
#ifdef __cplusplus
#define HAL_ALIGNAS8 alignas(8)
#else
#define HAL_ALIGNAS8 _Alignas(8)
#endif

typedef enum {
    HAL_OK = 0,
    HAL_TIMEOUT,        /* operation did not complete within the given budget */
    HAL_BUSY,           /* resource in use; caller may retry */
    HAL_INVALID_ARG,    /* bad argument to the call itself */
    HAL_NO_MEM,         /* allocation/pool exhaustion */
    HAL_IO,             /* transport-level failure; catch-all for raw vendor
                          * codes not otherwise in this table (ESP_FAIL etc.) */
    HAL_WEDGED,         /* latched, sticky failure requiring re-init; distinct
                          * from NOT_READY (recoverable by init) -- see
                          * esp_spi_owner's wedge latch */
    HAL_NOT_READY,      /* the pervasive "!initialized" guard; recoverable by
                          * calling init */
    HAL_VERIFY_FAILED,  /* register read-back mismatch (SX1509, MAX31856);
                          * callers retry on this, abort on HAL_IO */
    HAL_INVALID_SIZE,   /* wrong length, distinct from HAL_INVALID_ARG (bad
                          * argument shape) -- panel_spi_blit's done_cb needs
                          * to tell these apart */
    HAL_NOT_FOUND,      /* probe loops continue-on-NOT_FOUND (SX1509 scan);
                          * must not collapse into HAL_IO or the scan aborts */
    HAL_NOT_SUPPORTED,  /* optional capability not enabled/implemented on this
                          * backend (e.g. async transfer, uart wire-complete) */
} hal_status_t;

/* Stable-name lookup. Some callers forward this string over the wire
 * (gpio_probe -> PcTools matches on esp_err_to_name() text today), so once a
 * name ships it must not be renamed, only added to. */
const char *hal_status_to_name(hal_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_STATUS_H */
