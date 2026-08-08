#ifndef UART_TASK_IDS_H
#define UART_TASK_IDS_H

/* Shared uart_protocol task_id numbering. Both the ESP firmware and the PC
 * side must agree on these -- see pc_tools/src/uart_control/protocol.py for
 * the matching Python constants and command payload (de)serialization. */

/* Bump this any time a change here would break a peer running the old
 * value: renumbering a task_id or subcommand, changing a payload's byte
 * layout/length/endianness, or changing the envelope itself (header
 * layout, CRC algorithm, framing). Do NOT bump it for changes that don't
 * affect wire compatibility (comments, internal refactors, adding a
 * brand-new task_id/subcommand that old peers simply won't use).
 *
 * There's no automatic way to detect "does this change break the wire
 * format" -- it requires a human judgment call, so this is a manually
 * maintained integer, not a hash of this file. (A hash would catch every
 * edit including harmless ones -- comment tweaks, reordering -- which
 * would force a version bump on changes that were never actually
 * incompatible, defeating the point.)
 *
 * Carried in the GET_FW_VERSION response (see below) at a *fixed* byte
 * offset (0-1) that must never move across versions, so a receiver can
 * always read the version first and bail before attempting to parse
 * anything else -- see parse_fw_version_response() in devices.py. */
#define UART_PROTOCOL_VERSION 1u

#define UART_TASK_ID_DAC     1u
#define UART_TASK_ID_AD9833  2u
#define UART_TASK_ID_INFO    3u
#define UART_TASK_ID_OLED    4u
#define UART_TASK_ID_LOG     5u
#define UART_TASK_ID_SYSTEM  6u

/* --- DAC (task_id = UART_TASK_ID_DAC) command payload ---
 * byte0 = subcommand:
 *   0x01 SET_CHANNEL_PERCENT  byte1=channel(0-3)      bytes2..5 = percent  f32 LE
 *   0x02 SET_ALL_PERCENT      bytes1..16 = percent[4] f32 LE (ch0..ch3)
 *   0x03 POWER_DOWN           byte1=channel(0-3)      byte2=power_mode(0-3)
 */
#define DAC_CMD_SET_CHANNEL_PERCENT 0x01u
#define DAC_CMD_SET_ALL_PERCENT     0x02u
#define DAC_CMD_POWER_DOWN          0x03u

/* --- AD9833 (task_id = UART_TASK_ID_AD9833) command payload ---
 * byte0 = subcommand:
 *   0x01 SET_FREQUENCY   byte1=reg(0/1)  bytes2..9  = freq_hz  f64 LE
 *   0x02 SET_PHASE       byte1=reg(0/1)  bytes2..9  = degrees  f64 LE
 *   0x03 SET_WAVEFORM    byte1=waveform(0=sine,1=triangle,2=square,3=square_div2)
 *   0x04 SELECT_FREQ_REG byte1=reg(0/1)
 *   0x05 SELECT_PHASE_REG byte1=reg(0/1)
 *   0x06 RESET           byte1=hold(0/1)
 *   0x07 SLEEP           byte1=dac_power_down(0/1)  byte2=mclk_power_down(0/1)
 */
#define AD9833_CMD_SET_FREQUENCY    0x01u
#define AD9833_CMD_SET_PHASE        0x02u
#define AD9833_CMD_SET_WAVEFORM     0x03u
#define AD9833_CMD_SELECT_FREQ_REG  0x04u
#define AD9833_CMD_SELECT_PHASE_REG 0x05u
#define AD9833_CMD_RESET            0x06u
#define AD9833_CMD_SLEEP            0x07u

/* --- SSD1306 OLED (task_id = UART_TASK_ID_OLED) command payload ---
 * byte0 = subcommand:
 *   0x01 CLEAR         (no args) clears the in-RAM framebuffer only
 *   0x02 SET_CURSOR    byte1=col  byte2=row              (text-cell coords)
 *   0x03 PRINT         bytes1..(length-1) = ASCII text, NOT null-terminated,
 *                      written into the framebuffer at the cursor
 *   0x04 DISPLAY       (no args) flushes the framebuffer to the panel
 *   0x05 SET_CONTRAST  byte1=contrast (0-255)
 *   0x06 SET_INVERT    byte1=invert(0/1)
 *   0x07 SET_POWER     byte1=on(0/1)
 * CLEAR/SET_CURSOR/PRINT only touch the framebuffer -- nothing reaches the
 * panel until DISPLAY, same as calling the SSD1306_* functions directly.
 */
#define OLED_CMD_CLEAR         0x01u
#define OLED_CMD_SET_CURSOR    0x02u
#define OLED_CMD_PRINT         0x03u
#define OLED_CMD_DISPLAY       0x04u
#define OLED_CMD_SET_CONTRAST  0x05u
#define OLED_CMD_SET_INVERT    0x06u
#define OLED_CMD_SET_POWER     0x07u

