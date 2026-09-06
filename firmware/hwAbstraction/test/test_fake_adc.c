/* test_fake_adc.c -- standalone MSVC host test for hwAbstraction/host/fake_adc.c.
 * Compiled and run by test_host_fakes.ps1.
 */
#include <stdio.h>

#include "fake_adc.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

int main(void)
{
    /* --- before init: gpio_enable / select -> HAL_NOT_READY --- */
    fake_adc_reset();
    CHECK(fake_adc_is_initialized() == false);
    CHECK(hal_adc_gpio_enable(26) == HAL_NOT_READY);
    CHECK(hal_adc_select(0) == HAL_NOT_READY);
    CHECK(fake_adc_current_channel() == -1);
    CHECK(hal_adc_read_raw() == 0); /* no status on this signature; documented */

    /* --- init --- */
    CHECK(hal_adc_init() == HAL_OK);
    CHECK(fake_adc_is_initialized() == true);

    /* --- gpio_enable: invalid pin --- */
    CHECK(hal_adc_gpio_enable(-1) == HAL_INVALID_ARG);
    CHECK(hal_adc_gpio_enable(26) == HAL_OK);
    CHECK(fake_adc_is_gpio_enabled(26) == true);
    CHECK(fake_adc_is_gpio_enabled(27) == false);

    /* --- select: invalid channel --- */
    CHECK(hal_adc_select(-1) == HAL_INVALID_ARG);
    CHECK(hal_adc_select(FAKE_ADC_NUM_CHANNELS) == HAL_INVALID_ARG);
    CHECK(hal_adc_select(2) == HAL_OK);
    CHECK(fake_adc_current_channel() == 2);

    /* --- script_samples: invalid channel --- */
    uint16_t one_sample[1] = { 1234 };
    CHECK(fake_adc_script_samples(-1, one_sample, 1) == HAL_INVALID_ARG);
    CHECK(fake_adc_script_samples(FAKE_ADC_NUM_CHANNELS, one_sample, 1) == HAL_INVALID_ARG);

    /* --- scripted samples come back in order via read_raw --- */
    fake_adc_reset();
    CHECK(hal_adc_init() == HAL_OK);
    CHECK(hal_adc_select(3) == HAL_OK);
    uint16_t samples[3] = { 100, 2048, 4095 }; /* realistic 12-bit quantization */
    CHECK(fake_adc_script_samples(3, samples, 3) == HAL_OK);
    CHECK(fake_adc_pending_count(3) == 3);
    CHECK(hal_adc_read_raw() == 100);
    CHECK(hal_adc_read_raw() == 2048);
    CHECK(hal_adc_read_raw() == 4095);
    CHECK(fake_adc_pending_count(3) == 0);
    CHECK(hal_adc_read_raw() == 0); /* exhausted queue reads as 0 */

    /* --- switching channels reads that channel's own queue --- */
    fake_adc_reset();
    CHECK(hal_adc_init() == HAL_OK);
    uint16_t ch0[1] = { 11 };
    uint16_t ch1[1] = { 22 };
    CHECK(fake_adc_script_samples(0, ch0, 1) == HAL_OK);
    CHECK(fake_adc_script_samples(1, ch1, 1) == HAL_OK);
    CHECK(hal_adc_select(1) == HAL_OK);
    CHECK(hal_adc_read_raw() == 22);
    CHECK(hal_adc_select(0) == HAL_OK);
    CHECK(hal_adc_read_raw() == 11);

    /* --- buffer overflow: scripting past FAKE_ADC_SCRIPT_CAP -> HAL_NO_MEM --- */
    fake_adc_reset();
    CHECK(hal_adc_init() == HAL_OK);
    hal_status_t last = HAL_OK;
    for (int i = 0; i < FAKE_ADC_SCRIPT_CAP + 5; i++) {
        uint16_t s = (uint16_t)i;
        last = fake_adc_script_samples(5, &s, 1);
        if (last != HAL_OK) break;
    }
    CHECK(last == HAL_NO_MEM);
    CHECK(fake_adc_pending_count(5) == FAKE_ADC_SCRIPT_CAP);

    /* --- read_count tallies every call regardless of state --- */
    fake_adc_reset();
    size_t before = fake_adc_read_count();
    (void)hal_adc_read_raw();
    (void)hal_adc_read_raw();
    CHECK(fake_adc_read_count() == before + 2);

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
