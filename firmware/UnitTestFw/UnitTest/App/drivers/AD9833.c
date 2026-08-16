#include "AD9833.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "AD9833";

/* Control register (address bits D15:D14 = 00), per datasheet Table 8/9. */
#define AD9833_ADDR_CONTROL 0x0000u
#define AD9833_ADDR_FREQ0   0x4000u
#define AD9833_ADDR_FREQ1   0x8000u
#define AD9833_ADDR_PHASE0  0xC000u
#define AD9833_ADDR_PHASE1  0xE000u

#define AD9833_CTRL_B28      (1u << 13)
#define AD9833_CTRL_HLB      (1u << 12)
#define AD9833_CTRL_FSELECT  (1u << 11)
#define AD9833_CTRL_PSELECT  (1u << 10)
#define AD9833_CTRL_RESET    (1u << 8)
#define AD9833_CTRL_SLEEP1   (1u << 7)  // powers down the internal MCLK-driven core
#define AD9833_CTRL_SLEEP12  (1u << 6)  // powers down the on-chip DAC
#define AD9833_CTRL_OPBITEN  (1u << 5)
#define AD9833_CTRL_DIV2     (1u << 3)
#define AD9833_CTRL_MODE     (1u << 1)

#define AD9833_FREQ_WORD_BITS 28u
#define AD9833_PHASE_WORD_BITS 12u
#define AD9833_SPI_CLOCK_HZ (1 * 1000 * 1000) // conservative; part supports up to 40 MHz

static esp_err_t ad9833_write16(AD9833Class *gen, uint16_t word)
{
    if (!gen || !gen->owner_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = { (uint8_t)(word >> 8), (uint8_t)(word & 0xFF) };
    return spi_owner_transfer(&gen->owner, gen->dev, buf, sizeof(buf), NULL, 0, gen->cs_gpio);
}

static esp_err_t ad9833_write_control(AD9833Class *gen)
{
    return ad9833_write16(gen, AD9833_ADDR_CONTROL | gen->control_shadow);
}

esp_err_t AD9833_init(AD9833Class *gen,
                       spi_host_device_t host,
                       int sclk_gpio,
                       int mosi_gpio,
                       int cs_gpio,
                       uint32_t mclk_hz)
{
    if (!gen || mclk_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(gen, 0, sizeof(*gen));
    gen->host = host;
    gen->cs_gpio = cs_gpio;
    gen->mclk_hz = mclk_hz;

    /* FSYNC (CS) is bit-banged by spi_owner around each transfer, so it must
     * be driven high (idle/inactive) any time no transfer is in flight. */
    gpio_config_t cs_conf = {
        .pin_bit_mask = (1ULL << cs_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cs_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(cs) failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)cs_gpio, 1);

    spi_bus_config_t bus_config = {
        .mosi_io_num = mosi_gpio,
        .miso_io_num = -1,
        .sclk_io_num = sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4,
    };
    err = spi_bus_initialize(host, &bus_config, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }
    gen->bus_initialized = true;

    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = AD9833_SPI_CLOCK_HZ,
        .mode = 2, // AD9833 requires CPOL=1, CPHA=0
        .spics_io_num = -1, // CS driven manually by the owner, not by the SPI peripheral
        .queue_size = 1,
    };
    err = spi_bus_add_device(host, &dev_config, &gen->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        spi_bus_free(host);
        gen->bus_initialized = false;
        return err;
    }

    err = spi_owner_init(&gen->owner, host, 8, 5, 4096, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_owner_init failed: %s", esp_err_to_name(err));
        spi_bus_remove_device(gen->dev);
        spi_bus_free(host);
        gen->bus_initialized = false;
        return err;
    }
    gen->owner_initialized = true;

    gen->shadow_lock = xSemaphoreCreateMutex();
    if (!gen->shadow_lock) {
        ESP_LOGE(TAG, "failed to allocate shadow_lock");
        AD9833_deinit(gen);
        return ESP_ERR_NO_MEM;
    }

    /* Reset with B28 set (two 14-bit words per frequency write) and outputs
     * held quiet until the caller has programmed frequency/phase/waveform. */
    gen->control_shadow = AD9833_CTRL_B28 | AD9833_CTRL_RESET;
    err = ad9833_write_control(gen);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initial control write failed: %s", esp_err_to_name(err));
        AD9833_deinit(gen);
        return err;
    }

    return ESP_OK;
}

esp_err_t AD9833_deinit(AD9833Class *gen)
{
    if (!gen) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    if (gen->shadow_lock) {
        vSemaphoreDelete(gen->shadow_lock);
        gen->shadow_lock = NULL;
    }
    if (gen->owner_initialized) {
        esp_err_t e = spi_owner_deinit(&gen->owner);
        if (e != ESP_OK) err = e;
        gen->owner_initialized = false;
    }
    if (gen->dev) {
        esp_err_t e = spi_bus_remove_device(gen->dev);
        if (e != ESP_OK) err = e;
        gen->dev = NULL;
    }
    if (gen->bus_initialized) {
        esp_err_t e = spi_bus_free(gen->host);
        if (e != ESP_OK) err = e;
        gen->bus_initialized = false;
    }
    return err;
}

