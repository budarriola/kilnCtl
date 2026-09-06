#ifndef GPIO_PROBE_DENYLIST_H
#define GPIO_PROBE_DENYLIST_H

#include <stddef.h>
#include <stdbool.h>

#include "settings.h"

/* Pins gpio_probe.c may never touch -- not "should not", *may not*. Every one
 * of these is load-bearing for something the probe itself depends on (the PC
 * link), for another peripheral (SPI/I2C/the SX1509/the display), or is a
 * safety-domain pin whose misuse is silent and one-directional.
 * SAFETY_FAULT_IO (GPIO6) is the one the TODO calls out by name: a debug
 * tool that can drive it can silently tell the safety processor "the main
 * controller is fine" while nothing of the sort is true. THERMO_FAULT0/1/2_IO
 * are the same class of hazard on the thermocouple side -- a probe session
 * that sets one to OUTPUT and drives it can silently tell anything watching
 * that pin "no thermocouple fault" while a real fault is latched underneath.
 * Checked against SET_MODE, WRITE and READ alike -- "never touch" means
 * never touch, not "never write".
 *
 * SAFETY_TX_IO/SAFETY_RX_IO (GPIO4/5) are deliberately *not* on this list,
 * unlike GPIO6: they carry the safety-link UART data, not a one-way status
 * claim, so probing them cannot make the safety processor believe something
 * false -- at worst it collides with UART1 and the link stops responding,
 * which is self-evident rather than silent. `firmware/SaftyFW/docs/HARDWARE.md`
 * section 1's coordinated GPIO test (`tools/PcTools/TODO.md` capability 1c)
 * requires driving/reading exactly these two pins from the probe; denying
 * them here would make that test impossible to run through this path.
 *
 * Factored into its own header, dependency-free past settings.h (no
 * FreeRTOS, no uart_protocol.h), the same way bx_worker_reentrancy.h split
 * uart_bridge_ext.c's re-entrancy check out of a translation unit that pulls
 * too much real hardware plumbing to host-test as a whole -- gpio_probe.c
 * itself (CONFIG_KILNCTL_ENABLE_GPIO_PROBE build) pulls both gpio_probe.h's
 * REAL espInterfaces/uart_protocol.h (found via its own directory-relative
 * #include) and profile_executor.h's STUBBED uart_protocol.h (found via an
 * unqualified #include that falls back to test/stubs/) into the SAME
 * translation unit, which redefine every uart_protocol_t/uart_owner_t type
 * against each other -- unrelated to this deny-list and not worth resolving
 * just to reach it. This header lets test_gpio_probe.c exercise the real
 * deny-list logic (see test_gpio_probe_denylist.h.c... test_gpio_probe.c)
 * without compiling gpio_probe.c at all. */
static inline bool gpio_probe_pin_is_denied(int gpio_num)
{
    const int denied[] = {
        KILN_SPI_SCLK_IO, KILN_SPI_MOSI_IO, KILN_SPI_MISO_IO,
        THERMO_CS0_IO, THERMO_CS1_IO, THERMO_CS2_IO,
        I2C_MASTER_SCL_IO, I2C_MASTER_SDA_IO,
        SX1509_IRQ_IO, SX1509_RESET_IO,
        DISPLAY_CS_IO,
        UART_OWNER_TX_IO, UART_OWNER_RX_IO,
        SAFETY_FAULT_IO,
        THERMO_FAULT0_IO, THERMO_FAULT1_IO, THERMO_FAULT2_IO,
    };
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); ++i) {
        if (denied[i] == gpio_num) {
            return true;
        }
    }
    return false;
}

#endif // GPIO_PROBE_DENYLIST_H
