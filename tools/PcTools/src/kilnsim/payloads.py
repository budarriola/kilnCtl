"""Per-command-group payload encode/decode, per
``firmware/SimFW/docs/PROTOCOL.md`` sections 4-6 -- the byte layouts layered
on top of ``benchproto``'s envelope (``kilnsim.benchproto_codec``).

BENCHPROTO.md section 3's convention applies throughout: header fields are
big-endian (handled entirely by ``benchproto_codec``), but multi-byte fields
*inside* a command payload are little-endian. Every request payload's byte 0
is the command id (PROTOCOL.md sec 2: "A command's request payload is
[cmd_id, args...]"); every reply payload's byte 0 is the
``SIMFW_CMD_STATUS_*`` status byte (PROTOCOL.md sec 2's "Reply convention"),
a SimFW-level layer distinct from benchproto's own ACK/NACK.

This module is dict-in/dict-out at its public boundary (``encode_request`` /
``decode_reply``) so :mod:`kilnsim.link`'s ``SimLink.send_command(group, cmd,
payload: dict) -> dict`` surface -- the stable contract every other kilnsim
module (cli.py, gui.py, mcp_server.py, scenario.py) already codes against --
does not need to change shape for the wire-protocol lift. Only
``SerialSimLink`` (and ``MockSimLink``'s canned responses, for shape parity)
calls into this module.

Only command ids PROTOCOL.md documents as **implemented** are given real
codecs here; reserved/stub ids with no firmware handler to round-trip
against yet -- SYS RESET_SIM/SET_TIMESCALE/SET_SEED (PROTOCOL.md sec 4.3) --
still get a well-formed *request* encoding (so the round trip is exercisable
against real firmware and comes back ERR_NOT_IMPL, not a decode error) but
raise ``PayloadError`` for any command id PROTOCOL.md does not document at
all, e.g. RELAY_SET_CONTACT_FAULT (sec 5.4: "not allocated" -- no
`SIMFW_CMD_RELAY_*` id exists for it). TC_GET_MASTER_CONFIG (sec 5.2) *is*
now implemented here -- PROTOCOL.md was updated after this module's first
pass to note the firmware handler landed; see the TC group section below.
"""

from __future__ import annotations

import struct
from typing import Optional

from . import fault_catalog
from .protocol import CommandGroup, SYS_REBOOT_BOOTLOADER_MAGIC, TaskStatsId

STATUS_OK = 0x00
STATUS_ERR_NOT_IMPL = 0x01
STATUS_ERR_BAD_ARGS = 0x02
STATUS_ERR_INTERNAL = 0x03
STATUS_ERR_BUSY = 0x04
# The fixture's args were valid, but it has no reading to answer with yet --
# e.g. IO/READ against a pin whose expander has never ACKed a scan (no
# MCP23017 attached to J20), or no scan tick has run since boot. Distinct
# from ERR_BUSY (transient -- retry is likely to succeed) and ERR_BAD_ARGS
# (the request itself is wrong -- retrying never helps). Mirrors
# firmware/SimFW/src/tasks/cmd_ids.h's SIMFW_CMD_STATUS_ERR_NO_SAMPLE.
STATUS_ERR_NO_SAMPLE = 0x05

_STATUS_NAMES = {
    STATUS_OK: "OK",
    STATUS_ERR_NOT_IMPL: "ERR_NOT_IMPL",
    STATUS_ERR_BAD_ARGS: "ERR_BAD_ARGS",
    STATUS_ERR_INTERNAL: "ERR_INTERNAL",
    STATUS_ERR_BUSY: "ERR_BUSY",
    STATUS_ERR_NO_SAMPLE: "ERR_NO_SAMPLE",
}


class PayloadError(ValueError):
    pass


class CommandStatusError(RuntimeError):
    """Raised when a reply's status byte is not STATUS_OK."""

    def __init__(self, status: int, group, cmd: int):
        self.status = status
        self.group = group
        self.cmd = cmd
        name = _STATUS_NAMES.get(status, f"0x{status:02X}")
        super().__init__(f"{group.name}/{cmd}: {name}")


def _u8(v) -> int:
    return int(v) & 0xFF


def _bool_byte(v) -> int:
    return 1 if v else 0


