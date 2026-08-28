#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#include "MAX31856.h"
#include "esp_err.h"
#include "espInterfaces/uart_protocol.h"
#include "kiln_io.h"
#include "safety_link.h"
#include "screen_idle.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every bridge below has the same shape: it registers one task_id on `proto`,
 * spawns one task, blocks on that task_id's inbox, dispatches on payload
 * byte0 and -- for the subcommands uart_task_ids.h marks as QUERY -- answers
 * with its own DATA frame sent back to whoever asked (carried in the inbound
 * message's device/task_id), so the requester must itself be registered on a
 * task_id to receive it. Failures are logged, never signalled on the wire:
 * the requester sees a reply timeout rather than a fabricated value.
 *
 * The device handle passed to each must already be initialized and must
 * outlive the bridge task -- in practice a static in app_main. */

/* THERMO (task 1): the three MAX31856 channels on the daughterboard. Takes the
 * *bus*, not a channel, because every subcommand carries its own channel index
 * (or 0xFF for all three) and MAX31856_bus_channel() is the lookup. Implements
 * SET_AUTO_REPORT: a periodic unsolicited push of the READ payload for the
 * selected channels.
 *
 * 2026-08-19 (TODO.md 10.14 Phase 2): every actual MAX31856 access now goes
 * through thermo_owner.c -- see thermo_owner.h's top comment for why (NOT a
 * race fix, unlike kiln_io_owner's Phase 1; architectural consistency and a
 * future Phase 6 choke point). thermo_owner_start() must be called (with the
 * same bus passed here) before this function. `bus` is still required
 * because this task's signature is unchanged and MAX31856_start_all()'s
 * result is what main.c gates thermo_owner_start() on. */
esp_err_t uart_bridge_start_thermo_task(uart_protocol_t *proto, MAX31856BusClass *bus);

/* IO (task 2): the SX1509 expander through the kiln_io board layer -- relays,
 * the seven digital I/Os, the three ~DRDY inputs, plus raw register access.
 * Implements SET_AUTO_REPORT, which here is both periodic *and* edge-driven:
 * this bridge installs the GPIO ISR on the expander's ~INT line (kiln_io
 * deliberately installs none) and pushes a READ payload immediately on every
 * edge, so an input change is reported without waiting out the period.
 *
 * 2026-08-19 (TODO.md 10.14 Phase 1): every actual expander access, and the
 * ownership/safety-fault gate on SET_RELAY/SET_RELAY_MASK/SX_WRITE_REG/
 * SX_SET_DIR, now goes through kiln_io_owner.c -- see kiln_io_owner.h.
 * kiln_io_owner_start() must be called (with the live SafetyLinkClass, or
 * NULL if none came up) before this function; there is no `safety` param
 * here any more. A command that would turn a relay ON while a safety fault
 * is asserted is still refused, same as always -- see kiln_io_owner.h's doc
 * comments and docs/SAFETY_MODEL.md. */
esp_err_t uart_bridge_start_io_task(uart_protocol_t *proto, kiln_io_t *io);

/* DISPLAY (task 4): removed 2026-08-27 as dead code -- uart_bridge_start_display_task()
 * was never called (LVGL owns the ILI9488 outright now; see TODO.md 10.1).
 * DISPLAY_CMD_* and UART_TASK_ID_DISPLAY stay in uart_task_ids.h as wire-protocol
 * constants -- kilnctrl's gui.py/actions.py Display panel still speaks them,
 * even though nothing on the firmware side answers. */

/* TOUCH (task 13): the PC's window onto screen_idle's touch/idle state --
 * GET_STATE reads it, INJECT feeds it a synthetic touch that resets the idle
 * timer and wakes the screen exactly like a real NS2009 press would. `idle`
 * must already be up (screen_idle_init/_start succeeded); this task owns no
 * hardware itself. */
esp_err_t uart_bridge_start_touch_task(uart_protocol_t *proto, screen_idle_t *idle);

/* SAFETY (task 7): the PC's window onto the isolated link to the RP2040.
 * GET_STATUS/GET_LINK_STATS answer out of safety_link's cache, so a dead peer
 * is stale data rather than a hung request. */
esp_err_t uart_bridge_start_safety_task(uart_protocol_t *proto, SafetyLinkClass *link);

/* INFO (task 3): answers GET_PIN_CONFIG with this board's real ESP32-S3 GPIO
 * assignments (built from the same settings.h macros the drivers are
 * initialized with, so it can't drift) and GET_FW_VERSION with the protocol
 * version plus the git/build stamp from build_info.h. Also pushes one
 * unsolicited GET_FW_VERSION payload at boot. Owns no hardware. */
esp_err_t uart_bridge_start_info_task(uart_protocol_t *proto);

/* SYSTEM (task 6): admin commands against the link itself (RESTART_UART,
 * FACTORY_RESET). Needs the uart_owner_t directly, since that's where the
 * RX-flush primitive lives. */
esp_err_t uart_bridge_start_system_task(uart_protocol_t *proto, uart_owner_t *owner);

/* Creates the shared flash-safe executor task that CONTROL/PROFILES/AUTOTUNE
 * (tasks 8/9/10) run their message handlers on -- see the HAZARD and "MUST BE
 * CREATED EARLY" comment blocks in uart_bridge_ext.c. Its stack is 8192 bytes
 * of INTERNAL SRAM, so app_main MUST call this early, before display/LVGL
 * bring-up: after lvgl_port_start() the largest free internal block has been
 * measured at 7680 and the create fails, silently costing all three of those
 * bridge surfaces. Idempotent, and the three start functions still call the
 * same code lazily as a fallback -- but they return ESP_ERR_NO_MEM if it has
 * to run that late and fails. */
esp_err_t uart_bridge_ext_start_flash_worker(void);

/* Runs fn(arg) on the SAME internal-SRAM-stack worker task
 * uart_bridge_ext_start_flash_worker() creates, and blocks the calling task
 * until it returns -- for ANY caller elsewhere in the firmware that needs to
 * touch NVS/flash from a task whose own stack is not safely internal (see
 * uart_bridge_ext.c:104-127's HAZARD block: a flash operation disables the
 * cache, which makes PSRAM unreachable, and ESP-IDF's own
 * esp_task_stack_is_sane_cache_disabled() asserts -- aborts the whole board
 * -- if the calling task's stack lives there). safety_cfg_store.c's deferred
 * NVS flush (2026-08-23) is the first caller outside this file's own three
 * bridge handlers (CONTROL/PROFILES/AUTOTUNE), which is exactly what this
 * export exists for: `arg` may point at the caller's stack, since the caller
 * is blocked for the whole call and that storage stays live. Returns
 * ESP_ERR_INVALID_ARG if fn is NULL, ESP_FAIL if the worker is not started
 * or its job queue/lock could not be used (caller must not fall through to
 * running fn() itself in that case -- that is precisely the bug this
 * exists to prevent). */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);

