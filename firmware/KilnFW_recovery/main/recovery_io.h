// recovery_io.h -- hold the kiln OFF while the recovery image runs, and own
// the SX1509 I/O expander + I2C bus the LCD driver also needs.
//
// Why this exists: the recovery image is what runs when the main app has
// crash-looped, so it must not leave whatever relay state the dead app (or
// the expander's own reset default) left behind. Every relay gate is on the
// SX1509, and a bare expander reset leaves all 16 pins as inputs (relay gates
// then float), so recovery drives them as outputs, low, explicitly.
//
// Pin/bit facts (all mirrored, not linked, from the main app):
//   - SX1509 U5 at I2C 0x3E, SDA=GPIO8, SCL=GPIO9, ~RESET <- ESP GPIO10
//     (firmware/KilnFW/App/drivers/Kconfig: KILNCTL_I2C_SDA_IO/SCL_IO,
//     KILNCTL_SX1509_I2C_ADDR, KILNCTL_SX1509_RESET_IO).
//   - Relay drives are expander IO0..IO3, active high through a BSS138 gate
//     (firmware/KilnFW/App/drivers/common/uart_task_ids.h, "IO" section;
//     settings.h SX1509_RELAY1_PIN..SX1509_RELAY4_PIN). Those four are the
//     heater relays K1/K2/K3/K5. kiln_io.c's logical<->pin remap only permutes
//     which pin is "relay N"; forcing ALL FOUR low makes the permutation moot.
//   - K4 (heat enable / safety relay) is NOT an expander bit. It closes only
//     when the RP2040 safety processor is asked to via SAFETY_CMD_REQUEST_ENABLE
//     (firmware/KilnFW/App/drivers/control/heat_enable.h), and recovery never
//     sends that request, so K4 stays open under the Pico's own control. The
//     expander's "R4" readback bit is not K4 state (project notes, 2026-09-18
//     bench proof); this module therefore reports only that the four
//     expander-driven relays are held low and says nothing about K4.
//   - Write order mirrors kiln_io_init(): load RegData first while the pins
//     are still inputs, THEN make them outputs, so no coil sees a high.
#ifndef RECOVERY_IO_H
#define RECOVERY_IO_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings up I2C + the expander and forces relays IO0..IO3 low/output, with
// read-back verification (bounded retries, hard reset between attempts).
// Always returns; on failure the fault flag below is set and the failure is
// logged at error level. Call it first thing after NVS init.
void recovery_io_hold_relays_off(void);

// True when the expander did not answer or the read-back of its data/
// direction registers did not show all four relays low and driven as outputs.
// The status route and the LCD show "RELAY CTRL FAULT" when this is true.
bool recovery_io_relay_fault(void);

// True once the expander answered and the relay hold was verified.
bool recovery_io_relays_verified_off(void);

// NVS partitions that could not be initialised this boot (recovery never
// erases or reformats any of them; it continues without). Bit 0 = default
// `nvs`, bit 1 = `wifi_nvs`, bit 2 = `kiln_nvs`. Zero means all came up.
#define RECOVERY_NVS_FAIL_DEFAULT (1u << 0)
#define RECOVERY_NVS_FAIL_WIFI    (1u << 1)
#define RECOVERY_NVS_FAIL_KILN    (1u << 2)
void recovery_io_nvs_mark_failed(unsigned bit);
unsigned recovery_io_nvs_failed_mask(void);

// LCD helpers (D/C = expander IO15, ~RESET = expander IO14). Only those two
// bits can be changed through this call; relay bits are never touched.
// Returns ESP_ERR_INVALID_STATE if the expander is not up.
esp_err_t recovery_io_set_lcd_pins(bool dc_high, bool reset_high);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_IO_H