# ===========================================================================
# SYS group (PROTOCOL.md sec 4)
# ===========================================================================
def _sys_encode(cmd: int, payload: dict) -> bytes:
    if cmd == 1:  # PING
        return bytes([cmd])
    if cmd == 2:  # GET_VERSION
        return bytes([cmd])
    if cmd == 6:  # GET_CAPS
        return bytes([cmd])
    # RESET_SIM(3)/SET_TIMESCALE(4)/SET_SEED(5): PROTOCOL.md sec 4 -- ids
    # allocated, "no handler yet ... a request today gets
    # [SIMFW_CMD_STATUS_ERR_NOT_IMPL] back" against REAL firmware.
    # `virtual_simfw` (firmware/SimFW/tools/virtual_simfw/) DOES implement
    # all three for real (a documented virtual-device-only extension -- see
    # that tool's README.md), so their args are now encoded for real rather
    # than dropped: RESET_SIM [u8 keep_params], SET_TIMESCALE
    # [u32 timescale_x100 LE], SET_SEED [u32 value]. This used to be a bare
    # `bytes([cmd])` for all three, silently dropping
    # `payload["value"]`/`payload["keep_params"]` entirely -- a real bug,
    # since run_test_scenario()/cmd_run() both call SET_SEED/SET_TIMESCALE
    # expecting the value to actually reach the device. The round trip
    # against real (unmodified) firmware still comes back ERR_NOT_IMPL, not
    # a decode error, since real cmd_task.c's stub path never reads past its
    # own dispatch-table lookup on byte0.
    #
    # SET_TIMESCALE's wire shape (PROTOCOL.md sec 4 / real firmware's
    # cmd_task.c's `handle_sys_set_timescale()`) is `u32 timescale_x100 LE`
    # (DESIGN_NOTES.md 4.2/5.2's x100 fixed point: 1000 == 10.00x, 100 == 1.00x),
    # NOT a raw f32 -- this module previously encoded a bare f32 here,
    # matching `virtual_simfw.c`'s (also wrong) decoder rather than
    # PROTOCOL.md/real firmware, which would silently misbehave against
    # actual hardware (a raw IEEE-754 f32 bit pattern reinterpreted as a
    # u32 fixed-point value is nowhere near the intended timescale).
    # `payload["value"]` is the caller-facing multiplier (1.0 == real time,
    # matching GET_SIM_STATE/TELEMETRY's own `timescale_x100 / 100.0`
    # decode), converted to the wire's x100 fixed point here.
    if cmd == 3:
        return bytes([cmd, 1 if payload.get("keep_params") else 0])
    if cmd == 4:
        timescale_x100 = int(round(float(payload.get("value", 1.0)) * 100.0))
        return bytes([cmd]) + struct.pack("<I", timescale_x100 & 0xFFFFFFFF)
    if cmd == 5:
        return bytes([cmd]) + struct.pack("<I", int(payload.get("value", 0)) & 0xFFFFFFFF)
    if cmd == 7:  # GET_SIM_STATE (PROTOCOL.md sec 4) -- no args
        return bytes([cmd])
    if cmd == 8:  # REBOOT_BOOTLOADER (PROTOCOL.md sec 4): {u32 confirm LE}
        # Always the real magic unless a caller explicitly passes a wrong
        # one -- kilnsim's own selftest/negative-path tests are the only
        # legitimate reason to ever override this (proving the firmware
        # actually rejects a bad confirm value), so the override is an
        # explicit, named payload key, not a positional footgun.
        confirm = int(payload.get("confirm", SYS_REBOOT_BOOTLOADER_MAGIC)) & 0xFFFFFFFF
        return bytes([cmd]) + struct.pack("<I", confirm)
    if cmd == 9:  # SESSION_RESET (PROTOCOL.md sec 4): LINK state only, no args.
        return bytes([cmd])
    if cmd == 10:  # GET_TASK_STATS (PROTOCOL.md sec 4) -- no args
        return bytes([cmd])
    raise PayloadError(f"SYS: unknown command id {cmd}")


def _decode_version_block(data: bytes, off: int) -> tuple[dict, int]:
    protocol_version, min_compatible = struct.unpack_from("<HH", data, off)
    off += 4
    major, minor, patch, dirty, hash_len = struct.unpack_from("<BBBBB", data, off)
    off += 5
    git_hash = data[off : off + hash_len].decode("ascii", errors="replace")
    off += hash_len
    return (
        {
            "protocol_version": protocol_version,
            "min_compatible": min_compatible,
            "fw_version": f"{major}.{minor}.{patch}",
            "fw_version_major": major,
            "fw_version_minor": minor,
            "fw_version_patch": patch,
            "fw_git_dirty": bool(dirty),
            "fw_git_hash": git_hash,
        },
        off,
    )


def _sys_decode(cmd: int, status: int, data: bytes) -> dict:
    if cmd == 1:  # PING
        return {"pong": True}
    if cmd == 2:  # GET_VERSION
        block, _ = _decode_version_block(data, 0)
        return block
    if cmd == 6:  # GET_CAPS
        block, off = _decode_version_block(data, 0)
        (
            zone_count_min,
            zone_count_max,
            zone_count_default,
            tc_main_channels,
            tc_safety_channels,
            ct_channels,
            relay_channels,
        ) = struct.unpack_from("<BBBBBBB", data, off)
        off += 7
        (feature_bitmask,) = struct.unpack_from("<I", data, off)
        off += 4
        block.update(
            {
                "zone_count_min": zone_count_min,
                "zone_count_max": zone_count_max,
                "zone_count_default": zone_count_default,
                "tc_channel_count": tc_main_channels,
                "tc_main_channels": tc_main_channels,
                "tc_safety_channels": tc_safety_channels,
                "ct_channel_count": ct_channels,
                "relay_count": relay_channels,
                "feature_bitmask": feature_bitmask,
            }
        )
        return block
    if cmd in (3, 4, 5):
        return {}
    if cmd == 7:  # GET_SIM_STATE: [status, u32 seed, u8 snapshot_valid, u32 timescale_x100, u64 sim_time_us]
        seed, snapshot_valid, timescale_x100 = struct.unpack_from("<IBI", data, 0)
        (sim_time_us,) = struct.unpack_from("<Q", data, 9)
        return {
            "seed": seed,
            "snapshot_valid": bool(snapshot_valid),
            "timescale": timescale_x100 / 100.0,
            "sim_time_us": sim_time_us,
        }
    if cmd == 8:  # REBOOT_BOOTLOADER: {status} only -- see this module's own
        # docstring and cmd_ids.h's comment on SIMFW_CMD_SYS_REBOOT_BOOTLOADER:
        # a reply only ever arrives on the refusal path (bad magic, or the
        # safe-state confirmation timed out) -- the success path never sends
        # one at all, since the firmware jumps into the ROM bootloader before
        # it can ACK. decode_reply()'s caller (link.py) still runs this
        # decoder normally for whatever DOES arrive; there is nothing beyond
        # the shared status byte to decode.
        return {}
    if cmd == 9:  # SESSION_RESET: {status} only -- see this module's own
        # docstring and protocol.py's SysCmd.SESSION_RESET comment. Always
        # STATUS_OK on real firmware (no failure mode of its own); nothing
        # beyond the shared status byte to decode.
        return {}
    if cmd == 10:  # GET_TASK_STATS (PROTOCOL.md sec 4): [status, u8 task_count,
        # task_count * {u8 task_stats_id, u16 allocated_bytes LE, u16 hwm_free_bytes LE}].
        # `data` here has already had the shared status byte stripped by
        # decode_reply() -- byte 0 below is task_count, not status.
        task_count = data[0]
        tasks = []
        off = 1
        for _ in range(task_count):
            task_stats_id, allocated_bytes, hwm_free_bytes = struct.unpack_from("<BHH", data, off)
            off += 5
            try:
                id_name = TaskStatsId(task_stats_id).name
            except ValueError:
                id_name = f"UNKNOWN_{task_stats_id}"
            # peak_used_bytes/margin are derived here, not on the wire
            # (PROTOCOL.md sec 4: "the firmware reports the two raw
            # numbers, never a derived ratio") -- allocated_bytes is always
            # > 0 for a real FreeRTOS task (xTaskCreate() rejects a 0-depth
            # stack), so this division is safe without a guard.
            peak_used_bytes = allocated_bytes - hwm_free_bytes
            margin = (allocated_bytes / peak_used_bytes) if peak_used_bytes > 0 else float("inf")
            tasks.append(
                {
                    "task_stats_id": task_stats_id,
                    "task_stats_id_name": id_name,
                    "allocated_bytes": allocated_bytes,
                    "hwm_free_bytes": hwm_free_bytes,
                    "peak_used_bytes": peak_used_bytes,
                    "margin": margin,
                }
            )
        return {"task_count": task_count, "tasks": tasks}
    raise PayloadError(f"SYS: unknown command id {cmd}")


