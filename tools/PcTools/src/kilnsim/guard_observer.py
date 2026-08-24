"""Guard observation source for :func:`kilnsim.runner.run_scenario`'s
optional ``guard_observer=`` argument.

The real gap this module closes: ``kilnsim.protocol.EventType`` defines
``GUARD_TRIP``/``GUARD_WARN``/``LINK_UP``/``TRIP_INEFFECTIVE_LATCHED``
because ``firmware/SimFW/scenarios/*.yaml`` ``expect`` clauses reference
them, but that enum's own class comment says plainly that none of the four
has a corresponding ``sim_event_type_t`` value -- SimFW's EVT wire stream
cannot produce any of them, ever, no matter how long a scenario runs. Guard
state (S1..S13, SAFETY_MODEL.md) lives entirely on SaftyFW, the RP2040
safety processor -- a SECOND device this package otherwise never talks to.
Closing the gap for real means polling THAT device through kilnctrl's
SafetyClient (task 7, the isolated PC<->ESP<->Pico bridge,
``tools/PcTools/src/kilnctrl/safety.py``) and edge-detecting its cached
status into the synthetic events ``report.py``'s clause evaluators already
know how to match.

Two design points worth stating up front:

* **kilnctrl is imported lazily.** Nothing at this module's top level
  touches ``kilnctrl`` at all -- :class:`SafetyGuardObserver` accepts an
  already-constructed client object (duck-typed: anything exposing
  ``get_diag()``/``get_status()`` with kilnctrl's own return shapes), and
  only :func:`make_safety_guard_observer` (a convenience factory) imports
  ``kilnctrl.safety.SafetyClient``, inside its own body. This means
  ``import kilnsim.guard_observer`` -- and therefore ``import kilnsim.runner``,
  which only imports this module for a ``TYPE_CHECKING``-guarded type hint --
  never requires kilnctrl to be installed. A kilnsim install with no
  kilnctrl present keeps working for every scenario that never asks for a
  guard observer, which per ``run_scenario``'s own ``guard_observer=None``
  default is every scenario unless a caller opts in.
* **Seq namespace.** Guard events get their own disjoint seq range, base
  :data:`GUARD_OBSERVER_SEQ_BASE` = 2,000,000,000 -- deliberately a THIRD
  namespace, distinct from both real wire EVT seqs (small, bounded by one
  run's actual EVT traffic) and ``kilnsim.runner._SYNTHETIC_SEQ_BASE``
  (1,000,000,000, telemetry-derived entity/state edges). The reasoning is
  the same one that module's own comment gives for its choice: keep this
  source's seqs far enough from the other two that a bug can never make one
  look like the other, and specifically NOT adjacent to runner.py's own
  telemetry-derived base, because these two synthetic sources are combined
  into the SAME ``collected`` event list by ``run_scenario()`` and must stay
  tell-apart-able from each other too, not just from the wire. Both bases
  sit comfortably inside a 32-bit unsigned range (the wire ``seq`` field's
  own width, PROTOCOL.md sec 6) with roughly a billion seqs of headroom
  between and after them, orders of magnitude more than any real run could
  ever produce of either kind.
"""

from __future__ import annotations

import logging
import time as _time
from typing import TYPE_CHECKING, Optional

from .protocol import Event, EventType

if TYPE_CHECKING:  # pragma: no cover - type-checking only, never imported at runtime
    from kilnctrl.safety import SafetyClient

log = logging.getLogger(__name__)

#: See the module docstring's "Seq namespace" point.
GUARD_OBSERVER_SEQ_BASE = 2_000_000_000

#: firmware/SaftyFW/src/safety_guards.h's `safety_trip_t` -> SAFETY_MODEL.md's
#: own S<n> guard label. Deliberately NOT a `f"S{reason}"` formula:
#: safety_guards.h's own header comment ("4 and 11 are reserved gaps -- S4
#: and S10 are WARN-only, never produce a trip code") means reasons 6..14
#: are already offset from their guard numbers, and reasons 6/7 both belong
#: to guard "S6" under two different letters (S6a `SAFETY_TRIP_MAIN_FAULT`,
#: S6b `SAFETY_TRIP_LINK_DEAD`) -- see that header for the full enum this
#: table mirrors, and firmware/SaftyFW/src/tasks/link_frame.c's
#: link_frame_trip_mask_for_reason() (`trip_mask` bit = reason-1) for the
#: wire encoding this reason value ultimately came from. Reasons 15/16
#: (SAFETY_TRIP_CONFIG_CORRUPT/SAFETY_TRIP_SELF_TEST) are deliberately
#: absent -- neither is an S<n> guard in SAFETY_MODEL.md's numbering at all.
_TRIP_REASON_TO_GUARD = {
    1: "S1",
    2: "S2",
    3: "S3",
    5: "S5",
    6: "S6a",
    7: "S6b",
    8: "S7",
    9: "S8",
    10: "S9",
    12: "S11",
    13: "S12",
    14: "S13",
}

