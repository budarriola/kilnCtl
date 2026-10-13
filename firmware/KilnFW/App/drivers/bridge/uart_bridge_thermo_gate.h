// uart_bridge_thermo_gate.h -- system-mode gate for the THERMO (task 1) UART writers.
//
// Review 12 Part B: THERMO_CMD_CONFIG_CHANNEL / SET_THRESHOLDS / SET_CJ_OFFSET /
// CLEAR_FAULTS / WRITE_REG reach thermo_owner_command_*() and rewrite MAX31856
// registers (thermocouple type, fault thresholds, CJ offset, raw registers) with
// no mode gate, while the matching HTTP zone write is refused during a run. Same
// owner decision Q2 as uart_bridge_ext_control.c (docs/SYSTEM_MODE_GATE.md): refuse
// while a profile or autotune is RUNNING or PAUSED. Read-only subcommands (ONE_SHOT,
// READ, READ_FAULTS, SET_AUTO_REPORT, READ_REG) are never gated. Header-only so a
// host test can reach it without the task loop.
#ifndef UART_BRIDGE_THERMO_GATE_H
#define UART_BRIDGE_THERMO_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "relay_authority.h"
#include "system_mode_gate.h"
#include "uart_task_ids.h"

static inline bool thermo_bridge_subcmd_is_write(uint8_t subcmd)
{
    switch (subcmd) {
        case THERMO_CMD_CONFIG_CHANNEL:
        case THERMO_CMD_SET_THRESHOLDS:
        case THERMO_CMD_SET_CJ_OFFSET:
        case THERMO_CMD_CLEAR_FAULTS:
        case THERMO_CMD_WRITE_REG:
            return true;
        default:
            return false;
    }
}

/* true (refused) with `reason` filled, same shape as system_mode_gate_check(). */
static inline bool thermo_bridge_write_refused(uint8_t subcmd, char *reason, size_t reason_cap)
{
    if (!thermo_bridge_subcmd_is_write(subcmd)) {
        return false;
    }
    sys_mode_snapshot_t snap = { 0 };
    relay_authority_heat_run_active(&snap.profile_running, &snap.autotune_running);
    return system_mode_gate_check(SYS_ACTION_WRITE_ZONES_CONFIG, &snap, reason, reason_cap);
}

#endif
