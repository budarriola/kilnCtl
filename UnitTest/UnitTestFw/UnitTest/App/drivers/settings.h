#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "DcDac.h"

/* All hardware pin/bus settings below are `idf.py menuconfig`-configurable
 * (see App/drivers/Kconfig, under "kilnCtl Hardware Configuration") rather
 * than hardcoded here -- this file just translates the resulting CONFIG_*
 * macros into the plain names the rest of the drivers/app code uses, so
 * nothing downstream needs to know or care that they're Kconfig-backed. */

#define I2C_MASTER_SCL_IO      CONFIG_KILNCTL_I2C_SCL_IO
#define I2C_MASTER_SDA_IO      CONFIG_KILNCTL_I2C_SDA_IO
#define I2C_MASTER_FREQ_HZ     CONFIG_I2C_MASTER_FREQUENCY
#define MCP4728_DAC_VARIANT    DCDAC_VARIANT_MCP4728_AD
#define HEARTBEAT_LED_GPIO     CONFIG_KILNCTL_HEARTBEAT_LED_GPIO

/* Routed through the board's dedicated USB-UART bridge chip and its own
 * USB-C port -- distinct from the native USB / USB-Serial-JTAG port
 * (GPIO19/20) used for flashing and debugging. */
#if CONFIG_KILNCTL_UART_PORT_1
#define UART_OWNER_PORT_NUM    UART_NUM_1
#else
#define UART_OWNER_PORT_NUM    UART_NUM_0
#endif
#define UART_OWNER_TX_IO       CONFIG_KILNCTL_UART_TX_IO
#define UART_OWNER_RX_IO       CONFIG_KILNCTL_UART_RX_IO
#define UART_OWNER_BAUD_RATE   CONFIG_KILNCTL_UART_BAUD_RATE
#define UART_OWNER_QUEUE_LEN         CONFIG_KILNCTL_UART_OWNER_QUEUE_LEN
#define UART_OWNER_TASK_PRIORITY     CONFIG_KILNCTL_UART_OWNER_TASK_PRIORITY
#define UART_OWNER_STACK_SIZE        CONFIG_KILNCTL_UART_OWNER_STACK_SIZE
#define UART_PROTOCOL_TASK_PRIORITY  CONFIG_KILNCTL_UART_PROTOCOL_TASK_PRIORITY
#define UART_PROTOCOL_STACK_SIZE     CONFIG_KILNCTL_UART_PROTOCOL_STACK_SIZE

/* AD9833 waveform generator (SPI, no MISO -- CS/FSYNC bit-banged by
 * spi_owner). MCLK_HZ must match the crystal/oscillator actually wired to
 * the part's MCLK pin; double-check it in menuconfig, since a wrong value
 * skews every programmed frequency. */
#if CONFIG_KILNCTL_AD9833_SPI_HOST_SPI3
#define AD9833_SPI_HOST        SPI3_HOST
#else
#define AD9833_SPI_HOST        SPI2_HOST
#endif
#define AD9833_SCLK_IO         CONFIG_KILNCTL_AD9833_SCLK_IO
#define AD9833_MOSI_IO         CONFIG_KILNCTL_AD9833_MOSI_IO
#define AD9833_CS_IO           CONFIG_KILNCTL_AD9833_CS_IO
#define AD9833_MCLK_HZ         ((uint32_t)CONFIG_KILNCTL_AD9833_MCLK_HZ)

/* SSD1306 OLED (I2C, shares the DAC's bus -- see ctx.dac.bus in main.c). */
#define SSD1306_I2C_ADDR        CONFIG_KILNCTL_SSD1306_I2C_ADDR
#define SSD1306_WIDTH           CONFIG_KILNCTL_SSD1306_WIDTH
#define SSD1306_HEIGHT          CONFIG_KILNCTL_SSD1306_HEIGHT

#endif // SETTINGS_H