#: devices.py's SAFETY_TRIP_INEFFECTIVE (S9, "the contactor is welded/
#: bypassed and current keeps flowing after a commanded trip") -- kept as a
#: plain int literal here rather than importing kilnctrl.devices at module
#: level (see the module docstring's lazy-import point); the value is
#: pinned by safety_guards.h's own "do not renumber" comment, so hardcoding
#: it here carries the same risk as hardcoding any other frozen wire
#: constant elsewhere in this package (e.g. protocol.py's own
#: SYS_REBOOT_BOOTLOADER_MAGIC).
_SAFETY_TRIP_INEFFECTIVE_REASON = 10


class GuardObserver:
    """Documents the minimal protocol :func:`kilnsim.runner.run_scenario`'s
    ``guard_observer=`` argument expects -- NOT meant to be instantiated;
    Python has no runtime-enforced structural typing worth adding a
    dependency for here, so this class exists purely as the readable
    reference for what an observer must provide. :class:`SafetyGuardObserver`
    below is this package's only real implementation; a test fake only
    needs to match this same one-method shape."""

    def observe(self) -> list:
        """Returns a list of newly-observed :class:`~kilnsim.protocol.Event`\\ s
        (may be empty) since the last call. Called at the same cadence as
        ``run_scenario()``'s own telemetry poll -- see that function's
        ``poll_interval_s``."""
        raise NotImplementedError


