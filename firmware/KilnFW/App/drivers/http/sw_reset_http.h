// sw_reset_http -- owner request 2026-09-08: "add a sw reset item for the
// safty/esp under the reset menu. this should not touch configs but tell the
// safty processor and the [ESP] to reboot." A clean reboot from the UI,
// immediately useful for clearing an S6a-class stuck trip on the bench that
// currently needs JTAG to clear.
//
// Lives next to factory_reset.c and reuses its exact idiom (challenge/
// response auth via OTA_HTTP_CONTEXT_SW_RESET, ota_http_check_interlocks()
// for the firing/autotune refusal, the same respond-then-delay-then-reboot
// task shape) -- but this route erases NOTHING. No NVS partition is touched,
// no LittleFS format runs, no config store write happens anywhere on this
// path. See sw_reset_http.c's own handler comment for exactly what runs.
//
// Pico half, IMPORTANT LIMITATION: CommonFW's kilnlink wire protocol
// (docs/LINK_PROTOCOL.md) has no ESP->Pico "reboot yourself" command.
// SAFETY_CMD_ANNOUNCE_REBOOT (0x18) is a one-way courtesy notice ("I am
// about to go silent, this is not a crash") that only suppresses SaftyFW's
// own S6b link-dead guard for a grace window -- it carries no instruction
// and SaftyFW does nothing to itself on receipt. SAFETY_CMD_ROLLBACK (0x17)
// does cause the Pico to reboot, but into the OTHER bootloader slot,
// refusable by bootloader_decide_rollback(), and changes which firmware
// image is running -- not a "reboot in place" and not something to misuse
// for this feature. KILNLINK_PROTOCOL_VERSION (12) is not bumped by this
// change, per standing instruction: this file sends ANNOUNCE_REBOOT (the
// existing, version-independent courtesy notice) before rebooting the ESP,
// and otherwise leaves the Pico running. A real Pico self-reboot needs a new
// wire command and IS NOT implemented here -- see the sw_reset_post_handler()
// comment and the caller-facing report for the follow-up this leaves open.
#ifndef SW_RESET_HTTP_H
#define SW_RESET_HTTP_H

#include "esp_err.h"

#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers POST /api/sw_reset on the shared httpd instance. Must run after
// wifi_provision_http_start() has brought that server up, same convention as
// factory_reset_http_start(). safety_or_null is the same live SafetyLinkClass
// pointer main.c hands to ota_http_start() -- NULL on a board with no safety
// processor commissioned yet, in which case the ESP still reboots but no
// ANNOUNCE_REBOOT frame is sent (nothing to send it to).
esp_err_t sw_reset_http_start(SafetyLinkClass *safety_or_null);

#ifdef __cplusplus
}
#endif

#endif // SW_RESET_HTTP_H
