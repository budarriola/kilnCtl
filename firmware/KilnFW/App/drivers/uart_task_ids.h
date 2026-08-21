#ifndef UART_TASK_IDS_H
#define UART_TASK_IDS_H

#include "kilnlink/kilnlink_version.h"

/* Shared uart_protocol task_id numbering. Both the ESP firmware and the PC
 * side must agree on these -- see pc_tools/src/kilnctrl/protocol.py for the
 * matching Python constants and command payload (de)serialization. */

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
 * anything else -- see parse_fw_version_response() in devices.py.
 *
 * Version 2: kilnCtl main board. The unit-test fixture's devices (MCP4728
 * DAC = 1, AD9833 = 2, SSD1306 OLED = 4, PCF8575 = 7) are gone; those
 * task_ids are reused here for this board's hardware. INFO (3), LOG (5) and
 * SYSTEM (6) keep both their numbering and their payloads.
 *
 * Version 3 (2026-08-11): UART_PROTO_MAX_PAYLOAD raised 128 -> 253
 * (uart_protocol.h) to roughly halve the frame count for a DISPLAY blit --
 * the stop-and-wait ACK round trip, not raw baud, was the dominant cost.
 * Frame layout (header/CRC) is unchanged; only the payload ceiling and
 * DISPLAY_BLIT_CHUNK_PIXELS (pc_tools devices.py) moved. Bumped anyway
 * because a v2 peer's UART_PROTO_MAX_PAYLOAD=128 receive buffer would
 * truncate/reject a v3 sender's larger frame -- exactly what this version
 * gate exists to catch instead of silently misbehaving.
 *
 * Version 4 (2026-08-13): four new task_ids added (CONTROL=8, PROFILES=9,
 * AUTOTUNE=10, WIFI=11) so the PC-side GUI can drive everything the HTTP
 * dashboard offers without Wi-Fi, plus a new SYSTEM subcommand
 * (FACTORY_RESET). Existing task_ids 1-7 and their payloads are UNCHANGED --
 * this is purely additive, so it would not strictly need a version bump
 * under the "renumbering/layout change" policy above, but is bumped anyway
 * because an old PC build that has never heard of task_id 8-11 would
 * otherwise report a false "compatible" against firmware whose new commands
 * it cannot use, which is exactly the kind of silent skew
 * INFO_CMD_GET_FW_VERSION's exact-match policy exists to catch. See
 * docs/UART_PROTOCOL.md for the new task/command tables.
 *
 * Version 5 (2026-08-17): one new task_id, TOUCH=13, for the NS2009 touch
 * controller on the display panel (J2) -- GET_STATE plus INJECT, the latter
 * letting the PC/MCP side feed a synthetic touch that the firmware's idle
 * timer and wake logic treat exactly like a real press (screen_idle.h).
 * Existing task_ids 1-12 and their payloads are UNCHANGED; bumped for the
 * same reason Version 4 was -- an old PC build that has never heard of
 * task_id 13 would otherwise report a false "compatible" against firmware
 * whose new command it cannot use. See docs/UART_PROTOCOL.md.
 *
 * TODO.md Phase 7b.1 (2026-08-17): this is now an alias of CommonFW's
 * KILNLINK_PROTOCOL_VERSION (firmware/CommonFW/include/kilnlink/
 * kilnlink_version.h) rather than a second, independently-maintained number
 * -- that header's own doc comment says exactly this: "KilnFW's
 * UART_PROTOCOL_VERSION becomes an alias of this rather than a second
 * number", because kilnlink's framing layer is byte-for-byte the same
 * envelope uart_protocol.c already speaks for the PC link. If the isolated
 * safety link's contract ever changes independently of the PC link's, that
 * header's own comment says it becomes its own number at that point -- this
 * define does not need to move when that happens, only the value it aliases
 * does. */
#define UART_PROTOCOL_VERSION ((uint16_t)KILNLINK_PROTOCOL_VERSION)

#define UART_TASK_ID_THERMO   1u  /* MAX31856 x3 on the thermocouple board (J6) */
#define UART_TASK_ID_IO       2u  /* SX1509 expander: relays, digital I/O, DRDY */
#define UART_TASK_ID_INFO     3u
#define UART_TASK_ID_DISPLAY  4u  /* ILI9488 TFT on J2 */
#define UART_TASK_ID_LOG      5u
#define UART_TASK_ID_SYSTEM   6u
#define UART_TASK_ID_SAFETY   7u  /* opto-isolated link to the RP2040 safety processor */
#define UART_TASK_ID_CONTROL  8u  /* zone config + live per-zone control status -- mirrors zones_http.c */
#define UART_TASK_ID_PROFILES 9u  /* fire profile CRUD + execution -- mirrors profiles_http.c/dashboard_http.c */
#define UART_TASK_ID_AUTOTUNE 10u /* PID autotune -- mirrors dashboard_http.c's /api/autotune* */
#define UART_TASK_ID_WIFI     11u /* Wi-Fi status/scan/provision -- mirrors wifi_provision_http.c */
#define UART_TASK_ID_GPIO_PROBE 12u /* raw ESP32 GPIO probe -- CONFIG_KILNCTL_ENABLE_GPIO_PROBE, default off */
#define UART_TASK_ID_TOUCH    13u  /* NS2009 touch controller on the display panel (J2) */

/* --- THERMO (task_id = UART_TASK_ID_THERMO) ---
 * Three MAX31856 cold-junction-compensated thermocouple front ends living on
 * the thermocouple daughterboard, reached over the main board's shared SPI bus
 * through J6 (CS0/CS1/CS2 = channels 0/1/2). Each part's ~FAULT output comes
 * straight back to an ESP32-S3 GPIO; each part's ~DRDY output goes to the
 * SX1509 expander instead (IO8/IO9/IO10) and so is only observable through the
 * IO task or the firmware's own expander interrupt -- see docs/HARDWARE.md.
 *
 * byte0 = subcommand:
 *   0x01 CONFIG_CHANNEL       byte1=channel(0-2)
 *                             byte2=tc_type (THERMO_TC_* below)
 *                             byte3=avg_mode (THERMO_AVG_* below)
 *                             byte4=filter (0=60Hz, 1=50Hz)
 *                             byte5=conv_mode (0=one-shot/normally-off,
 *                                              1=automatic ~100ms)
 *                             Rewrites CR0/CR1 for that channel. The 50/60Hz
 *                             filter bit may only be changed while conversions
 *                             are off, so the driver stops auto conversion,
 *                             writes, then restores conv_mode.
 *   0x02 SET_THRESHOLDS       byte1=channel(0-2)
 *                             bytes2..5  = tc_high  f32 LE, degC
 *                             bytes6..9  = tc_low   f32 LE, degC
 *                             byte10     = cj_high  i8, degC
 *                             byte11     = cj_low   i8, degC
 *                             Thermocouple thresholds are stored by the part
 *                             at 0.0625 degC/LSB as int16; the driver rounds.
 *   0x03 SET_CJ_OFFSET        byte1=channel  bytes2..5 = offset f32 LE, degC
 *                             (part resolution 0.0625 degC, range +-8 degC)
 *   0x04 ONE_SHOT             byte1=channel -- triggers a single conversion.
 *                             The result is NOT returned by this command; poll
 *                             with READ (or enable AUTO_REPORT).
 *   0x05 READ                 byte1=channel(0-2) or 0xFF for all three
 *                                                        -- QUERY, see below
 *   0x06 READ_FAULTS          byte1=channel(0-2) or 0xFF -- QUERY, see below
 *   0x07 CLEAR_FAULTS         byte1=channel -- pulses CR0.FAULTCLR (only has
 *                             any effect in interrupt fault mode; in the
 *                             comparator mode this driver uses, fault bits
 *                             clear themselves when the condition goes away)
 *   0x08 SET_AUTO_REPORT      byte1=channel mask (bit N = channel N)
 *                             bytes2..3 = period_ms u16 LE (0 = off)
 *                             While on, the firmware pushes unsolicited READ
 *                             responses (identical layout to the 0x05 reply)
 *                             for the selected channels at that period.
 *   0x09 READ_REG             byte1=channel byte2=reg_addr byte3=len(1-16)
 *                                                        -- QUERY, debug
 *   0x0A WRITE_REG            byte1=channel byte2=reg_addr byte3=value
 *
 * READ / AUTO_REPORT response payload:
 *   byte0    = THERMO_CMD_READ (0x05)
 *   byte1    = count (N)
 *   N * 12 bytes, one per channel:
 *     [0]     channel
 *     [1..4]  thermocouple temperature, f32 LE, degC (linearized, 19-bit)
 *     [5..8]  cold-junction temperature, f32 LE, degC
 *     [9]     fault status register (SR, THERMO_FAULT_* below)
 *     [10]    flags: bit0 = ~FAULT pin asserted (low), bit1 = SPI read failed,
 *                    bit2 = reading is too old to trust (no usable conversion
 *                           for KILN_TEMP_STALE_AGE_MS, 10 s -- see MAX31856.h;
 *                           NOT "no conversion since last read", which is true
 *                           of a perfectly good sub-second-old value whenever
 *                           the host polls faster than the part converts)
 *     [11]    reserved, 0
 *   A channel whose SPI read failed still appears, with flags bit1 set and
 *   both temperatures set to NaN -- a missing channel is more confusing than
 *   an explicitly-bad one.
 *
 * READ_FAULTS response payload:
 *   byte0    = THERMO_CMD_READ_FAULTS (0x06)
 *   byte1    = count (N)
 *   N * 3 bytes: [0] channel, [1] SR (fault status), [2] MASK register
 *   A channel whose SPI read failed (never came up) is omitted from the
 *   list rather than reported with fabricated SR=0x00/MASK=0x00 -- matching
 *   MAX31856_read_all()'s own "never came up; report it as absent, not as
 *   0" convention. Bug fix, 2026-08-20: this used to always include every
 *   requested channel with zeroed SR/MASK regardless of read success, which
 *   read as "hardware present, no faults" even for a channel that was never
 *   on the bus -- directly contradicting the same channel's READ reply.
 *
 * READ_REG response payload:
 *   byte0 = THERMO_CMD_READ_REG (0x09)  byte1 = channel  byte2 = reg_addr
 *   byte3 = len (N)   N bytes = register contents
 *   N is 0 (no data bytes follow) if the channel's SPI read failed -- this
 *   is the *only* thermo query subcommand whose owner-side failure used to
 *   drop the reply outright (bug fix, 2026-08-20), leaving the host with a
 *   silent timeout instead of an answer.
 */