class SafetyGuardObserver:
    """Polls SaftyFW guard status through a kilnctrl
    :class:`~kilnctrl.safety.SafetyClient` and edge-detects it into
    GUARD_TRIP/GUARD_WARN/LINK_UP/TRIP_INEFFECTIVE_LATCHED events -- see the
    module docstring for why this has to be a second, independent link
    rather than anything SimFW's own EVT stream could ever carry.

    Two real, honestly-reported limitations, both inherited from what the
    firmware itself can currently tell the PC (not something this class
    could paper over without lying):

    * **GUARD_TRIP is precise; GUARD_WARN is not.** ``GET_DIAG``'s
      ``trip_mask`` sets one bit per ``safety_trip_t`` reason
      (``link_frame_trip_mask_for_reason()``), so a trip's guard identity is
      real. Its ``warn_mask`` is NOT the same: `firmware/SaftyFW/src/tasks/
      link_task.c`'s own comment on the wire-frame builder says plainly that
      this build's ``safety_guards.c`` "has no per-guard identity to report"
      for warnings at all -- ``warn_mask`` is a single aggregate bit ("is
      anything warning"), not a 13-guard mask. :meth:`_observe_warn` below
      reports ``{"guard": None}`` for exactly this reason: reporting a
      guessed guard name here would be worse than reporting none, because a
      scenario's ``event: {type: guard_warn, guard: "S4"}`` filter would
      then either wrongly match or wrongly miss depending on the guess,
      instead of correctly SKIPping (this observer genuinely cannot tell
      which guard is warning in this firmware build).
    * **No SaftyFW-reported "link recovered" edge exists to observe
      LINK_UP from cleanly beyond a plain flag read.** :class:`SafetyStatus`
      exposes ``link_up`` (a live flags bit, ``SafetyFlag.LINK_UP``), so
      :meth:`_observe_link` edge-detects False->True on that -- this is
      real signal, not fabricated, but it is a level read on every poll, not
      a push notification of the recovery instant; the reported event's
      timestamp is therefore only as precise as this observer's own poll
      cadence (``run_scenario()``'s ``poll_interval_s``), same caveat as
      every other edge this class produces.
    """

    def __init__(self, safety_client: "SafetyClient") -> None:
        # `safety_client`: an already-connected kilnctrl.safety.SafetyClient
        # (or any duck-typed equivalent exposing get_diag()/get_status()
        # with kilnctrl.devices' own SafetyDiag/SafetyStatus shapes) --
        # accepted as a plain constructor argument, never constructed here,
        # so this class never has an opinion about which serial port/UartLink
        # the caller used to reach the ESP<->Pico bridge. See
        # make_safety_guard_observer() below for the common case of building
        # one from a live kilnctrl UartLink directly.
        self._client = safety_client
        self.next_seq = GUARD_OBSERVER_SEQ_BASE
        self._last_trip_reason: Optional[int] = None
        self._last_warn_active: Optional[bool] = None
        self._last_link_up: Optional[bool] = None

    def observe(self) -> list:
        out: "list[Event]" = []

        try:
            diag = self._client.get_diag()
        except Exception:  # noqa: BLE001 - kilnctrl.safety.SafetyQueryError et al
            # SAFETY_CMD_GET_DIAG unreachable THIS poll -- kilnctrl.safety.
            # SafetyQueryError's own doc comment: this means the ESP didn't
            # answer, not that the isolated Pico link is down (a down link
            # is a normal, successful GET_DIAG/GET_STATUS reply with
            # link_up=False/ever_received=False -- see _observe_link/
            # _observe_trip's own None-guards for that case). A transient
            # PC<->ESP hiccup should not abort an otherwise-running
            # scenario; the next poll_interval_s tick tries again.
            log.debug("SafetyGuardObserver: GET_DIAG unreachable this poll", exc_info=True)
            diag = None
        if diag is not None and diag.ever_received:
            out.extend(self._observe_trip(diag))
            out.extend(self._observe_warn(diag))

        try:
            status = self._client.get_status()
        except Exception:  # noqa: BLE001
            log.debug("SafetyGuardObserver: GET_STATUS unreachable this poll", exc_info=True)
            status = None
        if status is not None:
            out.extend(self._observe_link(status))

        return out

    def _mk(self, event_type: EventType, payload: dict) -> Event:
        seq = self.next_seq
        self.next_seq += 1
        # Real wall-clock stamp (epoch seconds -> integer microseconds), NOT
        # a SimFW sim_time_us -- SaftyFW's guard state is on a wholly
        # separate device/clock domain from SimFW's simulation clock (module
        # docstring). This module has no idea what run_scenario()'s
        # start_wall/run_timescale even are, so it cannot produce a
        # SimFW-comparable sim_time_us itself; run_scenario() rescales this
        # onto the same "sim time since run start" axis SimFW's own wire
        # events use before the event enters the shared `collected` list --
        # see its own comment at that merge site for the (approximate, not
        # a synchronization claim) math.
        wall_time_us = int(_time.time() * 1_000_000)
        return Event(seq=seq, sim_time_us=wall_time_us, event_type=event_type, payload=payload)

    def _observe_trip(self, diag) -> list:
        out = []
        reason = int(diag.trip_reason)
        if reason != 0 and reason != self._last_trip_reason:
            guard = _TRIP_REASON_TO_GUARD.get(reason)
            payload = {"guard": guard, "trip_reason": reason}
            out.append(self._mk(EventType.GUARD_TRIP, dict(payload)))
            if reason == _SAFETY_TRIP_INEFFECTIVE_REASON:
                # S9: "the contactor didn't actually open" -- SafetyClient.
                # get_trip_event()'s own doc comment says to treat this
                # differently from an ordinary trip; TRIP_INEFFECTIVE_LATCHED
                # exists in kilnsim.protocol.EventType specifically for a
                # scenario to assert on that distinction, so emit both.
                out.append(self._mk(EventType.TRIP_INEFFECTIVE_LATCHED, dict(payload)))
        self._last_trip_reason = reason
        return out

    def _observe_warn(self, diag) -> list:
        out = []
        warn_active = bool(diag.warn_mask)
        if self._last_warn_active is not None and warn_active and not self._last_warn_active:
            # See this class's own docstring on why `guard` is honestly None
            # here, not a guess.
            out.append(self._mk(EventType.GUARD_WARN, {"guard": None}))
        self._last_warn_active = warn_active
        return out

    def _observe_link(self, status) -> list:
        out = []
        link_up = bool(status.link_up)
        if self._last_link_up is not None and link_up and not self._last_link_up:
            out.append(self._mk(EventType.LINK_UP, {}))
        self._last_link_up = link_up
        return out


def make_safety_guard_observer(uart_link) -> "SafetyGuardObserver":
    """Convenience factory: builds a kilnctrl ``SafetyClient`` over an
    already-open ``kilnctrl.serial_link.UartLink`` and wraps it.

    kilnctrl is imported HERE, inside this function body, not at module
    level -- see the module docstring's lazy-import point. Only a caller
    that actually wants a live SaftyFW observation source (this function)
    pays kilnctrl's import cost; every other kilnsim entry point, including
    :class:`SafetyGuardObserver` itself when constructed directly with an
    already-built client, does not.
    """
    from kilnctrl.safety import SafetyClient  # lazy: see module/function docstring

    return SafetyGuardObserver(SafetyClient(uart_link))
