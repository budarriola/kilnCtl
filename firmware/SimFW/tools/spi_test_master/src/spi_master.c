#include "spi_master.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#define SPI_MASTER_INST spi0

const uint8_t spi_master_cs_gpio[SPI_MASTER_CS_COUNT] = { 20u, 21u, 22u, 26u };

static uint32_t s_rate_hz = 1000000u; // 1 MHz default -- slow/forgiving start point for the sweep
static uint8_t s_cpha = 1u;           // mode 1 default, matches PLAN.md 3.2
static uint8_t s_selected_cs = 0u;

static void deassert_all_cs(void) {
    for (uint8_t i = 0; i < SPI_MASTER_CS_COUNT; i++) {
        gpio_put(spi_master_cs_gpio[i], 1); // idle-high, active-low CS
    }
}

void spi_master_init(void) {
    for (uint8_t i = 0; i < SPI_MASTER_CS_COUNT; i++) {
        gpio_init(spi_master_cs_gpio[i]);
        gpio_set_dir(spi_master_cs_gpio[i], GPIO_OUT);
    }
    deassert_all_cs();

    spi_init(SPI_MASTER_INST, s_rate_hz);
    spi_set_format(SPI_MASTER_INST, 8, SPI_CPOL_0, s_cpha ? SPI_CPHA_1 : SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(SPI_MASTER_SCK_GPIO, GPIO_FUNC_SPI);
    gpio_set_function(SPI_MASTER_MOSI_GPIO, GPIO_FUNC_SPI);
    gpio_set_function(SPI_MASTER_MISO_GPIO, GPIO_FUNC_SPI);
}

uint32_t spi_master_set_rate_hz(uint32_t hz) {
    uint32_t actual = spi_set_baudrate(SPI_MASTER_INST, hz);
    s_rate_hz = actual;
    return actual;
}

uint32_t spi_master_get_rate_hz(void) {
    return s_rate_hz;
}

bool spi_master_set_mode(uint8_t cpha) {
    if (cpha != 0u && cpha != 1u) {
        return false;
    }
    s_cpha = cpha;
    spi_set_format(SPI_MASTER_INST, 8, SPI_CPOL_0, s_cpha ? SPI_CPHA_1 : SPI_CPHA_0, SPI_MSB_FIRST);
    return true;
}

uint8_t spi_master_get_mode(void) {
    return s_cpha;
}

bool spi_master_select_cs(uint8_t cs_index) {
    if (cs_index >= SPI_MASTER_CS_COUNT) {
        return false;
    }
    s_selected_cs = cs_index;
    return true;
}

uint8_t spi_master_selected_cs(void) {
    return s_selected_cs;
}

void spi_master_transact(const uint8_t *tx, uint8_t *rx, uint32_t len) {
    gpio_put(spi_master_cs_gpio[s_selected_cs], 0); // assert (active low)
    spi_write_read_blocking(SPI_MASTER_INST, tx, rx, len);
    gpio_put(spi_master_cs_gpio[s_selected_cs], 1); // deassert
}
