/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef MAIN_INTERNAL_H
#define MAIN_INTERNAL_H

/* ROADMAP.md M15 1500-line item: main.c (1859 lines) split by boot phase,
 * 2026-09-04. app_main() itself stays in main.c and calls each phase in the
 * SAME ORDER it used to inline them -- order is semantics in this file (see
 * main.c's own top-of-file comment). This header carries:
 *   - main_boot_ctx_t: every piece of state a later phase needs that an
 *     earlier phase produced. It replaces what used to be a flat stack of
 *     `static` locals inside app_main() -- same lifetime (one instance,
 *     zero-initialized, lives for the process), just addressed through one
 *     struct instead of a dozen separate symbols, so each phase function can
 *     take a single pointer instead of an ever-growing argument list.
 *   - the two helpers every phase can call: main_heap_stage() (the boot-time
 *     internal-DRAM probe -- see its own doc comment in main.c) and
 *     main_kiln_enter_safe_state() (drop relays + assert the isolated fault
 *     line on any path that stops short of a working PC link).
 *   - one entry point per phase, called from app_main() in main.c in this
 *     exact order.
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/* SX1509Class is defined in plain SX1509.h -- SX1509_internal.h's write/
 * config API is owner-only (behind #define SX1509_OWNER_BUILD, see that
 * header's top comment) and this header is included everywhere, not just by
 * the one file that does the bring-up, so it must not pull in the gated
 * header itself. main_boot_early.c defines SX1509_OWNER_BUILD and includes
 * SX1509_internal.h directly for the calls it actually needs. */
#include "SX1509.h"
#include "kiln_io.h"
#include "MAX31856.h"
#include "panel_spi.h"
#include "touch_dev.h"
#include "screen_idle.h"
#include "backlight_pwm.h"
#include "safety_link.h"
#include "uart_owner.h"
#include "uart_protocol.h"

/* Was `static const char *TAG = "app_main";` -- every phase file logs under
 * the same tag main.c always used, so it is now shared rather than
 * duplicated (and renamed off the generic `TAG` per the split's symbol-audit
 * convention, since it is now widened to non-static). */
extern const char *MAIN_TAG;

typedef struct {
    /* --- early/board peripherals (main_boot_early.c) -------------------- */
    i2c_master_bus_handle_t i2c_bus;

    SX1509Class expander;
    kiln_io_t   kio;
    bool        io_ready;

    esp_err_t spi_err;

    /* Accumulated across bring-up, applied to the isolated fault line once
     * the safety link exists (main_control_bringup.c). */
    uint32_t boot_fault_sources;

    MAX31856BusClass thermo_bus;
    MAX31856Class     thermo_ch[MAX31856_CHANNEL_COUNT];

    /* --- display+touch (also main_boot_early.c) -------------------------- */
    ILI9488Class display;
    bool         display_ready;

    touch_dev_t touch_dev;
    bool        touch_ready;

    screen_idle_t screen_idle;
    bool          screen_idle_ready;

    backlight_pwm_t backlight;

    /* --- safety+IO / control subsystems (main_control_bringup.c) -------- */
    SafetyLinkClass safety;
    esp_err_t        safety_err;

    bool recovery_mode;

    esp_err_t exec_err;
    esp_err_t autotune_err;

    /* --- network+HTTP / PC link (main_network_http.c) -------------------- */
    esp_err_t dash_err;

    uart_owner_t    uart_owner;
    uart_protocol_t uart_proto;
    bool            pc_link_ready;
} main_boot_ctx_t;

/* Boot-stage internal-DRAM probe -- see main.c for the full doc comment.
 * Was `static void heap_stage(const char *stage)`; now shared by every phase
 * file, so it moved to main.c (which still owns its `static size_t
 * s_prev_largest` running state) and is exported here under the
 * `main_` prefix required by the split's symbol-audit convention. */
void main_heap_stage(const char *stage);

/* The state app_main must leave the board in on any path that stops short of
 * a working PC link -- see main.c for the full doc comment. Was `static void
 * kiln_enter_safe_state(...)`; shared by main_network_http.c and
 * main_bridges_bringup.c, so it now lives in main.c and is exported here. */
void main_kiln_enter_safe_state(kiln_io_t *io, SafetyLinkClass *safety, bool safety_ok,
                                 uint32_t fault_sources, const char *why);

/* One entry point per boot phase, called from app_main() in main.c in this
 * exact order -- see main.c's top-of-file comment on why order is load-
 * bearing here. Each function ends on the same heap_stage() checkpoint the
 * monolithic app_main() used to log at that point, so grepping the boot log
 * for "heap stage" still shows the identical sequence of stage names. */
void main_boot_early(main_boot_ctx_t *ctx);         /* entry .. "display+touch" */
void main_control_bringup(main_boot_ctx_t *ctx);    /* .. "executor+autotune" */
void main_network_http_bringup(main_boot_ctx_t *ctx); /* .. "uart_owner+proto" */
void main_bridges_bringup(main_boot_ctx_t *ctx);    /* .. "app_main_done" */

#endif /* MAIN_INTERNAL_H */