# ===========================================================================
# MODEL group (PROTOCOL.md sec 5.1)
# ===========================================================================
_ZONE_PARAMS_FMT = "<f f ffff f f f f"  # C, k_loss, k_couple[4], R_element, element_health, tc_lag_s, T0


def _pack_zone_params(p: dict) -> bytes:
    k_couple = list(p.get("k_couple", [0.0, 0.0, 0.0, 0.0]))
    k_couple = (k_couple + [0.0, 0.0, 0.0, 0.0])[:4]
    return struct.pack(
        _ZONE_PARAMS_FMT,
        float(p["C"]),
        float(p["k_loss"]),
        *[float(x) for x in k_couple],
        float(p["R_element"]),
        float(p["element_health"]),
        float(p["tc_lag_s"]),
        float(p["T0"]),
    )


def _unpack_zone_params(data: bytes, off: int) -> tuple[dict, int]:
    size = struct.calcsize(_ZONE_PARAMS_FMT)
    vals = struct.unpack_from(_ZONE_PARAMS_FMT, data, off)
    C, k_loss, k0, k1, k2, k3, R_element, element_health, tc_lag_s, T0 = vals
    return (
        {
            "C": C,
            "k_loss": k_loss,
            "k_couple": [k0, k1, k2, k3],
            "R_element": R_element,
            "element_health": element_health,
            "tc_lag_s": tc_lag_s,
            "T0": T0,
        },
        off + size,
    )


def _model_encode(cmd: int, payload: dict) -> bytes:
    if cmd == 1:  # SET_ZONE_PARAMS
        return bytes([cmd, _u8(payload["zone"])]) + _pack_zone_params(payload["params"])
    if cmd == 2:  # GET_ZONE_PARAMS
        return bytes([cmd, _u8(payload["zone"])])
    if cmd == 3:  # SET_AMBIENT
        return bytes([cmd]) + struct.pack("<f", float(payload["ambient_c"]))
    if cmd == 4:  # LOAD_PRESET
        preset = payload.get("preset", payload.get("name"))
        preset_id = preset if isinstance(preset, int) else _PRESET_NAME_TO_ID[str(preset)]
        return bytes([cmd, _u8(preset_id)])
    if cmd == 5:  # SET_TEMP
        mode = 1 if payload.get("mode") in (1, "manual", "MANUAL") else 0
        return bytes([cmd, _u8(payload["zone"]), mode]) + struct.pack(
            "<f", float(payload.get("temp_c", 0.0))
        )
    if cmd == 6:  # SET_TC_LAG
        return bytes([cmd, _u8(payload["zone"])]) + struct.pack("<f", float(payload["tc_lag_s"]))
    raise PayloadError(f"MODEL: unknown command id {cmd}")


_PRESET_NAME_TO_ID = {"fast_test": 0, "small_kiln": 1, "three_zone": 2, "stress": 3}
_PRESET_ID_TO_NAME = {v: k for k, v in _PRESET_NAME_TO_ID.items()}


def _model_decode(cmd: int, status: int, data: bytes) -> dict:
    if cmd == 1:  # SET_ZONE_PARAMS
        return {}
    if cmd == 2:  # GET_ZONE_PARAMS
        zone = data[0]
        params, _ = _unpack_zone_params(data, 1)
        return {"zone": zone, "params": params}
    if cmd in (3, 4, 5, 6):
        return {}
    raise PayloadError(f"MODEL: unknown command id {cmd}")


# ===========================================================================
# TC group (PROTOCOL.md sec 5.2)
# ===========================================================================
_TC_REGS_FMT = "<16s ff B ff III"  # regs[16], shadow_true_c, shadow_reported_c, dead_mode, noise_sigma, ber, txn, proto_err, underrun


