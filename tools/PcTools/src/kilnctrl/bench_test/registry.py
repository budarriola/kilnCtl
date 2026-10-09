"""Case registry: the single source of truth for what a case id means.

``tools/check_bench_test_registry.ps1`` diffs every case id named in
``docs/BENCH_TEST_SYSTEM_PLAN.md``'s tables against ``REGISTRY`` in both
directions, so this module and the plan document cannot silently drift.
Wave 0 only *implements* the read-only cases in the ``smoke`` suite (see
``cases_smoke.py``); ids from later waves are still declared here (with
``judge=None``, meaning "not implemented yet") so the registry-vs-doc check
is honest about the whole 203-case matrix from day one rather than only
wave 0's slice of it.
"""
from __future__ import annotations

import dataclasses
from typing import Any, Callable, Dict, List, Optional


class Verdict:
    """The five verdicts a case can report (plan doc §2.3).

    Plain string constants rather than ``enum.Enum`` so ``summary.json``
    serializes them with no custom encoder -- ``json.dumps`` writes a
    ``Verdict.PASS`` string exactly as ``"PASS"``.
    """

    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"
    NOT_RUN = "NOT_RUN"
    INCONCLUSIVE = "INCONCLUSIVE"

    ALL = (PASS, FAIL, SKIP, NOT_RUN, INCONCLUSIVE)


@dataclasses.dataclass
class CaseResult:
    """What a case's judge function returns."""

    verdict: str
    reason: str = ""
    observed: Optional[Dict[str, Any]] = None
    expected: Optional[Dict[str, Any]] = None
    evidence: List[str] = dataclasses.field(default_factory=list)

    def __post_init__(self) -> None:
        if self.verdict not in Verdict.ALL:
            raise ValueError(f"unknown verdict {self.verdict!r}, must be one of {Verdict.ALL}")
        if self.verdict != Verdict.PASS and not self.reason:
            raise ValueError(f"a non-PASS verdict ({self.verdict}) must carry a reason")


@dataclasses.dataclass
class CaseSpec:
    """One row of the plan doc's case tables.

    ``judge`` is ``Callable[[dict], CaseResult]`` -- called with the run's
    shared ``ctx`` dict (host, previously-collected case
    outputs, etc.) -- or ``None`` for a case this wave has not implemented
    yet (the runner reports ``NOT_RUN: not_implemented`` for those without
    ever calling anything).
    """

    id: str
    area: str
    description: str
    judge: Optional[Callable[[dict], CaseResult]] = None
    heat: bool = False
    #: Reason the runner reports when allow_heat=False pre-skips this case
    #: (default "allow_heat=False").
    heat_skip_reason: str = ""
    depends_on: Optional[str] = None
    est_duration_s: float = 5.0
    operator_only: bool = False


def _c(id: str, area: str, description: str, **kw) -> CaseSpec:
    return CaseSpec(id=id, area=area, description=description, **kw)


#: Every case id named anywhere in docs/BENCH_TEST_SYSTEM_PLAN.md §3.
#: Wave 0 fills in `judge` for the smoke-suite ids only; every other id is
#: declared with `judge=None` so check_bench_test_registry.ps1 can confirm
#: the plan and this table agree on the full matrix, not just what runs
#: today.
REGISTRY: Dict[str, CaseSpec] = {}


def register(spec: CaseSpec) -> CaseSpec:
    if spec.id in REGISTRY:
        raise ValueError(f"duplicate case id {spec.id!r}")
    REGISTRY[spec.id] = spec
    return spec


def get_case(case_id: str) -> CaseSpec:
    try:
        return REGISTRY[case_id]
    except KeyError as exc:
        raise KeyError(f"unknown bench-test case id: {case_id!r}") from exc


#: suite name -> ordered list of case ids. Populated below and by
#: cases_smoke.py's import (which registers judge functions for the ids
#: declared here). Only ids that exist in REGISTRY may appear here --
#: enforced by suite_case_ids().
SUITES: Dict[str, List[str]] = {}