/* --- LOG (task_id = UART_TASK_ID_LOG) ---
 * Firmware -> PC only, unsolicited (fire-and-forget, no reply expected and
 * no request ever sent to this task_id). Every ESP_LOGx call anywhere in the
 * firmware is captured via esp_log_set_vprintf() and forwarded here instead
 * of (not in addition to -- see uart_log_bridge.c) the USB-Serial-JTAG
 * console, so log output is visible over the same always-on link used for
 * device control, without needing a second cable/monitor/debugger attached.
 * One frame per log line (or truncated chunk of one line -- payload is
 * capped to fit UART_PROTO_MAX_PAYLOAD).
 * byte0 = level (see UART_LOG_LEVEL_* below)
 * bytes1..(length-1) = ASCII text "TAG: message", NOT null-terminated,
 *   truncated (never split across frames) if the formatted line doesn't fit
 *   in the remaining 127 bytes of payload.
 * Best-effort: dropped locally (never blocks the task that logged) if the
 * link is down or the internal queue is full -- see uart_log_bridge.c. */
#define UART_LOG_LEVEL_ERROR    0x00u
#define UART_LOG_LEVEL_WARN     0x01u
#define UART_LOG_LEVEL_INFO     0x02u
#define UART_LOG_LEVEL_DEBUG    0x03u
#define UART_LOG_LEVEL_VERBOSE  0x04u

/* --- SYSTEM (task_id = UART_TASK_ID_SYSTEM) ---
 * byte0 = subcommand:
 *   0x01 RESTART_UART  (no args) flushes the UART peripheral's RX ring
 *                       buffer and resets the rx-error counter -- an
 *                       on-demand recovery lever for a link that's gotten
 *                       stuck (buffer overflow, line noise) without having
 *                       to power-cycle the board. Deliberately RX-only: the
 *                       ACK for *this* request is still sitting in the TX
 *                       ring buffer when the handler runs (see
 *                       system_bridge_task() in uart_bridge.c), and
 *                       flushing that side too would eat it out from under
 *                       the very request that asked for the restart. The
 *                       framing layer already resyncs on the next 0x7E
 *                       delimiter and already auto-flushes RX on a HW
 *                       FIFO/ring-buffer overflow event (see uart_owner.c)
 *                       -- this just makes that same recovery available on
 *                       demand instead of waiting for it to trip. */
#define SYSTEM_CMD_RESTART_UART 0x01u

/* --- INFO (task_id = UART_TASK_ID_INFO) ---
 * Unlike DAC/AD9833, this is a query: the requester's DATA frame (byte0 =
 * subcommand, e.g. INFO_CMD_GET_PIN_CONFIG) gets ACKed as usual to confirm
 * delivery, but the actual answer arrives as a *separate* DATA frame sent
 * back from (ESP, UART_TASK_ID_INFO) to (whichever device/task_id sent the
 * request) -- so the requester must itself be registered to receive it.
 *
 * GET_PIN_CONFIG response payload:
 *   byte0 = entry_count (N)
 *   N * { byte[0]=gpio_num, byte[1]=function_id }   -- 2 bytes per entry
 * function_id values below; the human-readable meaning of each (including
 * which peripheral/driver owns it) lives in the PC-side constant table
 * alongside these, not on the wire, to keep the response small.
 */
#define INFO_CMD_GET_PIN_CONFIG 0x01u

#define PIN_FUNC_I2C_SDA        0x01u
#define PIN_FUNC_I2C_SCL        0x02u
#define PIN_FUNC_UART_TX        0x03u
#define PIN_FUNC_UART_RX        0x04u
#define PIN_FUNC_SPI_SCLK       0x05u
#define PIN_FUNC_SPI_MOSI       0x06u
#define PIN_FUNC_SPI_CS         0x07u
#define PIN_FUNC_LED_HEARTBEAT  0x08u

/* GET_FW_VERSION request payload: just byte0 = INFO_CMD_GET_FW_VERSION, no
 * args. The exact same payload shape is also sent *unsolicited* (i.e. not
 * in reply to any request) once at boot, right after the info task starts
 * -- see uart_bridge_start_info_task(). The PC side can't tell a boot push
 * apart from a query reply by looking at the frame itself (both are
 * ordinary DATA messages to task INFO); it's the boot push specifically
 * that's the signal "the device just started", which the GUI uses to know
 * when to roll over to a new log file.
 *
 * GET_FW_VERSION response payload:
 *   byte0-1        = UART_PROTOCOL_VERSION, u16 LE -- fixed offset, always
 *                    read this first and compare before trusting anything
 *                    that follows (see UART_PROTOCOL_VERSION above)
 *   byte2          = dirty flag (0 = clean, 1 = dirty or unknown)
 *   byte3          = commit_len (N1)
 *   N1 bytes       = git commit, ASCII, not null-terminated
 *   byte(4+N1)     = datetime_len (N2)
 *   N2 bytes       = build date+time, ASCII "YYYY-MM-DD HH:MM:SSZ", not
 *                    null-terminated
 */
#define INFO_CMD_GET_FW_VERSION 0x02u

#endif // UART_TASK_IDS_H