def _tc_encode(cmd: int, payload: dict) -> bytes:
    if cmd == 1:  # GET_REGS
        return bytes([cmd, _u8(payload["channel"])])
    if cmd == 2:  # FORCE_TEMP
        return bytes([cmd, _u8(payload["channel"])]) + struct.pack("<f", float(payload["temp_c"]))
    if cmd == 3:  # SET_MODE
        mode = 1 if payload.get("mode") in (1, "manual", "MANUAL") else 0
        return bytes([cmd, _u8(payload["channel"]), mode]) + struct.pack(
            "<f", float(payload.get("manual_temp_c", payload.get("temp_c", 0.0)))
        )
    if cmd == 4:  # INJECT_FAULT
        raw_kind = payload.get("fault_kind", payload.get("fault_type", 0))
        # fault_kind is fault_sched_fault_type_t's TC-only subset (0..8,
        # PROTOCOL.md sec 5.2) -- same numbering fault_catalog.py uses for
        # its TC-kind entries, so the same name table applies here too.
        kind_id = fault_catalog.fault_type_to_id(raw_kind) if isinstance(raw_kind, str) else int(raw_kind)
        return struct.pack(
            "<BHBBf",
            cmd,
            int(payload["fault_slot"]) if "fault_slot" in payload else int(payload.get("slot_id", 0)),
            _u8(payload["channel"]),
            _u8(kind_id),
            float(payload.get("param0", 0.0)),
        )
    if cmd == 5:  # CLEAR_FAULT
        slot = int(payload.get("fault_slot", payload.get("slot_id", 0)))
        return struct.pack("<BH", cmd, slot)
    if cmd == 6:  # GET_MASTER_CONFIG
        return bytes([cmd, _u8(payload["channel"])])
    raise PayloadError(f"TC: unknown/unimplemented command id {cmd}")


def _tc_decode(cmd: int, status: int, data: bytes) -> dict:
    if cmd == 1:  # GET_REGS
        channel, flags = data[0], data[1]
        regs = data[2:18]
        shadow_true_c, shadow_reported_c = struct.unpack_from("<ff", data, 18)
        dead_mode = data[26]
        noise_sigma_c, bit_error_rate = struct.unpack_from("<ff", data, 27)
        transactions, protocol_errors, first_byte_late = struct.unpack_from("<III", data, 35)
        return {
            "channel": channel,
            "flags": flags,
            "reg_image_valid": bool(flags & 0x01),
            "snapshot_valid": bool(flags & 0x02),
            "regs": regs,
            "shadow_temp_c": shadow_true_c,
            "shadow_reported_c": shadow_reported_c,
            "dead_mode": dead_mode,
            "noise_sigma_c": noise_sigma_c,
            "bit_error_rate": bit_error_rate,
            "spi_transactions": transactions,
            "spi_protocol_errors": protocol_errors,
            "spi_first_byte_late": first_byte_late,
        }
    if cmd in (2, 3):  # FORCE_TEMP / SET_MODE
        return {}
    if cmd == 4:  # INJECT_FAULT
        (slot_id,) = struct.unpack_from("<H", data, 0)
        return {"fault_slot": slot_id}
    if cmd == 5:  # CLEAR_FAULT
        return {}
    if cmd == 6:  # GET_MASTER_CONFIG (PROTOCOL.md sec 5.2)
        channel, flags = data[0], data[1]
        cr0, cr1, mask = data[2], data[3], data[4]
        return {
            "channel": channel,
            "flags": flags,
            # bit0: has the master EVER written this channel (monotonic,
            # trustworthy regardless of bit1). bit1: same busy/retry
            # semantics as GET_REGS's reg_image_valid -- when clear, cr0/
            # cr1/mask below are 0, not a real register read.
            "configured": bool(flags & 0x01),
            "reg_image_valid": bool(flags & 0x02),
            "cr0": cr0,
            "cr1": cr1,
            "mask": mask,
        }
    raise PayloadError(f"TC: unknown/unimplemented command id {cmd}")


# ===========================================================================
# CT group (PROTOCOL.md sec 5.3)
# ===========================================================================
def _ct_encode(cmd: int, payload: dict) -> bytes:
    if cmd == 1:  # SET_MODE
        mode = 1 if payload.get("mode") in (1, "manual", "MANUAL") else 0
        return bytes([cmd, _u8(payload["channel"]), mode])
    if cmd == 2:  # SET_AMPS
        return bytes([cmd, _u8(payload["channel"])]) + struct.pack("<f", float(payload["amps"]))
    if cmd == 3:  # SET_DISTORTION
        d = payload.get("distortion", payload)
        return bytes([cmd, _u8(payload["channel"])]) + struct.pack(
            "<ffBBB",
            float(d.get("dc_offset", 0.0)),
            float(d.get("clip_fraction", 0.0)),
            _bool_byte(d.get("dropout_half_cycle", False)),
            _bool_byte(d.get("dropout_negative_half", False)),
            _bool_byte(d.get("apply_immediately", True)),
        )
    if cmd == 4:  # GET_STATE
        return bytes([cmd, _u8(payload["channel"])])
    if cmd == 5:  # SET_PHASE
        return bytes([cmd, _u8(payload["channel"])]) + struct.pack("<f", float(payload["phase_deg"]))
    raise PayloadError(f"CT: unknown command id {cmd}")