def suite_case_ids(suite: str) -> List[str]:
    try:
        ids = SUITES[suite]
    except KeyError as exc:
        raise KeyError(
            f"unknown suite {suite!r}, known suites: {sorted(SUITES)}"
        ) from exc
    for cid in ids:
        get_case(cid)  # raises if a suite lists an id that was never registered
    return list(ids)


# ---------------------------------------------------------------------------
# Static declarations for every id in the plan's tables (§3). Judge
# functions for the smoke set are wired in by cases_smoke.py at import time
# (see the bottom of that module) -- this keeps registry.py itself free of
# any board-touching imports, so importing it alone (e.g. from the
# check_bench_test_registry.ps1 companion pytest) never needs kilnctrl's
# hardware modules.
# ---------------------------------------------------------------------------

_ST = [
    ("ST-01", "Repo checks"),
    ("ST-02", "PcTools tests"),
    ("ST-03", "SaftyFW host tests"),
    ("ST-04", "KilnFW target build"),
    ("ST-05", "Tree provenance"),
]
_FL = [
    ("FL-01", "Partition table matches source"),
    ("FL-02", "Running partition is app"),
    ("FL-03", "app_desc build matches the archived flash"),
    ("FL-04", "boot_guard counter"),
    ("FL-05", "Recovery image present and sized"),
    ("FL-06", "Coredump partition readable"),
    ("FL-07", "cfg partition state"),
    ("FL-08", "Pico slot metadata"),
    ("FL-09", "Pico image vs archive"),
    ("FL-10", "ESP JTAG flash round trip (opt-in)"),
    ("FL-11", "Pico JTAG flash round trip (opt-in)"),
]
_SK = [
    ("SK-01", "ESP high-water marks, idle"),
    ("SK-02", "ESP high-water marks, exercised"),
    ("SK-03", "Pico task margins"),
    ("SK-04", "Heap and DRAM floor"),
]
_OT = [
    ("OT-E01", "Push to running app refused (409)"), ("OT-E02", "Rollback"),
    ("OT-E03", "Corrupt image: bad CRC"), ("OT-E04", "Corrupt image: truncated"),
    ("OT-E05", "Wrong-build image"), ("OT-E06", "Power loss mid-write"),
    ("OT-E07", "Update during a firing is refused"),
    ("OT-E08", "Update during autotune is refused"),
    ("OT-E09", "Auth: no credential"), ("OT-E10", "Auth: web auth on, session"),
    ("OT-E11", "Recovery image receives an app image"),
    ("OT-E12", "otadata state after each case"),
    ("OT-P01", "Relay update into inactive slot"),
    ("OT-P02", "Boot from new slot, then rollback"),
    ("OT-P03", "Bad image falls back"),
    ("OT-P04", "Erase-time watchdog case"),
    ("OT-P05", "Update with a trip pending is refused"),
    ("OT-B01", "Dual reset handshake trip"),
    ("OT-B02", "Regression wrapper"),
    ("OT-G01", "Staged upload sha256 equals the local digest"),
    ("OT-G02", "Truncated stage upload leaves nothing staged"),
    ("OT-G03", "Release downgrade refused without allow_downgrade"),
    ("OT-G04", "Stage upload during a firing is refused"),
    ("OT-G05", "Wrong release repo fails cleanly"),
    ("OT-G06", "Stale stage auto-cleared at boot"),
]
_AT = [
    ("AT-01", "Step test, zone 0, bounded"), ("AT-02", "Abort is immediate"),
    ("AT-03", "Accept is guarded"), ("AT-04", "Relay-feedback test"),
    ("AT-05", "Coupling matrix visible"),
]
_HP = [
    ("HP-01", "Single zone"), ("HP-02", "All zones"),
    ("HP-03", "On/off device zone"), ("HP-04", "Pause / resume"),
    ("HP-05", "Stop"), ("HP-06", "Stop requires login"),
    ("HP-07", "Faulted run"), ("HP-08", "Firing history"),
]
_WEB_IDS = [
    "WEB-DASH-01", "WEB-DASH-02", "WEB-DASH-03", "WEB-DASH-04", "WEB-DASH-05",
    "WEB-DASH-06", "WEB-DASH-07", "WEB-DASH-08", "WEB-DASH-09", "WEB-DASH-10",
    "WEB-DASH-11", "WEB-DASH-12", "WEB-DASH-13",
    "WEB-PROF-01", "WEB-PROF-02", "WEB-PROF-03", "WEB-PROF-04", "WEB-PROF-05",
    "WEB-PROF-06", "WEB-PROF-07", "WEB-PROF-08", "WEB-PROF-09", "WEB-PROF-10",
    "WEB-PROF-11",
    "WEB-ZONE-01", "WEB-ZONE-02", "WEB-ZONE-03", "WEB-ZONE-04", "WEB-ZONE-05",
    "WEB-ZONE-06", "WEB-ZONE-07", "WEB-ZONE-08", "WEB-ZONE-09", "WEB-ZONE-10",
    "WEB-ZONE-11", "WEB-ZONE-12", "WEB-ZONE-13", "WEB-ZONE-14",
    "WEB-SAF-01", "WEB-SAF-02", "WEB-SAF-03", "WEB-SAF-04",
    "WEB-STIM-01", "WEB-STIM-02",
    "WEB-COMM-01", "WEB-COMM-02", "WEB-COMM-03", "WEB-COMM-04", "WEB-COMM-05",
    "WEB-COMM-06", "WEB-COMM-07",
    "WEB-RDY-01", "WEB-RDY-02", "WEB-RDY-03", "WEB-RDY-04",
    "WEB-WIZ-01", "WEB-WIZ-02", "WEB-WIZ-03", "WEB-WIZ-04", "WEB-WIZ-05",
    "WEB-WIZ-06", "WEB-WIZ-07", "WEB-WIZ-08", "WEB-WIZ-09", "WEB-WIZ-10",
    "WEB-WIZ-11",
    "WEB-DIAG-01", "WEB-DIAG-02", "WEB-DIAG-03", "WEB-DIAG-04", "WEB-DIAG-05",
    "WEB-DIAG-06", "WEB-DIAG-07", "WEB-DIAG-08", "WEB-DIAG-09", "WEB-DIAG-10",
    "WEB-DIAG-11",
    "WEB-OTA-01", "WEB-OTA-02", "WEB-OTA-03", "WEB-OTA-04", "WEB-OTA-05",
    "WEB-OTA-06", "WEB-OTA-07", "WEB-OTA-08",
    "WEB-WIFI-01", "WEB-WIFI-02", "WEB-WIFI-03", "WEB-WIFI-04", "WEB-WIFI-05",
    "WEB-WIFI-06",
    "WEB-SEC-01", "WEB-SEC-02", "WEB-SEC-03", "WEB-SEC-04", "WEB-SEC-05",
    "WEB-SEC-06",
    "WEB-BAK-01", "WEB-BAK-02", "WEB-BAK-03", "WEB-BAK-04",
    "WEB-KCFG-01", "WEB-KCFG-02", "WEB-KCFG-03", "WEB-KCFG-04", "WEB-KCFG-05",
    "WEB-SET-01", "WEB-SET-02", "WEB-SET-03", "WEB-SET-04",
    "WEB-DISP-01", "WEB-DISP-02", "WEB-DISP-03", "WEB-DISP-04",
    "WEB-LOG-01", "WEB-LOG-02", "WEB-LOG-03",
    "WEB-X-01", "WEB-X-02", "WEB-X-03",
]
_LCD_IDS = [f"LCD-{i:02d}" for i in range(1, 27)]
_SP = [
    ("SP-01", "Commissioning read-back"), ("SP-02", "Status and diag consistency"),
    ("SP-03", "Link stats over a firing"), ("SP-04", "Trip / clear"),
    ("SP-05", "E-stop verify"), ("SP-06", "Heat enable path"),
    ("SP-07", "Rate guard read-back"), ("SP-08", "E-stop press"),
    ("SP-09", "Link-loss"), ("SP-10", "CT / S9 / S14 / S15"),
    ("SP-11", "Pico stack margins"),
]
#: TOTP password-reset routes (docs/TOTP_PASSWORD_RESET_PLAN.md sections 4/
#: 6a). Unit-tested against a fake board only -- see cases_totp.py's module
#: docstring. TP-M01 is the one mutating case: it is opt-in the same way
#: OT-E01's image-path gate and WEB-SEC-03/04's credential-presence gate
#: are opt-in -- it SKIPs unless KILNCTL_TOTP_CODE and
#: KILNCTL_WEB_PASSWORD_NEW are both set in the environment, never a
#: separate boolean flag.
_TP = [
    ("TP-R01", "GET /api/auth/totp_status has a boolean 'enrolled' field"),
    ("TP-R02", "POST /api/auth/forgot with a wrong code still answers 202/503/429"),
    ("TP-R03", "forgot/reset never require an admin session (OPEN tier)"),
    ("TP-M01", "Full TOTP reset round trip, opt-in via env credentials"),
]

