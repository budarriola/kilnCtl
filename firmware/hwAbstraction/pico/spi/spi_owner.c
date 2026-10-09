// Moved here from firmware/SaftyFW/src/ by HAL Phase 1a
// (docs/HW_ABSTRACTION.md), body byte-identical apart from include
// paths.
#include "spi_owner.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

// HAL Phase 1b, "close the upward include" (docs/HW_ABSTRACTION.md):
// this used to #include "board_pins.h" (a SaftyFW header) straight across
// the hwAbstraction/SaftyFW boundary. Pin values are now passed in by the
// caller at spi_owner_init() time (spi_owner_pins_t, spi_owner.h) instead --
// SaftyFW's own callers still read board_pins.h and forward its values here.

// 4 MHz is a CEILING, not a default, and the assert below enforces it.
//
// The part's own fSCL limit is 5 MHz, so the silicon is not what caps this.
// The cap is an owner decision (2026-10-06): 4 MHz is ample for the MAX31856
// conversion rates, and faster SPI is not needed. (It was originally set for
// the SimFW bench fixture, deleted 2026-08-28; that is no longer the reason.)
//
// KilnFW holds its master to the same 4 MHz via a Kconfig `range` on
// KILNCTL_THERMO_SPI_CLOCK_HZ plus a matching _Static_assert in MAX31856.c;
// keep the two in step.
#define SPI_OWNER_BAUDRATE_HZ 4000000u
_Static_assert(SPI_OWNER_BAUDRATE_HZ <= 4000000u,
               "SPI_OWNER_BAUDRATE_HZ exceeds the 4 MHz cap (owner decision "
               "2026-10-06: 4 MHz is ample for the MAX31856)");

// Bounded below the 1 s hardware watchdog timeout (SAFTYFW_WATCHDOG_TIMEOUT_MS,
// main.c), not merely "bounded". thermo_task is the only caller today (see
// this file's own single-owner reasoning above), so this mutex is never
// actually contended in the current build -- but a wait timeout that is
// itself allowed to exceed the watchdog deadline is a live defect regardless
// of whether anything exercises it yet: the day a second caller is added (or
// this call is reached while a prior holder is mid-transfer for any other
// reason), thermo_task -- and with it watchdog_task_checkin(WATCHDOG_
// CHECKIN_THERMO_TASK), since thermo_task_fn() calls this synchronously
// before its own checkin -- could block for up to the old 2000 ms here alone,
// more than the entire 1 s hardware budget, with nothing else in this file to
// catch it. A genuine SPI transfer on this bus is a handful of microseconds
// (8 bytes at 4 MHz, spi_owner_init()'s own budget comment); 200 ms is
// already two orders of magnitude of margin over that, while still leaving
// thermo_task's own DRDY-silence wait (thermo_task.c's ~302 ms worst case)
// and every other core-1 task's own deadline room inside the 1 s watchdog
// window. See test/test_watchdog_budget_coverage.c, which fails loudly if
// this constant (or SAFTYFW_WATCHDOG_TIMEOUT_MS) ever drifts back out of this
// relationship.
#define SPI_OWNER_LOCK_TIMEOUT_MS 200u

static SemaphoreHandle_t s_lock = NULL;
static bool s_initialized = false;
static uint8_t s_cs0_pin = 0;

bool spi_owner_init(const spi_owner_pins_t *pins)
{
    if (!pins) {
        return false;
    }

    // spi0 is the RP2040's first SPI peripheral, wired to GPIO0/2/3 per
    // board_pins.h (forwarded in via pins by the caller); pins->cs0_pin is
    // deliberately NOT told to spi_init -- CS is bit-banged, same reasoning
    // MAX31856.c documents for spics_io_num = -1 on the main board (a
    // register burst must not let the SPI peripheral's own CS timing insert
    // a gap the part could see as a new transaction).
    spi_init(spi0, SPI_OWNER_BAUDRATE_HZ);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);

    gpio_set_function(pins->sck_pin, GPIO_FUNC_SPI);
    gpio_set_function(pins->mosi_pin, GPIO_FUNC_SPI);
    gpio_set_function(pins->miso_pin, GPIO_FUNC_SPI);

    // CS0: plain GPIO output, idling high. Set the output latch before the
    // direction, same "never briefly undriven" discipline main.c uses for
    // GPIO6 -- a CS that glitches low for even one SCLK edge while becoming
    // an output could be read by the part as the start of a transaction.
    s_cs0_pin = pins->cs0_pin;
    gpio_init(s_cs0_pin);
    gpio_put(s_cs0_pin, 1);
    gpio_set_dir(s_cs0_pin, GPIO_OUT);

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

    gpio_put(s_cs0_pin, 0);
    int written = rx ? spi_write_read_blocking(spi0, tx, rx, len)
                      : spi_write_blocking(spi0, tx, len);
    gpio_put(s_cs0_pin, 1);

    xSemaphoreGive(s_lock);

    return written == (int)len;
}