def _ct_decode(cmd: int, status: int, data: bytes) -> dict:
    if cmd in (1, 2, 3, 5):
        return {}
    if cmd == 4:  # GET_STATE
        mode = data[0]
        amps, phase_deg, dc_offset, clip_fraction = struct.unpack_from("<ffff", data, 1)
        dropout_half_cycle = data[17]
        dropout_negative_half = data[18]
        apply_immediately = data[19]
        last_pwm_scale = struct.unpack_from("<f", data, 20)[0]
        valid = data[24]
        return {
            "mode": mode,
            "amps": amps,
            "phase_deg": phase_deg,
            "distortion": {
                "dc_offset": dc_offset,
                "clip_fraction": clip_fraction,
                "dropout_half_cycle": bool(dropout_half_cycle),
                "dropout_negative_half": bool(dropout_negative_half),
                "apply_immediately": bool(apply_immediately),
            },
            "last_pwm_scale": last_pwm_scale,
            "valid": bool(valid),
        }
    raise PayloadError(f"CT: unknown command id {cmd}")


# ===========================================================================
# RELAY group (PROTOCOL.md sec 5.4)
# ===========================================================================
_RELAY_SIGNAL_NAMES = {0: "K1", 1: "K2", 2: "K3", 3: "K5", 4: "K4", 5: "FAULT_LINE"}


def _relay_encode(cmd: int, payload: dict) -> bytes:
    if cmd == 1:  # GET_STATES
        return bytes([cmd])
    if cmd == 2:  # GET_EDGES
        since_seq = int(payload.get("since_seq", 0))
        max_count = _u8(payload.get("max_count", 8))
        return struct.pack("<BIB", cmd, since_seq, max_count)
    raise PayloadError(f"RELAY: unknown/unimplemented command id {cmd}")


def _relay_decode(cmd: int, status: int, data: bytes) -> dict:
    if cmd == 1:  # GET_STATES
        (
            k1,
            k2,
            k3,
            k5,
            k4,
            fault_line_asserted,
        ) = data[0:6]
        (sample_time_us,) = struct.unpack_from("<Q", data, 6)
        valid = data[14]
        return {
            "k1_closed": bool(k1),
            "k2_closed": bool(k2),
            "k3_closed": bool(k3),
            "k5_closed": bool(k5),
            "k4_closed": bool(k4),
            "fault_line_asserted": bool(fault_line_asserted),
            "sample_time_us": sample_time_us,
            "valid": bool(valid),
        }
    if cmd == 2:  # GET_EDGES
        count = data[0]
        edges = []
        off = 1
        for _ in range(count):
            seq, signal, level = struct.unpack_from("<IBB", data, off)
            off += 6
            (time_us,) = struct.unpack_from("<Q", data, off)
            off += 8
            edges.append(
                {
                    "seq": seq,
                    "signal": _RELAY_SIGNAL_NAMES.get(signal, signal),
                    "level": level,
                    "sim_time_us": time_us,
                }
            )
        return {"returned_count": count, "edges": edges}
    raise PayloadError(f"RELAY: unknown/unimplemented command id {cmd}")


# ===========================================================================
# IO group (PROTOCOL.md sec 5.5)
# ===========================================================================
def _io_encode(cmd: int, payload: dict) -> bytes:
    if cmd == 1:  # SET_DIR
        return bytes(
            [
                cmd,
                _u8(payload.get("exp", 0)),
                _u8(payload["pin"]),
                _bool_byte(payload.get("is_input", payload.get("input", True))),
                _bool_byte(payload.get("pullup", False)),
            ]
        )
    if cmd == 2:  # WRITE
        return bytes([cmd, _u8(payload.get("exp", 0)), _u8(payload["pin"]), _bool_byte(payload["level"])])
    if cmd == 3:  # READ
        return bytes([cmd, _u8(payload.get("exp", 0)), _u8(payload["pin"])])
    if cmd == 4:  # ESTOP_SET
        return bytes([cmd, _bool_byte(payload["open"])])
    if cmd == 5:  # FAULT_LINE_GET
        return bytes([cmd])
    if cmd == 6:  # DUT_POWER_SET
        return bytes([cmd, _bool_byte(payload["on"])])
    if cmd == 7:  # ESTOP_GET
        return bytes([cmd])
    if cmd == 8:  # DUT_POWER_GET
        return bytes([cmd])
    if cmd == 9:  # DUT_POWER_SAFETY_SET
        return bytes([cmd, _bool_byte(payload["on"])])
    if cmd == 10:  # DUT_POWER_SAFETY_GET
        return bytes([cmd])
    if cmd == 11:  # BUS_SCAN -- no args, PROTOCOL.md sec 5.5
        return bytes([cmd])
    raise PayloadError(f"IO: unknown command id {cmd}")


#: 7-bit I2C address sweep range BUS_SCAN's found_bitmap covers -- must match
#: firmware/SimFW/src/tasks/i2c_owner.h's I2C_OWNER_BUS_SCAN_ADDR_MIN/MAX
#: exactly (bit n of the wire bitmap = address IO_BUS_SCAN_ADDR_MIN + n).
IO_BUS_SCAN_ADDR_MIN = 0x08
IO_BUS_SCAN_ADDR_MAX = 0x77
IO_BUS_SCAN_ADDR_COUNT = IO_BUS_SCAN_ADDR_MAX - IO_BUS_SCAN_ADDR_MIN + 1  # 112
IO_BUS_SCAN_BITMAP_BYTES = (IO_BUS_SCAN_ADDR_COUNT + 7) // 8  # 14