#: Spare-relay aux outputs (docs/SPARE_RELAY_ONOFF_PLAN.md section 12, steps
#: 1-7). Unit-tested against a fake board only -- see cases_aux.py. All
#: writes are gated by KILNCTL_AUX_BENCH_CONFIRM=1; AX-T01/K01/T02 start a
#: bench profile (heat); AX-T02 is operator-only (a Pico trip has no injector).
_AX = [
    ("AX-C01", "Configure relay 4 as an aux output, read-back"),
    ("AX-C02", "Enabling aux on a zone-owned relay is refused (400)"),
    ("AX-C03", "Zone relay_mask containing relay 4 is refused (400)"),
    ("AX-T01", "Aux rule toggles relay 4 (io shadow), min on/off honoured"),
    ("AX-K01", "Relay 4 follows its rule while PAUSED (K4 independence)"),
    ("AX-T02", "A Pico trip drops relay 4"),
    ("AX-R01", "Restore: relay 4 aux disabled, no BENCH_AUX_RULE slot left"),
]

for cid, desc in _ST:
    register(_c(cid, "ST", desc))
for cid, desc in _FL:
    register(_c(cid, "FL", desc, operator_only=(cid in ("FL-10", "FL-11"))))
for cid, desc in _SK:
    # SK-02 ("exercised") needs httpd-heavy traffic actually happening, per
    # the plan's web_ui_open load condition -- HP-01 (a single-zone firing)
    # is the cheapest case in the suite that reliably drives that path, so
    # SK-02 depends on it rather than fabricating load itself (single-
    # dependency rule, plan §5.3 rule 1).
    register(_c(cid, "SK", desc, depends_on="HP-01" if cid == "SK-02" else None))