#define THERMO_CMD_CONFIG_CHANNEL  0x01u
#define THERMO_CMD_SET_THRESHOLDS  0x02u
#define THERMO_CMD_SET_CJ_OFFSET   0x03u
#define THERMO_CMD_ONE_SHOT        0x04u
#define THERMO_CMD_READ            0x05u
#define THERMO_CMD_READ_FAULTS     0x06u
#define THERMO_CMD_CLEAR_FAULTS    0x07u
#define THERMO_CMD_SET_AUTO_REPORT 0x08u
#define THERMO_CMD_READ_REG        0x09u
#define THERMO_CMD_WRITE_REG       0x0Au

#define THERMO_CHANNEL_COUNT 3u
#define THERMO_CHANNEL_ALL   0xFFu

/* CR1.TC[3:0] -- thermocouple type. K is what this kiln ships with; the rest
 * are the part's other supported types plus its raw voltage modes. */
#define THERMO_TC_B    0x00u
#define THERMO_TC_E    0x01u
#define THERMO_TC_J    0x02u
#define THERMO_TC_K    0x03u
#define THERMO_TC_N    0x04u
#define THERMO_TC_R    0x05u
#define THERMO_TC_S    0x06u
#define THERMO_TC_T    0x07u
#define THERMO_TC_VMODE_G8  0x08u
#define THERMO_TC_VMODE_G32 0x0Cu

/* CR1.AVGSEL[2:0] -- samples averaged per conversion. More averaging is
 * quieter but slower (auto-conversion period grows by ~33ms per extra sample
 * beyond the first at 60Hz). */
#define THERMO_AVG_1   0x00u
#define THERMO_AVG_2   0x01u
#define THERMO_AVG_4   0x02u
#define THERMO_AVG_8   0x03u
#define THERMO_AVG_16  0x04u

/* Fault status register (SR) bits, as reported in the READ response. */
#define THERMO_FAULT_OPEN     0x01u /* thermocouple open circuit */
#define THERMO_FAULT_OVUV     0x02u /* over/under voltage on an input */
#define THERMO_FAULT_TCLOW    0x04u /* TC temperature below low threshold */
#define THERMO_FAULT_TCHIGH   0x08u /* TC temperature above high threshold */
#define THERMO_FAULT_CJLOW    0x10u /* cold junction below low threshold */
#define THERMO_FAULT_CJHIGH   0x20u /* cold junction above high threshold */
#define THERMO_FAULT_TCRANGE  0x40u /* TC temperature outside the type's range */
#define THERMO_FAULT_CJRANGE  0x80u /* cold junction outside -55..+125 degC */

/* --- IO (task_id = UART_TASK_ID_IO) ---
 * SX1509 16-channel I2C expander (U5, address 0x3E: ADDR1/ADDR0 both strapped
 * to GND). It owns everything slow on this board:
 *   IO0..IO3   relay drives K3/K1/K2/K5 -> J8/J3/J4/J11, active high through
 *              a BSS138 gate resistor (Relay1..Relay4 on the schematic; note
 *              the schematic's relay *numbers* are the expander bit order,
 *              which is NOT the K-designator order -- see docs/HARDWARE.md)
 *   IO4..IO7   general I/O 1-4 (IO_1 is an opto-isolated input from J24,
 *              IO_2 drives an opto-isolated output to J25, IO_3/IO_4 go
 *              straight out to the J20 terminal block)
 *   IO8..IO10  ~DRDY inputs from thermocouple channels 0/1/2 (active low)
 *   IO11..IO13 general I/O 5-7 (J21, J23)
 *   IO14       LCD_IORQ  -- ILI9488 data/command select (see DISPLAY below)
 *   IO15       LCD_Reset -- ILI9488 hardware reset, active low
 * ~INT -> ESP32-S3 GPIO7, ~RESET <- GPIO10.
 *
 * Both a board-level view (relays and I/O by their schematic names, 1-based)
 * and raw register access are exposed; the board-level commands are what the
 * GUI and any control loop should use.
 *
 * byte0 = subcommand:
 *   0x01 SET_RELAY        byte1=relay(1-4)  byte2=on(0/1)
 *   0x02 SET_RELAY_MASK   byte1=mask(bits0-3 = relay1-4, which to change)
 *                         byte2=value(bits0-3) -- one atomic register write,
 *                         so multiple relays switch on the same I2C transfer
 *   0x03 SET_IO           byte1=io(1-7)  byte2=level(0/1). Only meaningful for
 *                         an I/O currently configured as an output.
 *   0x04 SET_IO_DIR       byte1=io(1-7)  byte2=dir(0=output, 1=input)
 *                         byte3=pullup(0/1, input only)
 *   0x05 READ             (no args) -- QUERY, see below
 *   0x06 SET_AUTO_REPORT  bytes1..2 = period_ms u16 LE (0 = off). While on,
 *                         the firmware pushes unsolicited READ responses at
 *                         that period, and additionally immediately on every
 *                         ~INT edge (so an input change is reported without
 *                         waiting out the period).
 *   0x07 ALL_RELAYS_OFF   (no args) -- unconditional, and the state the
 *                         firmware falls back to on link loss or a safety
 *                         fault; kept as its own subcommand so it is one
 *                         short frame that can't be misparsed as anything
 *                         else.
 *   0x10 SX_WRITE_REG     byte1=reg_addr  byte2=value
 *   0x11 SX_READ_REG      byte1=reg_addr  byte2=len(1-16) -- QUERY, debug
 *   0x12 SX_SET_DIR       bytes1..2 = u16 LE (bit N: 1 = input, 0 = output --
 *                         matches the part's RegDir polarity)
 *   0x13 SX_SET_PULLUP    bytes1..2 = u16 LE
 *   0x14 SX_SET_OPENDRAIN bytes1..2 = u16 LE
 *   0x15 SX_SET_DEBOUNCE  bytes1..2 = enable mask u16 LE  byte3=config(0-7,
 *                         RegDebounceConfig: 0.5ms << config at 2MHz)
 *   0x16 SX_SET_INT_MASK  bytes1..2 = u16 LE (bit N = 1 masks/disables the
 *                         interrupt for pin N, matching RegInterruptMask)
 *                         bytes3..4 = u16 LE sense: 2 bits per *pin pair*, as
 *                         the part's RegSense registers already encode them
 *   0x17 SX_LED_DRIVER    byte1=pin(0-15) byte2=enable(0/1) byte3=intensity
 *                         (0-255, 0 = full on for this part's sink driver)
 *   0x18 SX_RESET         byte1=hard(0 = software reset via RegReset,
 *                                    1 = pulse the ~RESET pin on GPIO10)
 *   0x19 SX_SCAN          (no args) -- QUERY: probes 0x3E/0x3F/0x70/0x71
 *
 * READ / AUTO_REPORT response payload:
 *   byte0    = IO_CMD_READ (0x05)
 *   bytes1-2 = RegData, u16 LE, raw pin states (bit N = expander pin N)
 *   bytes3-4 = RegDir,  u16 LE (1 = input)
 *   byte5    = relay shadow, bits0-3 = relay1-4 as last commanded
 *   byte6    = digital I/O levels, bits0-6 = io1-io7
 *   byte7    = DRDY bits: bit0-2 = channel 0-2 ~DRDY asserted (i.e. pin low)
 *   byte8    = flags: bit0 = ~INT currently asserted,
 *                     bit1 = last I2C transfer failed
 *
 * SX_READ_REG response payload:
 *   byte0 = IO_CMD_SX_READ_REG (0x11)  byte1 = reg_addr  byte2 = len (N)
 *   N bytes = register contents
 *
 * SX_SCAN response payload:
 *   byte0 = IO_CMD_SX_SCAN (0x19)  byte1 = count (N)  N bytes = addresses
 */
