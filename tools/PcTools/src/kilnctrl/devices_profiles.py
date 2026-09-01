"""PROFILES wire encode/decode primitives.

Part of the devices.py split (pure refactor) -- see devices.py's module
docstring for the overall map. Moved verbatim, no logic changes.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .protocol import *  # noqa: F401,F403
from .devices_common import (  # noqa: F401
    OkReason,
    _check_bool_byte,
    _check_finite,
    _check_i8,
    _check_range,
    _check_u8,
    _check_u16,
    _decode_ok_reason,
    _decoded_float,
)


# ---------------------------------------------------------------------------
# PROFILES -- fire profile CRUD + execution control
# (task_id = UART_TASK_ID_PROFILES)
# ---------------------------------------------------------------------------
class ProfilesResponseError(ValueError):
    """Raised when a PROFILES response payload does not match its wire layout."""


@dataclass(frozen=True)
class ProfileSegment:
    target_c: float
    ramp_c_per_hr: float
    dwell_min: int


@dataclass(frozen=True)
class ProfileSummary:
    """One entry from a LIST reply."""

    id: int
    name: str
    zone_mask: int
    segment_count: int

    @property
    def builtin(self) -> bool:
        """True for a shipped read-only schedule (id >= 128)."""
        return self.id >= PROFILES_BUILTIN_ID_BASE


@dataclass(frozen=True)
class ProfileDetail:
    """A GET reply's full profile (segments included)."""

    id: int
    name: str
    zone_mask: int
    segments: "list[ProfileSegment]"

    @property
    def builtin(self) -> bool:
        """True for a shipped read-only schedule (id >= 128)."""
        return self.id >= PROFILES_BUILTIN_ID_BASE


@dataclass(frozen=True)
class ProfileSaveResult:
    ok: bool
    id: "Optional[int]" = None
    warning_count: int = 0
    error: str = ""


@dataclass(frozen=True)
class ZoneExecStatus:
    zone: int
    control_mode: int
    actual_c: float
    actual_valid: bool
    duty: float
    relay_commanded_on: bool
    faulted: bool
    fault_guard: int


@dataclass(frozen=True)
class ProfileExecStatus:
    state: int
    profile_id: int
    name: str
    zone_mask: int
    segment_index: int
    segment_count: int
    dwelling: bool
    target_c: float
    segment_elapsed_s: int
    dwell_remaining_s: int
    ramp_lock_held: bool
    ramp_lock_lagging_mask: int
    fault_guard: int
    zones: "list[ZoneExecStatus]"

    #: profile_exec_state_t values.
    STATE_NAMES = {0: "idle", 1: "running", 2: "paused", 3: "done", 4: "faulted"}

    @property
    def state_name(self) -> str:
        return self.STATE_NAMES.get(self.state, f"unknown({self.state})")


def _pack_str8(text: str, max_len: int, name: str) -> bytes:
    encoded = text.encode("ascii", errors="replace")
    if len(encoded) > max_len:
        raise ValueError(f"{name} too long: {len(encoded)} bytes > {max_len}")
    return struct.pack("<B", len(encoded)) + encoded


def _check_readable_profile_id(profile_id: int, name: str = "profile_id") -> int:
    """Validate an id for a READ/RUN operation (GET, START).

    Two disjoint ranges are legal: user slots ``0..PROFILES_MAX_COUNT-1`` and
    the read-only shipped catalogue at ``PROFILES_BUILTIN_ID_BASE..255``
    (``profiles_builtin.h``). The gap between them is not addressable, and the
    firmware -- not this check -- decides whether a given catalogue index
    actually exists.
    """
    if 0 <= profile_id < PROFILES_MAX_COUNT:
        return profile_id
    if PROFILES_BUILTIN_ID_BASE <= profile_id <= 0xFF:
        return profile_id
    raise ValueError(
        f"{name} must be a user slot 0..{PROFILES_MAX_COUNT - 1} or a built-in "
        f"schedule {PROFILES_BUILTIN_ID_BASE}..255, got {profile_id}"
    )


