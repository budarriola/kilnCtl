"""Python-side data model for SimFW's USB control protocol.

This is the **stable contract layer**: dataclasses/enums for the command
groups and payloads sketched in ``firmware/SimFW/docs/DESIGN_NOTES.md`` section 5
(command groups), 5.2 (representative payloads -- FAULT_SCHEDULE,
TC_GET_REGS reply, RELAY_GET_EDGES reply) and 5.3 (telemetry frame, EVT frame
fields), plus the trigger/duration/repeat spec in section 7.2 that
``scenario.compile_faults`` targets.

Everything in this module is shape, not wire bytes. The actual byte-level
encoding is SimFW's job; its protocol core was lifted from
``firmware/UnitTestFw`` into ``firmware/CommonFW`` as ``benchproto``
(DESIGN_NOTES.md sec 4.4/12) -- that lives in :mod:`kilnsim.link`, behind
:class:`~kilnsim.link.SimLink`. Nothing in this module needs to change for
that; only the encoder/decoder in ``link.py`` cares about wire bytes.

Command group / command numbering below is provisional (DESIGN_NOTES.md sec 5.2:
"byte layouts frozen in PROTOCOL.md at M-B") -- it exists so
``SimLink.send_command(group, cmd, payload)`` has something concrete to pass,
not as a claim about the eventual wire values. Once ``PROTOCOL.md`` exists
under ``firmware/SimFW/docs/``, reconcile the numbers here against it.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass, field
from typing import Optional, Union


# ---------------------------------------------------------------------------
# Command groups / task registration (DESIGN_NOTES.md sec 5, 5.1)
#
# "Each command group above registers as an addressable task in the
# protocol's task-registration model ... so the PC discovers the fixture's
# capabilities at connect time via ... GET_CAPS". The numbers below are this
# package's own placeholder addressing scheme, unrelated to any KilnFW/
# SaftyFW task id space -- SimFW is a different device on a different link.
# ---------------------------------------------------------------------------
class CommandGroup(enum.IntEnum):
    SYS = 1
    MODEL = 2
    TC = 3
    CT = 4
    RELAY = 5
    IO = 6
    FAULT = 7
    EVT = 8  # unsolicited only -- never a send_command() destination


# --- SYS (DESIGN_NOTES.md sec 5) ----------------------------------------------------
class SysCmd(enum.IntEnum):
    PING = 1
    GET_VERSION = 2
    RESET_SIM = 3
    SET_TIMESCALE = 4
    SET_SEED = 5
    GET_CAPS = 6
    # PROTOCOL.md sec 4 "GET_SIM_STATE": read-back companion to
    # SET_TIMESCALE/SET_SEED, added in the same gap-closure pass that gave
    # real firmware's cmd_task.c real (non-stub) handlers for all four ids
    # 0x03-0x07. Not in DESIGN_NOTES.md sec 5's original sketch.
    GET_SIM_STATE = 7
    # PROTOCOL.md sec 4 "REBOOT_BOOTLOADER" (later gap-closure pass, "flash
    # over USB without BOOTSEL"): drops the RP2040 into its ROM USB
    # bootloader after confirming the fixture reached a safe state
    # (E-stop open, both DUT power relays off, CT outputs silent --
    # firmware/SimFW/src/tasks/safe_reboot.c). Requires the request payload's
    # confirm value to equal REBOOT_BOOTLOADER_MAGIC exactly; on success the
    # device never replies at all (it jumps into the bootloader before it can
    # ACK) -- see kilnsim.link's SimLink.send_command_expect_reboot() and
    # payloads.py's _sys_encode/_sys_decode for cmd id 8.
    REBOOT_BOOTLOADER = 8
    # PROTOCOL.md sec 4 "SESSION_RESET" (bug-fix pass, "PC reconnect
    # misclassified as duplicate traffic"): clears the firmware's benchproto
    # dedup ring + per-task ACK cache for this PC's device id. LINK state
    # only -- never confuse with RESET_SIM (id 3) above, which is the
    # simulation reset; this one never touches sim_engine/fault_sched/
    # i2c_owner/wave_owner at all. kilnsim.link's _FramedSimLink.connect()
    # sends this as the very first command on every (re)connect, before
    # PING, specifically because kilnsim's own BenchprotoLink restarts its
    # msg_index counter at 0 on every connect() while the firmware's dedup
    # ring survives across USB reconnects (only initialized once, at MCU
    # boot) -- without this, the first few post-reconnect commands
    # (including PING itself) can be misclassified as duplicates of a
    # previous session's traffic and either time out or get back a stale,
    # unrelated cached reply. Tolerates an older firmware build that
    # predates this id (ERR_NOT_IMPL) without failing the connect.
    SESSION_RESET = 9


#: SIMFW_CMD_SYS_REBOOT_BOOTLOADER_MAGIC (firmware/SimFW/src/tasks/cmd_ids.h)
#: -- required exactly in SysCmd.REBOOT_BOOTLOADER's request payload. Kept as
#: a public constant (not buried in payloads.py) since it is meaningful at
#: this "what does this command mean" layer too, not just the wire-encoding
#: layer.
SYS_REBOOT_BOOTLOADER_MAGIC = 0xB007B007


# --- MODEL -------------------------------------------------------------------
class ModelCmd(enum.IntEnum):
    SET_ZONE_PARAMS = 1
    GET_ZONE_PARAMS = 2
    SET_AMBIENT = 3
    LOAD_PRESET = 4
    SET_TEMP = 5  # force a zone temp
    SET_TC_LAG = 6


# --- TC (MAX31856 emulation) --------------------------------------------------
class TcCmd(enum.IntEnum):
    GET_REGS = 1
    FORCE_TEMP = 2
    SET_MODE = 3  # MODEL / MANUAL
    INJECT_FAULT = 4
    CLEAR_FAULT = 5
    GET_MASTER_CONFIG = 6  # what did the DUT write?


# --- CT (current-transformer emulation) --------------------------------------
class CtCmd(enum.IntEnum):
    SET_MODE = 1  # MODEL / MANUAL
    SET_AMPS = 2
    SET_DISTORTION = 3
    GET_STATE = 4
    # PROTOCOL.md sec 5.3: "not in DESIGN_NOTES.md 5's original sketch; wired up
    # because wave_owner.h exposes it as a first-class public setter" --
    # payloads.py's _ct_encode/_ct_decode already handle cmd id 5, this enum
    # member was just missing.
    SET_PHASE = 5


# --- RELAY ---------------------------------------------------------------------
class RelayCmd(enum.IntEnum):
    GET_STATES = 1
    GET_EDGES = 2  # timestamped edge log
    # DESIGN_NOTES.md sec 5's original sketch had a SET_CONTACT_FAULT here, but
    # PROTOCOL.md sec 5.4 documents it as deliberately **not allocated**:
    # i2c_owner.h exposes no such setter (relay sense is read-only from this
    # task's perspective by design). FAULT_SCHEDULE's WELDED_RELAY/
    # STUCK_OPEN_RELAY types are the real path -- no SIMFW_CMD_RELAY_* id 3
    # exists on the wire, so no enum member is defined for it here either.


# --- IO (discrete I/O, E-stop, fault line, DUT power) -------------------------
class IoCmd(enum.IntEnum):
    SET_DIR = 1
    WRITE = 2
    READ = 3
    ESTOP_SET = 4
    FAULT_LINE_GET = 5
    DUT_POWER_SET = 6  # not in PLAN.md's sketch table but needed by 6.1's dut_power_set
    # ESTOP_GET/DUT_POWER_GET (PROTOCOL.md sec 5.5): not in DESIGN_NOTES.md 5's
    # original sketch either, but i2c_owner.h exposes both getters as
    # first-class public API and a client otherwise has no way to read back
    # what it last commanded, so SimFW wires them up too.
    ESTOP_GET = 7
    DUT_POWER_GET = 8
    # DUT_POWER_SAFETY_SET/GET (PROTOCOL.md sec 5.5, 2026-08-20): the fixture's
    # second, independent DUT-power relay (J19/safety domain). DUT_POWER_SET/
    # GET (6/8) above keep their original meaning -- main domain (J18) only --
    # rather than being redefined to mean "both relays": the whole point of
    # two relays is that GND_Main and GND_Safty never get bonded through a
    # shared control path, so a legacy client using only the old command can
    # never accidentally command the safety-domain relay too.
    DUT_POWER_SAFETY_SET = 9
    DUT_POWER_SAFETY_GET = 10


# --- FAULT (scheduler, DESIGN_NOTES.md sec 7) -----------------------------------------
class FaultCmd(enum.IntEnum):
    SCHEDULE = 1
    CANCEL = 2
    LIST = 3
    FIRE_NOW = 4
    # UNTIL_TRIGGER two-frame design (PROTOCOL.md sec 5.6): SCHEDULE with
    # duration_kind == 2 parks the fault's fields without arming it; this
    # command supplies the release trigger and performs the actual arm.
    SET_UNTIL_TRIGGER = 5


# --- EVT event types (DESIGN_NOTES.md sec 5.3, PROTOCOL.md sec 6) ---------------------
#
# RELAY_EDGE..PROTOCOL_ERROR's *numeric values* below were corrected to match
# firmware/SimFW/src/sim/sim_snapshot.h's real `sim_event_type_t` (0-based:
# SIM_EVENT_RELAY_EDGE=0 .. SIM_EVENT_PROTOCOL_ERROR=6), which is what
# actually appears in wire EVT frames' `event_type` byte (PROTOCOL.md sec 6:
# "sim_event_t.type verbatim ... sim_snapshot.h's own 1:1 wire-mapping
# guarantee"). This module originally numbered them 1-7, a mismatch found
# while wiring up kilnsim.payloads.decode_evt_frame() -- reported, not a
# firmware bug (kilnsim's own numbering was wrong, not sim_snapshot.h's).
#
# GUARD_TRIP/GUARD_WARN/LINK_UP/TRIP_INEFFECTIVE_LATCHED/SIM_CLOCK_MARK have
# **no corresponding `sim_event_type_t` value** -- `sim_snapshot.h` only
# defines the 7 types above, nothing SaftyFW-guard-shaped. These five were
# added because firmware/SimFW/scenarios/*.yaml `expect` clauses reference
# them, but nothing in SimFW's own EVT wire stream can produce them today;
# they are kept as kilnsim-local/synthetic values (a scenario runner or
# report generator's own derived vocabulary, e.g. inferred from a sequence of
# real EVT frames plus kilnctrl-side SaftyFW telemetry) and numbered starting
# at 100 specifically so they can never collide with a real wire byte and so
# Event.from_wire() below can tell "this came off the wire" from "this is
# kilnsim's own synthesis" by value range alone. This is a real gap between
# the scenario vocabulary and what SimFW's EVT stream can express — reported
# here, not silently worked around; see WIRE_EVENT_TYPES below for the set
# decode_evt_frame() can actually produce.
class EventType(enum.IntEnum):
    RELAY_EDGE = 0
    FAULT_FIRED = 1
    FAULT_CLEARED = 2
    THRESHOLD_CROSSED = 3
    MODE_CHANGED = 4
    DUT_POWER = 5
    PROTOCOL_ERROR = 6
    # --- kilnsim-local/synthetic only; never appear on SimFW's real wire ---
    SIM_CLOCK_MARK = 100
    GUARD_TRIP = 101  # SaftyFW guard S<n> tripped (SAFETY_MODEL.md)
    GUARD_WARN = 102  # SaftyFW guard S<n> warned without tripping
    LINK_UP = 103  # kilnlink came back up (e.g. after power_blip)
    TRIP_INEFFECTIVE_LATCHED = 104  # S9: a trip that did not actually cut power


#: The subset of EventType values sim_snapshot.h's sim_event_type_t can
#: actually produce on the wire (PROTOCOL.md sec 6) -- everything else in
#: EventType is kilnsim-local synthesis, see the class comment above.
WIRE_EVENT_TYPES = frozenset(
    {
        EventType.RELAY_EDGE,
        EventType.FAULT_FIRED,
        EventType.FAULT_CLEARED,
        EventType.THRESHOLD_CROSSED,
        EventType.MODE_CHANGED,
        EventType.DUT_POWER,
        EventType.PROTOCOL_ERROR,
    }
)


# ---------------------------------------------------------------------------
# Modes (DESIGN_NOTES.md sec 5: "Every mutable thing has a mode: MODEL ... or MANUAL")
# ---------------------------------------------------------------------------
class Mode(str, enum.Enum):
    MODEL = "model"
    MANUAL = "manual"


# ---------------------------------------------------------------------------
# Fault trigger/duration/repeat spec (DESIGN_NOTES.md sec 7.2)
# ---------------------------------------------------------------------------
class TriggerKind(str, enum.Enum):
    AT_SIM_TIME = "at_sim_time"
    AT_ZONE_TEMP = "at_zone_temp"
    ON_RELAY_EDGE = "on_relay_edge"
    ON_EVENT = "on_event"
    AFTER_FAULT = "after_fault"
    RANDOM_IN = "random_in"
    MANUAL = "manual"


class DurationKind(str, enum.Enum):
    PERMANENT = "permanent"
    FOR = "for"
    UNTIL_TRIGGER = "until_trigger"


class RepeatKind(str, enum.Enum):
    ONCE = "once"
    EVERY = "every"
    N_TIMES = "n_times"


class Edge(str, enum.Enum):
    RISING = "rising"
    FALLING = "falling"


@dataclass(frozen=True)
class Trigger:
    kind: TriggerKind
    # AT_SIM_TIME
    t: Optional[float] = None
    # AT_ZONE_TEMP
    zone: Optional[int] = None
    temp_c: Optional[float] = None
    # ON_RELAY_EDGE / AT_ZONE_TEMP: a plain word ("rising"/"falling" for a
    # temperature crossing, "open"/"closed"/"close" for a relay contact --
    # real firmware/SimFW/scenarios/*.yaml use both vocabularies for this one
    # field, so it is intentionally not the strict rising/falling Edge enum).
    edge: Optional[str] = None
    # ON_RELAY_EDGE
    relay: Optional[str] = None
    delay_s: float = 0.0
    # ON_EVENT
    event_name: Optional[str] = None
    # AFTER_FAULT
    fault_id: Optional[str] = None
    # RANDOM_IN
    t0: Optional[float] = None
    t1: Optional[float] = None


@dataclass(frozen=True)
class Duration:
    kind: DurationKind
    t: Optional[float] = None  # FOR
    until: Optional[Trigger] = None  # UNTIL_TRIGGER


@dataclass(frozen=True)
class Repeat:
    kind: RepeatKind = RepeatKind.ONCE
    period: Optional[float] = None  # EVERY
    jitter: float = 0.0  # EVERY
    n: Optional[int] = None  # N_TIMES


# ---------------------------------------------------------------------------
# FAULT_SCHEDULE request (DESIGN_NOTES.md sec 5.2)
#
#   {u16 fault_slot, u8 fault_type, u8 target,
#    trigger{u8 kind, f32 a, f32 b, u8 zone/relay},
#    duration{u8 kind, f32 t},
#    repeat{u8 kind, f32 period, f32 jitter, u16 n},
#    f32 param0..3}
#
# The dataclass below carries the same information as *named* fields (kind
# enums, not the packed u8/f32 sketch) -- link.py's placeholder framing is
# JSON, so there's no reason to pre-flatten this into the eventual byte
# layout before that layout exists. Whatever encodes this for real hardware
# is responsible for the packing.
# ---------------------------------------------------------------------------
@dataclass
class FaultScheduleCommand:
    fault_slot: int
    fault_type: str  # catalog name, DESIGN_NOTES.md sec 7.1 (e.g. "welded_ssr", "tc_disconnect")
    target: str  # e.g. "relay:K1", "tc:0", "ct:1"
    trigger: Trigger
    duration: Duration
    repeat: Repeat = field(default_factory=Repeat)
    params: tuple = (0.0, 0.0, 0.0, 0.0)  # param0..3, fault-type-specific


# ---------------------------------------------------------------------------
# TC_GET_REGS reply (DESIGN_NOTES.md sec 5.2): "the channel's full 16-byte register
# image + the emulator's shadow state (actual simulated temp before
# corruption, active fault list)".
# ---------------------------------------------------------------------------
@dataclass
class TcRegs:
    channel: int
    regs: bytes  # 16-byte MAX31856 register image
    shadow_temp_c: float  # true simulated TC temp, pre-corruption
    shadow_cj_c: float
    active_faults: list = field(default_factory=list)  # fault-type names currently applied


# ---------------------------------------------------------------------------
# RELAY_GET_EDGES reply (DESIGN_NOTES.md sec 5.2): "up to N {u8 relay, u8 edge,
# u64 sim_time_us} drained from the edge log".
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class RelayEdge:
    relay: str
    edge: Edge
    sim_time_us: int


# ---------------------------------------------------------------------------
# Telemetry frame (DESIGN_NOTES.md sec 5.3)
# ---------------------------------------------------------------------------
@dataclass
class ZoneTelemetry:
    t_zone: float
    t_tc_reported: float
    t_safety_reported: float
    i_amps: float


@dataclass
class TelemetryFrame:
    sim_time_us: int
    timescale: float
    seed: int
    zones: list  # list[ZoneTelemetry]
    relay_state_mask: int
    estop_open: bool
    fault_line_asserted: bool
    active_fault_count: int
    spi_txn_count: int
    spi_underrun_count: int
    event_ring_high_water: int
    # Real wire fields (firmware/SimFW/docs/PROTOCOL.md sec 6, "Loss
    # visibility"): the PC's report generator (report.py) refuses to certify
    # a run with a sequence gap, and these two counters let it do so without
    # reconstructing the answer purely from the EVT stream itself.
    evt_seq_gap_count: int = 0
    evt_send_drop_count: int = 0


# ---------------------------------------------------------------------------
# EVT frame (PROTOCOL.md sec 6): {u32 seq, u64 sim_time_us, u8 event_type,
# u8 a, u8 b, f32 f0} -- the real wire shape (20 bytes fixed, kilnsim.payloads
# .decode_evt_frame()). `payload` below is a kilnsim-local convenience
# dict built on top of the raw a/b/f0 fields (e.g. by scenario.py/report.py)
# for callers that want named fields instead of the raw wire bytes; it is not
# itself part of the wire format.
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Event:
    seq: int
    sim_time_us: int
    event_type: EventType
    payload: dict = field(default_factory=dict)
    # Raw wire fields, PROTOCOL.md sec 6's EVT frame layout. Default 0 so
    # existing payload-dict-only construction (tests, MockSimLink) keeps
    # working unchanged.
    a: int = 0
    b: int = 0
    f0: float = 0.0

    def to_dict(self) -> dict:
        return {
            "seq": self.seq,
            "sim_time_us": self.sim_time_us,
            "event_type": self.event_type.name,
            "payload": dict(self.payload),
            "a": self.a,
            "b": self.b,
            "f0": self.f0,
        }

    @classmethod
    def from_dict(cls, data: dict) -> "Event":
        return cls(
            seq=int(data["seq"]),
            sim_time_us=int(data["sim_time_us"]),
            event_type=EventType[data["event_type"]] if isinstance(data["event_type"], str)
            else EventType(data["event_type"]),
            payload=dict(data.get("payload") or {}),
            a=int(data.get("a", 0)),
            b=int(data.get("b", 0)),
            f0=float(data.get("f0", 0.0)),
        )

    @classmethod
    def from_wire(cls, fields: dict) -> "Event":
        """Builds an Event straight from kilnsim.payloads.decode_evt_frame()'s
        dict -- the raw a/b/f0 wire fields, no `payload` interpretation
        attempted (PROTOCOL.md sec 6 does not define per-event-type a/b/f0
        semantics; a caller wanting that mapping builds it on top of this).

        A wire `event_type` byte outside sim_event_type_t's defined range
        (WIRE_EVENT_TYPES) is preserved as PROTOCOL_ERROR with the original
        byte kept in `payload["raw_event_type"]`, rather than raising --
        a malformed/future firmware byte should not crash the receive path;
        see kilnsim.link's broadcast demux for how this is surfaced."""
        raw_type = fields["event_type"]
        try:
            event_type = EventType(raw_type)
            payload = {}
        except ValueError:
            event_type = EventType.PROTOCOL_ERROR
            payload = {"raw_event_type": raw_type}
        return cls(
            seq=fields["seq"],
            sim_time_us=fields["sim_time_us"],
            event_type=event_type,
            payload=payload,
            a=fields.get("a", 0),
            b=fields.get("b", 0),
            f0=fields.get("f0", 0.0),
        )


# ---------------------------------------------------------------------------
# GET_CAPS reply (DESIGN_NOTES.md sec 5.1): "protocol version, SimFW version + git
# hash, zone count limits, channel counts, and a feature bitmask".
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Capabilities:
    protocol_version: int
    fw_version: str
    fw_git_hash: str
    zone_count_min: int
    zone_count_max: int
    tc_channel_count: int
    ct_channel_count: int
    relay_count: int
    feature_bitmask: int = 0