#define IO_CMD_SET_RELAY        0x01u
#define IO_CMD_SET_RELAY_MASK   0x02u
#define IO_CMD_SET_IO           0x03u
#define IO_CMD_SET_IO_DIR       0x04u
#define IO_CMD_READ             0x05u
#define IO_CMD_SET_AUTO_REPORT  0x06u
#define IO_CMD_ALL_RELAYS_OFF   0x07u
#define IO_CMD_SX_WRITE_REG     0x10u
#define IO_CMD_SX_READ_REG      0x11u
#define IO_CMD_SX_SET_DIR       0x12u
#define IO_CMD_SX_SET_PULLUP    0x13u
#define IO_CMD_SX_SET_OPENDRAIN 0x14u
#define IO_CMD_SX_SET_DEBOUNCE  0x15u
#define IO_CMD_SX_SET_INT_MASK  0x16u
#define IO_CMD_SX_LED_DRIVER    0x17u
#define IO_CMD_SX_RESET         0x18u
#define IO_CMD_SX_SCAN          0x19u

#define IO_RELAY_COUNT 4u
#define IO_DIGITAL_COUNT 7u

/* --- DISPLAY (task_id = UART_TASK_ID_DISPLAY) ---
 * ILI9488 480x320 SPI TFT (BIGTREETECH TFT35 SPI V2.1) on J2. SCK/MOSI/MISO
 * are the board's shared SPI bus; CS3 is a real ESP32-S3 GPIO, but D/C
 * (LCD_IORQ) and ~RESET (LCD_Reset) hang off the SX1509 -- so every
 * command/data transition costs an I2C transfer. The driver therefore batches
 * hard: one D/C toggle per command, then all of that command's data in a
 * single SPI transaction. Do not expect per-pixel throughput; full-screen
 * work should go through FILL_RECT/BLIT rather than repeated small writes.
 *
 * Colors are RGB565 u16 LE on the wire; the driver expands to the 18-bit
 * (RGB666) format the ILI9488 requires over SPI.
 *
 * byte0 = subcommand:
 *   0x01 RESET            byte1=hard(0 = software reset command,
 *                                    1 = pulse ~RESET via the expander)
 *   0x02 SET_POWER        byte1=on(0/1) -- display off + sleep-in when 0
 *   0x03 SET_ROTATION     byte1=rotation(0-3) -- MADCTL, 0/2 portrait 320x480,
 *                         1/3 landscape 480x320
 *   0x04 SET_INVERT       byte1=invert(0/1)
 *   0x05 CLEAR            bytes1..2 = color u16 LE (whole screen)
 *   0x06 FILL_RECT        bytes1..2=x  3..4=y  5..6=w  7..8=h  9..10=color,
 *                         all u16 LE
 *   0x07 DRAW_RECT        same args as FILL_RECT -- 1px outline
 *   0x08 DRAW_LINE        bytes1..2=x0 3..4=y0 5..6=x1 7..8=y1 9..10=color
 *   0x09 SET_TEXT_CURSOR  bytes1..2=x  3..4=y (pixels, top-left of the glyph)
 *   0x0A SET_TEXT_STYLE   bytes1..2=fg  3..4=bg  byte5=size(1-8 integer scale)
 *                         byte6=opaque_background(0/1)
 *   0x0B PRINT            bytes1..(length-1) = ASCII, NOT null-terminated,
 *                         drawn at the cursor, which advances (and wraps at
 *                         the right edge)
 *   0x0C BLIT_BEGIN       bytes1..2=x 3..4=y 5..6=w 7..8=h -- opens a pixel
 *                         window; the driver keeps the SPI window open and
 *                         streams subsequent BLIT_DATA into it
 *   0x0D BLIT_DATA        bytes1..(length-1) = RGB565 pixels, u16 LE, in
 *                         row-major order continuing where the last chunk
 *                         stopped (odd trailing byte is an error)
 *   0x0E BLIT_END         (no args) closes the window. Sending anything other
 *                         than BLIT_DATA/BLIT_END while a blit is open is an
 *                         error and aborts the blit.
 *   0x0F READ_ID          (no args) -- QUERY, see below
 *
 * READ_ID response payload:
 *   byte0 = DISPLAY_CMD_READ_ID (0x0F)
 *   byte1 = ok(0/1) -- 0 if the read failed or the panel answered all-zero
 *   bytes2..4 = the three ID bytes from RDDID (0x04)
 *   bytes5..8 = width u16 LE, height u16 LE, as currently rotated
 */
#define DISPLAY_CMD_RESET           0x01u
#define DISPLAY_CMD_SET_POWER       0x02u
#define DISPLAY_CMD_SET_ROTATION    0x03u
#define DISPLAY_CMD_SET_INVERT      0x04u
#define DISPLAY_CMD_CLEAR           0x05u
#define DISPLAY_CMD_FILL_RECT       0x06u
#define DISPLAY_CMD_DRAW_RECT       0x07u
#define DISPLAY_CMD_DRAW_LINE       0x08u
#define DISPLAY_CMD_SET_TEXT_CURSOR 0x09u
#define DISPLAY_CMD_SET_TEXT_STYLE  0x0Au
#define DISPLAY_CMD_PRINT           0x0Bu
#define DISPLAY_CMD_BLIT_BEGIN      0x0Cu
#define DISPLAY_CMD_BLIT_DATA       0x0Du
#define DISPLAY_CMD_BLIT_END        0x0Eu
#define DISPLAY_CMD_READ_ID         0x0Fu

/* --- TOUCH (task_id = UART_TASK_ID_TOUCH) ---
 * The NS2009 touch controller on the same J2 panel as DISPLAY -- see
 * NS2009.h and screen_idle.h. Not wired to any drawing: this task only
 * reports touch state and lets the host inject synthetic touches, both
 * against screen_idle's idle/wake state machine, which is what actually
 * blanks and wakes the panel.
 *
 * byte0 = subcommand:
 *   0x01 GET_STATE  (no args) -- QUERY, see below
 *   0x02 INJECT      x(u16 LE), y(u16 LE), pressed(u8) -- fire-and-forget,
 *                    no reply. Feeds screen_idle exactly as a real press
 *                    would: resets the idle timer and wakes the screen if
 *                    it is currently blanked. x/y are carried through only
 *                    for a future UI-hit-test use; today they do not affect
 *                    the idle/wake decision. pressed=0 (a release) is
 *                    accepted but changes nothing -- there is no "held"
 *                    state to end.
 *
 * GET_STATE response payload:
 *   byte0 = TOUCH_CMD_GET_STATE (0x01)
 *   byte1 = screen_on (0/1)
 *   bytes2..5 = idle_ms, u32 LE -- milliseconds since the last touch
 *               activity (real or injected); saturates at UINT32_MAX rather
 *               than wrapping.
 */
#define TOUCH_CMD_GET_STATE 0x01u
#define TOUCH_CMD_INJECT    0x02u