def profiles_list(start_id: int = 0) -> bytes:
    """0x01 LIST request (query), PAGED.

    Returns every existing profile with ``id >= start_id`` in ascending id
    order (user slots first, then the shipped catalogue) that fits in one
    253-byte reply frame. Page by re-asking with ``last id + 1`` until a reply
    comes back empty -- :meth:`kilnctrl.profiles.ProfilesClient.list_all` does
    exactly that. ``start_id=0`` with no paging yields only the first page,
    which is what the un-argumented request has always returned.
    """
    return struct.pack("<BB", PROFILES_CMD_LIST, _check_u8(start_id, "start_id"))


def profiles_get(profile_id: int) -> bytes:
    """0x02 GET request (query): a user slot 0-7 or a built-in schedule 128+.

    A built-in's full 12-segment reply is 165 bytes against a 253-byte frame,
    so nothing here needs paging.
    """
    return struct.pack("<BB", PROFILES_CMD_GET, _check_readable_profile_id(profile_id))


def profiles_save(
    profile_id: int, name: str, zone_mask: int, segments: "list[ProfileSegment]"
) -> bytes:
    """0x03 SAVE request: id (or PROFILES_SAVE_ID_NEW), name, zone_mask,
    segment_count, then 12 bytes/segment (target_c f32, ramp f32, dwell u32).

    Replies ok/fail (see :func:`parse_profiles_response`).

    A ``profile_id`` in the built-in range means "save a copy into the first
    free user slot", not "overwrite the built-in" -- the catalogue is const
    data in flash and cannot be written. That is the same redirect the HTTP
    side performs (``profiles_http_save()``'s SAVE-VS-COPY note), and the
    reply's ``id`` field reports the slot it actually landed in, so nothing
    about it is silent.
    """
    if profile_id != PROFILES_SAVE_ID_NEW and not (
        PROFILES_BUILTIN_ID_BASE <= profile_id <= 0xFF
    ):
        _check_range(profile_id, 0, PROFILES_MAX_COUNT - 1, "profile_id")
    if not 1 <= len(segments) <= 12:
        raise ValueError(f"segment count must be 1..12, got {len(segments)}")
    body = struct.pack("<BB", PROFILES_CMD_SAVE, profile_id)
    body += _pack_str8(name, 15, "name")
    body += struct.pack("<BB", _check_u8(zone_mask, "zone_mask"), len(segments))
    for i, seg in enumerate(segments):
        body += struct.pack(
            "<ffI",
            _check_finite(seg.target_c, f"segment {i} target_c"),
            _check_finite(seg.ramp_c_per_hr, f"segment {i} ramp_c_per_hr"),
            _check_range(seg.dwell_min, 0, 0xFFFFFFFF, f"segment {i} dwell_min"),
        )
    return body


def profiles_delete(profile_id: int) -> bytes:
    """0x04 DELETE request: a user slot 0-7. Replies ok/fail.

    Built-in ids are accepted on the wire so the firmware can answer with its
    own refusal ("read-only; hide it instead"), matching the HTTP side rather
    than failing differently here.
    """
    return struct.pack(
        "<BB", PROFILES_CMD_DELETE, _check_readable_profile_id(profile_id)
    )


def profiles_get_exec_status() -> bytes:
    """0x05 GET_EXEC_STATUS request (query): no args."""
    return struct.pack("<B", PROFILES_CMD_GET_EXEC_STATUS)


def profiles_start(profile_id: int) -> bytes:
    """0x06 START request: a user slot 0-7 or a built-in schedule 128+.

    Replies ok/fail (+ error text on failure). A built-in runs with every
    configured zone selected -- the catalogue is zone-agnostic, so
    ``profiles_http_get()`` fills the mask from the Zones settings.
    """
    return struct.pack("<BB", PROFILES_CMD_START, _check_readable_profile_id(profile_id))


def profiles_stop() -> bytes:
    """0x07 STOP request: no args. Always replies ok."""
    return struct.pack("<B", PROFILES_CMD_STOP)


def profiles_pause() -> bytes:
    """0x08 PAUSE request: no args. Replies ok/fail."""
    return struct.pack("<B", PROFILES_CMD_PAUSE)


def profiles_resume() -> bytes:
    """0x09 RESUME request: no args. Replies ok/fail."""
    return struct.pack("<B", PROFILES_CMD_RESUME)


