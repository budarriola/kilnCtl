// tusb_config.h -- TinyUSB device-stack configuration for SimFW's native USB
// CDC link (docs/PLAN.md section 4.1/5, "USB CDC (TinyUSB): frame RX/TX,
// CRC, dispatch to command queue, telemetry TX"). Only src/tasks/usb_owner.c
// and src/tasks/usb_descriptors.c include tusb.h -- single-owner-per-
// peripheral doctrine (PLAN.md section 4's opening paragraph) extends to
// "only the owner's translation units speak TinyUSB", not just "only the
// owner's task touches the registers." This header itself is included by
// TinyUSB's own sources (tusb.c etc, pulled in via the pico-sdk
// `tinyusb_device` INTERFACE target), never by application code directly.
//
// CFG_TUSB_MCU/CFG_TUSB_DEBUG are supplied by pico-sdk's own
// tinyusb_common_base target (src/rp2_common/tinyusb/CMakeLists.txt, via
// lib/tinyusb/hw/bsp/rp2040/family.cmake) as compiler `-D` flags, not by
// this file -- redefining them here would just create a second, possibly
// conflicting, definition. CFG_TUSB_OS=OPT_OS_FREERTOS is instead forced by
// setting the `TINYUSB_OPT_OS` CMake variable before pico_sdk_init() runs
// (CMakeLists.txt), which is what makes tud_task() below block on a real
// FreeRTOS queue (osal_freertos.h) instead of a bare WFE spin loop
// (OPT_OS_PICO, that family.cmake's own default) -- the latter would look
// like READY-not-BLOCKED to the FreeRTOS scheduler and starve every
// lower-priority task sharing usb_owner's core.
//
// Modeled on lib/tinyusb/examples/device/cdc_msc_freertos/src/tusb_config.h
// (the pico-sdk-vendored TinyUSB checkout's own FreeRTOS CDC example -- the
// closest precedent in this toolchain, since neither SaftyFW nor any other
// RP2040 project in this repo speaks USB; SaftyFW's own link is a plain UART,
// see uart_owner.c), trimmed to CDC-only (no MSC/HID -- SimFW's link carries
// only the benchproto command/telemetry stream) and sized for benchproto's
// own frame ceiling rather than that example's defaults.
#ifndef SIMFW_TUSB_CONFIG_H
#define SIMFW_TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error "CFG_TUSB_MCU must already be defined by pico-sdk's tinyusb_common_base target -- see this file's header comment."
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_FREERTOS
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

// Full-speed only -- the RP2040's USB controller has no high-speed PHY.
#define CFG_TUD_ENABLED   1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

//------------- CLASS -------------//
// CDC only: SimFW's whole USB surface is the benchproto command/telemetry
// stream (usb_descriptors.c's single interface). No MSC/HID/MIDI/vendor
// class -- there is nothing else on this link.
#define CFG_TUD_CDC    1
#define CFG_TUD_MSC    0
#define CFG_TUD_HID    0
#define CFG_TUD_MIDI   0
#define CFG_TUD_VENDOR 0

// CDC FIFO/endpoint buffer sizes: sized to comfortably hold one full
// BENCHPROTO_FRAME_STUFFED_MAX (~530 bytes, benchproto_frame.h) worth of
// wire bytes without TinyUSB itself needing multiple USB transactions per
// frame at full speed's 64-byte max packet size -- not a hard requirement
// (usb_owner.c's own byte-at-a-time SLIP assembler tolerates a frame
// arriving split across several tud_cdc_read() calls regardless), just
// generous enough that the common case is one clean read.
#define CFG_TUD_CDC_RX_BUFSIZE 640
#define CFG_TUD_CDC_TX_BUFSIZE 640
#define CFG_TUD_CDC_EP_BUFSIZE 64

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TUSB_CONFIG_H
