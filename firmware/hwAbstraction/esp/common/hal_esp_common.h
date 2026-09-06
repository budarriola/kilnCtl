/* hal_esp_common.h -- shared esp_err_t -> hal_status_t mapping for ESP-IDF
 * hwAbstraction backends (hal_gpio_esp.c, hal_adc_esp.c, and any future
 * esp/ backend). See docs/HW_ABSTRACTION_PLAN.md Phase 1b.
 *
 * Phase 1a/1b note: this header is a Phase-1b addition (Phase 0 shipped only
 * the portable interface/ headers), so it is ESP-IDF-facing on purpose --
 * it includes esp_err.h and must never be pulled in by interface/ or by
 * host/ code.
 */
#ifndef KILNCTL_HAL_ESP_COMMON_H
#define KILNCTL_HAL_ESP_COMMON_H

#include "esp_err.h"

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maps an esp_err_t returned by an ESP-IDF driver call onto the shared
 * hal_status_t table (hal_status.h). Every hwAbstraction/esp backend routes
 * its driver return codes through this one function so the mapping cannot
 * drift between backends.
 *
 * Mapping rationale (only the codes ESP-IDF's gpio/adc driver families
 * actually return are given a specific home; everything else is HAL_IO,
 * the documented catch-all for "raw vendor codes not otherwise in this
 * table"):
 *   ESP_OK                    -> HAL_OK
 *   ESP_ERR_TIMEOUT            -> HAL_TIMEOUT
 *   ESP_ERR_INVALID_ARG        -> HAL_INVALID_ARG
 *   ESP_ERR_INVALID_STATE      -> HAL_NOT_READY   (the pervasive ESP-IDF
 *                                 "not initialized / wrong state" code;
 *                                 matches hal_status.h's own description of
 *                                 HAL_NOT_READY as "recoverable by calling
 *                                 init")
 *   ESP_ERR_INVALID_SIZE       -> HAL_INVALID_SIZE
 *   ESP_ERR_NO_MEM             -> HAL_NO_MEM
 *   ESP_ERR_NOT_FOUND          -> HAL_NOT_FOUND
 *   ESP_ERR_NOT_SUPPORTED      -> HAL_NOT_SUPPORTED
 *   anything else (ESP_FAIL et al.) -> HAL_IO
 *
 * HAL_BUSY, HAL_WEDGED and HAL_VERIFY_FAILED have no ESP-IDF gpio/adc
 * equivalent and are never produced by this mapping; callers that need them
 * (e.g. a wedge latch) set them directly, same as esp_spi_owner does today.
 */
hal_status_t hal_esp_err_to_status(esp_err_t err);

/* Reverse of the above, for App-level callers that still expose an
 * esp_err_t-shaped API (e.g. panel_spi_bringup.c's ILI9488_init()) but now
 * get their GPIO status from hal_gpio.h. Approximate by construction: the
 * forward map is lossy (HAL_IO absorbs every uncommon esp_err_t), so this
 * side only reconstructs the codes callers actually branch/log on; anything
 * without a specific home (HAL_IO, HAL_BUSY, HAL_WEDGED, HAL_VERIFY_FAILED,
 * HAL_NOT_SUPPORTED's rarer callers) comes back as ESP_FAIL. */
esp_err_t hal_status_to_esp_err(hal_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_ESP_COMMON_H */