def profiles_ack_last_run() -> bytes:
    """0x0A ACK_LAST_RUN request: no args. Replies ok/fail."""
    return struct.pack("<B", PROFILES_CMD_ACK_LAST_RUN)


def parse_profiles_response(payload: bytes) -> "tuple[int, object]":
    """Decode a PROFILES reply into ``(subcmd, value)``.

    Layouts (uart_task_ids.h) -- see the module docstring cross-reference for
    the byte-level offsets; this mirrors them field for field.

    DELETE/PAUSE/RESUME/ACK_LAST_RUN/STOP value is an :class:`OkReason` --
    ``bx_reply_ok_err()`` appends a reason string on refusal (e.g. DELETE's
    "cannot delete a builtin profile") that used to be decoded here and then
    discarded (``bool(payload[1])``), same bug class as CONTROL's SET_* fix
    above. ``OkReason`` is still truthy/falsy like the old bare bool, so
    ``if not result:`` call sites keep working unchanged.
    """
    if len(payload) < 1:
        raise ProfilesResponseError("PROFILES response is empty")
    subcommand = payload[0]

    if subcommand == PROFILES_CMD_LIST:
        if len(payload) < 2:
            raise ProfilesResponseError("LIST response is missing its count byte")
        count = payload[1]
        offset = 2
        summaries = []
        for i in range(count):
            if offset + 2 > len(payload):
                raise ProfilesResponseError(f"LIST entry {i} header truncated")
            pid = payload[offset]
            name_len = payload[offset + 1]
            name_start = offset + 2
            name_end = name_start + name_len
            if name_end + 2 > len(payload):
                raise ProfilesResponseError(f"LIST entry {i} truncated")
            name = payload[name_start:name_end].decode("ascii", errors="replace")
            zone_mask = payload[name_end]
            segment_count = payload[name_end + 1]
            summaries.append(
                ProfileSummary(id=pid, name=name, zone_mask=zone_mask, segment_count=segment_count)
            )
            offset = name_end + 2
        if offset != len(payload):
            raise ProfilesResponseError(
                f"LIST response has {len(payload) - offset} trailing bytes"
            )
        return subcommand, summaries

    if subcommand == PROFILES_CMD_GET:
        if len(payload) < 2:
            raise ProfilesResponseError("GET response is missing its ok byte")
        ok = payload[1]
        if not ok:
            return subcommand, None
        if len(payload) < 4:
            raise ProfilesResponseError("GET response header is truncated")
        pid = payload[2]
        name_len = payload[3]
        name_start = 4
        name_end = name_start + name_len
        if name_end + 2 > len(payload):
            raise ProfilesResponseError("GET response name/header truncated")
        name = payload[name_start:name_end].decode("ascii", errors="replace")
        zone_mask = payload[name_end]
        segment_count = payload[name_end + 1]
        seg_start = name_end + 2
        expected = seg_start + segment_count * PROFILES_SEGMENT_LEN
        if len(payload) != expected:
            raise ProfilesResponseError(
                f"GET segment_count={segment_count} implies {expected} bytes, "
                f"got {len(payload)}"
            )
        segments = []
        for i in range(segment_count):
            off = seg_start + i * PROFILES_SEGMENT_LEN
            target_c, ramp, dwell = struct.unpack_from("<ffI", payload, off)
            segments.append(
                ProfileSegment(target_c=target_c, ramp_c_per_hr=ramp, dwell_min=dwell)
            )
        return subcommand, ProfileDetail(
            id=pid, name=name, zone_mask=zone_mask, segments=segments
        )

    if subcommand == PROFILES_CMD_SAVE:
        if len(payload) < 2:
            raise ProfilesResponseError("SAVE response is missing its ok byte")
        ok = payload[1]
        if ok:
            if len(payload) < 4:
                raise ProfilesResponseError("SAVE ok response header is truncated")
            return subcommand, ProfileSaveResult(
                ok=True, id=payload[2], warning_count=payload[3]
            )
        err_len = payload[2] if len(payload) > 2 else 0
        error = ""
        if err_len:
            error = payload[3 : 3 + err_len].decode("ascii", errors="replace")
        return subcommand, ProfileSaveResult(ok=False, error=error)

    if subcommand in (
        PROFILES_CMD_DELETE,
        PROFILES_CMD_PAUSE,
        PROFILES_CMD_RESUME,
        PROFILES_CMD_ACK_LAST_RUN,
        PROFILES_CMD_STOP,
    ):
        return subcommand, _decode_ok_reason(
            payload, ProfilesResponseError, "DELETE/PAUSE/RESUME/ACK_LAST_RUN/STOP"
        )

    if subcommand == PROFILES_CMD_START:
        if len(payload) < 2:
            raise ProfilesResponseError("START response is missing its ok byte")
        ok = payload[1]
        if ok:
            return subcommand, ProfileSaveResult(ok=True)
        err_len = payload[2] if len(payload) > 2 else 0
        error = ""
        if err_len:
            error = payload[3 : 3 + err_len].decode("ascii", errors="replace")
        return subcommand, ProfileSaveResult(ok=False, error=error)

    if subcommand == PROFILES_CMD_GET_EXEC_STATUS:
        if len(payload) < 4:
            raise ProfilesResponseError("GET_EXEC_STATUS response header is truncated")
        state = payload[1]
        profile_id = payload[2]
        name_len = payload[3]
        name_start = 4
        name_end = name_start + name_len
        # fixed-width block after the name: zone_mask(1) segment_index(1)
        # segment_count(1) dwelling(1) target_c(4) segment_elapsed_s(4)
        # dwell_remaining_s(4) ramp_lock_held(1) ramp_lock_lagging_mask(1)
        # fault_guard(1) zone_count(1) = 20 bytes
        fixed_end = name_end + 20
        if fixed_end > len(payload):
            raise ProfilesResponseError("GET_EXEC_STATUS fixed block truncated")
        name = payload[name_start:name_end].decode("ascii", errors="replace")
        zone_mask = payload[name_end]
        segment_index = payload[name_end + 1]
        segment_count = payload[name_end + 2]
        dwelling = bool(payload[name_end + 3])
        target_c, segment_elapsed_s, dwell_remaining_s = struct.unpack_from(
            "<fII", payload, name_end + 4
        )
        ramp_lock_held = bool(payload[name_end + 16])
        ramp_lock_lagging_mask = payload[name_end + 17]
        fault_guard = payload[name_end + 18]
        zone_count = payload[name_end + 19]
        zones_start = fixed_end
        expected = zones_start + zone_count * 14
        if len(payload) != expected:
            raise ProfilesResponseError(
                f"GET_EXEC_STATUS zone_count={zone_count} implies {expected} bytes, "
                f"got {len(payload)}"
            )
        zones = []
        for i in range(zone_count):
            off = zones_start + i * 14
            zone, control_mode = payload[off], payload[off + 1]
            actual_c = struct.unpack_from("<f", payload, off + 2)[0]
            actual_valid = bool(payload[off + 6])
            duty = struct.unpack_from("<f", payload, off + 7)[0]
            relay_on = bool(payload[off + 11])
            faulted = bool(payload[off + 12])
            fg = payload[off + 13]
            zones.append(
                ZoneExecStatus(
                    zone=zone,
                    control_mode=control_mode,
                    actual_c=actual_c,
                    actual_valid=actual_valid,
                    duty=duty,
                    relay_commanded_on=relay_on,
                    faulted=faulted,
                    fault_guard=fg,
                )
            )
        return subcommand, ProfileExecStatus(
            state=state,
            profile_id=profile_id,
            name=name,
            zone_mask=zone_mask,
            segment_index=segment_index,
            segment_count=segment_count,
            dwelling=dwelling,
            target_c=target_c,
            segment_elapsed_s=segment_elapsed_s,
            dwell_remaining_s=dwell_remaining_s,
            ramp_lock_held=ramp_lock_held,
            ramp_lock_lagging_mask=ramp_lock_lagging_mask,
            fault_guard=fault_guard,
            zones=zones,
        )

    raise ProfilesResponseError(f"unknown PROFILES response subcommand 0x{subcommand:02X}")