/* CONTROL (task 8): zone config reads + narrow PID/model writes -- see
 * uart_task_ids.h for the scope cap versus /api/zones. No hardware handle
 * needed; everything routes through zones_http.c's public getters/setters. */
esp_err_t uart_bridge_start_control_task(uart_protocol_t *proto);

/* PROFILES (task 9): fire profile CRUD + execution control -- mirrors
 * profiles_http.c and dashboard_http.c's /api/profile_exec*. No hardware
 * handle needed; routes through profiles_http_get() and the
 * profile_executor_* API, both already safe to call with nothing attached. */
esp_err_t uart_bridge_start_profiles_task(uart_protocol_t *proto);

/* AUTOTUNE (task 10): mirrors dashboard_http.c's /api/autotune*, minus the
 * bulk matrix/CSV endpoints (see uart_task_ids.h for why those stay
 * HTTP-only). Routes through the autotune_engine_* API. */
esp_err_t uart_bridge_start_autotune_task(uart_protocol_t *proto);

/* WIFI (task 11): mirrors wifi_provision_http.c's status/scan/provision/
 * networks/forget surface, so Wi-Fi can be configured over a link that
 * works even with Wi-Fi down. Routes through wifi_prov.h; never touches
 * kiln_io/relay_authority/safety_link. */