esp_err_t AD9833_reset(AD9833Class *gen, bool hold)
{
    if (!gen || !gen->shadow_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(gen->shadow_lock, portMAX_DELAY);
    if (hold) {
        gen->control_shadow |= AD9833_CTRL_RESET;
    } else {
        gen->control_shadow &= ~AD9833_CTRL_RESET;
    }
    esp_err_t err = ad9833_write_control(gen);
    xSemaphoreGive(gen->shadow_lock);
    return err;
}

esp_err_t AD9833_set_frequency(AD9833Class *gen, AD9833FreqReg reg, double freq_hz)
{
    if (!gen || !gen->shadow_lock || gen->mclk_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Reject NaN/Inf and anything past Nyquist -- the hardware would just
     * silently alias/wrap such a request into a bogus in-range output. */
    if (!isfinite(freq_hz) || freq_hz < 0.0 || freq_hz > (double)gen->mclk_hz / 2.0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(gen->shadow_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    if (!(gen->control_shadow & AD9833_CTRL_B28)) {
        gen->control_shadow |= AD9833_CTRL_B28;
        err = ad9833_write_control(gen);
    }

    if (err == ESP_OK) {
        double scale = (double)(1ULL << AD9833_FREQ_WORD_BITS) / (double)gen->mclk_hz;
        uint32_t freq_word = (uint32_t)(freq_hz * scale + 0.5) & 0x0FFFFFFFu;
        uint16_t lsw = (uint16_t)(freq_word & 0x3FFFu);
        uint16_t msw = (uint16_t)((freq_word >> 14) & 0x3FFFu);
        uint16_t addr = (reg == AD9833_FREQ0) ? AD9833_ADDR_FREQ0 : AD9833_ADDR_FREQ1;

        err = ad9833_write16(gen, addr | lsw);
        if (err == ESP_OK) {
            err = ad9833_write16(gen, addr | msw);
        }
    }

    xSemaphoreGive(gen->shadow_lock);
    return err;
}

esp_err_t AD9833_set_phase(AD9833Class *gen, AD9833PhaseReg reg, double degrees)
{
    if (!gen || !gen->shadow_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!isfinite(degrees)) {
        return ESP_ERR_INVALID_ARG;
    }

    double normalized = fmod(degrees, 360.0);
    if (normalized < 0.0) {
        normalized += 360.0;
    }
    uint16_t phase_word = (uint16_t)((normalized / 360.0) * (double)(1u << AD9833_PHASE_WORD_BITS) + 0.5)
                           & 0x0FFFu;
    uint16_t addr = (reg == AD9833_PHASE0) ? AD9833_ADDR_PHASE0 : AD9833_ADDR_PHASE1;

    /* Doesn't touch control_shadow, but still serialize against it so a
     * concurrent shadow update can't interleave its own multi-write
     * sequence with this one on the wire. */
    xSemaphoreTake(gen->shadow_lock, portMAX_DELAY);
    esp_err_t err = ad9833_write16(gen, addr | phase_word);
    xSemaphoreGive(gen->shadow_lock);
    return err;
}

esp_err_t AD9833_select_freq_reg(AD9833Class *gen, AD9833FreqReg reg)
{
    if (!gen || !gen->shadow_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(gen->shadow_lock, portMAX_DELAY);
    if (reg == AD9833_FREQ1) {
        gen->control_shadow |= AD9833_CTRL_FSELECT;
    } else {
        gen->control_shadow &= ~AD9833_CTRL_FSELECT;
    }
    esp_err_t err = ad9833_write_control(gen);
    xSemaphoreGive(gen->shadow_lock);
    return err;
}

esp_err_t AD9833_select_phase_reg(AD9833Class *gen, AD9833PhaseReg reg)
{
    if (!gen || !gen->shadow_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(gen->shadow_lock, portMAX_DELAY);
    if (reg == AD9833_PHASE1) {
        gen->control_shadow |= AD9833_CTRL_PSELECT;
    } else {
        gen->control_shadow &= ~AD9833_CTRL_PSELECT;
    }
    esp_err_t err = ad9833_write_control(gen);
    xSemaphoreGive(gen->shadow_lock);
    return err;
}

esp_err_t AD9833_set_waveform(AD9833Class *gen, AD9833Waveform waveform)
{
    if (!gen || !gen->shadow_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    if (waveform != AD9833_WAVE_SINE && waveform != AD9833_WAVE_TRIANGLE &&
        waveform != AD9833_WAVE_SQUARE && waveform != AD9833_WAVE_SQUARE_DIV2) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(gen->shadow_lock, portMAX_DELAY);
    gen->control_shadow &= ~(AD9833_CTRL_OPBITEN | AD9833_CTRL_DIV2 | AD9833_CTRL_MODE);
    switch (waveform) {
        case AD9833_WAVE_SINE:
            break;
        case AD9833_WAVE_TRIANGLE:
            gen->control_shadow |= AD9833_CTRL_MODE;
            break;
        case AD9833_WAVE_SQUARE:
            gen->control_shadow |= AD9833_CTRL_OPBITEN;
            break;
        case AD9833_WAVE_SQUARE_DIV2:
            gen->control_shadow |= AD9833_CTRL_OPBITEN | AD9833_CTRL_DIV2;
            break;
    }
    esp_err_t err = ad9833_write_control(gen);
    xSemaphoreGive(gen->shadow_lock);
    return err;
}

esp_err_t AD9833_sleep(AD9833Class *gen, bool dac_power_down, bool mclk_power_down)
{
    if (!gen || !gen->shadow_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(gen->shadow_lock, portMAX_DELAY);
    if (dac_power_down) {
        gen->control_shadow |= AD9833_CTRL_SLEEP12;
    } else {
        gen->control_shadow &= ~AD9833_CTRL_SLEEP12;
    }

    if (mclk_power_down) {
        gen->control_shadow |= AD9833_CTRL_SLEEP1;
    } else {
        gen->control_shadow &= ~AD9833_CTRL_SLEEP1;
    }

    esp_err_t err = ad9833_write_control(gen);
    xSemaphoreGive(gen->shadow_lock);
    return err;
}