def _io_decode(cmd: int, status: int, data: bytes) -> dict:
    if cmd == 1:  # SET_DIR
        return {}
    if cmd == 2:  # WRITE
        return {}
    if cmd == 3:  # READ
        return {"level": bool(data[0])}
    if cmd == 4:  # ESTOP_SET
        return {}
    if cmd == 5:  # FAULT_LINE_GET
        asserted = data[0]
        (sample_time_us,) = struct.unpack_from("<Q", data, 1)
        valid = data[9]
        return {
            "asserted": bool(asserted),
            "sample_time_us": sample_time_us,
            "valid": bool(valid),
        }
    if cmd == 6:  # DUT_POWER_SET
        return {}
    if cmd == 7:  # ESTOP_GET
        return {"open": bool(data[0])}
    if cmd == 8:  # DUT_POWER_GET
        return {"on": bool(data[0])}
    if cmd == 9:  # DUT_POWER_SAFETY_SET
        return {}
    if cmd == 10:  # DUT_POWER_SAFETY_GET
        return {"on": bool(data[0])}
    if cmd == 11:  # BUS_SCAN -- [configured_addr1, configured_addr2, found_bitmap[14]]
        configured_addr1 = data[0]
        configured_addr2 = data[1]
        bitmap = data[2 : 2 + IO_BUS_SCAN_BITMAP_BYTES]
        found_addresses = [
            IO_BUS_SCAN_ADDR_MIN + n
            for n in range(IO_BUS_SCAN_ADDR_COUNT)
            if bitmap[n // 8] & (1 << (n % 8))
        ]
        found_set = set(found_addresses)
        # "match" is the whole point of this command (see cli.py's `io scan`
        # printing): both addresses this firmware is CURRENTLY configured to
        # use must have actually ACKed the scan. Extra, unexpected addresses
        # found on the bus do not by themselves count as a mismatch here --
        # only a configured address that failed to ACK does -- since a third
        # device sharing the bus is a separate (if noteworthy) condition from
        # "the firmware's own two expanders aren't where it thinks they are".
        match = configured_addr1 in found_set and configured_addr2 in found_set
        return {
            "configured_addr1": configured_addr1,
            "configured_addr2": configured_addr2,
            "found_addresses": found_addresses,
            "match": match,
        }
    raise PayloadError(f"IO: unknown command id {cmd}")


# ===========================================================================
# FAULT group (PROTOCOL.md sec 5.6)
# ===========================================================================
_TRIGGER_KIND_TO_ID = {
    "at_sim_time": 0,
    "at_zone_temp": 1,
    "on_relay_edge": 2,
    "on_event": 3,
    "after_fault": 4,
    "random_in": 5,
    "manual": 6,
}
_TRIGGER_ID_TO_KIND = {v: k for k, v in _TRIGGER_KIND_TO_ID.items()}
_DURATION_KIND_TO_ID = {"permanent": 0, "for": 1, "until_trigger": 2}
_REPEAT_KIND_TO_ID = {"once": 0, "every": 1, "n_times": 2}
_FAULT_SLOT_STATE_NAMES = {0: "idle", 1: "armed", 2: "active", 3: "expired"}


def _encode_trigger_fields(trigger: dict) -> bytes:
    """Byte-identical trigger encoding (PROTOCOL.md sec 5.6) shared by
    FAULT_SCHEDULE's own ARM trigger and FAULT_SET_UNTIL_TRIGGER's release
    trigger: `u8 trigger_kind, f64 trigger_a, f64 trigger_b, u16 trigger_ref,
    u8 trigger_edge, char[24] event_name` -- 44 bytes."""
    trigger_kind = trigger.get("kind", "manual")
    trigger_kind_id = trigger_kind if isinstance(trigger_kind, int) else _TRIGGER_KIND_TO_ID[str(trigger_kind)]
    trigger_a = float(
        trigger.get("t", trigger.get("temp_c", trigger.get("delay_s", trigger.get("t0", 0.0)))) or 0.0
    )
    trigger_b = float(trigger.get("t1", 0.0) or 0.0)
    _relay_bit = {"K1": 0, "K2": 1, "K3": 2, "K5": 3, "K4": 4}  # sim_snapshot.h's sim_relay_bit_t order
    if "relay" in trigger and isinstance(trigger["relay"], str):
        trigger_ref = _relay_bit.get(trigger["relay"].upper(), 0)
    else:
        trigger_ref = int(trigger.get("zone", trigger.get("relay", trigger.get("after_fault_slot", 0))) or 0)
    edge_raw = trigger.get("edge")
    trigger_edge = 1 if edge_raw in ("falling", "open", 1) else 0
    event_name = (trigger.get("event_name") or "").encode("ascii")[:24]
    event_name = event_name + b"\x00" * (24 - len(event_name))

    out = bytearray()
    out += struct.pack("<B", trigger_kind_id)
    out += struct.pack("<d", trigger_a)
    out += struct.pack("<d", trigger_b)
    out += struct.pack("<H", trigger_ref)
    out += struct.pack("<B", trigger_edge)
    out += event_name
    return bytes(out)


def _normalize_kind_field(value, kind_map: dict, default: str):
    """`duration` and `repeat` are documented as dicts ({"kind": ..., ...}),
    but the CLI (cli.py's `fault` subcommand) and the MCP server's
    `fault_schedule` tool both shipped passing a bare string naming the kind
    ("permanent" / "once") instead -- an AttributeError on every single
    `kilnsim fault` invocation, since `duration.get("kind", ...)` doesn't
    exist on a str. Nothing caught it because selftest.py is the only thing
    that exercised this encoder, and it always used the dict form. Accepting
    dict / bare-string-kind / int-kind / None (-> `default`) here means any
    future caller in any of these shapes is normalized in one place instead
    of re-breaking silently. Returns (kind_id, extra_fields_dict) where
    extra_fields_dict is the original dict (for pulling out "t"/"period"/etc)
    or {} when the caller only supplied a bare kind.
    """
    if value is None:
        value = default
    if isinstance(value, dict):
        kind = value.get("kind", default)
        extra = value
    else:
        kind = value
        extra = {}
    kind_id = kind if isinstance(kind, int) else kind_map[str(kind)]
    return kind_id, extra


def _encode_fault_schedule(payload: dict) -> bytes:
    slot_id = int(payload.get("fault_slot", payload.get("slot_id", 0)))
    # fault_type/target arrive as the scenario's own catalog strings (e.g.
    # "welded_ssr" / "relay:K1", DESIGN_NOTES.md sec 7.1/8.1) -- fault_catalog.py is
    # the name<->wire-numeric translation this module never had (a bare
    # `_u8(payload.get("fault_type", 0))` would TypeError on a string, or
    # silently default to 0/TC_DISCONNECTED for a missing one). Also accept
    # an already-numeric fault_type/target for callers (tests, sim_raw_command)
    # that want to bypass the catalog and address the wire directly.
    raw_type = payload.get("fault_type", 0)
    raw_target = payload.get("target", 0)
    if isinstance(raw_type, str):
        type_id = fault_catalog.fault_type_to_id(raw_type)
        target = fault_catalog.parse_target(type_id, raw_target) if isinstance(raw_target, str) else int(raw_target)
    else:
        type_id = int(raw_type)
        target = int(raw_target)
    fault_type = _u8(type_id)

    trigger = payload.get("trigger", {})
    trigger_fields = _encode_trigger_fields(trigger)

    # duration_kind == 2 (UNTIL_TRIGGER, PROTOCOL.md sec 5.6): this frame
    # parks fault_type/target/the ARM trigger/repeat/params firmware-side
    # without arming the slot; duration_for_s is ignored. The release
    # trigger is supplied separately via FAULT_SET_UNTIL_TRIGGER (cmd 0x05),
    # which performs the actual arm -- see that command's encoder below.
    duration_kind_id, duration = _normalize_kind_field(payload.get("duration"), _DURATION_KIND_TO_ID, "permanent")
    duration_for_s = float(duration.get("t", 0.0) or 0.0)

    repeat_kind_id, repeat = _normalize_kind_field(payload.get("repeat"), _REPEAT_KIND_TO_ID, "once")
    repeat_period_s = float(repeat.get("period", 0.0) or 0.0)
    repeat_jitter_s = float(repeat.get("jitter", 0.0) or 0.0)
    repeat_n = int(repeat.get("n", 0) or 0)

    params = list(payload.get("params", (0.0, 0.0, 0.0, 0.0)))
    params = (params + [0.0, 0.0, 0.0, 0.0])[:4]

    out = bytearray()
    out += struct.pack("<BHBH", 0x01, slot_id, fault_type, target)
    out += trigger_fields
    out += struct.pack("<B", duration_kind_id)
    out += struct.pack("<d", duration_for_s)
    out += struct.pack("<B", repeat_kind_id)
    out += struct.pack("<d", repeat_period_s)
    out += struct.pack("<d", repeat_jitter_s)
    out += struct.pack("<H", repeat_n)
    out += struct.pack("<ffff", *[float(x) for x in params])
    return bytes(out)


def _encode_fault_set_until_trigger(payload: dict) -> bytes:
    """FAULT_SET_UNTIL_TRIGGER (cmd 0x05, PROTOCOL.md sec 5.6): frame 2 of
    the UNTIL_TRIGGER two-frame design. `[0x05, u16 slot_id, <trigger
    encoding, byte-identical to FAULT_SCHEDULE's own ARM trigger>]`, 47
    bytes total incl. cmd_id."""
    slot_id = int(payload.get("fault_slot", payload.get("slot_id", 0)))
    trigger = payload.get("trigger", {})
    return struct.pack("<BH", 0x05, slot_id) + _encode_trigger_fields(trigger)


def _fault_encode(cmd: int, payload: dict) -> bytes:
    if cmd == 1:  # SCHEDULE
        return _encode_fault_schedule(payload)
    if cmd == 2:  # CANCEL
        slot = int(payload.get("fault_slot", payload.get("slot_id", 0)))
        return struct.pack("<BH", cmd, slot)
    if cmd == 3:  # LIST
        start_index = _u8(payload.get("start_index", 0))
        max_count = _u8(payload.get("max_count", 8))
        return bytes([cmd, start_index, max_count])
    if cmd == 4:  # FIRE_NOW
        slot = int(payload.get("fault_slot", payload.get("slot_id", 0)))
        return struct.pack("<BH", cmd, slot)
    if cmd == 5:  # SET_UNTIL_TRIGGER
        return _encode_fault_set_until_trigger(payload)
    raise PayloadError(f"FAULT: unknown command id {cmd}")


def _fault_decode(cmd: int, status: int, data: bytes) -> dict:
    if cmd == 1:  # SCHEDULE
        (slot_id,) = struct.unpack_from("<H", data, 0)
        return {"fault_slot": slot_id}
    if cmd == 2:  # CANCEL
        return {}
    if cmd == 5:  # SET_UNTIL_TRIGGER -- reply [status, u16 slot_id echo]
        (slot_id,) = struct.unpack_from("<H", data, 0)
        return {"fault_slot": slot_id}
    if cmd == 3:  # LIST
        count = data[0]
        entries = []
        off = 1
        for _ in range(count):
            slot_id, state, fault_type, target, fire_count = struct.unpack_from("<HBHHI", data, off)
            off += 11
            (active_since_s,) = struct.unpack_from("<f", data, off)
            off += 4
            entries.append(
                {
                    "fault_slot": slot_id,
                    "state": _FAULT_SLOT_STATE_NAMES.get(state, state),
                    "fault_type": fault_type,
                    "target": target,
                    "fire_count": fire_count,
                    "active_since_s": active_since_s,
                }
            )
        return {"returned_count": count, "faults": entries}
    if cmd == 4:  # FIRE_NOW
        return {}
    raise PayloadError(f"FAULT: unknown command id {cmd}")


# ===========================================================================
# Dispatch tables
# ===========================================================================
_ENCODERS = {
    CommandGroup.SYS: _sys_encode,
    CommandGroup.MODEL: _model_encode,
    CommandGroup.TC: _tc_encode,
    CommandGroup.CT: _ct_encode,
    CommandGroup.RELAY: _relay_encode,
    CommandGroup.IO: _io_encode,
    CommandGroup.FAULT: _fault_encode,
}
_DECODERS = {
    CommandGroup.SYS: _sys_decode,
    CommandGroup.MODEL: _model_decode,
    CommandGroup.TC: _tc_decode,
    CommandGroup.CT: _ct_decode,
    CommandGroup.RELAY: _relay_decode,
    CommandGroup.IO: _io_decode,
    CommandGroup.FAULT: _fault_decode,
}


def encode_request(group: CommandGroup, cmd: int, payload: Optional[dict] = None) -> bytes:
    """Returns the request payload bytes ([cmd_id, args...], PROTOCOL.md
    sec 2) for `group`/`cmd`. Raises PayloadError for an unknown command
    id, or KeyError/struct.error for a malformed `payload` dict (missing
    required field, wrong type)."""
    encoder = _ENCODERS.get(group)
    if encoder is None:
        raise PayloadError(f"{group.name}: not a request/reply group")
    return encoder(int(cmd), payload or {})


def decode_reply(group: CommandGroup, cmd: int, data: bytes) -> dict:
    """Decodes a reply payload (status byte + fields, PROTOCOL.md sec 2)
    into a dict. Raises CommandStatusError if the status byte isn't
    STATUS_OK -- the SimFW-level failure layer, distinct from benchproto's
    own ACK/NACK (which only says the frame was delivered)."""
    if not data:
        raise PayloadError(f"{group.name}/{cmd}: empty reply payload (missing status byte)")
    status = data[0]
    if status != STATUS_OK:
        raise CommandStatusError(status, group, cmd)
    decoder = _DECODERS.get(group)
    if decoder is None:
        raise PayloadError(f"{group.name}: not a request/reply group")
    return decoder(int(cmd), status, data[1:])


# ===========================================================================
# EVT group (PROTOCOL.md sec 6) -- unsolicited BROADCAST frames only, never
# a send_command() destination. telemetry.c sends two frame shapes from the
# same SIMFW_TASK_ID_EVT source, disambiguated by payload byte 0
# (SIMFW_EVT_FRAME_KIND_*).
# ===========================================================================
EVT_FRAME_KIND_TELEMETRY = 0x01
EVT_FRAME_KIND_EVENT = 0x02

_ZONE_TELEMETRY_FMT = "<ffff"  # T_true_c, T_tc_reported_c, T_safety_reported_c, I_amps
_ZONE_TELEMETRY_SIZE = struct.calcsize(_ZONE_TELEMETRY_FMT)


def decode_telemetry_frame(data: bytes) -> dict:
    """PROTOCOL.md sec 6 "TELEMETRY frame" -- `data` is the full EVT-source
    payload, byte 0 already checked == EVT_FRAME_KIND_TELEMETRY by the
    caller (see kilnsim.link's broadcast demux)."""
    sim_time_us, timescale_x100, seed, zone_count = struct.unpack_from("<QIIB", data, 1)
    off = 1 + 8 + 4 + 4 + 1
    zones = []
    for _ in range(zone_count):
        t_true, t_tc, t_safety, i_amps = struct.unpack_from(_ZONE_TELEMETRY_FMT, data, off)
        off += _ZONE_TELEMETRY_SIZE
        zones.append(
            {
                "t_zone": t_true,
                "t_tc_reported": t_tc,
                "t_safety_reported": t_safety,
                "i_amps": i_amps,
            }
        )
    relay_mask, estop_open, fault_line_asserted, active_fault_count = struct.unpack_from(
        "<HBBH", data, off
    )
    off += 6
    spi_txn_total, spi_underrun_total, evt_ring_hwm, evt_gap_count, evt_drop_count = struct.unpack_from(
        "<IIIII", data, off
    )
    return {
        "sim_time_us": sim_time_us,
        "timescale": timescale_x100 / 100.0,
        "seed": seed,
        "zones": zones,
        "relay_state_mask": relay_mask,
        "estop_open": bool(estop_open),
        "fault_line_asserted": bool(fault_line_asserted),
        "active_fault_count": active_fault_count,
        "spi_txn_count": spi_txn_total,
        "spi_underrun_count": spi_underrun_total,
        "event_ring_high_water": evt_ring_hwm,
        "evt_seq_gap_count": evt_gap_count,
        "evt_send_drop_count": evt_drop_count,
    }


def decode_evt_frame(data: bytes) -> dict:
    """PROTOCOL.md sec 6 "EVT frame" -- {seq, sim_time_us, event_type, a, b,
    f0}, `data` is the full EVT-source payload with byte 0 already checked
    == EVT_FRAME_KIND_EVENT by the caller."""
    seq, sim_time_us, event_type, a, b, f0 = struct.unpack_from("<IQBBBf", data, 1)
    return {
        "seq": seq,
        "sim_time_us": sim_time_us,
        "event_type": event_type,
        "a": a,
        "b": b,
        "f0": f0,
    }