esp_err_t uart_bridge_start_wifi_task(uart_protocol_t *proto);

/* --------------------------------------------------------------------------
 * PC link watchdog -- the implementation of
 * CONFIG_KILNCTL_SX1509_RELAYS_OFF_ON_LINK_LOSS.
 *
 * WHAT "THE LINK WENT AWAY" MEANS HERE, in wall-clock terms:
 *
 *   The link is *alive* for UART_BRIDGE_LINK_TIMEOUT_MS after the last moment
 *   either (a) a frame from the host was delivered to any bridge task, or
 *   (b) a frame this firmware sent to the host was ACKed by it. (b) matters as
 *   much as (a): a host that has switched on an element and is now only
 *   watching auto-reports sends no commands of its own, but its ACKs still
 *   prove the cable, the USB bridge, the driver and the application are all
 *   still there. Once that window lapses the link is *lost*, and stays lost
 *   until the next frame or ACK.
 *
 *   This is a heartbeat requirement on the host, and deliberately so. Silence
 *   is indistinguishable from a pulled cable, a crashed GUI or a wedged USB
 *   stack, and on a kiln all four have the same correct answer. A host that
 *   wants relays to stay energized must keep talking -- any command, or simply
 *   an enabled auto-report whose pushes it ACKs -- at least once per window.
 *
 * WHAT HAPPENS ON LOSS, in this order:
 *   1. Every relay NOT claimed by an on-board owner is dropped, and the drop
 *      is retried on every check tick for as long as the link stays down and
 *      the commanded state is not already off -- one failed I2C transfer must
 *      not be what leaves an element energized.
 *
 *      "Not claimed by an on-board owner" means relay_authority says the relay
 *      is NONE or MANUAL. This qualification was added 2026-08-27 and is not
 *      cosmetic. This watchdog predates the board running firings by itself,
 *      when the serial host was the only thing that could energize a relay and
 *      so "the host went quiet" really did mean "nobody is in control".
 *      profile_executor, autotune_engine and danger mode changed that, and
 *      because the link counts as lost until a host has EVER spoken, an
 *      unqualified drop meant a standalone board -- no PC attached, the normal
 *      deployment -- force-opened all four relays every 250 ms forever, so a
 *      firing could not hold a relay closed for one check tick. A relay under
 *      a PROFILE/RULE/AUTOTUNE owner is left alone: the serial link's health
 *      says nothing about that owner, which has its own watchdogs (guard 9,
 *      the safety-link 30 s silence abort) that do apply to it. Danger mode
 *      suppresses the drop entirely for its window, since an operator is
 *      deliberately holding relays closed from the browser with no serial
 *      traffic at all.
 *   2. SAFETY_FAULT_SRC_PC_LINK is raised on the isolated fault line, telling
 *      the RP2040 safety processor that the main controller is no longer under
 *      control. That line is a wire, not a message, so it keeps working when
 *      everything else here has failed.
 * On recovery the fault source is cleared. The relays are NOT restored: they
 * come back only when the host explicitly commands them, because "what was on
 * before the link died" is not a state anything should resume into by itself.
 *
 * BEFORE THE HOST HAS EVER SPOKEN the link counts as lost, so a board that
 * boots with nothing attached sits with the fault line asserted and its
 * unowned relays off. That is the same state as a link that died, which is the
 * point: the fault line reflects "this controller is not being controlled",
 * and at boot it is not. Note this is the permanent resting state of a
 * standalone board, which is precisely why item 1 above must not extend to
 * relays a firing owns.
 *
 * Pass io = NULL if the expander never came up (nothing to drop, but the fault
 * line still gets asserted -- see app_main) or link = NULL if the safety link
 * failed to start; at least one must be non-NULL. */
#define UART_BRIDGE_LINK_TIMEOUT_MS 5000u
#define UART_BRIDGE_LINK_CHECK_MS    250u

esp_err_t uart_bridge_start_link_watchdog(kiln_io_t *io, SafetyLinkClass *link);

#ifdef __cplusplus
}
#endif

#endif // UART_BRIDGE_H
