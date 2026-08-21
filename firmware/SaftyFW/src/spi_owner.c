#include "spi_owner.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "board_pins.h"

// 4 MHz is a CEILING, not a default, and the assert below enforces it.
//
// The part's own fSCL limit is 5 MHz, so the silicon is not what caps this.
// The SimFW bench fixture is: its PIO/DMA MAX31856 slave emulation has to have
// the first response byte on MISO before the master's sample point, which is
// one SCLK period minus setup -- about 250 ns at 4 MHz, tightening to about
// 200 ns at 5 MHz. The implemented DMA-fed path takes about 150-215 ns at the
// RP2040's stock 125 MHz sysclk, so 4 MHz has margin and 5 MHz largely does
// not. Derivation: firmware/SimFW/docs/SPI_ACCESS_AUDIT.md section 9.
//
// Blowing the deadline produces no fault signal. The whole register burst
// shifts one byte position and returns plausible-looking wrong temperatures --
// on the safety processor, of all places. Hence an assert rather than a note.
//
// KilnFW holds its master to the same 4 MHz via a Kconfig `range` on
// KILNCTL_THERMO_SPI_CLOCK_HZ plus a matching _Static_assert in MAX31856.c;
// keep the two in step.
#define SPI_OWNER_BAUDRATE_HZ 4000000u
_Static_assert(SPI_OWNER_BAUDRATE_HZ <= 4000000u,
               "SPI_OWNER_BAUDRATE_HZ exceeds the 4 MHz cap the SimFW slave "
               "emulation's first-byte deadline requires -- see "
               "SimFW/docs/SPI_ACCESS_AUDIT.md section 9");

#define SPI_OWNER_LOCK_TIMEOUT_MS 2000u

static SemaphoreHandle_t s_lock = NULL;
static bool s_initialized = false;

bool spi_owner_init(void)
{
    // spi0 is the RP2040's first SPI peripheral, wired to GPIO0/2/3 per
    // board_pins.h; SAFTYFW_PIN_SPI0_CS0 is deliberately NOT told to spi_init
    // -- CS is bit-banged, same reasoning MAX31856.c documents for
    // spics_io_num = -1 on the main board (a register burst must not let the
    // SPI peripheral's own CS timing insert a gap the part could see as a
    // new transaction).
    spi_init(spi0, SPI_OWNER_BAUDRATE_HZ);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);

    gpio_set_function(SAFTYFW_PIN_SPI0_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SAFTYFW_PIN_SPI0_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SAFTYFW_PIN_SPI0_MISO, GPIO_FUNC_SPI);

    // CS0: plain GPIO output, idling high. Set the output latch before the
    // direction, same "never briefly undriven" discipline main.c uses for
    // GPIO6 -- a CS that glitches low for even one SCLK edge while becoming
    // an output could be read by the part as the start of a transaction.
    gpio_init(SAFTYFW_PIN_SPI0_CS0);
    gpio_put(SAFTYFW_PIN_SPI0_CS0, 1);
    gpio_set_dir(SAFTYFW_PIN_SPI0_CS0, GPIO_OUT);

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return false;
    }

    s_initialized = true;
    return true;
}

bool spi_owner_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!s_initialized || !s_lock || !tx || len == 0) {
        return false;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(SPI_OWNER_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }

    gpio_put(SAFTYFW_PIN_SPI0_CS0, 0);
    int written = rx ? spi_write_read_blocking(spi0, tx, rx, len)
                      : spi_write_blocking(spi0, tx, len);
    gpio_put(SAFTYFW_PIN_SPI0_CS0, 1);

    xSemaphoreGive(s_lock);

    return written == (int)len;
}
