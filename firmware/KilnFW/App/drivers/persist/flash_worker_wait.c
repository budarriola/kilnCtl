// See flash_worker_wait.h for the full rationale.
#include "flash_worker_wait.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Hand-declared rather than #include "uart_bridge.h" -- same reasoning as
 * cfg_fs_mount.c's/factory_reset.c's identical block: that header pulls in
 * hardware-bridge task declarations this file needs none of. Keep in sync
 * with uart_bridge.h by hand if the signature ever changes. */
bool uart_bridge_ext_flash_worker_started(void);

bool flash_worker_wait_until_started(bool (*started_fn)(void), uint32_t poll_ms, uint32_t ceiling_ms)
{
    if (!started_fn) {
        return false;
    }
    uint32_t waited_ms = 0;
    while (!started_fn()) {
        if (waited_ms >= ceiling_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        waited_ms += poll_ms;
    }
    return true;
}

bool flash_worker_wait_default(void)
{
    return flash_worker_wait_until_started(uart_bridge_ext_flash_worker_started,
                                            FLASH_WORKER_WAIT_POLL_MS_DEFAULT, FLASH_WORKER_WAIT_CEILING_MS_DEFAULT);
}