for cid, desc in _OT:
    register(_c(cid, "OT", desc, heat=cid in ("OT-E07", "OT-E08", "OT-G04"),
                heat_skip_reason=("allow_heat not set; OT-E07/E08/G04 start their own heat"
                                  if cid in ("OT-E07", "OT-E08", "OT-G04") else "")))
#: OT-P02 boots the slot OT-P01 relayed into and rolls it back, OT-P03 needs
#: OT-P01's captured commit_before to prove an untouched running image, and
#: OT-P04 is a pure observer over OT-P01's own captured relay data -- three
#: cases pinned to the one sanctioned OT-P01 dependency (plan section 5.3
#: rule 1), never each other. OT-P05 has no case dependency: it needs a real
#: trip pending (e.g. FL-11's S6a before it's cleared), which is a runtime
#: precondition its own judge reports INCONCLUSIVE for, not a prior case.
get_case("OT-P02").depends_on = "OT-P01"
get_case("OT-P03").depends_on = "OT-P01"
get_case("OT-P04").depends_on = "OT-P01"
#: AT-03 reads back the unsettled fit AT-02 (its short, self-aborted step
#: run) leaves behind; AT-05 reads GET /api/autotune/matrix after AT-01's
#: zone-0 fit populates it. Both are the one sanctioned dependency each
#: (plan section 5.3 rule 1) rather than trying to literally share another
#: case's live window (see cases_autotune.py's module docstring for why).
_AT_DEPENDS_ON = {"AT-03": "AT-02", "AT-05": "AT-01"}
_AT_DURATION_S = {"AT-01": 1200, "AT-02": 60, "AT-03": 5, "AT-04": 1200, "AT-05": 2}
for cid, desc in _AT:
    register(_c(
        cid, "AT", desc, heat=cid in ("AT-01", "AT-02", "AT-04"), est_duration_s=_AT_DURATION_S[cid],
        depends_on=_AT_DEPENDS_ON.get(cid),
    ))
