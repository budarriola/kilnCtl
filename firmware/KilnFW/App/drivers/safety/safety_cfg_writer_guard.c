#include "safety_cfg_writer_guard.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "cfg_writer_guard";

/* Every window s_mux guards is a few instructions, never a blocking wait --
 * same short-critical-section reasoning http_async_job.c's old s_busy flag
 * used. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static safety_cfg_writer_t s_owner = SAFETY_CFG_WRITER_NONE;

bool safety_cfg_writer_try_claim(safety_cfg_writer_t who)
{
    if (who == SAFETY_CFG_WRITER_NONE) {
        return false;
    }
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_owner == SAFETY_CFG_WRITER_NONE) {
        s_owner = who;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool safety_cfg_writer_release(safety_cfg_writer_t who)
{
    bool ok = false;
    safety_cfg_writer_t seen;
    portENTER_CRITICAL(&s_mux);
    seen = s_owner;
    if (who != SAFETY_CFG_WRITER_NONE && s_owner == who) {
        s_owner = SAFETY_CFG_WRITER_NONE;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!ok) {
        ESP_LOGE(TAG, "release by %d refused: current owner is %d -- unpaired claim/release", (int)who, (int)seen);
    }
    return ok;
}

safety_cfg_writer_t safety_cfg_writer_owner(void)
{
    portENTER_CRITICAL(&s_mux);
    safety_cfg_writer_t o = s_owner;
    portEXIT_CRITICAL(&s_mux);
    return o;
}
