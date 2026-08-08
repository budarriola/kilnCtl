// AD9833 programmable waveform generator driver (SPI)
#ifndef AD9833_H
#define AD9833_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_spi_owner.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AD9833_WAVE_SINE = 0,
    AD9833_WAVE_TRIANGLE,
    AD9833_WAVE_SQUARE,       // MSB of DAC, full MCLK/... square at programmed frequency
    AD9833_WAVE_SQUARE_DIV2,  // MSB/2, square at half the programmed frequency
} AD9833Waveform;

typedef enum {
    AD9833_FREQ0 = 0,
    AD9833_FREQ1 = 1,
} AD9833FreqReg;

typedef enum {
    AD9833_PHASE0 = 0,
    AD9833_PHASE1 = 1,
} AD9833PhaseReg;

typedef struct {
    spi_host_device_t host;
    spi_device_handle_t dev;
    spi_owner_t owner;
    bool owner_initialized;
    bool bus_initialized;
    int cs_gpio;
    uint32_t mclk_hz;
    uint16_t control_shadow; // last-written control register bits, so partial updates (e.g. waveform) don't clobber others
    SemaphoreHandle_t shadow_lock; // guards read-modify-write of control_shadow across concurrent callers
} AD9833Class;

// Brings up the SPI bus/device and the queueing owner task, then resets the
// part (held in reset, outputs disabled) ready for AD9833_set_frequency/etc.
// mclk_hz is the AD9833's reference clock (crystal/oscillator on MCLK pin,
// commonly 25,000,000 on breakout boards) and is required for correct
// frequency-word math.
esp_err_t AD9833_init(AD9833Class *gen,
                       spi_host_device_t host,
                       int sclk_gpio,
                       int mosi_gpio,
                       int cs_gpio,
                       uint32_t mclk_hz);
esp_err_t AD9833_deinit(AD9833Class *gen);

// Holds (true) or releases (false) the internal RESET bit. The part resets
// into (and AD9833_init leaves it in) the held state with DAC output at
// midscale; call with hold=false once frequency/phase/waveform are set.
esp_err_t AD9833_reset(AD9833Class *gen, bool hold);

// Programs a 28-bit frequency register from a frequency in Hz.
esp_err_t AD9833_set_frequency(AD9833Class *gen, AD9833FreqReg reg, double freq_hz);
// Programs a 12-bit phase register from a phase in degrees (0-360).
esp_err_t AD9833_set_phase(AD9833Class *gen, AD9833PhaseReg reg, double degrees);

// Selects which FREQx/PHASEx register the output uses.
esp_err_t AD9833_select_freq_reg(AD9833Class *gen, AD9833FreqReg reg);
esp_err_t AD9833_select_phase_reg(AD9833Class *gen, AD9833PhaseReg reg);

esp_err_t AD9833_set_waveform(AD9833Class *gen, AD9833Waveform waveform);

// Powers down the internal DAC and/or the MCLK-driven oscillator core.
esp_err_t AD9833_sleep(AD9833Class *gen, bool dac_power_down, bool mclk_power_down);

#ifdef __cplusplus
}
#endif

#endif // AD9833_H
