"""Python port of ``firmware/KilnFW/App/drivers/cone_table.c``/``.h``
(commit d01dfe9), for the "ramp assist" simulator (§7 of
``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md``).

MIRROR DISCIPLINE: this module must reproduce the C implementation's table
values, band definition and Arrhenius weight formula EXACTLY -- if this and
the C diverge, the simulator built on top of it (``ramp_assist.py``) proves
nothing about the feature it is meant to validate. Every function below is a
line-by-line port of its ``cone_table_*`` counterpart; see ``cone_table.h``'s
module docstring for the full rationale (Orton self-supporting 108 F/hr
table, non-uniform cone spacing, Ea=300 kJ/mol Arrhenius approximation).

PRECISION NOTE: the C implementation uses ``float`` (32-bit); this port uses
Python's native ``float`` (64-bit/double) throughout, since there is no C
compiler available in this environment to build a byte-exact float32
reference and 64-bit arithmetic is the natural choice for a Python model
that will itself be compared against noisy simulated/measured data at a much
coarser tolerance than float32-vs-float64 rounding. This is a known,
documented divergence from the C build, not an oversight: the two should
agree to several significant figures (float32 has ~7 decimal digits of
precision) but not bit-for-bit. ``tests/test_cone_table.py`` pins values
computed independently from the documented formula (not by importing this
module's own helpers), so a *structural* divergence -- wrong table entry,
wrong band definition, wrong Ea, Celsius used instead of Kelvin in the
exponent -- is caught; float32-vs-float64 rounding is not something a test
at that tolerance would catch, nor is it the kind of divergence that would
make the simulator's conclusions wrong.

No FreeRTOS/ESP-IDF equivalent here obviously -- pure data plus pure math,
matching the C module's own "no I/O, no globals, no dynamic allocation"
discipline in spirit (this module has no mutable state either).
"""
from __future__ import annotations

import math
from typing import List, Tuple

# ---------------------------------------------------------------------------
# Constants -- verbatim from cone_table.c
# ---------------------------------------------------------------------------

GAS_CONSTANT_J_PER_MOL_K = 8.314
EA_J_PER_MOL = 300000.0
ABS_ZERO_C = -273.15

# Orton self-supporting cone equivalents at 108 F/hr (60 C/hr), converted to
# Celsius -- verbatim transcription of cone_table.c's s_cones[] (cone 022
# .. cone 14, CONE_TABLE_COUNT == 36). Spacing is intentionally non-uniform;
# do not smooth or re-round these.
CONE_TABLE: List[Tuple[str, float]] = [
    ("022", 586.1), ("021", 600.0), ("020", 626.1), ("019", 677.8),
    ("018", 715.0), ("017", 737.8), ("016", 772.2), ("015", 791.1),
    ("014", 807.2), ("013", 837.2), ("012", 861.1), ("011", 875.0),
    ("010", 902.8), ("09", 920.0), ("08", 942.2), ("07", 976.1),
    ("06", 997.8), ("05", 1031.1), ("04", 1062.8), ("03", 1086.1),
    ("02", 1102.2), ("01", 1118.9), ("1", 1137.2), ("2", 1142.2),
    ("3", 1152.2), ("4", 1162.2), ("5", 1186.1), ("6", 1222.2),
    ("7", 1238.9), ("8", 1248.9), ("9", 1260.0), ("10", 1285.0),
    ("11", 1293.9), ("12", 1306.1), ("13", 1331.1), ("14", 1365.0),
]

CONE_TABLE_COUNT = len(CONE_TABLE)
assert CONE_TABLE_COUNT == 36, "cone_table.c documents CONE_TABLE_COUNT == 36"


class ConeTableError(ValueError):
    """Raised for the same failure modes ``cone_table_status_t`` reports in
    C (out-of-range low/high, invalid input) -- Python raises rather than
    returning a status enum, but every raise site below is commented with
    the C status code it mirrors."""


