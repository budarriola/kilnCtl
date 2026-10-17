#pragma once
/* Flash mmap-reader vs SPI1 writer serialization (REVIEW_FLASH_MMAP_SERIALIZATION MED-1).
 *
 * With CONFIG_SPIRAM_XIP_FROM_PSRAM=y, ESP-IDF defines SPI_FLASH_CACHE_NO_DISABLE: an
 * SPI1 erase/program takes only a private mutex and leaves the cache running. A read
 * through an mmap window (esp_partition_mmap, read_otadata, esp_image_verify's
 * bootloader_mmap, esp_core_dump_get_summary) then goes out on SPI0 while the chip is
 * busy and can return garbage (no CONFIG_SPI_FLASH_AUTO_SUSPEND).
 *
 * Fix: flash_xip_lock_install() wraps esp_flash_default_chip->os_func start/end so every
 * SPI1 operation (NVS, LittleFS, OTA, coredump erase, Wi-Fi NVS -- whatever task) holds one
 * recursive app mutex for its bracket. mmap-class callers bracket themselves with
 * flash_xip_reader_enter()/exit(), so no SPI1 op can be busy while they read. Recursive:
 * a reader that also writes (esp_ota_mark_app_valid_cancel_rollback, esp_ota_end,
 * esp_ota_set_boot_partition) re-enters from its own task. Lock order is always
 * app mutex -> IDF s_spi1_flash_mutex, so no inversion.
 *
 * Rule: inside a reader bracket call only IDF flash/ota/image/coredump functions; never
 * wait on another app task (bx_flash_worker etc.), which may itself be blocked here. */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void (*take)(void); /* recursive, blocking */
    void (*give)(void);
} flash_xip_lock_ops_t;

typedef int (*flash_xip_start_fn)(void *arg, uint32_t flags);
typedef int (*flash_xip_end_fn)(void *arg);

#if defined(ESP_PLATFORM) || defined(FLASH_XIP_LOCK_CORE_TEST)
/* Portable core (flash_xip_lock.c). ops NULL = pass-through (not installed). */
void flash_xip_lock_set_ops(const flash_xip_lock_ops_t *ops);
int  flash_xip_lock_wrap_start(flash_xip_start_fn orig, void *arg, uint32_t flags);
int  flash_xip_lock_wrap_end(flash_xip_end_fn orig, void *arg);
void flash_xip_reader_enter(void);
void flash_xip_reader_exit(void);
#else
/* Other host-test builds: no flash, no lock. */
static inline void flash_xip_reader_enter(void) {}
static inline void flash_xip_reader_exit(void) {}
#endif

#if defined(ESP_PLATFORM)
#include "esp_err.h"
/* Target (flash_xip_lock_esp.c). Call from app_main before any other task can write
 * flash. No-op returning ESP_OK when XIP_FROM_PSRAM is off (IDF disables the cache). */
esp_err_t flash_xip_lock_install(void);
#endif