for cid, desc in _HP:
    register(_c(cid, "HP", desc, heat=True, est_duration_s=360))
for cid in _WEB_IDS:
    # WEB-WIFI-06 (AP-mode toggle) needs a human to watch the AP fallback
    # come up and go back to normal -- operator_only, gated by --attended
    # via operator.require_attended() inside its case function.
    register(_c(cid, "WEB", cid, operator_only=(cid == "WEB-WIFI-06")))
_LCD_DEPENDS_ON = {
    "LCD-02": "HP-01", "LCD-03": "HP-04", "LCD-04": "OT-B01", "LCD-19": "WEB-SEC-04",
}
for cid in _LCD_IDS:
    register(_c(cid, "LCD", cid, depends_on=_LCD_DEPENDS_ON.get(cid), heat=(cid in ("LCD-22", "LCD-23", "LCD-24", "LCD-25"))))
_SP_DEPENDS_ON = {"SP-03": "HP-02", "SP-06": "HP-01", "SP-04": "OT-B01"}
for cid, desc in _SP:
    register(_c(
        cid, "SP", desc, heat=(cid == "SP-09"), operator_only=cid in ("SP-08", "SP-09"),
        depends_on=_SP_DEPENDS_ON.get(cid),
    ))
for cid, desc in _TP:
    register(_c(cid, "TP", desc))
_AX_HEAT = ("AX-T01", "AX-K01", "AX-T02")
for cid, desc in _AX:
    register(_c(
        cid, "AX", desc, heat=cid in _AX_HEAT, operator_only=(cid == "AX-T02"),
        depends_on=None if cid in ("AX-C01", "AX-R01") else "AX-C01",
        est_duration_s=150 if cid in _AX_HEAT else 5,
        heat_skip_reason="allow_heat not set; AX-T01/K01/T02 start a bench profile" if cid in _AX_HEAT else ""))

#: Fixed run-order ranks for `nightly`/`full` (plan §5.2): ST first, then
#: FL (read-only), then the fixed SK-01/03/04 subset, then SP (read-only),
#: then everything else, with the heat-originating areas (AT-*, HP-*) last
#: of all -- never alphabetical, since that would run heat cases (AT-*)
#: before read-only smoke-style checks (SK-*, SP-*, ST-*).
_ORDER_RANK = {"ST": 0, "FL": 1, "SK": 2, "SP": 3}


#: WEB-SEC-05 (lockout: 6 bad logins -> 429) denies every new address for a
#: while (memory project_login_lockout_saturation_accepted) -- it must run
#: dead last in ANY suite that includes it, after even the heat-originating
#: cases, so nothing else in the same run risks tripping over its own
#: lockout. This is a hard exception to the "heat last" rule below, not a
#: new general rule -- exactly one case gets this treatment.
_ALWAYS_LAST = "WEB-SEC-05"