/* --- SAFETY (task_id = UART_TASK_ID_SAFETY) ---
 * The RP2040 safety processor (A1) sits in its own ground domain: the only
 * connections across the barrier are two opto-isolated UART lines and one
 * opto-isolated fault line, all three through TCMT1109 optocouplers. The
 * Pico -- not the ESP -- owns the safety thermocouple board on J7, the three
 * current-sense channels, the E-stop input and the safety relay K4.
 *
 * Two things about that barrier that are easy to get backwards, both traced
 * from the schematic (see docs/HARDWARE.md for the full trace):
 *   - DataToSafty is the ESP's *TX* (GPIO4, feeding U2's LED, whose collector
 *     is the Pico's RX) and DataFromSafty is the ESP's *RX* (GPIO5, collector
 *     of U3, whose LED is driven by the Pico's TX). U3 is drawn mirrored
 *     relative to U1/U2 -- reading it with their orientation is what makes
 *     this look backwards when it is not.
 *   - Both directions are logically INVERTED. The driving side's LED is on
 *     when its line is high, which pulls the receiving side's collector low,
 *     so an idle-high UART line arrives as idle-low. The firmware fixes this
 *     with uart_set_line_inverse(TXD_INV | RXD_INV) rather than in software.
 *     GPIO5's only external pull-up is R15 (U3's collector has nothing else
 *     on that net besides the ESP); the internal pull-up is enabled too, as
 *     belt-and-braces.
 *   - The Fault line (GPIO6) is an ESP *output*: driving it high lights U1's
 *     LED, which pulls the Pico's mainFault input low. There is no hardware
 *     path for the Pico to signal the ESP outside the UART.
 *
 * This task is the PC's window onto that link. The ESP polls the Pico over
 * the isolated UART and caches the last good answer; GET_STATUS returns that
 * cache rather than blocking on the far side, so a dead link shows up as a
 * stale/invalid status instead of a hung request.
 *
 * NOTE: the ESP<->Pico wire format on the isolated UART is the *same*
 * uart_protocol framing used for the PC link, with device id
 * UART_PROTO_DEVICE_SAFETY; the Pico firmware that answers it is not part of
 * this repository yet, so until it exists GET_STATUS reports link_up = 0.
 *
 * byte0 = subcommand:
 *   0x01 GET_STATUS      (no args) -- QUERY, see below
 *   0x02 REQUEST_ENABLE  byte1=enable(0/1) -- asks the safety processor to
 *                        permit (or drop) heating. Advisory only: the Pico
 *                        can refuse, and its own interlocks always win.
 *   0x03 PING            (no args) -- forces an immediate poll of the far
 *                        side instead of waiting for the next poll tick
 *   0x04 GET_LINK_STATS  (no args) -- QUERY, see below
 *   0x05 SET_POLL_PERIOD bytes1..2 = period_ms u16 LE (0 = stop polling)
 *   0x06 SET_FAULT_OUT   byte1=assert(0/1) -- drives the isolated Fault line
 *                        (GPIO6) that tells the safety processor the main
 *                        controller has faulted. Manual override of a line
 *                        the firmware otherwise asserts on its own (loss of
 *                        the PC link, a thermocouple fault, watchdog).
 *
 * GET_STATUS response payload:
 *   byte0     = SAFETY_CMD_GET_STATUS (0x01)
 *   byte1     = flags: bit0 link_up (a valid reply within 3 poll periods)
 *                      bit1 Fault line currently asserted by this firmware
 *                      bit2 estop asserted (as reported by the Pico)
 *                      bit3 safety relay K4 energized
 *                      bit4 heating enable currently granted
 *                      bit5 safety thermocouple reading valid
 *   bytes2..5 = safety thermocouple temperature, f32 LE, degC
 *   bytes6..9 = safety cold-junction temperature, f32 LE, degC
 *   byte10    = safety thermocouple fault status (same bits as THERMO_FAULT_*)
 *   bytes11..14 = current sense 1, f32 LE, amps
 *   bytes15..18 = current sense 2, f32 LE, amps
 *   bytes19..22 = current sense 3, f32 LE, amps
 *   bytes23..24 = age of this data, u16 LE, ms (65535 = never received)
 *
 * GET_LINK_STATS response payload:
 *   byte0      = SAFETY_CMD_GET_LINK_STATS (0x04)
 *   bytes1..4  = frames sent,     u32 LE
 *   bytes5..8  = frames received, u32 LE
 *   bytes9..12 = CRC/framing errors, u32 LE
 *   bytes13..16= timeouts, u32 LE
 *   bytes17..18= poll period, u16 LE, ms
 *
 * SAFETY_CMD_FW_VERSION (0x0B, Pico -> ESP) and SAFETY_CMD_ANNOUNCE_VERSION
 * (0x0F, ESP -> Pico) share one layout (CommonFW/docs/LINK_PROTOCOL.md
 * section 4/6), truncated at boot_id for the ESP's outbound ANNOUNCE_VERSION
 * (no config_version/config_crc -- those describe the Pico's own active
 * config, which the ESP has none of to report):
 *   byte0       = 0x0B or 0x0F
 *   bytes1..2   = KILNLINK_PROTOCOL_VERSION, u16 LE -- fixed offset, read
 *                 before anything else
 *   bytes3..4   = KILNLINK_MIN_COMPATIBLE, u16 LE -- fixed offset
 *   byte5       = dirty (0 clean, 1 dirty OR unknown)
 *   byte6       = commit_len (N1)
 *   bytes7..    = git commit hash, ASCII, N1 bytes
 *   next byte   = datetime_len (N2)
 *   next N2     = build date+time, ASCII
 *   next byte   = boot_id
 *   [FW_VERSION only, from here]
 *   next byte   = config_version
 *   next 2      = config_crc, u16 LE
 * See safety_link.c's ANNOUNCE_VERSION payload builder / FW_VERSION parser.
 */
#define SAFETY_CMD_GET_STATUS     0x01u
#define SAFETY_CMD_REQUEST_ENABLE 0x02u
#define SAFETY_CMD_PING           0x03u
#define SAFETY_CMD_GET_LINK_STATS 0x04u
#define SAFETY_CMD_SET_POLL_PERIOD 0x05u
#define SAFETY_CMD_SET_FAULT_OUT   0x06u
/* ROADMAP.md M5: ESP->Pico context broadcast, LINK_PROTOCOL.md sec 4.
 * Same value as kilnlink_context.h's KILNLINK_CONTEXT_CMD -- defined again
 * here purely so this file's SAFETY_CMD_* list stays the one place every
 * subcommand on this wire is enumerated; safety_link.c encodes the payload
 * through the shared codec, not by hand, so it uses KILNLINK_CONTEXT_CMD
 * directly rather than this macro. */
#define SAFETY_CMD_PUSH_CONTEXT   0x07u
/* Ids reserved by CommonFW/docs/LINK_PROTOCOL.md section 4/6 as part of the
 * "compatibility floor": both sides must parse/emit these regardless of
 * whether KILNLINK_PROTOCOL_VERSION agrees, because they are how a mismatch
 * gets discovered and fixed in the first place. 0x0B is Pico->ESP (also
 * pushed unsolicited at Pico boot); 0x0F is ESP->Pico only (this firmware
 * never expects to receive it). Both share the same byte layout, see
 * safety_link.c's ANNOUNCE_VERSION payload builder and FW_VERSION parser. */
#define SAFETY_CMD_FW_VERSION      0x0Bu
#define SAFETY_CMD_ANNOUNCE_VERSION 0x0Fu

/* ESP->Pico, CommonFW/docs/LINK_PROTOCOL.md sec 4 -- the GUI's path to
 * acknowledging a Pico-latched trip. Same value as kilnlink_clear_trip.h's
 * KILNLINK_CLEAR_TRIP_CMD, defined again here for the same "one place every
 * subcommand on this wire is enumerated" reason SAFETY_CMD_PUSH_CONTEXT is;
 * safety_link.c's safety_link_send_clear_trip() encodes the payload through
 * that shared codec, not by hand.
 *
 * Doubles, additively, as the PC->ESP subcommand a future pc_tools MCP tool
 * can send on UART_TASK_ID_SAFETY (mirroring SAFETY_CMD_REQUEST_ENABLE's own
 * PC->ESP use above) -- no arguments: the ESP derives trip_mask itself from
 * its own cached Pico DIAG state rather than trusting one supplied over the
 * PC link, see safety_link_send_clear_trip()'s doc comment for why. */
#define SAFETY_CMD_CLEAR_TRIP 0x0Au

/* Pico -> ESP telemetry, Frame B (CommonFW/docs/LINK_PROTOCOL.md sec 6):
 * "Everything the 23-byte [status] frame has no room for" -- trip/warn
 * masks, boot reason, context-frame health counters, the Pico's own
 * armed/warn/tripped state. Pushed unsolicited by the Pico, same 500 ms
 * cadence as the status frame (Frame A); this build never requests it.
 * 26-byte fixed payload -- see safety_link.c's safety_apply_diag() for the
 * field-by-field layout and firmware/CommonFW/include/kilnlink/
 * kilnlink_diag.h for the byte-exact codec this is mirrored from
 * (kilnlink_diag.c is not compiled into this component today -- see this
 * file's kilnlink component CMakeLists.txt comment -- so this driver
 * hand-parses it the same way it already hand-parses GET_STATUS/POWER,
 * rather than depending on that codec). */
#define SAFETY_CMD_DIAG 0x08u

/* Pico -> ESP telemetry, Frame E (CommonFW/docs/LINK_PROTOCOL.md sec 6):
 * "No guard reads any of this. It exists to be displayed." Pushed
 * unsolicited by the Pico; this build never requests it. 55-byte fixed
 * payload -- see safety_link.c's safety_apply_power() for the field-by-field
 * layout and firmware/CommonFW/include/kilnlink/kilnlink_power.h for the
 * byte-exact codec this is mirrored from (kilnlink_power.c is not compiled
 * into this component today -- see this file's kilnlink component
 * CMakeLists.txt comment -- so this driver hand-parses it the same way it
 * already hand-parses GET_STATUS, rather than depending on that codec). */
#define SAFETY_CMD_POWER 0x0Eu

/* Pico -> ESP telemetry, Frame D (CommonFW/docs/LINK_PROTOCOL.md sec 6):
 * pushed immediately the moment a trip latches (not on the 500 ms cadence),
 * repeated a few times over the next second since there is no ACK; the ESP
 * dedups on trip_seq (byte1). "Capturing the deciding values at the instant
 * of the trip is the whole point" -- the answer to "why did the kiln stop."
 * 29-byte fixed payload -- see safety_link.c's safety_apply_trip_event() for
 * the field-by-field layout and firmware/CommonFW/include/kilnlink/
 * kilnlink_trip.h for the byte-exact codec this is mirrored from, same
 * not-compiled-into-this-component reasoning as SAFETY_CMD_DIAG/POWER
 * above. */
#define SAFETY_CMD_TRIP_EVENT 0x0Du

