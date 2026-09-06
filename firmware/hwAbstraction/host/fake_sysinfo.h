/* fake_sysinfo.h -- host fake backend for hal_sysinfo.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION_PLAN.md "hal_time / hal_wdt / hal_pwm /
 * hal_sysinfo" and the real backend this fake mirrors (esp/sysinfo/
 * hal_sysinfo_esp.c): reset reason, running-partition facts, build
 * descriptor, chip temperature (install/enable/get_celsius/uninstall
 * lifecycle), esp_random()-shaped random words, and core-dump presence/
 * erase. Every one of these is a scriptable host value rather than a
 * simulated device model -- there is no meaningful "device behavior" to
 * fake for a reset-reason register or a build-descriptor struct, only a
 * value a test wants to control.
 *
 * Defaults after fake_sysinfo_reset_all(): reset reason HAL_RESET_POWERON
 * (matching real silicon's first-ever boot), an invalid (zeroed,
 * valid=false) build info -- matching hal_sysinfo_get_build_info()'s real
 * "esp_app_get_description() returned NULL" case -- a zeroed partition-info
 * result available via hal_sysinfo_get_running_partition(), temperature
 * uninitialized (HAL_NOT_READY on read until fake_sysinfo_temp_init()-
 * equivalent hal_sysinfo_temp_init() succeeds), an empty random sequence
 * (falls back to a fixed deterministic value, see below), and no core-dump
 * present.
 *
 * Random sequence: fake_sysinfo_script_random_sequence() arms an ordered
 * list of values hal_sysinfo_random_u32() returns one at a time; once
 * exhausted (or if none was ever armed), it returns a fixed deterministic
 * fallback value (0xA5A5A5A5) rather than anything that looks like real
 * entropy -- a test relying on "looks random" input is exactly the
 * idealized-input bug class this directory's fakes exist to avoid; a test
 * that needs specific values must script them.
 *
 * Core-dump erase clears presence: hal_sysinfo_coredump_erase() sets the
 * scripted presence flag back to false, matching real esp_core_dump_
 * image_erase()'s effect on a later esp_core_dump_image_check().
 */
#ifndef KILNCTL_FAKE_SYSINFO_H
#define KILNCTL_FAKE_SYSINFO_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_sysinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resets every scripted value to its default (see this header's top
 * comment) and un-initializes the temperature lifecycle. Call between test
 * cases. */
void fake_sysinfo_reset_all(void);

/* hal_sysinfo_reset_reason() returns this until changed again. */
void fake_sysinfo_set_reset_reason(hal_reset_reason_t reason);

/* hal_sysinfo_get_running_partition() copies *info out and returns HAL_OK.
 * If never called (or called with info == NULL), the query returns HAL_IO,
 * matching the real backend's "esp_ota_get_running_partition() returned
 * NULL" case. */
void fake_sysinfo_set_running_partition(const hal_sysinfo_partition_info_t *info);

/* hal_sysinfo_get_build_info() copies *info out (valid forced true) until
 * changed again, or fake_sysinfo_set_build_info_invalid() reverts to the
 * "no build descriptor" (valid=false, all-zero) default. */
void fake_sysinfo_set_build_info(const hal_sysinfo_build_info_t *info);
void fake_sysinfo_set_build_info_invalid(void);

/* Scripted temperature-sensor read value: hal_sysinfo_temp_read_celsius()
 * returns this once hal_sysinfo_temp_init() has succeeded. */
void fake_sysinfo_set_temp_celsius(float celsius);

/* One-shot override for the NEXT hal_sysinfo_temp_init() or
 * hal_sysinfo_temp_read_celsius() call (whichever is called next),
 * respectively -- lets a test exercise a sensor-install or a mid-read
 * failure without a real chip. HAL_OK disarms early. */
void fake_sysinfo_script_temp_init_status(hal_status_t status);
void fake_sysinfo_script_temp_read_status(hal_status_t status);

/* Arms an ordered sequence hal_sysinfo_random_u32() returns one value at a
 * time, in order; once exhausted, calls fall back to the fixed
 * deterministic value described in this header's top comment. Passing
 * count == 0 clears any previously armed sequence. */
void fake_sysinfo_script_random_sequence(const uint32_t *values, uint32_t count);

/* Sets whether a core-dump image is "present". hal_sysinfo_coredump_erase()
 * clears this back to false -- see this header's top comment. */
void fake_sysinfo_set_coredump_present(bool present);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_SYSINFO_H */
