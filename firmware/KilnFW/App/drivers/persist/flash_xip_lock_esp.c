/* Target install of the flash mmap-reader vs SPI1 writer lock; see flash_xip_lock.h. */
#include "flash_xip_lock.h"

#include "sdkconfig.h"
#include "esp_flash.h"
#include "esp_flash_chips/esp_flash_types.h" /* esp_flash_t / esp_flash_os_functions_t bodies */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *FXL_TAG = "flash_xip_lock";

#if CONFIG_SPIRAM_XIP_FROM_PSRAM
_Static_assert(sizeof(esp_err_t) == sizeof(int), "os_func start/end return esp_err_t == int");

static StaticSemaphore_t                s_mtx_buf;
static SemaphoreHandle_t                s_mtx;
static const esp_flash_os_functions_t  *s_orig;
static esp_flash_os_functions_t         s_wrapped;

static void fxl_take(void) { (void)xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
static void fxl_give(void) { (void)xSemaphoreGiveRecursive(s_mtx); }
static const flash_xip_lock_ops_t s_ops = { .take = fxl_take, .give = fxl_give };

static esp_err_t fxl_start(void *arg, uint32_t flags)
{
    return flash_xip_lock_wrap_start(s_orig->start, arg, flags);
}

static esp_err_t fxl_end(void *arg)
{
    return flash_xip_lock_wrap_end(s_orig->end, arg);
}
#endif

esp_err_t flash_xip_lock_install(void)
{
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
    if (s_orig) {
        return ESP_OK;
    }
    esp_flash_t *chip = esp_flash_default_chip;
    if (chip == NULL || chip->os_func == NULL || chip->os_func->start == NULL || chip->os_func->end == NULL) {
        ESP_LOGE(FXL_TAG, "default flash chip has no os functions; mmap readers unserialized");
        return ESP_ERR_INVALID_STATE;
    }
    s_mtx = xSemaphoreCreateRecursiveMutexStatic(&s_mtx_buf);
    s_orig = chip->os_func;
    s_wrapped = *s_orig;
    s_wrapped.start = fxl_start;
    s_wrapped.end = fxl_end;
    flash_xip_lock_set_ops(&s_ops);
    /* app_main, before any other app task: no SPI1 op is mid-bracket during the swap. */
    chip->os_func = &s_wrapped;
    ESP_LOGI(FXL_TAG, "SPI1 ops and mmap readers serialized (XIP from PSRAM)");
#endif
    return ESP_OK;
}
