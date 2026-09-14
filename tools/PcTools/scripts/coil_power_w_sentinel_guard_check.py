#!/usr/bin/env python3
"""Guard against zone_cfg_t::coil_power_w's 0.0f "not overridden" sentinel
being read as a real wattage anywhere new.

Background (docs/audits/joint_load_model_class_design_2026-09-11.md,
"R1. The total-power arithmetic" section): `coil_power_w` was added at
ZONES_CFG_VERSION 24->25 (dbd8ff52) as an OPTIONAL per-zone nameplate
override -- 0.0f means "not overridden, use an equal share of the
whole-kiln max_expected_power_w sum" (see zones_config_json.h's own field
comment and zones_http_post_parse.c's z%u_coilpower validation). This is a
real, wired, bidirectional field: the web UI (zones_page.html's `coilpower`
input) POSTs it, zones_http_post_parse.c validates and stores it,
zones_config_accessors.c persists it (NVS + cfg-fs dual-write, standard
pattern), and zones_http_get.c reads it back. It is NOT an instance of the
"consumer without producer" class documented in this repo's standing
practice notes -- an operator producer path exists end to end. Nobody has
used it yet because this bench's coils are nameplate-equal, which is the
expected, correct state, not a bug.

The one genuine hazard is the "reset one side of a pair" shape applied to a
sentinel instead of a counter: `coil_power_w`'s ONLY consumer today,
zone_sweep_expected_coil_current_a() (zones_current_sweep_engine.c), must
keep treating 0.0f as "fall back to the equal-share default" rather than as
a literal zero-watt coil (which would make every downstream
I_expected = P_share / mains_voltage_v silently evaluate to 0 A and report
a spurious nameplate mismatch against any real measured current). A joint
modelling document (the audit above) already almost made the same mistake
in the opposite direction -- treating the unset 0.0f as real, present data
for a P_total = sum(u_i * coil_power_w[i]) computation -- and caught itself
before landing any code.

This is NOT a general "does every declared field have a producer" check --
this repo has explicitly rejected over-broad checks of that shape before
(several per-zone fields are legitimately optional and default to 0/off).
What IS mechanically checkable, and is what this script enforces:

  1. `coil_power_w` must not appear in any firmware C/H file outside the
     known, reviewed plumbing (struct/JSON/HTTP/accessor layers) and the
     one reviewed consumer. A new file referencing the field is a NEW
     consumer or producer that has not been reviewed for the sentinel
     hazard above, so it fails loud rather than silently compiling.
  2. The one reviewed consumer must still contain the literal sentinel
     guard (`coil_power_w_override > 0.0f`) that distinguishes "overridden"
     from "not overridden" -- if that guard is ever weakened or removed,
     0.0f becomes a real wattage and every derived expected-current
     computation for every zone silently reads 0 A.

Usage: python coil_power_w_sentinel_guard_check.py <repo_root>
Exit 0 = clean, 1 = an unreviewed reference or a missing/weakened guard.
"""
from __future__ import annotations

import sys
from pathlib import Path

FIELD = "coil_power_w"

# Every firmware file allowed to reference coil_power_w at all. Each one has
# been read and reasoned about for this check; a new file appearing in a
# `grep -rl coil_power_w` run that isn't listed here has not been, so it
# fails rather than silently gaining a new producer/consumer.
ALLOWED_FILES = {
    "firmware/KilnFW/App/drivers/persist/zones_config_json.h",
    "firmware/KilnFW/App/drivers/persist/zones_config_json.c",
    "firmware/KilnFW/App/drivers/persist/zones_config_migrate.c",
    "firmware/KilnFW/App/drivers/persist/zones_config_accessors.c",
    "firmware/KilnFW/App/drivers/persist/zones_config_accessors.h",
    "firmware/KilnFW/App/drivers/persist/zones_http_internal.h",
    "firmware/KilnFW/App/drivers/http/zones_http_get.c",
    "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
    "firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c",
    "firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c",
    # 2026-09-14: test_zones_http.c's autotune_baseline_k_dc GET test
    # mentions coil_power_w only in a comment, listing it alongside
    # hyst_c/ease_off_window_mult as another 0-sentinel field on the same
    # endpoint that must be emitted raw -- it does not read, write, or do
    # arithmetic with the field. Reviewed: no new producer/consumer, no
    # sentinel hazard.
    "firmware/KilnFW/App/test/test_zones_http.c",
}

# The one file that actually consumes the value (does arithmetic with it,
# rather than just moving it between wire/storage formats), and the exact
# guard pattern that must still gate that arithmetic.
CONSUMER_FILE = "firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c"
REQUIRED_GUARD = "coil_power_w_override > 0.0f"

FIRMWARE_EXTS = (".c", ".h")
SEARCH_ROOTS = ["firmware/KilnFW"]


def check(repo_root: Path) -> list[str]:
    failures: list[str] = []

    for root_rel in SEARCH_ROOTS:
        root = repo_root / root_rel
        if not root.is_dir():
            failures.append(f"{root_rel}: directory not found -- check is stale")
            continue
        for path in root.rglob("*"):
            if path.suffix not in FIRMWARE_EXTS or not path.is_file():
                continue
            rel = path.relative_to(repo_root).as_posix()
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            if FIELD not in text:
                continue
            if rel not in ALLOWED_FILES:
                failures.append(
                    f"{rel}: references {FIELD!r} but is not in ALLOWED_FILES -- "
                    "this is a new producer/consumer that has not been reviewed for "
                    "the 0.0f 'not overridden' sentinel hazard (see this script's "
                    "module docstring). Review it, then add it to ALLOWED_FILES."
                )

    consumer_path = repo_root / CONSUMER_FILE
    if not consumer_path.is_file():
        failures.append(f"{CONSUMER_FILE}: file not found -- check is stale, update CONSUMER_FILE")
    else:
        text = consumer_path.read_text(encoding="utf-8", errors="ignore")
        if REQUIRED_GUARD not in text:
            failures.append(
                f"{CONSUMER_FILE}: missing required sentinel guard {REQUIRED_GUARD!r} -- "
                f"without it, {FIELD}=0.0f ('not overridden') is read as a real zero-watt "
                "coil instead of falling back to the equal-share default, so every "
                "derived expected current silently becomes 0 A"
            )

    return failures


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: coil_power_w_sentinel_guard_check.py <repo_root>", file=sys.stderr)
        return 2
    repo_root = Path(argv[1]).resolve()
    failures = check(repo_root)
    if failures:
        print("COIL_POWER_W SENTINEL GUARD CHECK: FAILED")
        for f in failures:
            print(f"  {f}")
        return 1
    print(
        "COIL_POWER_W SENTINEL GUARD CHECK: ok -- no unreviewed references, "
        "sentinel guard intact"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