/* PC->ESP queries, LINK_PROTOCOL.md sec 7: "Mirror all of it on the PC-link
 * SAFETY task as well" -- give pc_tools/the MCP server the same DIAG/
 * TRIP_EVENT data the HTTP dashboard (dashboard_http.c) and LCD
 * (ui_page_safety.c) already read, without needing Wi-Fi. Answered from the
 * cache only, exactly like GET_STATUS/GET_LINK_STATS above -- never by
 * talking to the Pico -- see safety_link_build_diag_payload() /
 * safety_link_build_trip_event_payload() in safety_link.c. No arguments.
 *
 * GET_DIAG response payload (mirrors safety_link_status_t's diag_* fields,
 * safety_link.h):
 *   byte0      = SAFETY_CMD_GET_DIAG (0x0C)
 *   byte1      = flags: bit0 diag_ever_received (all other bytes below are
 *                       meaningless/zero if this bit is clear)
 *   byte2      = diag_trip_reason (SAFETY_TRIP_*, 0 = none)
 *   bytes3..4  = diag_warn_mask, u16 LE
 *   bytes5..6  = diag_trip_mask, u16 LE
 *   bytes7..10 = diag_uptime_ms, u32 LE
 *   byte11     = diag_boot_reason (SAFETY_LINK_DIAG_BOOT_* bits)
 *   byte12     = diag_context_age_100ms (255 = never)
 *   bytes13..16= diag_context_frames_ok, u32 LE
 *   bytes17..20= diag_context_frames_bad, u32 LE
 *   bytes21..24= diag_tx_frames_dropped, u32 LE
 *   byte25     = diag_state (SAFETY_LINK_DIAG_STATE_*)
 *   byte26     = diag_flags (SAFETY_LINK_DIAG_FLAG_* bits)
 *
 * GET_TRIP_EVENT response payload (mirrors safety_link_status_t's trip_*
 * fields, safety_link.h):
 *   byte0      = SAFETY_CMD_GET_TRIP_EVENT (0x15)
 *   byte1      = flags: bit0 trip_event_ever_received (all other bytes
 *                       below are meaningless/zero if this bit is clear)
 *   byte2      = trip_last_seq
 *   byte3      = trip_reason (SAFETY_TRIP_* at the moment of this trip)
 *   bytes4..7  = trip_uptime_ms, u32 LE
 *   bytes8..11 = trip_safety_tc_c, f32 LE, degC
 *   bytes12..15= trip_deciding_threshold, f32 LE
 *   bytes16..19= trip_current_a[0], f32 LE, amps
 *   bytes20..23= trip_current_a[1], f32 LE, amps
 *   bytes24..27= trip_current_a[2], f32 LE, amps
 *   byte28     = trip_relay_recent_mask
 *   byte29     = trip_context_age_100ms
 *   bytes30..33= trip_event_age_ms, u32 LE, ms (0 if never received)
 */
#define SAFETY_CMD_GET_DIAG       0x0Cu

/* Phase 10 (SaftyFW) / TODO.md 9.5's Pico firmware-update relay
 * (CommonFW/docs/UPDATE_PROTOCOL.md section 4). Ids are byte-for-byte
 * SaftyFW's own src/tasks/link_frame.h LINK_FRAME_UPDATE_*_CMD values --
 * mirrored here rather than shared via a header, since SaftyFW is a
 * separate repository/build target this project cannot #include from. All
 * five are ESP->Pico except UPDATE_STATUS, which is the reply (also sent
 * unsolicited, e.g. on refusal or as a periodic gap report while a transfer
 * is active). All are sent/received as UART_PROTO_MSG_BROADCAST -- "the
 * Pico never participates in the ACK'd DATA/ACK/NACK transport" for these
 * frames, see safety_link.c's safety_link_send_update_frame().
 *   0x10 UPDATE_BEGIN  36 B payload -- see App/drivers/ota_pico_relay.c's
 *                      header comment for the exact field layout (mirrors
 *                      SaftyFW's src/update/image_header.h, the frozen
 *                      source of truth for this frame).
 *   0x11 UPDATE_DATA   4 B offset (u32 LE) + up to 248 B image data
 *   0x12 UPDATE_END    4 B: image CRC32 (repeated from UPDATE_BEGIN)
 *   0x13 UPDATE_ABORT  no args
 *   0x14 UPDATE_STATUS (Pico -> ESP) 16 B header + up to 32 * 2 B gap chunk
 *                      indices -- see safety_link.h's
 *                      safety_link_update_status_t for the parsed layout,
 *                      mirrored from SaftyFW's src/tasks/update_task.c
 *                      (that file's own header comment: "invented here
 *                      since nothing else in this codebase defines it").
 */
#define SAFETY_CMD_UPDATE_BEGIN   0x10u
#define SAFETY_CMD_UPDATE_DATA    0x11u
#define SAFETY_CMD_UPDATE_END     0x12u
#define SAFETY_CMD_UPDATE_ABORT   0x13u
#define SAFETY_CMD_UPDATE_STATUS  0x14u

/* See SAFETY_CMD_GET_DIAG's doc comment above for both GET_DIAG and
 * GET_TRIP_EVENT's payload layouts -- 0x15 is the next free id after the
 * 0x10-0x14 UPDATE_* block. */
#define SAFETY_CMD_GET_TRIP_EVENT 0x15u

/* ESP->Pico, CommonFW/docs/LINK_PROTOCOL.md sec 4 -- the GUI's path to
 * commissioning SaftyFW's config_store.h record (SaftyFW TODO.md Phase 9's
 * "SAFETY_CMD_SET_CONFIG (wire command)"). Same value as
 * kilnlink_set_config.h's KILNLINK_SET_CONFIG_CMD, defined again here for
 * the same "one place every subcommand on this wire is enumerated" reason
 * SAFETY_CMD_CLEAR_TRIP is; safety_link.c's safety_link_send_set_config()
 * encodes the payload through that shared codec, not by hand.
 *
 * Doubles, additively, as the PC->ESP subcommand pc_tools' MCP server sends
 * on UART_TASK_ID_SAFETY (mirroring SAFETY_CMD_CLEAR_TRIP's own PC->ESP use
 * above) -- 1 byte argument:
 *   byte1 = tc_type, 0-0x0F (MAX31856 CR1 TC[3:0] nibble, same range
 *           THERMO_CMD_CONFIG_CHANNEL's own tc_type byte uses above) */
#define SAFETY_CMD_SET_CONFIG 0x16u

/* ESP->Pico, CommonFW/docs/LINK_PROTOCOL.md sec 4 -- tools/PcTools/TODO.md's
 * `ota_rollback(processor)` line, Pico half (the ESP half is
 * ota_http.c's POST /api/ota/esp/rollback). Explicit "revert to the
 * previously-running bootloader slot, right now" -- distinct from the
 * automatic boot_attempts fallback (bootloader_decide_boot()), which only
 * fires after the active slot has already failed real CRC checks. Same
 * value as kilnlink_rollback.h's KILNLINK_ROLLBACK_CMD, defined again here
 * for the same "one place every subcommand on this wire is enumerated"
 * reason SAFETY_CMD_CLEAR_TRIP/SET_CONFIG are; safety_link.c's
 * safety_link_send_rollback() encodes the (empty) payload through that
 * shared codec, not by hand.
 *
 * No payload -- 1 byte, cmd only, same shape as SAFETY_CMD_PING/CLEAR_TRIP's
 * PC->ESP no-args use above. Fire-and-forget, never ACKs on the wire: the
 * Pico refuses (ARMED, or no valid slot to fall back to) or reboots; the PC
 * observes the outcome via the next GET_STATUS/GET_FW_VERSION poll (a
 * changed boot_id/active-slot version), not a reply to this frame. See
 * SaftyFW's src/tasks/link_task.c's link_task_handle_rollback() and
 * bootloader/metadata.c's bootloader_decide_rollback() for the refusal
 * logic -- most importantly, this is refused unless the OTHER slot is
 * currently VALID/PENDING_VERIFY, so a rollback can never strand the board
 * with zero bootable slots. */
#define SAFETY_CMD_ROLLBACK 0x17u

/* ESP->Pico, CommonFW/docs/LINK_PROTOCOL.md sec 4 -- KilnFW/TODO.md's
 * "SAFETY_CMD_ANNOUNCE_REBOOT sent before the ESP reboots" line. Unsolicited
 * courtesy notice sent immediately before an OTA self-update's
 * esp_restart(): tells SaftyFW's link_task.c "the next several seconds of
 * silence are expected, not a crash," so S6b (the link-dead guard,
 * SAFETY_MODEL.md section 4) can suppress its own trip for a bounded window
 * instead of nuisance-tripping on every routine update. Same value as
 * kilnlink_announce_reboot.h's KILNLINK_ANNOUNCE_REBOOT_CMD, defined again
 * here for the same "one place every subcommand on this wire is enumerated"
 * reason SAFETY_CMD_CLEAR_TRIP/SET_CONFIG/ROLLBACK are; safety_link.c's
 * safety_link_send_announce_reboot() encodes the (empty) payload through
 * that shared codec, not by hand.
 *
 * No payload -- 1 byte, cmd only, same shape as SAFETY_CMD_PING/ROLLBACK's
 * no-args use above. Fire-and-forget, never ACKs on the wire: by the time a
 * reply could matter the ESP is already rebooting. This is advisory only --
 * it grants no permission to heat, does not touch relay_owner, and does not
 * suppress any guard other than S6b's own trip condition. See
 * src/tasks/link_task.c's link_task_handle_announce_reboot() and
 * src/safety_guards.c's S6b block for the suppression logic. */
#define SAFETY_CMD_ANNOUNCE_REBOOT 0x18u

