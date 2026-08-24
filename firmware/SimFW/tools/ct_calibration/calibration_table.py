"""Calibration table persistence -- PC-side JSON file, not fixture flash.

**Persistence decision, spelled out here because DESIGN_NOTES.md 3.3 says "store the
table in fixture flash keyed by channel" and this module deliberately does
NOT do that.** `firmware/SimFW/src/` has no config/flash-persistence
subsystem at all today (unlike `firmware/SaftyFW/src/config_store.c` +
`config_store_flash.c`, the established pattern in this codebase) -- grepped
for at the time this module was written, zero hits. Building one is a real
firmware subsystem (flash sector layout, wear-levelling or at least a
write-count budget, a load-at-boot path, a wire command to push a new table)
and is explicitly out of this task's scope (see the constraint against
touching `firmware/SimFW/src/**`). Writing a versioned file the PC keeps and
loads at connect time is useful *today* with zero firmware risk, and it is a
strict subset of what flash persistence would eventually need to hold, so
nothing here is wasted if/when that firmware work happens -- see this
package's README, "Remaining firmware work", for exactly what that would
involve.

Schema is intentionally boring (plain JSON, no numpy/pickle) so it is
diffable in a PR and readable without this package installed.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from fit import LinearFit

#: Bumped only on an incompatible layout change. `load()` refuses a file
#: with a newer major-incompatible version rather than silently
#: misinterpreting fields (the same "the code wins, but only after
#: refusing bad input" instinct as the rest of this package's fit/crosstalk
#: gates).
SCHEMA_VERSION = 1


class CalibrationTableError(ValueError):
    """Raised for a table that cannot be saved or loaded safely."""


@dataclass(frozen=True)
class SweepPoint:
    commanded: float
    measured_a: float


@dataclass(frozen=True)
class ChannelCalibration:
    """One channel's accepted fit, plus the raw sweep it came from (kept so
    a human can re-plot/re-fit later without re-running the bench sweep)."""

    channel: int
    gain: float
    offset: float
    r2: float
    n_points: int
    max_abs_residual_a: float
    sweep: "tuple[SweepPoint, ...]"

    @classmethod
    def from_fit(cls, channel: int, fit: LinearFit, sweep: "list[SweepPoint]") -> "ChannelCalibration":
        return cls(
            channel=channel,
            gain=fit.gain,
            offset=fit.offset,
            r2=fit.r2,
            n_points=fit.n,
            max_abs_residual_a=fit.max_abs_residual,
            sweep=tuple(sweep),
        )

    def to_command(self, target_amps: float, lo: float = 0.0, hi: float = 1.0) -> float:
        """The commanded (today: raw PWM-scale 0..1) value that this
        channel's fit predicts will make the DUT read `target_amps`. This
        is the calculation `wave_owner.c`'s calibrated
        `ct_wave_amps_to_pwm_scale()` needs to perform per channel -- see
        this package's README."""
        if self.gain == 0:
            raise CalibrationTableError(f"channel {self.channel} has zero gain, cannot invert")
        raw = (target_amps - self.offset) / self.gain
        return min(hi, max(lo, raw))


@dataclass(frozen=True)
class CalibrationTable:
    schema_version: int
    created_at: str
    crosstalk_passed: bool
    channels: "dict[int, ChannelCalibration]"
    notes: str = ""
    #: Identifies the physical CT this table's channels were swept against
    #: (docs/PLAN.md section 11 item 12). "" means "not recorded" -- either
    #: an older table written before this field existed, or a run where
    #: nobody supplied one; both collapse to the same "unknown" state on
    #: load(), same as gen_ct_cal_table.load_ct_id(). A real id is never
    #: empty, so "" is an unambiguous sentinel here, no separate bool needed
    #: on the Python side (unlike ct_cal_table_t's ct_id_known in C, which
    #: exists so the *compiled-in* representation can't collide an
    #: accidentally-empty-but-"valid" id with "nobody recorded one").
    ct_id: str = ""

    @classmethod
    def new(
        cls,
        crosstalk_passed: bool,
        channels: "dict[int, ChannelCalibration]",
        notes: str = "",
        ct_id: str = "",
    ) -> "CalibrationTable":
        return cls(
            schema_version=SCHEMA_VERSION,
            created_at=datetime.now(timezone.utc).isoformat(),
            crosstalk_passed=crosstalk_passed,
            channels=channels,
            notes=notes,
            ct_id=ct_id,
        )

    def get(self, channel: int) -> ChannelCalibration:
        try:
            return self.channels[channel]
        except KeyError:
            raise CalibrationTableError(f"no calibration stored for channel {channel}") from None

    def to_dict(self) -> dict:
        return {
            "schema_version": self.schema_version,
            "created_at": self.created_at,
            "crosstalk_passed": self.crosstalk_passed,
            "notes": self.notes,
            "ct_id": self.ct_id,
            "channels": {
                str(ch): {
                    "gain": cal.gain,
                    "offset": cal.offset,
                    "r2": cal.r2,
                    "n_points": cal.n_points,
                    "max_abs_residual_a": cal.max_abs_residual_a,
                    "sweep": [
                        {"commanded": p.commanded, "measured_a": p.measured_a} for p in cal.sweep
                    ],
                }
                for ch, cal in self.channels.items()
            },
        }

    def save(self, path: "Path | str") -> None:
        """Refuses to write a table that was never validated as
        crosstalk-clean, or that carries zero channels -- there is no
        "partial, trust it anyway" mode. Individual channel entries are
        expected to already have passed `fit.evaluate_fit()` before being
        handed to `CalibrationTable.new()`; this is the last line of
        defence, not the primary gate (see `calibrate_ct.py`, which never
        constructs a `ChannelCalibration` for a rejected fit in the first
        place)."""
        if not self.crosstalk_passed:
            raise CalibrationTableError(
                "refusing to save a calibration table whose crosstalk check "
                "did not pass on all channels -- see CURRENT_SENSE.md Sec.5"
            )
        if not self.channels:
            raise CalibrationTableError("refusing to save a calibration table with zero channels")
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.to_dict(), indent=2, sort_keys=True) + "\n", encoding="utf-8")

    @classmethod
    def load(cls, path: "Path | str") -> "CalibrationTable":
        path = Path(path)
        raw = json.loads(path.read_text(encoding="utf-8"))
        version = raw.get("schema_version")
        if version != SCHEMA_VERSION:
            raise CalibrationTableError(
                f"{path}: schema_version {version!r} != supported {SCHEMA_VERSION}"
            )
        channels = {}
        for ch_s, cal in raw.get("channels", {}).items():
            channels[int(ch_s)] = ChannelCalibration(
                channel=int(ch_s),
                gain=cal["gain"],
                offset=cal["offset"],
                r2=cal["r2"],
                n_points=cal["n_points"],
                max_abs_residual_a=cal["max_abs_residual_a"],
                sweep=tuple(
                    SweepPoint(p["commanded"], p["measured_a"]) for p in cal.get("sweep", [])
                ),
            )
        return cls(
            schema_version=version,
            created_at=raw.get("created_at", ""),
            crosstalk_passed=bool(raw.get("crosstalk_passed", False)),
            channels=channels,
            notes=raw.get("notes", ""),
            # "" for a table written before this field existed (older,
            # already-checked-in tables must still load) -- indistinguishable
            # here from a table that explicitly recorded an empty id, which
            # is intentional: see this field's docstring above.
            ct_id=str(raw.get("ct_id", "") or ""),
        )