#: TP-M01 (when its env credentials are set) changes the admin password and
#: does not restore it, so every later re-login in the same run would use a
#: stale KILNCTL_WEB_PASSWORD (cases_totp.py's module docstring). It sorts
#: after every other case, heat included, but still before WEB-SEC-05,
#: whose lockout would otherwise refuse TP-M01's own verifying login.
_LATE_MUTATING = frozenset({"TP-M01", "AX-R01"})


def _fixed_order(ids) -> List[str]:
    """Sort `ids` by the §5.2 fixed order, not alphabetically. Every
    heat-originating case (AT-*, HP-*, or any other case flagged
    ``heat=True``, e.g. OT-E07/OT-E08) sorts after every read-only case
    regardless of area; ties within a bucket fall back to plain id order.
    WEB-SEC-05 is a further, singular exception: it sorts after everything
    else, heat included (see _ALWAYS_LAST). TP-M01 sorts just before it
    (see _LATE_MUTATING)."""
    def key(cid: str) -> "tuple":
        if cid == _ALWAYS_LAST:
            return (3, 0, cid)
        if cid in _LATE_MUTATING:
            return (2, 0, cid)
        spec = REGISTRY[cid]
        return (1 if spec.heat else 0, _ORDER_RANK.get(spec.area, 7), cid)
    ordered = sorted(ids, key=key)
    return _after_their_dependencies(ordered)


def _after_their_dependencies(ordered: List[str]) -> List[str]:
    """Move every case whose ``depends_on`` names another case in the SAME
    list to sit immediately after that case, preserving the §5.2 order for
    everything else.

    Without this, the heat/read-only partition above sorts every observer
    (SP-06, SP-03, SP-04, LCD-02/03/04, SK-02, AT-03, AT-05 -- all
    read-only) *ahead* of the heat case it observes, so each one runs
    against a board where its dependency has not happened yet. That is the
    same exemption `_NIGHTLY_ORDER` already spells out by hand ("SP-04 ...
    deliberately sit inside the OTA block"); this generalizes it so the
    `full` suite cannot silently drift out of it as new dependent cases are
    registered. A dependency chain is followed transitively, and a
    dependency on a case not in this list is left alone (running one case
    on its own is still allowed).
    """
    remaining = list(ordered)
    placed: List[str] = []
    present = set(ordered)
    deferrals: Dict[str, int] = {}
    limit = len(ordered) + 1
    while remaining:
        cid = remaining.pop(0)
        dep = REGISTRY[cid].depends_on
        if deferrals.get(cid, 0) >= limit:
            # A dependency cycle (or a self-dependency): stop deferring and
            # place it, rather than spinning forever. Nothing registers one
            # today; this is a guard, not a supported configuration.
            placed.append(cid)
            continue
        if dep and dep in present and dep not in placed:
            # Its dependency is still ahead of us: defer this case until
            # just after the dependency lands.
            try:
                idx = remaining.index(dep)
            except ValueError:  # pragma: no cover - dep already consumed
                placed.append(cid)
                continue
            deferrals[cid] = deferrals.get(cid, 0) + 1
            remaining.insert(idx + 1, cid)
            continue
        placed.append(cid)
    return placed


#: The suite names bench_test_run()/bench_test.ps1 accept. `smoke` is fully
#: populated by cases_smoke.py below; `nightly`/`full` are declared per the
#: plan's §5.1 membership but, in wave 0, only actually contain the subset
#: of that membership this wave implements (their remaining ids report
#: NOT_RUN: not_implemented until later waves land judge functions for
#: them) -- so `bench_test_run(suite="nightly")` is honest today rather
#: than silently pretending to be the full nightly run from §5.1.
#: The 18 "-01" WEB render cases (17 pages + WEB-X-01's nav.js shape check)
#: plus WEB-X-03's generated tier sweep (plan §5.1's smoke membership) --
#: implemented by cases_web.py (Wave 1a). Every other WEB id stays
#: judge=None until a later wave.
_WEB_SMOKE_IDS = [cid for cid in _WEB_IDS if cid.endswith("-01")] + ["WEB-X-03"]

