#!/usr/bin/env python3
"""Bench commissioning script for the RP2040 safety-processor current-sense
channels -- `firmware/SaftyFW/docs/CURRENT_SENSE.md` section 5, "Commissioning
check" (5-step per-channel procedure).

**UNEXERCISED AGAINST REAL HARDWARE.** No MAX31856/CT wiring exists on the
bench yet and no RP2040 SaftyFW firmware answers `SAFETY_CMD_GET_STATUS` with
real current data at the time this script was written (`safety.py`'s own
docstring: "The Pico firmware that answers this protocol does not exist in
this repository yet"). This script is groundwork written in anticipation of
that hardware -- it has only been exercised by: (a) importing cleanly, (b)
its CLI/argument parsing, and (c) unit tests against synthetic/mocked
readings (`tools/PcTools/tests/test_current_sense_commissioning.py`). Do not
treat a clean run of this file as evidence the current-sense hardware works.

What this automates, per `CURRENT_SENSE.md` Sec.5:
  1. All relays off: record each channel's current_a as a baseline "zero".
     NOTE: the wire protocol (`LINK_PROTOCOL.md` Frame A / `SafetyStatus.
     current_a`) only carries the firmware's already zero-subtracted,
     clamped-at-0 amps value -- not the raw `zero_counts` ADC table entry
     itself. This script's "zero check" therefore confirms current_a stays
     small with everything off and cold, not the flash-resident zero_counts
     constant CURRENT_SENSE.md's table describes. That constant is only
     visible on the RP2040 side (no MCP tool reads it back -- see below).
  2. Command one relay on at 100% duty; confirm EXACTLY ONE channel responds
     and it is the expected one. This is the step that gates S3/S4 per the
     doc ("until it passes on all three channels, S3 and S4 must be left
     disabled").
  3. Manual: display the computed amps, prompt the operator for a clamp-meter
     reading, and record both plus the delta. This script does NOT attempt to
     read a clamp meter, and it does NOT write `k_ct_v_per_a` back to the
     RP2040 -- grepping this repo's `mcp_server.py` for anything like
     `safety_set_calibration`/`current_sense_set_zero` turned up nothing: no
     MCP tool exists to push calibration constants to flash yet. The operator
     must hand-enter the recorded values elsewhere (or apply them once such
     tooling exists).
  4. Command the relay off; confirm decay to <5% of the on-reading within
     ~4 s.
  5. Repeat steps 2-4 for the remaining channels.

Usage:
    uv run --project tools/PcTools python tools/PcTools/scripts/current_sense_commissioning.py
    uv run --project tools/PcTools python tools/PcTools/scripts/current_sense_commissioning.py \\
        --relay-map 1:0,2:1,3:2 --threshold-a 2.0 --settle-s 1.0
"""

from __future__ import annotations

import argparse
import logging
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Optional, Sequence

sys.path.insert(0, __file__.rsplit("scripts", 1)[0] + "src")

from kilnctrl.io_expander import IoClient  # noqa: E402
from kilnctrl.link_hub import get_shared_link  # noqa: E402
from kilnctrl.safety import SafetyClient, SafetyQueryError  # noqa: E402

log = logging.getLogger(__name__)

PROJECT_DIR = Path(__file__).resolve().parents[2]
LOG_DIR = PROJECT_DIR / "logs" / "commissioning"

CHANNEL_COUNT = 3
RELAY_COUNT = 4

#: Default relay -> current-sense-channel expectation. This mapping is board
#: wiring, not something any doc in this repo has confirmed yet (no CT
#: hardware attached), so it is deliberately overridable via --relay-map
#: rather than presented as authoritative.
DEFAULT_RELAY_MAP: dict[int, int] = {1: 0, 2: 1, 3: 2}

DEFAULT_THRESHOLD_A = 2.0  # CURRENT_SENSE.md Sec.5 i_present_a, "typical"
DEFAULT_DECAY_PCT = 5.0
DEFAULT_DECAY_DEADLINE_S = 4.0
DEFAULT_SETTLE_S = 1.0
DEFAULT_ZERO_SAMPLES = 5