#define SAFETY_FLAG_LINK_UP      0x01u
#define SAFETY_FLAG_FAULT        0x02u
#define SAFETY_FLAG_ESTOP        0x04u
#define SAFETY_FLAG_RELAY        0x08u
#define SAFETY_FLAG_ENABLED      0x10u
#define SAFETY_FLAG_TEMP_VALID   0x20u

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
 *                       demand instead of waiting for it to trip.
 *   0x02 FACTORY_RESET byte1=scope (0=wifi, 1=kiln, 2=profiles, 3=all --
 *                       factory_reset_scope_t in factory_reset.h). Mirrors
 *                       POST /api/factory_reset (factory_reset.c) exactly:
 *                       same per-partition erase, same unconditional reboot
 *                       ~500ms later so this command's own ACK has a chance
 *                       to leave first. No reply frame either way -- like
 *                       every other non-query SYSTEM/IO/THERMO command, the
 *                       ACK is the only confirmation, and the reboot itself
 *                       (visible as a fresh unsolicited GET_FW_VERSION push
 *                       from INFO, see uart_task_ids.h's boot-push doc
 *                       comment) is the real evidence the erase happened. An
 *                       out-of-range scope byte is rejected with no erase
 *                       and no reboot, same "reject cleanly before touching
 *                       anything" discipline as every other bridge here. */
#define SYSTEM_CMD_RESTART_UART  0x01u
#define SYSTEM_CMD_FACTORY_RESET 0x02u

/* --- INFO (task_id = UART_TASK_ID_INFO) ---
 * Unlike the device tasks, this is a query: the requester's DATA frame
 * (byte0 = subcommand, e.g. INFO_CMD_GET_PIN_CONFIG) gets ACKed as usual to
 * confirm delivery, but the actual answer arrives as a *separate* DATA frame
 * sent back from (ESP, UART_TASK_ID_INFO) to (whichever device/task_id sent
 * the request) -- so the requester must itself be registered to receive it.
 *
 * GET_PIN_CONFIG response payload:
 *   byte0 = entry_count (N)
 *   N * { byte[0]=gpio_num, byte[1]=function_id }   -- 2 bytes per entry
 * function_id values below; the human-readable meaning of each (including
 * which peripheral/driver owns it) lives in the PC-side constant table
 * alongside these, not on the wire, to keep the response small.
 *
 * Only real ESP32-S3 GPIOs appear here. Signals that live on the SX1509
 * (the DRDY inputs, the relay drives, LCD_IORQ/LCD_Reset) are expander pins,
 * not GPIOs, and are reported through the IO task's READ instead.
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
#define PIN_FUNC_SPI_MISO       0x09u
#define PIN_FUNC_THERMO_FAULT   0x0Au /* ~FAULT from a MAX31856, active low */
#define PIN_FUNC_EXPANDER_IRQ   0x0Bu /* SX1509 ~INT */
#define PIN_FUNC_EXPANDER_RST   0x0Cu /* SX1509 ~RESET */
#define PIN_FUNC_SAFETY_TX      0x0Du /* opto-isolated, inverted UART to the RP2040 */
#define PIN_FUNC_SAFETY_RX      0x0Eu
#define PIN_FUNC_SAFETY_FAULT   0x0Fu /* opto-isolated fault line OUT to the RP2040 */

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

/* GET_WIFI_STATUS request payload: just byte0 = INFO_CMD_GET_WIFI_STATUS, no
 * args. Reports wifi_prov's own tracked state (see wifi_prov.h) -- this
 * never touches kiln_io/relay_authority/safety_link, matching wifi_prov's
 * "losing Wi-Fi must never affect control/safety" rule; it is purely so a
 * PC-side GUI can offer an "open dashboard" link when there's somewhere to
 * send it.
 *
 * GET_WIFI_STATUS response payload:
 *   byte0        = connected flag (0/1, == wifi_prov_is_sta_connected())
 *   byte1        = ip_len (N, 0 if not connected)
 *   N bytes      = station IP, dotted-quad ASCII, not null-terminated
 */
#define INFO_CMD_GET_WIFI_STATUS 0x03u

/* --- CONTROL (task_id = UART_TASK_ID_CONTROL) ---
 * Zone configuration and manual relay control -- mirrors zones_http.c's
 * /api/zones (config) and dashboard_http.c's /api/relay (control). Manual
 * relay switching is NOT duplicated here: use IO_CMD_SET_RELAY /
 * IO_CMD_SET_RELAY_MASK (task UART_TASK_ID_IO) -- they already pass through
 * relay_authority_on_blocked(), the exact same gate /api/relay uses, so a
 * second implementation here would just be a second place for that gate to
 * drift out of sync.
 *
 * Scope cap: zones_cfg_t has ~20 fields per zone (name, heater window/min-on/
 * min-off timing, cross-zone delta, plant model) and /api/zones is a
 * whole-page validate-and-commit that cross-checks all of them together --
 * that does not fit one 253-byte frame and does not decompose into a safe
 * per-field write without reimplementing zones_http.c's whole-page
 * commit-or-reject discipline over the wire. Only the fields a live control
 * loop actually needs to read or retune are exposed: GET_ZONES reads back
 * everything zones_config_get_*() already exposes as a getter, and
 * SET_ZONE_PID/SET_ZONE_MODEL write through the two setters zones_http.h
 * already exports publicly (zones_config_set_pid/zones_config_set_model --
 * the same ones autotune_engine_accept() uses). Zone naming, relay
 * assignment, heater window timing and the cross-zone guard threshold remain
 * HTTP-only; use the Thermocouples & Zones page for those.
 *
 * byte0 = subcommand:
 *   0x01 GET_ZONES     (no args) -- QUERY, see below
 *   0x02 SET_ZONE_PID   byte1=zone_index
 *                        bytes2..5  = kp f32 LE
 *                        bytes6..9  = ki f32 LE
 *                        bytes10..13= kd f32 LE
 *                       Same validation as zones_config_set_pid() (finite,
 *                       >= 0); rejected without writing anything otherwise.
 *   0x03 SET_ZONE_MODEL byte1=zone_index
 *                        bytes2..5  = model_k_dc f32 LE
 *                        bytes6..9  = model_tau_s f32 LE
 *                        bytes10..13= model_dead_time_s f32 LE
 *                       Same validation as zones_config_set_model(); an
 *                       all-zero triple clears the model (documented
 *                       "no model" encoding), a negative value is rejected.
 * Both SET_* commands answer with a one-byte ok/fail QUERY-style reply
 * (unlike THERMO/IO's silent SET_*) because a rejected zone_index or an
 * out-of-range gain is exactly the kind of mistake a GUI needs to surface
 * immediately, the same way the HTTP JSON {"ok":false,...} responses do.
 *
 * GET_ZONES response payload:
 *   byte0 = CONTROL_CMD_GET_ZONES (0x01)
 *   byte1 = thermo_count (zones_config_get_thermo_count())
 *   byte2 = relay_count (KILN_IO_RELAY_COUNT on this board)
 *   byte3 = count (N, == thermo_count, capped at MAX31856_CHANNEL_COUNT)
 *   N * 31 bytes, one per zone:
 *     [0]      index
 *     [1]      relay_mask
 *     [2]      control_mode (zone_control_mode_t)
 *     [3..6]   cal_offset_c, f32 LE
 *     [7..10]  pid_kp, f32 LE
 *     [11..14] pid_ki, f32 LE
 *     [15..18] pid_kd, f32 LE
 *     [19..22] max_ramp_c_per_hr, f32 LE
 *     [23..26] max_temp_c, f32 LE
 *     [27..30] min_temp_c, f32 LE
 *
 * SET_ZONE_PID / SET_ZONE_MODEL response payload:
 *   byte0 = the subcommand echoed back (0x02 / 0x03)
 *   byte1 = ok (0/1)
 */
#define CONTROL_CMD_GET_ZONES    0x01u
#define CONTROL_CMD_SET_ZONE_PID 0x02u
#define CONTROL_CMD_SET_ZONE_MODEL 0x03u

