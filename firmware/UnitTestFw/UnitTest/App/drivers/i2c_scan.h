#ifndef I2C_SCAN_H
#define I2C_SCAN_H

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probes every valid 7-bit I2C address on bus and ESP_LOGI's each one that
 * ACKs, plus a one-line summary -- both captured by uart_log_bridge.c like
 * any other ESP_LOGx call, so the results show up in the GUI's Device
 * Console (or mcp_server's get_device_log) without any dedicated wire
 * message of their own. Call once, right after the bus is created and
 * before anything else touches it (see main.c) -- a device mid-transaction
 * when probed can NACK/misbehave, so this is meant to run as a clean,
 * one-shot startup step, not a live diagnostic. */
void i2c_scan_bus(i2c_master_bus_handle_t bus);

#ifdef __cplusplus
}
#endif

#endif // I2C_SCAN_H