# ---------------------------------------------------------------------------
# Pure logic -- no serial/IO involved, exercised by the pytest file with
# synthetic readings.
# ---------------------------------------------------------------------------
@dataclass
class Step2Result:
    responded: list[int]
    expected_channel: int
    passed: bool
    message: str


def check_single_response(
    currents: Sequence[float], expected_channel: int, threshold_a: float
) -> Step2Result:
    """Step 2's decision: with one relay commanded on, exactly one channel
    must read above `threshold_a`, and it must be `expected_channel`.

    `currents` is the zero-subtracted, clamped-at-0 amps reading per channel
    (SafetyStatus.current_a), same shape and index order as the wire's
    current1..3 fields (`LINK_PROTOCOL.md` Sec.6 Frame A).
    """
    responded = [i for i, a in enumerate(currents) if a > threshold_a]

    if len(responded) == 0:
        return Step2Result(
            responded, expected_channel, False,
            f"no channel responded (all currents <= {threshold_a:.2f} A) -- "
            "check the relay actually switched and the CT is fitted",
        )
    if len(responded) > 1:
        return Step2Result(
            responded, expected_channel, False,
            f"channels {responded} all responded (expected exactly one) -- "
            "possible cross-talk, shared conductor, or a CT clamped around "
            "the wrong/multiple conductors",
        )
    got = responded[0]
    if got != expected_channel:
        return Step2Result(
            responded, expected_channel, False,
            f"channel {got} responded when the expected channel was "
            f"{expected_channel} -- CT likely plugged into the wrong jack",
        )
    return Step2Result(
        responded, expected_channel, True,
        f"channel {got} responded as expected",
    )


@dataclass
class DecaySample:
    elapsed_s: float
    current_a: float


@dataclass
class DecayResult:
    passed: bool
    pct_remaining: Optional[float]
    elapsed_s: Optional[float]
    message: str


def decay_pct_remaining(baseline_a: float, sample_a: float) -> float:
    """Fraction of the on-state reading still present, as a percentage.

    Returns 0.0 when baseline is non-positive (nothing to decay from) rather
    than dividing by zero -- a caller with a zero/negative baseline has a
    different problem (step 2 should have already failed).
    """
    if baseline_a <= 0:
        return 0.0
    return max(0.0, sample_a) / baseline_a * 100.0


def check_decay(
    baseline_a: float,
    samples: Sequence[DecaySample],
    threshold_pct: float = DEFAULT_DECAY_PCT,
    deadline_s: float = DEFAULT_DECAY_DEADLINE_S,
) -> DecayResult:
    """Step 4's decision: after commanding the relay off, does the reading
    fall below `threshold_pct` of the on-state baseline within `deadline_s`?

    `samples` must be in increasing `elapsed_s` order (time since the relay
    was commanded off). Passes on the first sample that both meets the
    threshold and is within the deadline.
    """
    if baseline_a <= 0:
        return DecayResult(
            False, None, None,
            f"baseline current {baseline_a:.2f} A is not positive -- "
            "cannot evaluate decay from an on-state that never registered",
        )
    for s in samples:
        pct = decay_pct_remaining(baseline_a, s.current_a)
        if s.elapsed_s <= deadline_s and pct <= threshold_pct:
            return DecayResult(
                True, pct, s.elapsed_s,
                f"decayed to {pct:.1f}% within {s.elapsed_s:.2f}s "
                f"(threshold {threshold_pct:.1f}% within {deadline_s:.1f}s)",
            )
    if samples:
        last = samples[-1]
        pct = decay_pct_remaining(baseline_a, last.current_a)
        return DecayResult(
            False, pct, last.elapsed_s,
            f"still at {pct:.1f}% after {last.elapsed_s:.2f}s (threshold "
            f"{threshold_pct:.1f}% within {deadline_s:.1f}s) -- a much "
            "slower decay than expected suggests C57 or R77 is wrong "
            "(CURRENT_SENSE.md Sec.5)",
        )
    return DecayResult(False, None, None, "no decay samples were collected")


@dataclass
class ZeroResult:
    currents: list[float]
    passed: bool
    message: str