def cone_for_temp_c(temp_c: float) -> int:
    """Mirrors ``cone_table_cone_for_temp_c``: hottest cone whose equivalent
    temperature is <= temp_c. Above the highest cone is NOT an error (clamps
    to the last index); below the lowest cone raises (CONE_TABLE_ERR_OUT_OF_
    RANGE_LOW)."""
    if not math.isfinite(temp_c):
        raise ConeTableError("invalid_input")  # CONE_TABLE_ERR_INVALID_INPUT
    if temp_c < CONE_TABLE[0][1]:
        raise ConeTableError("out_of_range_low")  # CONE_TABLE_ERR_OUT_OF_RANGE_LOW
    for i in range(CONE_TABLE_COUNT - 1, -1, -1):
        if temp_c >= CONE_TABLE[i][1]:
            return i
    raise ConeTableError("out_of_range_low")  # unreachable, kept for parity with the C guard


def band_bottom_c(target_c: float) -> float:
    """Mirrors ``cone_table_band_bottom_c``: half-cone-step band bottom --
    halfway from ``target_c`` DOWN to the equivalent temperature of the next
    lower cone. ``target_c`` need not be an exact table entry; the lower
    bracketing cone is used directly (see cone_table.h's comment on this
    function -- deliberately NOT a linear interpolation of the band width
    itself, just of which lower cone anchors it)."""
    if not math.isfinite(target_c):
        raise ConeTableError("invalid_input")  # CONE_TABLE_ERR_INVALID_INPUT
    if target_c <= CONE_TABLE[0][1]:
        raise ConeTableError("out_of_range_low")  # CONE_TABLE_ERR_OUT_OF_RANGE_LOW
    if target_c > CONE_TABLE[-1][1]:
        raise ConeTableError("out_of_range_high")  # CONE_TABLE_ERR_OUT_OF_RANGE_HIGH

    lower_idx = None
    for i in range(CONE_TABLE_COUNT - 1, -1, -1):
        if CONE_TABLE[i][1] < target_c:
            lower_idx = i
            break
    if lower_idx is None:
        raise ConeTableError("out_of_range_low")  # unreachable, parity guard

    lower_temp_c = CONE_TABLE[lower_idx][1]
    return target_c - (target_c - lower_temp_c) / 2.0


def _arrhenius_rate(temp_k: float) -> float:
    """``exp(-Ea / (R * T))``, T in Kelvin. Mirrors ``arrhenius_rate`` in
    cone_table.c exactly. Caller guarantees T > 0."""
    return math.exp(-EA_J_PER_MOL / (GAS_CONSTANT_J_PER_MOL_K * temp_k))


def heat_work_weight(current_c: float, target_c: float) -> float:
    """Mirrors ``cone_table_heat_work_weight``: relative heat-work rate in
    [0, 1], 1.0 at/above target, 0.0 at/below the half-cone-step band
    bottom, Arrhenius-shaped in between. current_c/target_c must be finite
    and above absolute zero (mirrors the C guard, which exists because the
    Arrhenius form divides by absolute temperature in Kelvin)."""
    if not (math.isfinite(current_c) and math.isfinite(target_c)):
        raise ConeTableError("invalid_input")  # CONE_TABLE_ERR_INVALID_INPUT
    if current_c <= ABS_ZERO_C or target_c <= ABS_ZERO_C:
        raise ConeTableError("invalid_input")  # CONE_TABLE_ERR_INVALID_INPUT

    band_bottom = band_bottom_c(target_c)  # propagates band-bottom's own errors
    if band_bottom <= ABS_ZERO_C:
        raise ConeTableError("invalid_input")  # CONE_TABLE_ERR_INVALID_INPUT

    if current_c >= target_c:
        return 1.0
    if current_c <= band_bottom:
        return 0.0

    t_target_k = target_c - ABS_ZERO_C
    t_bottom_k = band_bottom - ABS_ZERO_C
    t_current_k = current_c - ABS_ZERO_C

    rate_target = _arrhenius_rate(t_target_k)
    rate_bottom = _arrhenius_rate(t_bottom_k)
    rate_current = _arrhenius_rate(t_current_k)

    denom = rate_target - rate_bottom
    if denom <= 0.0 or not math.isfinite(denom):
        # Degenerate band -- no meaningful rate span to normalise against.
        # Mirrors the C function's "treat as no credit" fallback.
        return 0.0

    weight = (rate_current - rate_bottom) / denom
    if weight < 0.0:
        weight = 0.0
    elif weight > 1.0:
        weight = 1.0
    return weight
