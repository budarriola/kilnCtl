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
// Pico half: SAFETY_CMD_REBOOT (0x29, CommonFW's kilnlink_reboot.h) --
// added 2026-09-09, the genuine "safety processor, reboot yourself in place,
// into the SAME firmware slot" command this route needed and did not have
// when it first shipped (da506105 reported that gap rather than working
// around it). safety_link_send_reboot() sends it on the REQUEST task, waits
// a bounded window for SAFETY_CMD_REBOOT_RESULT (0x2A), and this route puts
// the real, per-processor answer in its own HTTP response before the ESP's
// own delayed reboot task runs.
//
// What it is NOT, still worth stating because both neighbours are one wire
// id away: SAFETY_CMD_ROLLBACK (0x17) reboots the Pico into the OTHER
// bootloader slot -- different firmware, refusable by
// bootloader_decide_rollback(), a metadata flash write -- and must never be
// repurposed as a reboot-in-place. SAFETY_CMD_ANNOUNCE_REBOOT (0x18) is a
// one-way courtesy notice about the ESP's OWN reboot that instructs the Pico
// to do nothing; this route still sends it too, from the delayed reboot
// task, because it is a different and still-needed fact (it keeps a Pico
// that REFUSED its own reboot from nuisance-tripping S6b on this ESP's
// absence).
//
// The Pico can refuse: update_task_reboot_allowed() (SaftyFW) refuses while
// its heating relay is ARMED, the same gate config writes and rollback use.
// A refusal, an unanswered request, and an accepted reboot are three
// distinct outcomes, and the HTTP response says which one happened -- it
// never claims both processors rebooted when only one did.
//
// KILNLINK_PROTOCOL_VERSION stays 12. This pair is request-triggered in both
// directions (only a build with the feature sends 0x29; the Pico only sends
// 0x2A in reply to a 0x29), so no old peer can ever receive an id it does
// not know, and a too-old Pico simply never answers -- which this route
// reports as NOT CONFIRMED, never as success. Full argument:
// kilnlink_version.h's "12 -> 12, DELIBERATELY NOT BUMPED" entry.
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