def check_zero(currents: Sequence[float], max_expected_a: float) -> ZeroResult:
    """Step 1's decision: with everything off and cold, each channel's
    (already zero-subtracted) reading should be a small, stable value -- not
    exactly 0 (op-amp Vos / D14 leakage), but well under `max_expected_a`."""
    bad = [i for i, a in enumerate(currents) if a > max_expected_a]
    if bad:
        return ZeroResult(
            list(currents), False,
            f"channel(s) {bad} read above {max_expected_a:.2f} A with "
            "everything off -- zero_counts likely drifted or a channel is "
            "stuck reading a phantom current (CURRENT_SENSE.md Sec.5)",
        )
    return ZeroResult(list(currents), True, "all channels read a small, stable value")


@dataclass
class ChannelResult:
    channel: int
    relay: int
    zero: Optional[ZeroResult] = None
    step2: Optional[Step2Result] = None
    manual_amps: Optional[float] = None
    manual_delta_a: Optional[float] = None
    decay: Optional[DecayResult] = None

    @property
    def passed(self) -> bool:
        """All *automated* steps (1, 2, 4) passed. Step 3 is cosmetic per the
        doc ("This step only affects the power estimate") and never gates
        this."""
        return bool(
            self.zero and self.zero.passed
            and self.step2 and self.step2.passed
            and self.decay and self.decay.passed
        )


def s3_s4_safe(results: Sequence[ChannelResult]) -> bool:
    """CURRENT_SENSE.md Sec.5: "Step 2 is the one that gates the guards ...
    until it passes on all three channels, S3 and S4 must be left disabled."
    """
    return len(results) >= CHANNEL_COUNT and all(
        r.step2 is not None and r.step2.passed for r in results
    )


