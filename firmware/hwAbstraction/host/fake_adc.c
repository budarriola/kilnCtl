/* fake_adc.c -- host fake backend for hal_adc.h. See fake_adc.h. */
#include "fake_adc.h"

#include <string.h>

#define FAKE_ADC_NUM_GPIO_PINS 64

typedef struct {
    uint16_t samples[FAKE_ADC_SCRIPT_CAP];
    size_t   head;
    size_t   count;
} fake_adc_channel_t;

static bool                s_initialized;
static bool                s_gpio_enabled[FAKE_ADC_NUM_GPIO_PINS];
static int                 s_selected_channel = -1;
static fake_adc_channel_t  s_channels[FAKE_ADC_NUM_CHANNELS];
static size_t              s_read_count;

static bool chan_in_range(int ch)
{
    return ch >= 0 && ch < FAKE_ADC_NUM_CHANNELS;
}

void fake_adc_reset(void)
{
    memset(s_gpio_enabled, 0, sizeof(s_gpio_enabled));
    memset(s_channels, 0, sizeof(s_channels));
    s_initialized = false;
    s_selected_channel = -1;
    s_read_count = 0;
}

hal_status_t fake_adc_script_samples(int channel, const uint16_t *samples, size_t count)
{
    if (!chan_in_range(channel)) return HAL_INVALID_ARG;
    if (samples == NULL && count > 0) return HAL_INVALID_ARG;
    fake_adc_channel_t *c = &s_channels[channel];
    for (size_t i = 0; i < count; i++) {
        if (c->count >= FAKE_ADC_SCRIPT_CAP) return HAL_NO_MEM; /* buffer overflow */
        size_t idx = (c->head + c->count) % FAKE_ADC_SCRIPT_CAP;
        c->samples[idx] = samples[i];
        c->count++;
    }
    return HAL_OK;
}

size_t fake_adc_pending_count(int channel)
{
    return chan_in_range(channel) ? s_channels[channel].count : 0;
}

bool fake_adc_is_initialized(void)
{
    return s_initialized;
}

int fake_adc_current_channel(void)
{
    return s_initialized ? s_selected_channel : -1;
}

bool fake_adc_is_gpio_enabled(int pin)
{
    return pin >= 0 && pin < FAKE_ADC_NUM_GPIO_PINS && s_gpio_enabled[pin];
}

size_t fake_adc_read_count(void)
{
    return s_read_count;
}

hal_status_t hal_adc_init(void)
{
    s_initialized = true;
    return HAL_OK;
}

hal_status_t hal_adc_gpio_enable(int pin)
{
    if (!s_initialized) return HAL_NOT_READY;
    if (pin < 0 || pin >= FAKE_ADC_NUM_GPIO_PINS) return HAL_INVALID_ARG;
    s_gpio_enabled[pin] = true;
    return HAL_OK;
}

hal_status_t hal_adc_select(int channel)
{
    if (!s_initialized) return HAL_NOT_READY;
    if (!chan_in_range(channel)) return HAL_INVALID_ARG;
    s_selected_channel = channel;
    return HAL_OK;
}

uint16_t hal_adc_read_raw(void)
{
    /* No hal_status_t return on this signature -- uninitialized or
     * no-channel-selected reads as 0 rather than reporting an error.
     * Documented limitation; see the report for this task. */
    s_read_count++;
    if (!s_initialized || s_selected_channel < 0) return 0;
    fake_adc_channel_t *c = &s_channels[s_selected_channel];
    if (c->count == 0) return 0;
    uint16_t v = c->samples[c->head];
    c->head = (c->head + 1) % FAKE_ADC_SCRIPT_CAP;
    c->count--;
    return v;
}
