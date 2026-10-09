// confirm.c -- see confirm.h.
#include "confirm.h"

uint8_t update_confirm_missing(const update_confirm_checklist_t *c)
{
    uint8_t flags = 0;
    if (!c->config_crc_ok) {
        flags |= (uint8_t)UPDATE_CONFIRM_MISSING_CONFIG_CRC;
    }
    if (!c->thermocouple_plausible) {
        flags |= (uint8_t)UPDATE_CONFIRM_MISSING_THERMOCOUPLE;
    }
    if (!c->adc_sampling_ok) {
        flags |= (uint8_t)UPDATE_CONFIRM_MISSING_ADC;
    }
    if (!c->all_tasks_checked_in) {
        flags |= (uint8_t)UPDATE_CONFIRM_MISSING_WATCHDOG_CHECKINS;
    }
    if (!c->telemetry_sent_ok) {
        flags |= (uint8_t)UPDATE_CONFIRM_MISSING_TELEMETRY;
    }
    return flags;
}