/* --- PROFILES (task_id = UART_TASK_ID_PROFILES) ---
 * Fire profile storage (mirrors profiles_http.c's /api/profiles, /api/profile,
 * /api/profile/delete) and execution (mirrors dashboard_http.c's
 * /api/profile_exec and /api/profile_exec/start|stop|pause|resume|
 * ack_last_run). GET_EXEC_STATUS's per-zone block mirrors /api/control's
 * tuning-focused per-zone shape (control_mode/actual/duty/PID terms) rather
 * than /api/profile_exec's fuller one -- that is the shape a live control
 * loop or tuning GUI actually needs; the lifecycle-only fields
 * (segment_index/segment_count/dwelling/target_c/ramp-lock) are still
 * carried at the top level so nothing from /api/profile_exec is lost, only
 * the fault_reason strings and last_run breadcrumb are dropped for space --
 * use HTTP for those.
 *
 * Unlike THERMO/IO's silent SET_*, every mutating command here (SAVE,
 * DELETE, START, PAUSE, RESUME, ACK_LAST_RUN) answers with an explicit
 * ok/fail reply, because these can fail for an operator-relevant reason
 * (feasibility check, no such profile, nothing running to pause) the same
 * way their HTTP counterparts' JSON bodies report one -- a silent failure
 * a GUI would only notice on the next poll is the wrong UX for "did my
 * firing actually start".
 *
 * byte0 = subcommand:
 *   0x01 LIST            (no args) -- QUERY, see below
 *   0x02 GET             byte1=id(0-7) -- QUERY, see below
 *   0x03 SAVE            byte1=id(0-7, or 0xFF for "first free slot")
 *                         byte2=name_len(N1, 0-15)  N1 bytes=name (ASCII)
 *                         byte(3+N1)=zone_mask
 *                         byte(4+N1)=segment_count(1-12)
 *                         segment_count * 12 bytes:
 *                           target_c f32 LE, ramp_c_per_hr f32 LE,
 *                           dwell_min u32 LE
 *                         Same target_c/ramp_c_per_hr range and
 *                         feasibility validation as POST /api/profile;
 *                         rejected (whole submission) without writing
 *                         anything on any failure. -- QUERY-style reply,
 *                         see below.
 *   0x04 DELETE          byte1=id(0-7) -- QUERY-style reply, see below
 *   0x05 GET_EXEC_STATUS (no args) -- QUERY, see below
 *   0x06 START           byte1=id(0-7) -- QUERY-style reply, see below
 *   0x07 STOP            (no args) -- QUERY-style reply, always ok
 *   0x08 PAUSE           (no args) -- QUERY-style reply, see below
 *   0x09 RESUME          (no args) -- QUERY-style reply, see below
 *   0x0A ACK_LAST_RUN    (no args) -- QUERY-style reply, see below
 *
 * LIST response payload:
 *   byte0 = PROFILES_CMD_LIST (0x01)
 *   byte1 = count (N, used slots only)
 *   N * variable: id(1) name_len(1) name(name_len bytes) zone_mask(1)
 *                 segment_count(1)
 *
 * GET response payload:
 *   byte0 = PROFILES_CMD_GET (0x02)
 *   byte1 = ok (0/1, 0 = no such profile -- nothing else follows)
 *   [if ok] byte2=id byte3=name_len(N1) N1 bytes=name byte(4+N1)=zone_mask
 *           byte(5+N1)=segment_count(N2)
 *           N2 * 12 bytes: target_c f32 LE, ramp_c_per_hr f32 LE,
 *                          dwell_min u32 LE
 *
 * SAVE response payload:
 *   byte0 = PROFILES_CMD_SAVE (0x03)
 *   byte1 = ok (0/1)
 *   [if ok]  byte2=id  byte3=warning_count (capped; see HTTP for the full
 *            warning text -- this just tells the GUI "N segments are within
 *            20% of a zone's ramp ceiling, go check the details over HTTP")
 *   [if !ok] byte2=err_len(N)  N bytes=error text (ASCII, truncated to fit)
 *
 * DELETE / START / PAUSE / RESUME / ACK_LAST_RUN response payload:
 *   byte0 = the subcommand echoed back
 *   byte1 = ok (0/1)
 *   [START, if !ok] byte2=err_len(N)  N bytes=error text (truncated to fit)
 *
 * GET_EXEC_STATUS response payload:
 *   byte0        = PROFILES_CMD_GET_EXEC_STATUS (0x05)
 *   byte1        = state (profile_exec_state_t: 0=idle 1=running 2=paused
 *                  3=done 4=faulted)
 *   byte2        = profile_id
 *   byte3        = name_len (N1)
 *   N1 bytes     = profile_name, ASCII
 *   byte(4+N1)   = zone_mask
 *   byte(5+N1)   = segment_index
 *   byte(6+N1)   = segment_count
 *   byte(7+N1)   = dwelling (0/1)
 *   bytes(8+N1)..(11+N1)  = target_c, f32 LE
 *   bytes(12+N1)..(15+N1) = segment_elapsed_s, u32 LE
 *   bytes(16+N1)..(19+N1) = dwell_remaining_s, u32 LE
 *   byte(20+N1)  = ramp_lock_held (0/1)
 *   byte(21+N1)  = ramp_lock_lagging_mask
 *   byte(22+N1)  = fault_guard (thermal_guard_trip_t; only meaningful when
 *                  state == faulted)
 *   byte(23+N1)  = zone_count (N2, active zones only)
 *   N2 * 14 bytes, one per active zone:
 *     [0]     zone index
 *     [1]     control_mode (zone_control_mode_t)
 *     [2..5]  actual_c, f32 LE (meaningless if !actual_valid)
 *     [6]     actual_valid (0/1)
 *     [7..10] duty, f32 LE
 *     [11]    relay_commanded_on (0/1)
 *     [12]    faulted (0/1)
 *     [13]    fault_guard (thermal_guard_trip_t; only meaningful if faulted)
 */
#define PROFILES_CMD_LIST             0x01u
#define PROFILES_CMD_GET              0x02u
#define PROFILES_CMD_SAVE             0x03u
#define PROFILES_CMD_DELETE           0x04u
#define PROFILES_CMD_GET_EXEC_STATUS  0x05u
#define PROFILES_CMD_START            0x06u
#define PROFILES_CMD_STOP             0x07u
#define PROFILES_CMD_PAUSE            0x08u
#define PROFILES_CMD_RESUME           0x09u
#define PROFILES_CMD_ACK_LAST_RUN     0x0Au

#define PROFILES_SAVE_ID_NEW 0xFFu /* byte1 sentinel for "first free slot" */

/* --- AUTOTUNE (task_id = UART_TASK_ID_AUTOTUNE) ---
 * Mirrors dashboard_http.c's /api/autotune (GET) and /api/autotune/
 * start|abort|accept. /api/autotune/matrix (the cross-zone coupling matrix +
 * RGA) and /api/autotune/trace.csv|history.csv (the raw sample dumps) are
 * NOT mirrored -- both are bulk/table data (up to MAX31856_CHANNEL_COUNT^2
 * matrix cells with nested arrays, or up to AUTOTUNE_ENGINE_MAX_SAMPLES CSV
 * rows) that do not fit this protocol's 253-byte payload cap and do not have
 * a meaningful truncated form -- a partial coupling matrix or a truncated
 * CSV trace is actively misleading rather than merely incomplete. Use HTTP
 * for those.
 *
 * byte0 = subcommand:
 *   0x01 GET_STATUS (no args) -- QUERY, see below
 *   0x02 START      byte1=zone_index
 *                    byte2=method (0=step, 1=relay)
 *                    bytes3..6  = step_duty (method 0) or setpoint_c
 *                                 (method 1), f32 LE
 *                    bytes7..10 = relay_d, f32 LE (method 1 only; <=0 means
 *                                 "engine default", same as the HTTP form)
 *                    bytes11..14= relay_h_c, f32 LE (method 1 only; <=0 =
 *                                 default)
 *                    byte15     = rule (method 1 only: 0=tyreus-luyben,
 *                                 1=ziegler-nichols)
 *                    Always send all 16 argument bytes; the ones the
 *                    selected method doesn't use are ignored, same as the
 *                    HTTP form's optional fields defaulting server-side.
 *                    -- QUERY-style reply, see below
 *   0x03 ABORT      (no args) -- QUERY-style reply, always ok
 *   0x04 ACCEPT     (no args) -- QUERY-style reply, see below
 *
 * GET_STATUS response payload:
 *   byte0        = AUTOTUNE_CMD_GET_STATUS (0x01)
 *   byte1        = state (autotune_engine_state_t: 0=idle 1=settling
 *                  2=stepping 3=relay_approach 4=relay_cycling 5=done
 *                  6=aborted)
 *   byte2        = method (0=step, 1=relay)
 *   byte3        = zone_index
 *   bytes4..7    = elapsed_s, u32 LE
 *   bytes8..9    = sample_count, u16 LE
 *   bytes10..13  = actual_c, f32 LE (meaningless if !actual_valid)
 *   byte14       = actual_valid (0/1)
 *   bytes15..18  = duty, f32 LE
 *   byte19       = model_valid (0/1, step method only)
 *   bytes20..23  = model.k_gain_c_per_duty, f32 LE
 *   bytes24..27  = model.tau_s, f32 LE
 *   bytes28..31  = model.dead_time_s, f32 LE
 *   bytes32..35  = proposed_gains.kp, f32 LE
 *   bytes36..39  = proposed_gains.ki, f32 LE
 *   bytes40..43  = proposed_gains.kd, f32 LE
 *   byte44       = proposed_gains.rule (autotune_rule_t: 0=simc
 *                  1=ziegler-nichols 2=tyreus-luyben)
 *   bytes45..48  = predicted_max_ramp_c_per_hr, f32 LE (step method only)
 *   byte49       = relay.valid (0/1, relay method only)
 *   bytes50..53  = relay.ku, f32 LE
 *   bytes54..57  = relay.tu_s, f32 LE
 *   bytes58..61  = relay.amplitude_c, f32 LE
 *   byte62       = abort_reason_len (N, only meaningful when state ==
 *                  aborted; 0 otherwise)
 *   N bytes      = abort_reason, ASCII, truncated to whatever fits the
 *                  253-byte cap (see HTTP for the untruncated text)
 *
 * START response payload:
 *   byte0 = AUTOTUNE_CMD_START (0x02)
 *   byte1 = ok (0/1)
 *   [if !ok] byte2=err_len(N)  N bytes=error text (truncated to fit)
 *
 * ABORT / ACCEPT response payload:
 *   byte0 = the subcommand echoed back
 *   byte1 = ok (0/1)
 */
#define AUTOTUNE_CMD_GET_STATUS 0x01u
#define AUTOTUNE_CMD_START      0x02u
#define AUTOTUNE_CMD_ABORT      0x03u
#define AUTOTUNE_CMD_ACCEPT     0x04u