SUITES["smoke"] = [
    "ST-05",
    "FL-01", "FL-02", "FL-03", "FL-04", "FL-05", "FL-06", "FL-07", "FL-08", "FL-09",
    "SK-01", "SK-03", "SK-04",
    "SP-01", "SP-02", "SP-05", "SP-07",
    *_WEB_SMOKE_IDS,
]
SUITES["static"] = ["ST-01", "ST-02", "ST-03", "ST-04", "ST-05"]
SUITES["flash"] = [c for c in REGISTRY if c.startswith("FL-")]
SUITES["stack"] = [c for c in REGISTRY if c.startswith("SK-")]
SUITES["ota"] = [c for c in REGISTRY if c.startswith("OT-") and c != "OT-B02"] + ["OT-B02"]  # summary runs last
SUITES["autotune"] = [c for c in REGISTRY if c.startswith("AT-")]
SUITES["heat"] = [c for c in REGISTRY if c.startswith("HP-")]
SUITES["web"] = list(_WEB_IDS)
SUITES["lcd"] = list(_LCD_IDS)
SUITES["safety"] = [c for c in REGISTRY if c.startswith("SP-")]
SUITES["totp"] = [c for c in REGISTRY if c.startswith("TP-")]
#: Explicit order: configure, refusals, rule/pause, trip last, restore.
SUITES["aux"] = ["AX-C01", "AX-C02", "AX-C03", "AX-T01", "AX-K01", "AX-T02", "AX-R01"]
# nightly/full memberships per plan §5.1 -- wave 2 implements the harness
# ordering for the full §5.1 nightly membership; some of these ids still
# report NOT_RUN: not_implemented until the WEB/LCD/OTA case wave(s) land
# judge functions for them (registry.py never gates a suite's *membership*
# on what has a judge yet -- see the module docstring).
#
# §5.2's fixed order, restricted to the ids nightly actually includes, is
# NOT simply `_fixed_order()` (that helper only knows ST/FL/SK/SP's ranks
# and puts everything else -- WEB, LCD, HP, OT -- in one alphabetical
# bucket after them, which would alphabetize OT-* ahead of WEB-* and
# scatter the HP/SP-observer and HP/SK-02 pairs). Wave 2 instead writes the
# nightly order out explicitly, encoding §5.2's actual sequence and §5.3's
# interdependence rules:
#   - ST first, then read-only FL/SK-01,03,04/SP, matching `smoke`.
#   - WEB render cases (`smoke`'s -01/-X-03 set), then the nightly-only WEB
#     read/write round trips (auth still off) and WEB-X-01/02, then the
#     nightly-only LCD captures -- all before any heat, since none of them
#     need a firing running.
#   - HP-01 immediately followed by its observer SP-06 (plan §5.2: "HP-01
#     (with ... SP-04 observers)" generalizes to any SP case whose
#     `depends_on` names it -- SP-06 depends_on="HP-01"), then HP-02
#     immediately followed by its observer SP-03 (depends_on="HP-02"),
#     then HP-04/05/06/08, then SK-02 last of the HP group since it
#     depends_on="HP-01" and the plan places SK-02 after the HP block
#     (§5.2: "... HP-05/06 -> SK-02 -> HP-03 -> ...").
#   - WEB-SEC-03 (an auth-on/off round trip) grouped with the OTA block per
#     §5.2's "OT-E10 with WEB-SEC-03" pairing and §5.3 rule 4 (auth-on
#     cases grouped at the end so a failure to disable auth blinds as few
#     later cases as possible).
#   - OT-B01 then OT-E01/E02/E03/E12 strictly last ("OTA last"): every
#     other nightly case that could affect board state (HP writes zones
#     config live briefly, WEB round trips write and restore config) has
#     already run and been torn down by the time an OTA reboots the board.
_NIGHTLY_ORDER: List[str] = [
    "ST-01", "ST-02", "ST-03", "ST-04", "ST-05",
    "FL-01", "FL-02", "FL-03", "FL-04", "FL-05", "FL-06", "FL-07", "FL-08", "FL-09",
    "SK-01", "SK-03", "SK-04",
    "SP-01", "SP-02", "SP-05", "SP-07",
    *_WEB_SMOKE_IDS,
    # WEB-DASH-13 is not in the plan's own §5.1 nightly bullet list but is
    # the same read/write-round-trip shape and is implemented in this wave,
    # so it rides along with the other WEB-DASH round trips.
    "WEB-DASH-03", "WEB-DASH-06", "WEB-DASH-07", "WEB-DASH-09", "WEB-DASH-13",
    "WEB-PROF-02", "WEB-PROF-03", "WEB-PROF-04", "WEB-PROF-05",
    "WEB-PROF-06", "WEB-PROF-07", "WEB-PROF-08", "WEB-PROF-09",
    "WEB-ZONE-02", "WEB-ZONE-03", "WEB-ZONE-05", "WEB-ZONE-09", "WEB-ZONE-12",
    "WEB-BAK-02", "WEB-BAK-03",
    "WEB-KCFG-02", "WEB-KCFG-03",
    "WEB-DIAG-07", "WEB-DIAG-08",
    # WEB-OTA-01 and WEB-X-01 are already in `_WEB_SMOKE_IDS` above (both
    # end in "-01", same as every WEB render case) -- only WEB-OTA-02 and
    # WEB-X-02 are new to nightly.
    "WEB-OTA-02",
    "WEB-X-02",
    # Only the LCD captures with no dependency can run here; LCD-02
    # (depends_on HP-01), LCD-03 (HP-04) and LCD-04 (OT-B01) are observers
    # and must follow the case they read, same as the SP observers.
    "LCD-09", "LCD-14", "LCD-16",
    "HP-01", "SP-06", "LCD-02",
    "HP-02", "SP-03",
    "HP-04", "LCD-03", "HP-05", "HP-06", "HP-08",
    "SK-02",
    "WEB-SEC-03",
    # SP-04 is a pure observer of OT-B01's already-collected trip/clear data
    # (cases_safety._case_sp04, depends_on="OT-B01"), so it follows it directly.
    "OT-B01", "SP-04", "LCD-04",
    "OT-E01", "OT-E02", "OT-E03", "OT-E12",
]
assert len(_NIGHTLY_ORDER) == len(set(_NIGHTLY_ORDER)), "duplicate id in _NIGHTLY_ORDER"
SUITES["nightly"] = list(_NIGHTLY_ORDER)
SUITES["full"] = _fixed_order(REGISTRY.keys())


def _enforce_always_last() -> None:
    """Move `_ALWAYS_LAST` to the end of EVERY suite that contains it, not
    just the ones built through `_fixed_order()`. `SUITES["web"]` is a plain
    `list(_WEB_IDS)`, so WEB-SEC-05 sat mid-list there with 24 web cases
    scheduled after it -- each one then facing the per-IP lockout that case
    deliberately trips, which has no admin unlock route (memory
    project_login_lockout_saturation_accepted). Anything that appends a new
    suite below this call must call it again."""
    for name, ids in SUITES.items():
        if _ALWAYS_LAST in ids and ids[-1] != _ALWAYS_LAST:
            SUITES[name] = [c for c in ids if c != _ALWAYS_LAST] + [_ALWAYS_LAST]


_enforce_always_last()