# ---------------------------------------------------------------------------
# Bench I/O -- talks to the shared UART link. Not covered by unit tests.
# ---------------------------------------------------------------------------
class _Logger:
    """Tees print() to stdout and a timestamped file, matching the
    console_capture.py / other bench-script convention of leaving a permanent
    record under tools/PcTools/logs/."""

    def __init__(self, log_dir: Path = LOG_DIR) -> None:
        log_dir.mkdir(parents=True, exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.path = log_dir / f"current_sense_commissioning_{ts}.log"
        self._fh = open(self.path, "w", encoding="utf-8")

    def line(self, text: str = "") -> None:
        print(text)
        self._fh.write(text + "\n")
        self._fh.flush()

    def close(self) -> None:
        self._fh.close()


def _read_currents(safety: SafetyClient) -> "tuple[float, float, float]":
    status = safety.get_status()
    return status.current_a


def _sample_zero(safety: SafetyClient, n: int, settle_s: float, out: _Logger) -> ZeroResult:
    readings: list[tuple[float, float, float]] = []
    for i in range(n):
        readings.append(_read_currents(safety))
        if i < n - 1:
            time.sleep(settle_s)
    avg = tuple(sum(r[c] for r in readings) / len(readings) for c in range(CHANNEL_COUNT))
    out.line(f"  zero samples: {readings}")
    out.line(f"  zero average: {[f'{a:.3f}' for a in avg]} A")
    return check_zero(avg, max_expected_a=DEFAULT_THRESHOLD_A)


def run_channel(
    channel: int,
    relay: int,
    safety: SafetyClient,
    io: IoClient,
    threshold_a: float,
    settle_s: float,
    decay_deadline_s: float,
    decay_threshold_pct: float,
    out: _Logger,
    interactive: bool,
) -> ChannelResult:
    out.line(f"\n=== Channel {channel} (relay {relay}) ===")
    result = ChannelResult(channel=channel, relay=relay)

    # Step 1
    out.line("-- Step 1: zero baseline (relay off) --")
    result.zero = _sample_zero(safety, DEFAULT_ZERO_SAMPLES, settle_s, out)
    out.line(f"  {'PASS' if result.zero.passed else 'FAIL'}: {result.zero.message}")

    # Step 2
    out.line(f"-- Step 2: relay {relay} ON at 100% duty --")
    relay_result = io.set_relay(relay, True)
    out.line(f"  SET_RELAY({relay}, on) -> {relay_result}")
    if not relay_result:
        result.step2 = Step2Result(
            [], channel, False,
            f"relay {relay} ON refused: {relay_result.describe()}",
        )
        out.line(f"  FAIL: {result.step2.message}")
        return result
    time.sleep(settle_s)
    currents = _read_currents(safety)
    out.line(f"  currents: {[f'{a:.3f}' for a in currents]} A")
    result.step2 = check_single_response(currents, channel, threshold_a)
    out.line(f"  {'PASS' if result.step2.passed else 'FAIL'}: {result.step2.message}")

    on_current = currents[channel]

    # Step 3 (manual)
    out.line("-- Step 3: manual clamp-meter comparison (cosmetic, power-estimate only) --")
    out.line(f"  computed amps on channel {channel}: {on_current:.3f} A")
    if interactive:
        try:
            raw = input(
                f"  enter clamp-meter reading for channel {channel} in amps "
                "(blank to skip): "
            ).strip()
        except EOFError:
            raw = ""
        if raw:
            try:
                measured = float(raw)
                result.manual_amps = measured
                result.manual_delta_a = on_current - measured
                out.line(
                    f"  recorded: computed={on_current:.3f} A measured={measured:.3f} A "
                    f"delta={result.manual_delta_a:+.3f} A"
                )
                out.line(
                    "  NOTE: no MCP tool exists to write k_ct_v_per_a back to the "
                    "RP2040 -- hand-apply this once such tooling exists."
                )
            except ValueError:
                out.line(f"  could not parse {raw!r} as a number -- skipping record")
        else:
            out.line("  skipped (no reading entered)")
    else:
        out.line("  non-interactive run: skipped")

    # Step 4
    out.line(f"-- Step 4: relay {relay} OFF, watching decay --")
    relay_off_result = io.set_relay(relay, False)
    out.line(f"  SET_RELAY({relay}, off) -> {relay_off_result}")
    if not relay_off_result:
        out.line(
            f"  WARNING: relay {relay} OFF refused: {relay_off_result.describe()} -- "
            "falling back to all_relays_off() before continuing"
        )
        io.all_relays_off()
    t0 = time.monotonic()
    samples: list[DecaySample] = []
    poll_s = max(0.1, decay_deadline_s / 20)
    while time.monotonic() - t0 <= decay_deadline_s + poll_s:
        elapsed = time.monotonic() - t0
        a = _read_currents(safety)[channel]
        samples.append(DecaySample(elapsed_s=elapsed, current_a=a))
        pct = decay_pct_remaining(on_current, a)
        out.line(f"  t+{elapsed:5.2f}s: {a:.3f} A ({pct:.1f}% of on-state)")
        if elapsed >= decay_deadline_s:
            break
        time.sleep(poll_s)
    result.decay = check_decay(on_current, samples, decay_threshold_pct, decay_deadline_s)
    out.line(f"  {'PASS' if result.decay.passed else 'FAIL'}: {result.decay.message}")

    out.line(f"=== Channel {channel} overall: {'PASS' if result.passed else 'FAIL'} ===")
    return result


def parse_relay_map(text: str) -> dict[int, int]:
    """Parse '--relay-map 1:0,2:1,3:2' into {relay: channel}."""
    mapping: dict[int, int] = {}
    for pair in text.split(","):
        pair = pair.strip()
        if not pair:
            continue
        relay_s, chan_s = pair.split(":")
        mapping[int(relay_s)] = int(chan_s)
    return mapping


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=(
            "Bench commissioning check for SaftyFW current-sense channels "
            "(CURRENT_SENSE.md Sec.5). UNEXERCISED against real hardware -- "
            "see this file's module docstring."
        )
    )
    p.add_argument(
        "--relay-map", default="1:0,2:1,3:2",
        help="relay->channel expectation as 'relay:chan,...' (default 1:0,2:1,3:2). "
             "Channel indices are 0-based, matching SafetyStatus.current_a order.",
    )
    p.add_argument("--threshold-a", type=float, default=DEFAULT_THRESHOLD_A,
                    help=f"i_present_a-style response threshold (default {DEFAULT_THRESHOLD_A})")
    p.add_argument("--decay-pct", type=float, default=DEFAULT_DECAY_PCT,
                    help=f"decay pass threshold, %% of on-state (default {DEFAULT_DECAY_PCT})")
    p.add_argument("--decay-deadline-s", type=float, default=DEFAULT_DECAY_DEADLINE_S,
                    help=f"decay must reach threshold within this many seconds (default {DEFAULT_DECAY_DEADLINE_S})")
    p.add_argument("--settle-s", type=float, default=DEFAULT_SETTLE_S,
                    help=f"settle time after a relay command / between zero samples (default {DEFAULT_SETTLE_S})")
    p.add_argument("--non-interactive", action="store_true",
                    help="skip the step-3 clamp-meter prompt (for smoke-testing the flow)")
    p.add_argument("--log-dir", default=None,
                    help="override the log directory (default tools/PcTools/logs/commissioning/)")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")

    relay_map = parse_relay_map(args.relay_map)
    log_dir = Path(args.log_dir) if args.log_dir else LOG_DIR
    out = _Logger(log_dir)

    out.line("=== SaftyFW current-sense commissioning (CURRENT_SENSE.md Sec.5) ===")
    out.line(
        "UNEXERCISED against real hardware -- no MAX31856/CT wiring or RP2040 "
        "firmware answering with real current data exists at the time of this run."
    )
    out.line(f"relay map (relay -> expected channel): {relay_map}")
    out.line(f"threshold_a={args.threshold_a} decay_pct={args.decay_pct} "
             f"decay_deadline_s={args.decay_deadline_s} settle_s={args.settle_s}")
    out.line(f"log file: {out.path}\n")

    link = get_shared_link()
    safety = SafetyClient(link)
    io = IoClient(link)

    results: list[ChannelResult] = []
    try:
        out.line("--- Pre-flight: all relays off ---")
        io.all_relays_off()
        time.sleep(args.settle_s)

        for relay, channel in sorted(relay_map.items(), key=lambda kv: kv[1]):
            try:
                results.append(
                    run_channel(
                        channel, relay, safety, io,
                        threshold_a=args.threshold_a,
                        settle_s=args.settle_s,
                        decay_deadline_s=args.decay_deadline_s,
                        decay_threshold_pct=args.decay_pct,
                        out=out,
                        interactive=not args.non_interactive,
                    )
                )
            except SafetyQueryError as exc:
                out.line(f"  ERROR: SAFETY query failed for channel {channel}: {exc}")
                results.append(ChannelResult(channel=channel, relay=relay))
    finally:
        out.line("\n--- Restoring: all relays off ---")
        try:
            io.all_relays_off()
        except Exception:  # noqa: BLE001 - best-effort cleanup
            log.exception("failed to switch all relays off during cleanup")
        safety.close()
        io.close()

    out.line("\n=== Summary ===")
    for r in results:
        out.line(f"channel {r.channel} (relay {r.relay}): {'PASS' if r.passed else 'FAIL'}")
        if r.zero:
            out.line(f"  step1 (zero):  {'PASS' if r.zero.passed else 'FAIL'} -- {r.zero.message}")
        if r.step2:
            out.line(f"  step2 (route): {'PASS' if r.step2.passed else 'FAIL'} -- {r.step2.message}")
        if r.manual_amps is not None:
            out.line(f"  step3 (clamp): computed vs measured delta {r.manual_delta_a:+.3f} A (cosmetic)")
        if r.decay:
            out.line(f"  step4 (decay): {'PASS' if r.decay.passed else 'FAIL'} -- {r.decay.message}")

    safe = s3_s4_safe(results)
    out.line("")
    if safe:
        out.line("S3/S4 GATING: step 2 passed on all channels -- S3/S4 MAY be enabled "
                 "per CURRENT_SENSE.md Sec.5.")
    else:
        out.line("S3/S4 GATING: step 2 did NOT pass on all channels -- per "
                 "CURRENT_SENSE.md Sec.5, S3 and S4 MUST be left disabled.")

    out.close()
    return 0 if all(r.passed for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