#define AUTOTUNE_METHOD_WIRE_STEP  0x00u
#define AUTOTUNE_METHOD_WIRE_RELAY 0x01u
#define AUTOTUNE_RULE_WIRE_TL 0x00u
#define AUTOTUNE_RULE_WIRE_ZN 0x01u

/* --- WIFI (task_id = UART_TASK_ID_WIFI) ---
 * Mirrors wifi_provision_http.c's GET /status, GET /scan, POST /provision,
 * GET /networks, POST /forget. Exists specifically so Wi-Fi can be
 * configured over a link that works even when Wi-Fi itself is down or
 * unconfigured (TODO.md section 1's "losing Wi-Fi must never be fatal to
 * the control loop" cuts both ways: the control/UART path must also never
 * *depend* on Wi-Fi being up to fix Wi-Fi). This task never touches
 * kiln_io/relay_authority/safety_link, same rule wifi_prov.h documents for
 * its own HTTP surface.
 *
 * SCAN and GET_NETWORKS results are capped (WIFI_WIRE_MAX_SCAN_ENTRIES,
 * WIFI_WIRE_MAX_NETWORK_ENTRIES below) to fit one 253-byte payload -- a
 * truncated flag is set rather than splitting across frames, since this
 * protocol has no existing multi-part convention to reuse (see
 * docs/UART_PROTOCOL.md) and a stale/partial scan list is still useful,
 * unlike a truncated CSV dump. Use GET /scan or /networks over HTTP for the
 * complete list on a crowded RF environment.
 *
 * byte0 = subcommand:
 *   0x01 GET_STATUS   (no args) -- QUERY, see below
 *   0x02 SCAN         (no args) -- QUERY, see below
 *   0x03 ADD_NETWORK  byte1=ssid_len(N1, 1-32)  N1 bytes=ssid
 *                     byte(2+N1)=password_len(N2, 0-64)  N2 bytes=password
 *                     Mirrors wifi_prov_add_network() exactly (adds to the
 *                     saved-network list, or updates the password of an
 *                     already-saved SSID; switches to home mode). --
 *                     QUERY-style reply, see below
 *   0x04 SET_MODE     byte1=mode (0=home, 1=ap)
 *                     -- QUERY-style reply, see below
 *   0x05 SET_AP_IDENTITY
 *                     byte1=has_ssid(0/1)
 *                     [if 1] byte2=ap_ssid_len(N1,1-32) N1 bytes=ap_ssid
 *                     byte(2+has_ssid?N1:0)=has_password(0/1)
 *                     [if 1] byte+1=ap_password_len(N2,0 or 8-63)
 *                            N2 bytes=ap_password
 *                     Either field may be omitted (has_*=0) to leave it
 *                     unchanged, mirroring /provision's independent
 *                     ap_ssid/ap_password form fields. -- QUERY-style
 *                     reply, see below
 *   0x06 GET_NETWORKS (no args) -- QUERY, see below
 *   0x07 FORGET       byte1=ssid_len(N)  N bytes=ssid
 *                     -- QUERY-style reply, see below
 *
 * Every mutating command here answers with an explicit ok/fail reply, same
 * reasoning as PROFILES above -- a rejected SSID/password length or "no
 * such saved network" is exactly what an operator provisioning a board with
 * no other network access needs to see immediately.
 *
 * GET_STATUS response payload:
 *   byte0       = WIFI_CMD_GET_STATUS (0x01)
 *   byte1       = mode (0=home, 1=ap)
 *   byte2       = state (wifi_prov_state_t: 0=ap_mode 1=unprovisioned
 *                 2=connecting 3=connected 4=reconnecting)
 *   byte3       = sta_connected (0/1)
 *   byte4       = ssid_len (N1)         N1 bytes = ssid (active/best-guess)
 *   byte(5+N1)  = ap_ssid_len (N2)      N2 bytes = ap_ssid
 *   byte(6+N1+N2) = ap_password_len (N3) N3 bytes = ap_password (plaintext
 *                 -- see wifi_prov_get_ap_password()'s doc comment for why
 *                 this one field is deliberately not a saved-network secret)
 *   byte(7+N1+N2+N3) = sta_ip_len (N4)  N4 bytes = sta_ip, dotted-quad ASCII
 *   byte(8+N1+N2+N3+N4) = sta_rssi, i8 (signed, dBm; -127 if not connected)
 *   byte(9+N1+N2+N3+N4) = ap_clients
 *
 * SCAN response payload:
 *   byte0 = WIFI_CMD_SCAN (0x02)
 *   byte1 = count (N, capped at WIFI_WIRE_MAX_SCAN_ENTRIES)
 *   byte2 = truncated (0/1 -- more results existed than fit)
 *   N * variable: ssid_len(1) ssid(ssid_len bytes) rssi(i8) secure(0/1)
 *
 * ADD_NETWORK / SET_MODE / SET_AP_IDENTITY / FORGET response payload:
 *   byte0 = the subcommand echoed back
 *   byte1 = ok (0/1)
 *
 * GET_NETWORKS response payload:
 *   byte0 = WIFI_CMD_GET_NETWORKS (0x06)
 *   byte1 = count (N, capped at WIFI_WIRE_MAX_NETWORK_ENTRIES)
 *   byte2 = truncated (0/1)
 *   N * variable: ssid_len(1) ssid(ssid_len bytes) saved(0/1) in_range(0/1)
 *                 rssi(i8, only meaningful if in_range) secure(0/1, only
 *                 meaningful if in_range) connected(0/1)
 */
#define WIFI_CMD_GET_STATUS     0x01u
#define WIFI_CMD_SCAN           0x02u
#define WIFI_CMD_ADD_NETWORK    0x03u
#define WIFI_CMD_SET_MODE       0x04u
#define WIFI_CMD_SET_AP_IDENTITY 0x05u
#define WIFI_CMD_GET_NETWORKS   0x06u
#define WIFI_CMD_FORGET         0x07u

#define WIFI_WIRE_MAX_SCAN_ENTRIES 6u
#define WIFI_WIRE_MAX_NETWORK_ENTRIES 5u

/* --- GPIO_PROBE (task_id = UART_TASK_ID_GPIO_PROBE) ---
 * Raw ESP32-S3 pin control for answering "is this net actually where the
 * schematic says" without building and flashing a one-off firmware --
 * tools/PcTools/TODO.md capability 1. Compiled in only when
 * CONFIG_KILNCTL_ENABLE_GPIO_PROBE is set (default off, gpio_probe.c); on a
 * build without it this task is never registered and every subcommand below
 * goes unanswered, same as any other unregistered task_id.
 *
 * This is deliberately NOT a general escape hatch:
 *   - A hard deny-list refuses the SPI bus, the I2C bus, the SX1509 IRQ/RESET
 *     pins, the display CS, the PC-link UART pins and -- above all -- the
 *     three safety-link pins (SAFETY_TX_IO, SAFETY_RX_IO, and especially
 *     SAFETY_FAULT_IO/GPIO6, the isolated line the safety processor reads as
 *     "main controller faulted"). See gpio_probe.c's deny-list table.
 *   - WRITE and SET_MODE are refused outright while a profile is
 *     RUNNING or PAUSED (profile_executor_get_status()), so this cannot
 *     become a second, ungoverned relay control path.
 *   - Every accepted and every refused call is logged at the point of
 *     refusal, with the reason.
 *
 * byte0 = subcommand:
 *   0x01 SET_MODE   byte1=gpio_num  byte2=mode (GPIO_PROBE_MODE_*)
 *   0x02 WRITE      byte1=gpio_num  byte2=level(0/1)
 *   0x03 READ       byte1=gpio_num                        -- QUERY
 *   0x04 READ_ALL   (no args)                              -- QUERY
 *
 * SET_MODE / WRITE reply (echoed on every call, success or refusal):
 *   byte0 = subcommand
 *   byte1 = ok (0/1)
 *   [if !ok] byte2 = length-prefixed ASCII reason (1-byte length + bytes)
 *
 * READ reply:
 *   byte0 = GPIO_PROBE_CMD_READ
 *   byte1 = ok (0/1)
 *   [if ok]  byte2 = level (0/1)
 *   [if !ok] byte2 = length-prefixed ASCII reason
 *
 * READ_ALL reply -- every pin this connection has SET_MODE'd since boot or
 * since the probe's own reset, up to GPIO_PROBE_MAX_TRACKED entries:
 *   byte0 = GPIO_PROBE_CMD_READ_ALL
 *   byte1 = count (N)
 *   N * 3 bytes: gpio_num, mode, level
 */
#define GPIO_PROBE_CMD_SET_MODE  0x01u
#define GPIO_PROBE_CMD_WRITE     0x02u
#define GPIO_PROBE_CMD_READ      0x03u
#define GPIO_PROBE_CMD_READ_ALL  0x04u

#define GPIO_PROBE_MODE_INPUT          0u
#define GPIO_PROBE_MODE_INPUT_PULLUP   1u
#define GPIO_PROBE_MODE_INPUT_PULLDOWN 2u
#define GPIO_PROBE_MODE_OUTPUT         3u

#define GPIO_PROBE_MAX_TRACKED 16u

#endif // UART_TASK_IDS_H
