"""Python-side data model for SimFW's USB control protocol.

This is the **stable contract layer**: dataclasses/enums for the command
groups and payloads sketched in ``firmware/SimFW/docs/PLAN.md`` section 5
(command groups), 5.2 (representative payloads -- FAULT_SCHEDULE,
TC_GET_REGS reply, RELAY_GET_EDGES reply) and 5.3 (telemetry frame, EVT frame
fields), plus the trigger/duration/repeat spec in section 7.2 that
``scenario.compile_faults`` targets.

Everything in this module is shape, not wire bytes. The actual byte-level
encoding is SimFW's job once its protocol core is lifted from
``firmware/UnitTestFw`` into ``firmware/CommonFW`` (PLAN.md sec 4.4/12) --
that happens in :mod:`kilnsim.link`, behind :class:`~kilnsim.link.SimLink`.
Nothing in this module should need to change when that lift lands; only the
encoder/decoder in ``link.py`` does.

Command group / command numbering below is provisional (PLAN.md sec 5.2:
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
# Command groups / task registration (PLAN.md sec 5, 5.1)
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


# --- SYS (PLAN.md sec 5) ----------------------------------------------------
class SysCmd(enum.IntEnum):
    PING = 1
    GET_VERSION = 2
    RESET_SIM = 3
    SET_TIMESCALE = 4
    SET_SEED = 5
    GET_CAPS = 6


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


# --- RELAY ---------------------------------------------------------------------
class RelayCmd(enum.IntEnum):
    GET_STATES = 1
    GET_EDGES = 2  # timestamped edge log
    SET_CONTACT_FAULT = 3  # welded/stuck-open, at the *sense* interpretation level


# --- IO (discrete I/O, E-stop, fault line, DUT power) -------------------------
class IoCmd(enum.IntEnum):
    SET_DIR = 1
    WRITE = 2
    READ = 3
    ESTOP_SET = 4
    FAULT_LINE_GET = 5
    DUT_POWER_SET = 6  # not in PLAN.md's sketch table but needed by 6.1's dut_power_set


# --- FAULT (scheduler, PLAN.md sec 7) -----------------------------------------
class FaultCmd(enum.IntEnum):
    SCHEDULE = 1
    CANCEL = 2
    LIST = 3
    FIRE_NOW = 4


# --- EVT event types (PLAN.md sec 5.3) ----------------------------------------
#
# RELAY_EDGE, FAULT_FIRED/CLEARED, DUT_POWER etc. are PLAN.md sec 5.3's own
# list ("relay edges, fault fired/cleared, threshold crossings, mode
# changes, DUT power switch, protocol errors"). GUARD_TRIP/GUARD_WARN/
# LINK_UP/TRIP_INEFFECTIVE_LATCHED were added once firmware/SimFW/scenarios/
# *.yaml (written in parallel against this same PLAN.md sec 8.1 schema)
# showed real `expect` clauses referencing them -- a scenario asserting "S9
# escalates" needs an event *type* for that escalation to reference, and
# PLAN.md sec 1's whole point ("exercise every guard ... on the bench") is
# guard trips/warns being observable, so these round out the vocabulary
# rather than inventing something unrelated to the plan.
class EventType(enum.IntEnum):
    RELAY_EDGE = 1
    FAULT_FIRED = 2
    FAULT_CLEARED = 3
    THRESHOLD_CROSSED = 4
    MODE_CHANGED = 5
    DUT_POWER = 6
    PROTOCOL_ERROR = 7
    SIM_CLOCK_MARK = 8
    GUARD_TRIP = 9  # SaftyFW guard S<n> tripped (SAFETY_MODEL.md)
    GUARD_WARN = 10  # SaftyFW guard S<n> warned without tripping
    LINK_UP = 11  # kilnlink came back up (e.g. after power_blip)
    TRIP_INEFFECTIVE_LATCHED = 12  # S9: a trip that did not actually cut power


# ---------------------------------------------------------------------------
# Modes (PLAN.md sec 5: "Every mutable thing has a mode: MODEL ... or MANUAL")
# ---------------------------------------------------------------------------
class Mode(str, enum.Enum):
    MODEL = "model"
    MANUAL = "manual"


# ---------------------------------------------------------------------------
# Fault trigger/duration/repeat spec (PLAN.md sec 7.2)
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
# FAULT_SCHEDULE request (PLAN.md sec 5.2)
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
    fault_type: str  # catalog name, PLAN.md sec 7.1 (e.g. "welded_ssr", "tc_disconnect")
    target: str  # e.g. "relay:K1", "tc:0", "ct:1"
    trigger: Trigger
    duration: Duration
    repeat: Repeat = field(default_factory=Repeat)
    params: tuple = (0.0, 0.0, 0.0, 0.0)  # param0..3, fault-type-specific


# ---------------------------------------------------------------------------
# TC_GET_REGS reply (PLAN.md sec 5.2): "the channel's full 16-byte register
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
# RELAY_GET_EDGES reply (PLAN.md sec 5.2): "up to N {u8 relay, u8 edge,
# u64 sim_time_us} drained from the edge log".
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class RelayEdge:
    relay: str
    edge: Edge
    sim_time_us: int


# ---------------------------------------------------------------------------
# Telemetry frame (PLAN.md sec 5.3)
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


# ---------------------------------------------------------------------------
# EVT frame (PLAN.md sec 5.3): {u32 seq, u64 sim_time_us, u8 event_type, payload}
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Event:
    seq: int
    sim_time_us: int
    event_type: EventType
    payload: dict = field(default_factory=dict)

    def to_dict(self) -> dict:
        return {
            "seq": self.seq,
            "sim_time_us": self.sim_time_us,
            "event_type": self.event_type.name,
            "payload": dict(self.payload),
        }

    @classmethod
    def from_dict(cls, data: dict) -> "Event":
        return cls(
            seq=int(data["seq"]),
            sim_time_us=int(data["sim_time_us"]),
            event_type=EventType[data["event_type"]] if isinstance(data["event_type"], str)
            else EventType(data["event_type"]),
            payload=dict(data.get("payload") or {}),
        )


# ---------------------------------------------------------------------------
# GET_CAPS reply (PLAN.md sec 5.1): "protocol version, SimFW version + git
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
