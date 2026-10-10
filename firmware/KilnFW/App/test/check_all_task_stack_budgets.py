#!/usr/bin/env python3
"""Worst-case stack usage for every stack_margin_register()-registered KilnFW
task that does NOT already have its own dedicated checker, measured out of
the built ELF, driven from one table instead of one file per task.

WHY THIS EXISTS
---------------
The board has been bricked/panicked by task-stack overflows FOUR times
(docs/audits/boot_hang_2026-09-08.md "main", check_httpd's httpd_worker
incident, check_system_uart_bridge's system_bridge_task audit, and
docs/audits/executor_panic_stack_overflow_2026-09-09.md's profile_executor)
-- yet as of 2026-09-09, of the ~33 tasks registered via
stack_margin_register(), only 5 (main, httpd_worker, system_uart_bridge,
uart_log_bridge, profile_executor) had a regression check at all. The other
~28 -- including `safety_poll` and `lvgl`, the two tasks directly named in
the 2026-09-04 panic post-mortem (`s_lvgl_task_stack` in lvgl_port.c sitting
close enough behind thermo_owner.c's `s_slots[]` in .bss that a deep,
reentrant LVGL call path reached from `safety_poll`'s configASSERT smashed
it) -- had NO regression gate whatsoever. This script closes that gap for
all of them at once.

STRUCTURE: ONE SCRIPT + ONE SHARED LIBRARY, NOT 28 FILES
---------------------------------------------------------
stack_budget_lib.py (new, address-keyed to correctly handle the real
`owner_task` name collision between kiln_io_owner.c and thermo_owner.c --
see that module's docstring) holds the ELF-disassembly/call-graph/deepest-
path machinery. TASKS below is the single table this script iterates:
adding task #29 means adding one dict, not one new *.py/*.ps1 pair. This
was chosen over "one script per task" (the existing pattern) because 28
near-duplicate files differing only in a root name and three constants is
exactly the kind of duplication that lets one of them silently rot
unnoticed -- which is how this gap happened in the first place. It was also
chosen over "one script per task GROUP" (e.g. one for UART bridges, one for
owners, one for HTTP/OTA) because there is no grouping boundary here worth
the seam: every task uses the identical measure-compare-report shape, ELF
parsing is one shared pass (parse() runs the (slow, ~1-2s) objdump
invocation exactly ONCE for all 28 tasks, not 28 times), and a single
sorted table is easier to audit end-to-end for "did every registered task
get a row" than 28 file diffs would be. If this table ever grows enough
that per-task logic genuinely diverges (a task needing a bespoke
live-measurement cross-check the way check_executor_task_stack_budget.py's
1220 B UNMODELED_OVERHEAD_BYTES does), split THAT task out into its own
checker file the way profile_executor's was -- the existing five dedicated
checkers remain untouched by this script for exactly that reason.

NO HAND-COPIED STACK SIZES (see CLAUDE.md's "reset one side of a pair" /
"two pieces of state joined by a semantic contract, expressed nowhere as a
single owning type" bug class)
------------------------------------------------------------------------
Every TASKS[*]['stack'] entry is a callable that reads the DECLARED stack
size back out of the actual xTaskCreate*/i2c_owner_init call site (or, for
the four tasks whose stack is `UART_OWNER_STACK_SIZE`/`UART_PROTOCOL_STACK_SIZE`,
out of sdkconfig's CONFIG_KILNCTL_UART_*_STACK_SIZE, after first confirming
via regex that the call site actually references that macro and not some
other expression) -- never a bare integer typed into this file. A stack
size silently drifting out of sync with the real call site is precisely
the failure mode that bit test_display_power_wiring.c's screen_idle check
before it added the same kind of source-derived assertion; see
extract_int_literal()/extract_sdkconfig_macro()/extract_local_macro() below.

OVERHEAD CONSTANT, STATED HONESTLY (requirement: never inflate to force a
pass -- see feedback_negative_test_every_check.md)
------------------------------------------------------------------------
None of these 28 tasks have a dedicated live high-water-mark measurement
the way profile_executor's 1220 B or uart_log_bridge's 1032 B do (those
were derived from a specific hardware DRAM_PSRAM_STATUS.md capture; no
equivalent capture exists yet for these tasks). Absent that,
`UNMODELED_OVERHEAD_BYTES = 300` is used uniformly -- this is exactly
check_system_uart_bridge_stack_budget.py's own precedent and its own
stated reasoning: none of these tasks has an ESP-IDF httpd dispatch layer
underneath it (they are plain FreeRTOS tasks blocking on a queue or a
protocol inbox), so there is no per-request dispatch cost to add on top of
the ISR-window-spill allowance itself. 300 B is a conservative,
order-of-magnitude placeholder for that alone -- an ASSUMPTION, not a
measured constant, same caveat check_system_uart_bridge_stack_budget.py
states for its own use of the same number. If a live measurement becomes
available for any of these tasks (e.g. via get_stack_margin() on hardware),
replace that task's entry with a real figure and cite the source, the same
way the five dedicated checkers already do -- do not raise this number
speculatively to manufacture headroom, and do not lower it to force a red
result either.

`safety_poll` and `lvgl` are flagged in TASKS with `historical_note` because
they are the two tasks directly implicated in the 2026-09-04 panic --
worth prioritizing for an eventual real live measurement over the other 26,
even though this script currently treats them the same as every other row.

CEILINGS
--------
Same "ceiling, not a headroom-fraction budget" convention as
check_httpd_task_stack_budget.py / check_executor_task_stack_budget.py:
each CEILING_BYTES below is the deepest static path measured against
KilnCtrl.elf as built 2026-09-09 (the run that added this check), not a
theoretical maximum. This check exists to catch that number getting WORSE,
not to relitigate whatever it happens to measure right now. Retighten a
ceiling down if a fix legitimately shrinks it; never raise one to paper
over a regression without documenting why in this file.

WHAT IT DOES NOT MODEL (same LIMITS as every other checker in this family)
---------------------------------------------------------------------------
  * Indirect calls (`callx4/8/12` through a function pointer) are not
    followed -- stack_budget_lib.has_unresolved_dispatch() detects when a
    task's reachable call graph contains one of these and, as of 2026-09-09
    (opus review of 316967b7's initial version, which measured `lvgl` at
    752 B against its 8192 B stack and reported a confident pass -- the real
    depth was entirely behind lv_timer_handler()'s function-pointer
    dispatch), such a task is printed and counted as INDETERMINATE, never as
    a plain pass. In practice this now flags every one of the 28 tasks in
    this table -- ESP-IDF's own driver/HAL layer is function-pointer-based
    almost everywhere -- so a ceiling-based regression gate is still applied
    (and still fails a task that regresses or goes negative), but the
    "measured and within budget" headline is reserved for tasks with zero
    indirect dispatch in their reachable graph, which turns out to be none
    of them today. `lvgl` additionally has extra_roots: known callback entry
    points (the flush callback, the touch read callback, every page's
    lv_timer_create() refresh callback) that this script measures as their
    own roots and adds the deepest of onto lvgl's base -- a real,
    source-derived improvement over the bare 752 B, but still not a full
    measurement (LVGL's own internal animation/event-callback dispatch stays
    invisible), so `lvgl` still reports INDETERMINATE too.
  * Recursion is cut at the first repeat rather than unrolled; deepest()'s
    memo only caches a node's result when its subtree computed with no cut
    in it, so a cut branch is recomputed rather than poisoning later reuse
    of that node from a different call path (fixed 2026-09-09, see
    deepest()'s docstring in stack_budget_lib.py).
  * ISR/window-overflow spill beyond UNMODELED_OVERHEAD_BYTES is not modelled.
All three make this an UNDER-estimate of true worst-case depth, never an
over-estimate: anything this reports as too deep genuinely is too deep, and
an INDETERMINATE task's true depth may be worse than what is printed.

KCONFIG-GATED TASKS: two rows (gpio_probe, backlight_pwm) name a `kconfig`
symbol because their task body is #if'd out when that option is off. Such a
row is adjudicated against the sdkconfig belonging to the ELF being measured
(resolved from --elf, NOT from the repo root -- `sdkconfig` is gitignored and
the shared main tree's differs from any clean worktree's; see
use_sdkconfig_for_elf's RESOLUTION ORDER comment, which has no repo-root
fallback and refuses archived ELFs outright): option on => the row is
measured normally and a vanished root still FAILS; option off => the root is
REQUIRED to be absent AND the task's source-side definition must still be
present and still gated on that symbol, and the row is then reported as
excluded-by-config; a root that resolves anyway FAILS as an ELF/sdkconfig
disagreement. It is not a SKIP: the run still measures and grades every other
row. `kconfig=` names a literal CONFIG_* symbol tested by a preprocessor
conditional -- a root gated on a NORMALISED macro instead cannot be expressed
this way; see verify_kconfig_guard().

EXIT CODES: 0 OK (all ceiling-graded tasks within budget; INDETERMINATE tasks
are noted, not failed, as long as their own known lower bound is within
budget), 1 FAIL (at least one task over its ceiling, honest-negative, or
missing a CEILING_BYTES entry -- a table row must never silently go
ungraded), 3 SKIP (no ELF / no objdump -- never claims success without
measuring).
run_all_checks.ps1 sorts check_*.ps1 by plain FullName; this file's
"check_a..." prefix sorts it after check_00_kilnfw_target_build.ps1 (which
publishes the fresh ELF this script reads) and before the untouched
check_e/check_h/check_m/check_s/check_u dedicated stack-budget siblings --
order among the stack-budget checkers themselves does not matter since none
of them depend on another's output, only on check_00's ELF.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import stack_budget_lib as lib  # noqa: E402
import lvgl_callback_discovery  # noqa: E402

REPO_ROOT = lib.REPO_ROOT
DEFAULT_ELF = lib.DEFAULT_ELF
DEFAULT_SDKCONFIG = lib.DEFAULT_SDKCONFIG

APP_DIR = os.path.join(REPO_ROOT, "firmware", "KilnFW", "App")

UNMODELED_OVERHEAD_BYTES = 300  # see module docstring "OVERHEAD CONSTANT, STATED HONESTLY"

# MECHANICAL DISCOVERY OF lvgl's extra_roots (2026-09-24), replacing a hand-kept
# list that only ever covered timer/flush/touch callbacks -- see
# lvgl_callback_discovery.py's own module docstring for the full rationale and
# the FAIL-vs-NOTE justification for unresolved names. Run once at import time
# (source scan only, no ELF needed yet) so TASKS below can reference the result
# directly, the same way every other row's `stack=` callable is source-derived
# rather than hand-typed.
#
# MIN_PLAUSIBLE_LVGL_CALLBACKS is the vacuity floor this check's own task
# description demanded ("print the count found; fail if 0 or implausibly
# low" -- see feedback_powershell_filter_no_char_classes-class bugs where a
# broken scan quietly returns nothing and reads as "no callbacks exist").
# Measured 2026-09-24: 75 raw registration-site matches (56 lv_obj_add_event_cb
# + 6 lv_timer_create + 1 lv_display_set_flush_cb + 1 lv_indev_set_read_cb + 11
# .on_confirm= + 0 .on_cancel=, hand-counted independently and matching the
# scanner's own raw_count exactly) resolving to 79 unique concrete roots, 0
# unresolved bare names, 4 dynamic (struct-field, genuinely not scannable)
# forwards in ui_topbar.c. 40 is set well under half of that observed 75 so
# an honest future drop in UI callback count (a page removed) does not itself
# trip the floor -- only a scan that is mechanically broken (e.g. a directory-
# exclusion regression that walks into nothing) should.
MIN_PLAUSIBLE_LVGL_CALLBACKS = 40
_LVGL_ROOTS, _LVGL_DISCOVERY_ERRORS, _LVGL_DISCOVERY_NOTES, _LVGL_RAW_COUNT = \
    lvgl_callback_discovery.discover(APP_DIR)


def _read(rel_path):
    path = os.path.join(APP_DIR, rel_path)
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def extract_int_literal(rel_path, pattern):
    """Read the declared stack size directly out of the real call site."""
    text = _read(rel_path)
    m = re.search(pattern, text, re.DOTALL)
    if not m:
        raise ValueError(f"pattern not found in {rel_path}: {pattern!r}")
    return int(m.group(1))


def extract_local_macro(rel_path, define_pattern, usage_pattern):
    """A #define'd constant local to one file (e.g. BX_WORKER_STACK,
    SAFETY_POLL_TASK_STACK): confirm the call site actually uses the macro
    name (usage_pattern), then read the macro's value from its #define
    (define_pattern) -- both derived from source, nothing hand-copied."""
    text = _read(rel_path)
    if not re.search(usage_pattern, text, re.DOTALL):
        raise ValueError(f"call site does not reference the expected macro in {rel_path}: {usage_pattern!r}")
    m = re.search(define_pattern, text)
    if not m:
        raise ValueError(f"macro #define not found in {rel_path}: {define_pattern!r}")
    return int(m.group(1))


_SDKCONFIG_CACHE = {}

# A real esp-idf sdkconfig carries on the order of 1500 CONFIG_ keys. Anything
# at or under this floor is not a config this check is willing to grade a
# build against -- see _load_sdkconfig()'s "USABLE" definition.
MIN_PLAUSIBLE_SDKCONFIG_KEYS = 50

# The sdkconfig that produced the ELF being measured. `sdkconfig` is
# gitignored, so it differs between the shared main working tree (where a
# bench operator may have toggled a debug option on) and any clean worktree
# (where idf.py regenerates it from sdkconfig.defaults + Kconfig defaults).
# Reading the wrong one is how a Kconfig-gated task's presence gets
# mis-adjudicated.
#
# RESOLUTION ORDER, and why there is NO repo-root fallback (2026-09-15).
# ---------------------------------------------------------------------
# The original version of this resolution fell back to the repo-root
# `sdkconfig` whenever --elf had no sibling config, silently and with no
# warning -- which reinstated exactly the behaviour it was written to remove,
# and could grade any ELF anywhere against whatever happens to be configured
# in this tree right now. Resolution is now ordered, explicit, and terminal:
#
#   1. --sdkconfig, if the caller supplied one. Authoritative; must exist.
#   2. `<elf-stem>.sdkconfig` or `sdkconfig` sitting IN the ELF's own
#      directory -- a config PUBLISHED alongside the artifact it produced.
#      This is the provenance-carrying case and it is preferred over
#      anything inferred from directory layout.
#   3. `<elf-dir>/../sdkconfig` -- the ordinary "ELF is in a build directory
#      inside an esp-idf project" case. For the default
#      build/KilnCtrl.elf this resolves to the project's live sdkconfig,
#      which is correct: that build directory and that config are the same
#      configuration of the same tree.
#
# ARCHIVED ELFs ARE REFUSED, not guessed at. KilnFW/elf_archive/ is a SIBLING
# of build/, so rule 3 would resolve an archived ELF to the config of
# whatever is configured NOW -- confidently the wrong file, and precisely the
# provenance error the ELF archive exists to prevent (see CLAUDE.md's
# "Symbolize a crash against the ELF that matches the RUNNING image"). Rule 3
# is therefore not offered for an ELF sitting in an `elf_archive` directory:
# absent a published sibling config (rule 2) or an explicit --sdkconfig
# (rule 1), resolution FAILS and every row that needs the config says so.
#
# If nothing resolves, SDKCONFIG_PATH is None and _load_sdkconfig() raises
# with SDKCONFIG_ORIGIN's explanation. That is deliberately a per-row FAIL,
# not an "assume off".
SDKCONFIG_PATH = DEFAULT_SDKCONFIG
SDKCONFIG_ORIGIN = "module default (no ELF resolved yet)"

ELF_ARCHIVE_DIRNAME = "elf_archive"


def _sdkconfig_candidates(elf):
    """(path, human-readable origin) pairs, most authoritative first."""
    elf_dir = os.path.dirname(os.path.abspath(elf))
    stem = os.path.splitext(os.path.basename(elf))[0]
    cands = [
        (os.path.join(elf_dir, stem + ".sdkconfig"),
         f"published beside the ELF as {stem}.sdkconfig"),
        (os.path.join(elf_dir, "sdkconfig"),
         "published inside the ELF's own directory"),
    ]
    if os.path.basename(elf_dir).lower() != ELF_ARCHIVE_DIRNAME:
        cands.append((os.path.abspath(os.path.join(elf_dir, os.pardir, "sdkconfig")),
                      "the ELF's build-directory parent"))
    return cands


def _config_lines(path):
    """{CONFIG_KEY: value} for every non-comment `CONFIG_*=value` line.

    A "# CONFIG_X is not set" comment line is excluded here, matching
    _load_sdkconfig's convention that absence and "is not set" are the same
    value (the "n" value): a key present as `=y` in one file and absent (or
    "is not set") in the other IS a real disagreement and is still reported
    as one, via the set-difference of parsed keys.
    """
    parsed = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            if "=" in line:
                k, _, v = line.strip().partition("=")
                if k.startswith("CONFIG_"):
                    parsed[k] = v
    return parsed


def _check_sibling_pair_agreement(elf_dir):
    """Compare `<elf_dir>/sdkconfig` against `<elf_dir>/../sdkconfig`.

    check_00_kilnfw_target_build.ps1 publishes an isolated checkbuild's
    sdkconfig into the invoking tree's build/ next to the ELF it also
    publishes there. A later plain `idf.py build` in that same build/
    directory replaces the ELF but leaves the published sdkconfig sibling
    in place, so the two can silently drift apart: the sibling then
    reflects an EARLIER build than the ELF it sits next to, and grading the
    newer ELF against it is exactly the "reset one side of a pair" failure
    this check exists to catch (see CLAUDE.md).

    Comparing file mtimes was tried and rejected: in the ordinary case of an
    unchanged tree, `idf.py build` can leave build/sdkconfig with an OLDER
    mtime than a freshly linked ELF even though the two are byte-identical
    in content, which would false-positive. Comparing the CONFIG_ lines
    themselves is the actual invariant that matters.

    Returns None if the pair agrees (or either file is missing/unreadable --
    that is not this function's failure mode). Returns a loud message string
    naming both paths and the differing symbols if they disagree.
    """
    if os.path.basename(elf_dir).lower() == ELF_ARCHIVE_DIRNAME:
        return None  # archived ELFs never resolve against the parent config; see rule 3
    sibling = os.path.join(elf_dir, "sdkconfig")
    parent = os.path.abspath(os.path.join(elf_dir, os.pardir, "sdkconfig"))
    if not (os.path.isfile(sibling) and os.path.isfile(parent)):
        return None
    try:
        sibling_cfg = _config_lines(sibling)
        parent_cfg = _config_lines(parent)
    except OSError:
        return None
    keys = sorted(set(sibling_cfg) | set(parent_cfg))
    diffs = [
        (k, sibling_cfg.get(k, "<unset>"), parent_cfg.get(k, "<unset>"))
        for k in keys
        if sibling_cfg.get(k) != parent_cfg.get(k)
    ]
    if not diffs:
        return None
    diff_lines = "\n".join(f"  {k}: sibling={sv!r} parent={pv!r}" for k, sv, pv in diffs)
    return (
        f"UNRESOLVED: {sibling} (published beside the ELF's own build) and {parent} "
        "(the ELF's build-directory parent) DISAGREE on "
        f"{len(diffs)} CONFIG_ symbol(s):\n{diff_lines}\n"
        "This is the check_00_kilnfw_target_build.ps1 publish leaving a stale sibling "
        "sdkconfig behind a newer ELF from a later plain `idf.py build` in the same "
        "directory -- grading that ELF against either file without knowing which one "
        "actually produced it risks a silently wrong result either way. Fix: delete "
        f"{sibling} (falls back to the parent config), rebuild so build_kilnfw refreshes "
        "it to match, or pass --sdkconfig naming the config that actually produced this ELF."
    )


def use_sdkconfig_for_elf(elf, explicit=None):
    """Point sdkconfig reads at the config belonging to `elf`'s own build.

    Returns (path_or_None, origin_description). Never silently falls back to
    the repo root; see the RESOLUTION ORDER comment above."""
    global SDKCONFIG_PATH, SDKCONFIG_ORIGIN
    _SDKCONFIG_CACHE.clear()
    if explicit:
        SDKCONFIG_PATH = os.path.abspath(explicit)
        SDKCONFIG_ORIGIN = "supplied explicitly with --sdkconfig"
        return SDKCONFIG_PATH, SDKCONFIG_ORIGIN
    elf_dir = os.path.dirname(os.path.abspath(elf))
    cands = _sdkconfig_candidates(elf)
    for path, origin in cands:
        if origin == "published inside the ELF's own directory" and os.path.isfile(path):
            # This is the specific pairing (`<elf-dir>/sdkconfig` vs.
            # `<elf-dir>/../sdkconfig`) that can drift apart -- see
            # _check_sibling_pair_agreement's docstring. A rule-1 stem-named
            # config (checked earlier in this loop) is per-ELF and provenance-
            # carrying by construction, so it is never subject to this check.
            disagreement = _check_sibling_pair_agreement(elf_dir)
            if disagreement is not None:
                SDKCONFIG_PATH = None
                SDKCONFIG_ORIGIN = disagreement
                return SDKCONFIG_PATH, SDKCONFIG_ORIGIN
        if os.path.isfile(path):
            SDKCONFIG_PATH = os.path.abspath(path)
            SDKCONFIG_ORIGIN = origin
            return SDKCONFIG_PATH, SDKCONFIG_ORIGIN
    SDKCONFIG_PATH = None
    tried = "; ".join(f"{p} ({o})" for p, o in cands)
    if os.path.basename(os.path.dirname(os.path.abspath(elf))).lower() == ELF_ARCHIVE_DIRNAME:
        SDKCONFIG_ORIGIN = (
            f"UNRESOLVED: {elf} is an ARCHIVED ELF. The archive is a sibling of build/, so "
            "the live project sdkconfig is NOT the config that produced this ELF and this "
            "check refuses to grade an archived artifact against it. Tried: " + tried +
            ". Supply the config that actually produced this ELF with --sdkconfig.")
    else:
        SDKCONFIG_ORIGIN = (
            "UNRESOLVED: no sdkconfig found for this ELF (build the project first, or pass "
            "--sdkconfig). Tried: " + tried +
            ". There is deliberately no repo-root fallback: grading an ELF against a config "
            "that did not produce it is the failure this resolution exists to prevent.")
    return SDKCONFIG_PATH, SDKCONFIG_ORIGIN


def _load_sdkconfig():
    """Parse the resolved sdkconfig, or raise.

    USABLE, defined (2026-09-15): the file exists, parses to at least
    MIN_PLAUSIBLE_SDKCONFIG_KEYS `CONFIG_*=value` assignments, and contains
    CONFIG_IDF_TARGET (which esp-idf writes into every generated sdkconfig,
    and which sdkconfig.defaults pins for this project). Anything else --
    empty, truncated mid-write, or garbage -- RAISES.

    Before this, any file that could be opened was treated as authoritative
    and a key simply missing from it read as "that option is off", so an
    empty or half-written sdkconfig silently reported EVERY option disabled
    and excused every Kconfig-gated row. That was inconsistent inside a
    single run: the four CONFIG_KILNCTL_UART_*_STACK_SIZE rows read the same
    file through _sdkconfig_value(), which has always FAILed loudly on a
    missing key. A truncated sdkconfig is not hypothetical here -- this file
    is copied between trees by check_00_kilnfw_target_build.ps1, and this
    repo has had a non-atomic-publish incident already
    (docs/audits/stack_budget_remeasure_after_elf_publish_bug_2026-09-10.md).
    """
    if _SDKCONFIG_CACHE:
        return
    if SDKCONFIG_PATH is None:
        raise ValueError(SDKCONFIG_ORIGIN)
    if not os.path.isfile(SDKCONFIG_PATH):
        raise ValueError(f"no sdkconfig at {SDKCONFIG_PATH} (build the project first)")
    parsed = {}
    for line in open(SDKCONFIG_PATH, encoding="utf-8", errors="replace"):
        if line.startswith("#"):
            continue  # "# CONFIG_X is not set" -- absence IS the 'n' value
        if "=" in line:
            k, _, v = line.strip().partition("=")
            parsed[k] = v
    config_keys = sum(1 for k in parsed if k.startswith("CONFIG_"))
    if config_keys <= MIN_PLAUSIBLE_SDKCONFIG_KEYS or "CONFIG_IDF_TARGET" not in parsed:
        raise ValueError(
            f"{SDKCONFIG_PATH} exists but is not a usable sdkconfig: parsed {config_keys} "
            f"CONFIG_* assignment(s) (need more than {MIN_PLAUSIBLE_SDKCONFIG_KEYS}) and "
            f"CONFIG_IDF_TARGET is {'present' if 'CONFIG_IDF_TARGET' in parsed else 'ABSENT'}. "
            "A real esp-idf sdkconfig has ~1500 keys. This is empty, truncated mid-write, or "
            "not an sdkconfig at all -- refusing to read every option as 'off' from it, which "
            "would silently excuse every Kconfig-gated row.")
    _SDKCONFIG_CACHE.update(parsed)


def _sdkconfig_value(key):
    _load_sdkconfig()
    if key not in _SDKCONFIG_CACHE:
        raise ValueError(f"{key} not found in {SDKCONFIG_PATH}")
    return int(_SDKCONFIG_CACHE[key])


def sdkconfig_bool(key):
    """True iff `key` is set to y in the ELF's own sdkconfig. A bool left at
    n is written as a "# CONFIG_X is not set" comment (or omitted), so
    absence means disabled -- but a MISSING sdkconfig file raises instead,
    because "I could not read the config" must never be mistaken for "that
    option is off"."""
    _load_sdkconfig()
    return _SDKCONFIG_CACHE.get(key) == "y"


def extract_sdkconfig_macro(rel_path, usage_pattern, sdkconfig_key):
    """UART_OWNER_STACK_SIZE / UART_PROTOCOL_STACK_SIZE resolve through
    settings.h to a CONFIG_KILNCTL_* sdkconfig value. Confirm the call site
    references the macro, then read the real configured value out of
    sdkconfig -- never hand-copied."""
    text = _read(rel_path)
    if not re.search(usage_pattern, text, re.DOTALL):
        raise ValueError(f"call site does not reference the expected macro in {rel_path}: {usage_pattern!r}")
    return _sdkconfig_value(sdkconfig_key)


def verify_kconfig_guard(rel_path, key):
    """Confirm `rel_path` still gates something on `key` with a preprocessor
    conditional. Used only on the EXCLUDED path -- see main()'s "SOURCE-SIDE
    VERIFICATION" note. Matches `#if`/`#elif` mentioning the literal key, so
    it holds for both `#if CONFIG_X` (backlight_pwm.c:57) and the negated
    `#if !CONFIG_X` (gpio_probe.c:7).

    NOTE, and this is the limit of what `kconfig=` can express: the key is
    matched as a literal CONFIG_* symbol in a preprocessor conditional. A
    root gated instead by a NORMALISED macro (drivers/hw/settings.h defines
    e.g. KILNCTL_SPI_ASYNC_FLUSH as 1/0 from CONFIG_KILNCTL_SPI_ASYNC_FLUSH,
    and the #if names the normalised macro, not the CONFIG_ key) cannot be
    described by this mechanism. No TASKS root is gated that way today; if
    one ever is, this table needs a real indirection, not a `kconfig=` key --
    otherwise the row would fall through to "missing root = FAIL"."""
    text = _read(rel_path)
    pat = r'^[ \t]*#[ \t]*(?:if|elif)\b[^\n]*\b' + re.escape(key) + r'\b'
    if not re.search(pat, text, re.M):
        raise ValueError(
            f"no `#if`/`#elif` line in {rel_path} mentions {key}. This row declares that task "
            f"is compiled out by {key}, but the source no longer gates it on that symbol "
            "(option renamed, guard removed, or the file restructured), so the root's absence "
            "from the ELF cannot honestly be attributed to that option being off.")


NORMALISED_MACRO_SOURCE = os.path.join("drivers", "hw", "settings.h")


def normalised_gate_violations(roots):
    """Enforce the limit documented in verify_kconfig_guard(): no TASKS root may
    be compiled out by a NORMALISED macro.

    drivers/hw/settings.h defines a family of object-like macros as literal 1/0
    derived from CONFIG_KILNCTL_* keys (KILNCTL_SPI_ASYNC_FLUSH and siblings),
    and source gates on the normalised name, not on the CONFIG_ key. A
    `kconfig=` row names a literal CONFIG_* symbol and matches it against an
    `#if`/`#elif` line, so it CANNOT describe such a gate. That was recorded as
    F5 of docs/audits/stack_budget_kconfig_gating_review_2026-09-15.md, whose
    remedy was a comment -- which is what verify_kconfig_guard()'s docstring and
    the TASKS header became. A comment, though, only helps somebody who reads it
    before moving a root; this function makes the same statement enforceable.

    The failure it prevents is NOT a false pass -- an unexpressible gate makes a
    root vanish from a build that has the option off, and the row falls through
    to "missing root = FAIL". It is a confusing, seemingly-unrelated failure in
    a check about stack sizes, arriving whenever somebody happens to build with
    that option off. This says the real thing instead, in that file, at that
    line: the table cannot express this gate, and it needs a real indirection
    rather than a `kconfig=` key.

    Returns a list of human-readable violations (empty when clean)."""
    settings_text = _read(NORMALISED_MACRO_SOURCE)
    normalised = set(re.findall(r'^#[ \t]*define[ \t]+(KILNCTL_\w+)[ \t]+[01][ \t]*$',
                                settings_text, re.M))
    if not normalised:
        # Vacuity floor: if the macro family cannot be found, this guard would
        # silently approve everything. Say so rather than pass.
        return [f"no normalised 1/0 KILNCTL_* macros found in {NORMALISED_MACRO_SOURCE} -- "
                "either the file moved or its macro shape changed, and this guard is "
                "vacuous as written. Not reporting a clean result on an unexamined tree."]
    if not roots:
        return ["normalised_gate_violations() was given no root symbols to look for, "
                "so it examined nothing. Refusing to report clean."]

    root_pat = re.compile(r'^[A-Za-z_][\w \t\*]*\b(' + "|".join(sorted(roots)) + r')\s*\(')
    cond_pat = re.compile(r'^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')
    trees = [APP_DIR, os.path.join(REPO_ROOT, "firmware", "hwAbstraction", "esp")]

    violations = []
    for tree in trees:
        for dirpath, _dirs, files in os.walk(tree):
            if os.sep + "build" in dirpath:
                continue
            for fn in files:
                if not fn.endswith(".c"):
                    continue
                path = os.path.join(dirpath, fn)
                stack = []
                with open(path, encoding="utf-8", errors="replace") as fh:
                    for lineno, line in enumerate(fh, 1):
                        cm = cond_pat.match(line)
                        if cm:
                            kw, rest = cm.group(1), cm.group(2).strip()
                            if kw in ("if", "ifdef", "ifndef"):
                                stack.append(rest)
                            elif kw in ("elif", "else"):
                                if stack:
                                    stack[-1] = rest or "<else>"
                            elif kw == "endif":
                                if stack:
                                    stack.pop()
                            continue
                        rm = root_pat.match(line)
                        if not (rm and stack):
                            continue
                        conds = " && ".join(stack)
                        named = sorted(n for n in normalised
                                       if re.search(r'\b' + n + r'\b', conds))
                        if named:
                            rel = os.path.relpath(path, REPO_ROOT)
                            violations.append(
                                f"{rel}:{lineno}: TASKS root `{rm.group(1)}` is defined inside a "
                                f"preprocessor conditional gated on the NORMALISED macro(s) "
                                f"{', '.join(named)} (controlling condition: {conds}). The TASKS "
                                f"table's `kconfig=` key matches a literal CONFIG_* symbol in an "
                                f"`#if`/`#elif` and cannot express this gate, so with that option "
                                f"off the root vanishes and the row fails as a missing root. This "
                                f"needs a real indirection in the table, not a `kconfig=` key -- "
                                f"see verify_kconfig_guard() and F5 of "
                                f"docs/audits/stack_budget_kconfig_gating_review_2026-09-15.md.")
    return violations


# ---------------------------------------------------------------------------
# DECLARED_EDGES (2026-10-09). Call edges the static walk cannot see because
# the call goes through a function pointer, applied ONLY to a task row that
# opts in with `declared_edges=True` (today: bx_flash_worker). Format:
# {caller_name: [callee_name, ...]}; names must resolve uniquely in the ELF.
#
# nvs_save -> zones_autosave_job: zones_config_store.c's nvs_save() calls
# zones_autosave_job through `void (*volatile job)(void *arg)` when it is
# ALREADY on the flash worker (the volatile pointer defeats inlining and a
# plain edge). Every other task dispatches that job onto the worker, so the
# edge only costs stack on the worker. Without it control_handle_message's
# worker path under-measures by the whole autosave chain (~2 KB).
# Each entry needs a source-side proof that the edge still exists:
# DECLARED_EDGE_SOURCE_GUARDS below; the check FAILS if the site is gone.
# ---------------------------------------------------------------------------
DECLARED_EDGES = {
    "nvs_save": ["zones_autosave_job"],
}

# (caller, callee) -> (source file relative to App/, regex that must match).
DECLARED_EDGE_SOURCE_GUARDS = {
    ("nvs_save", "zones_autosave_job"): (
        os.path.join("drivers", "persist", "zones_config_store.c"),
        r"uart_bridge_ext_is_on_flash_worker\(\)\s*\)\s*\{\s*"
        r"void\s*\(\*\s*volatile\s+job\)\s*\(\s*void\s*\*\s*arg\s*\)\s*=\s*zones_autosave_job\s*;"),
}


def declared_edge_violations(read=None):
    """Errors for every DECLARED_EDGES entry whose source site is gone (or
    that has no guard at all). `read` is injectable for tests."""
    read = read or _read
    errs = []
    for caller, callees in DECLARED_EDGES.items():
        for callee in callees:
            guard = DECLARED_EDGE_SOURCE_GUARDS.get((caller, callee))
            if guard is None:
                errs.append(f"DECLARED_EDGES {caller}->{callee} has no DECLARED_EDGE_SOURCE_GUARDS entry")
                continue
            rel, rx = guard
            try:
                text = read(rel)
            except OSError as e:
                errs.append(f"DECLARED_EDGES {caller}->{callee}: cannot read {rel}: {e}")
                continue
            if not re.search(rx, text):
                errs.append(f"DECLARED_EDGES {caller}->{callee}: the dispatch site "
                            f"(`void (*volatile job)(void *arg) = {callee};` next to "
                            f"uart_bridge_ext_is_on_flash_worker()) was not found in {rel} -- the "
                            "declared edge is stale or the site moved; update DECLARED_EDGES.")
    return errs


def apply_declared_edges(parsed, resolve):
    """A copy of `parsed` with DECLARED_EDGES added. `resolve(name)` -> addr
    (raises ValueError when missing/ambiguous)."""
    import copy
    view = copy.copy(parsed)
    view.calls = {k: set(v) for k, v in parsed.calls.items()}
    for caller, callees in DECLARED_EDGES.items():
        ca = resolve(caller)
        for callee in callees:
            view.calls.setdefault(ca, set()).add(resolve(callee))
    return view


# ---------------------------------------------------------------------------
# TASKS: one row per stack_margin_register() call site with no dedicated
# checker of its own. `root` is the task's own C entry function (the first
# argument to whichever xTaskCreate* family function creates it -- NOT the
# stack_margin_register() name string, which is only a label). `expect_path`
# disambiguates a root symbol name that is not unique in the ELF (currently
# only `owner_task`, shared verbatim between kiln_io_owner.c and
# thermo_owner.c -- see stack_budget_lib.py's docstring).
# ---------------------------------------------------------------------------
TASKS = [
    dict(name="gpio_probe", root="gpio_probe_task",
         # Whole task is #if CONFIG_KILNCTL_ENABLE_GPIO_PROBE'd out
         # (gpio_probe.c:7). That option defaults to n, so it is COMPILED OUT
         # of every clean-worktree build and present only where someone has
         # turned it on locally for bench work -- see the `kconfig` handling
         # in main().
         kconfig="CONFIG_KILNCTL_ENABLE_GPIO_PROBE",
         kconfig_src="drivers/bridge/gpio_probe.c",
         stack=lambda: extract_int_literal("drivers/bridge/gpio_probe.c",
             r'xTaskCreatePinnedToCoreWithCaps\(gpio_probe_task,\s*"gpio_probe",\s*(\d+)')),
    dict(name="link_watchdog", root="link_watchdog_task",
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge.c",
             r'xTaskCreatePinnedToCoreWithCaps\(link_watchdog_task,\s*"link_watchdog",\s*(\d+)')),
    dict(name="bx_flash_worker", root="bx_worker_task", declared_edges=True,
         stack=lambda: extract_local_macro("drivers/bridge/uart_bridge_ext.c",
             r'#define BX_WORKER_STACK\s+(\d+)',
             r'xTaskCreatePinnedToCore\(bx_worker_task,\s*"bx_flash_worker",\s*BX_WORKER_STACK'),
         # 2026-09-16 (HIGH 2 of the adversarial review of 60d6552f: this
         # task's stack was unmeasured while that commit routed more work
         # through it). bx_worker_task's own body is a queue receive plus
         # `job.fn(job.arg)` -- an indirect call the static walk cannot
         # follow -- so the bare walk reported 48 B, which is the depth of
         # the dispatch loop itself and not a measurement of anything this
         # task actually does.
         #
         # Unlike LVGL's internal callback dispatch, this task's target set
         # is ENUMERABLE from source: every job it can ever run is a function
         # passed to uart_bridge_ext_run_on_flash_worker(),
         # uart_bridge_ext_run_on_flash_worker_timeout() or
         # uart_bridge_ext_post_on_flash_worker(). Those call sites are
         # listed below, measured as their own roots; the deepest is added
         # onto the loop's own depth (only one job runs at a time -- the
         # worker is single-threaded by construction, which is the point of
         # it).
         #
         # ADDING A DISPATCH TARGET MEANS ADDING IT HERE: a new job function
         # deeper than every one below grows this task's real depth, and
         # nothing else in the repo would notice.
         #
         # Still INDETERMINATE, honestly: the jobs themselves call into NVS/
         # esp_partition/LittleFS, whose internals dispatch indirectly. This
         # is a real, source-derived lower bound over the actual work the
         # task performs, not a full measurement, and it is reported as such.
         extra_roots=[
             ("control_handle_message", "uart_bridge_ext_control.c"),
             ("profiles_handle_message", "uart_bridge_ext_control.c"),
             ("autotune_handle_message", "uart_bridge_ext_autotune.c"),
             ("coupling_persist_job", "autotune_engine_step_identify.c"),
             ("save_kibase_job", "adaptive_tune.c"),
             ("zones_autosave_job", "zones_config_store.c"),
             ("cfg_fs_auto_format_job_run", "cfg_fs_mount.c"),
             ("cfg_fs_write_job_run", "cfg_fs_mount.c"),
             ("cfg_fs_confirm_format_job_run", "cfg_fs_mount.c"),
             ("log_store_job_run", "log_store_mount.c"),
             ("reset_persist_job", "relay_cycles.c"),
             ("nvs_save_store_job", "safety_cfg_store.c"),
             ("crash_ack_job", "crash_report.c"),
             ("crash_clear_job", "crash_report.c"),
             ("cfgfs_file_write_job", "diagnostics_http.c"),
             ("execute_scope_job", "factory_reset.c"),
             ("safety_poll_pico_half_recapture_job", "safety_link_poll.c"),
         ]),
    dict(name="pico_auto_update", root="pico_auto_update_task",
         # Review finding (Pico-image-embed pass, 2026-09-20): this task's
         # call into ota_http_check_interlocks() does SPI reads and zone
         # snapshots normally run on the 8 KB httpd stack (see this file's
         # own httpd_task_stack_budget-adjacent notes). Raised 4096 -> 8192
         # (PICO_AUTO_UPDATE_TASK_STACK, drivers/net/pico_auto_update_boot.c)
         # 2026-09-21, owner-authorized, after a real hardware stack overflow
         # in this task -- the earlier static ceiling excluded flash/NVS
         # internals the embedded-staging path calls into. Registered for
         # stack-margin reporting per CLAUDE.md's "register every new task"
         # standing instruction.
         stack=lambda: extract_local_macro("drivers/net/pico_auto_update_boot.c",
             r'#define PICO_AUTO_UPDATE_TASK_STACK\s+(\d+)',
             r'xTaskCreate\(pico_auto_update_task,\s*"pico_auto_update",\s*PICO_AUTO_UPDATE_TASK_STACK')),
    dict(name="info_uart_bridge", root="info_bridge_task",
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge_info.c",
             r'xTaskCreatePinnedToCoreWithCaps\(info_bridge_task,\s*"info_uart_bridge",\s*(\d+)')),
    dict(name="io_uart_bridge", root="io_bridge_task",
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge_io.c",
             r'xTaskCreatePinnedToCoreWithCaps\(io_bridge_task,\s*"io_uart_bridge",\s*(\d+)')),
    dict(name="safety_uart_bridge", root="safety_bridge_task",
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge_safety.c",
             r'xTaskCreatePinnedToCoreWithCaps\(safety_bridge_task,\s*"safety_uart_bridge",\s*(\d+)')),
    dict(name="thermo_uart_bridge", root="thermo_bridge_task",
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge_thermo.c",
             r'xTaskCreatePinnedToCoreWithCaps\(thermo_bridge_task,\s*"thermo_uart_bridge",\s*(\d+)')),
    dict(name="touch_uart_bridge", root="touch_bridge_task",
         # Raised 3072 -> 4096 2026-09-24 (TOUCH_UART_BRIDGE_STACK_BYTES,
         # uart_bridge_touch.c) after bench SK-02 measured 440 B high-water
         # free against the 512 B absolute floor. PSRAM stack: 0 B DRAM impact.
         stack=lambda: extract_local_macro("drivers/bridge/uart_bridge_touch.c",
             r'#define TOUCH_UART_BRIDGE_STACK_BYTES\s+(\d+)',
             r'xTaskCreatePinnedToCoreWithCaps\(touch_bridge_task,\s*"touch_uart_bridge",\s*TOUCH_UART_BRIDGE_STACK_BYTES')),
    dict(name="autotune_engine", root="task_entry",
         stack=lambda: extract_int_literal("drivers/control/autotune_engine.c",
             r'xTaskCreatePinnedToCoreWithCaps\(task_entry,\s*"autotune_engine",\s*(\d+)')),
    dict(name="profile_exec_wdt", root="watchdog_task_entry",
         stack=lambda: extract_int_literal("drivers/control/profile_executor_start.c",
             r'xTaskCreatePinnedToCore\(watchdog_task_entry,\s*"profile_exec_wdt",\s*(\d+)')),
    dict(name="ota_rollback_reboot", root="ota_rollback_reboot_task",
         stack=lambda: extract_int_literal("drivers/http/ota_http_esp.c",
             r'xTaskCreate\(ota_rollback_reboot_task,\s*"ota_rollback_reboot",\s*(\d+)')),
    dict(name="recovery_boot", root="ota_recovery_boot_reboot_task",
         stack=lambda: extract_int_literal("drivers/http/ota_http_recovery.c",
             r'xTaskCreate\(ota_recovery_boot_reboot_task,\s*"recovery_boot",\s*(\d+)')),
    dict(name="ota_pico_rollback", root="ota_pico_rollback_task",
         stack=lambda: extract_int_literal("drivers/http/ota_http_pico.c",
             r'xTaskCreate\(ota_pico_rollback_task,\s*"ota_pico_rollback",\s*(\d+)')),
    dict(name="http_async_job", root="http_async_job_task",
         # docs/HTTP_POST_OWNER_MIGRATION.md slice A1: the shared
         # single-flight async-job helper (http_async_job.c). The stack size
         # is a parameter, not a literal in http_async_job.c's own
         # xTaskCreate() call, so the source-derived regex reads it out of
         # the first (and, as of A1, only) call site instead --
         # safety_cfg_http.c's ct_auto_zero_post_handler().
         stack=lambda: extract_int_literal("drivers/http/safety_cfg_http.c",
             r'http_async_job_try_start\([^,]+,\s*"http_async_job",\s*(\d+)'),
         # http_async_job_task's own body is the trampoline (run_job(), a
         # bare fn(...) call the static walk can't follow through the
         # http_async_job_fn_t pointer) -- ct_auto_zero_job() (this A1
         # slice's only registered fn) is enumerated explicitly as an extra
         # root, same technique as bx_flash_worker's dispatch-target list
         # above. 2026-09-25 fix-then-push review: the walk from the
         # trampoline alone reported a 32 B lower bound and let a 2736 B
         # ceiling (borrowed from ota_pico_rollback, WRONG -- see that
         # entry's own resolved depth of 1888 B) pass silently. Real
         # resolved depth from ct_auto_zero_job down through
         # safety_cfg_write_apply_pairs -> apply_pairs_ex ->
         # safety_cfg_store_refetch_locked -> safety_link_get_config_page
         # (a 1024 B frame) -> uart_protocol_send_broadcast ->
         # frame_and_send is 3280 B; adding it here makes the checker
         # measure and grade the real number instead of the trampoline's.
         # bench_preset_job (docs/HTTP_POST_OWNER_MIGRATION.md slice A2)
         # is this helper's second registered fn, added the same way -- only
         # one job runs at a time, so the loop above takes the DEEPER of the
         # two rather than summing them (see that loop's own comment). It is
         # only ever compiled in behind CONFIG_KILNCTL_DEV_TOOLS
         # (safety_cfg_http.c's #if around bench_preset_job's own
         # definition, guarding the "Apply test preset" button), so this is a
         # callable, evaluated lazily once sdkconfig_bool() has a loaded
         # sdkconfig to read (see the extra_roots-callable comment on the
         # measurement loop above) rather than a plain list built at import
         # time, before --elf/--sdkconfig are even parsed.
         # backup_import_job (slice A4, 2026-09-28) is the third registered
         # fn: backup_import_apply()'s two-pass commit, which used to run on
         # httpd_worker's 8192 B stack, now runs here on 6144 B -- added in
         # the A4 review so the checker measures that chain instead of
         # silently grading only the two A1/A2 jobs.
         # crash_report_clear_job (slice A3, 2026-10-02) is the fourth: it
         # used to run inline on httpd_worker, and the coredump/NVS erase
         # itself is dispatched to bx_flash_worker, so this job only waits.
         extra_roots=lambda: [("ct_auto_zero_job", "safety_cfg_http.c"),
                              ("backup_import_job", "backup_import.c"),
                              ("crash_report_clear_job", "diagnostics_http.c")] +
             ([("bench_preset_job", "safety_cfg_http.c")]
              if sdkconfig_bool("CONFIG_KILNCTL_DEV_TOOLS") else [])),
    dict(name="recovery_exit", root="ota_recovery_exit_reboot_task",
         stack=lambda: extract_int_literal("drivers/http/ota_http_recovery.c",
             r'xTaskCreate\(ota_recovery_exit_reboot_task,\s*"recovery_exit_reboot",\s*(\d+)')),
    dict(name="zone_sweep", root="zone_sweep_task",
         # Added 2026-09-24 (review finding at 4ddad119, CLAUDE.md "Register
         # every new task for stack-margin reporting"): this one-shot,
         # self-deleting task had a stack_margin_register() call site added
         # (zones_current_sweep_task.c, registered via &s_sweep.task, same
         # slot xTaskCreate() writes and the task itself clears to NULL on
         # exit) but no ceiling row of its own -- same on-demand shape as
         # recovery_exit/ota_pico_rollback above. NOT dispatch-free:
         # zone_sweep_run_all_zones() (zones_current_sweep_engine.c) calls
         # the hw_deps/hw_hooks tables zone_sweep_task() builds (9 deps + 5
         # hooks) through function pointers, which this walk cannot follow.
         # Measured separately 2026-09-24 against the same ELF: deepest
         # callback zone_sweep_hw_read_temp 560 B, so the real dispatch path
         # is about 432 (task) + 256 (run_all_zones) + 560 = 1248 B, well
         # under the 2896 B refetch path graded below. No extra_roots on
         # purpose: this checker ADDS the deepest extra root to the task's
         # whole deepest path (3456 B), which double-counts two paths that
         # never nest. Re-measure the callbacks if the sweep engine grows.
         stack=lambda: extract_int_literal("drivers/control/zones_current_sweep_task.c",
             r'xTaskCreate\(zone_sweep_task,\s*"zone_sweep",\s*(\d+)')),
    dict(name="backlight_pwm", root="backlight_pwm_task",
         # backlight_pwm.c:57's #if CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE wraps
         # both the task and its xTaskCreate (a no-op stub is compiled in its
         # place). Unlike gpio_probe this defaults to y -- so it is normally
         # present -- but a board without the backlight flying wire fitted is
         # explicitly told by that Kconfig help text to turn it off, and this
         # check must stay correct on such a board too.
         kconfig="CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE",
         kconfig_src="drivers/hw/backlight_pwm.c",
         stack=lambda: extract_int_literal("drivers/hw/backlight_pwm.c",
             r'xTaskCreate\(backlight_pwm_task,\s*"backlight_pwm",\s*(\d+)')),
    dict(name="i2c_owner_ns2009", root="i2c_owner_task",
         stack=lambda: extract_int_literal("drivers/hw/NS2009.c",
             r'i2c_owner_init\(&t->owner,\s*bus,\s*8,\s*5,\s*(\d+)')),
    dict(name="i2c_owner_sx1509", root="i2c_owner_task",
         stack=lambda: extract_int_literal("drivers/hw/SX1509.c",
             r'i2c_owner_init\(&e->owner,\s*bus,\s*8,\s*5,\s*(\d+)')),
    dict(name="kiln_io_owner", root="owner_task", expect_path="kiln_io_owner.c",
         stack=lambda: extract_int_literal("drivers/owners/kiln_io_owner.c",
             r'xTaskCreatePinnedToCore\(owner_task,\s*"kiln_io_owner",\s*(\d+)')),
    dict(name="thermo_owner", root="owner_task", expect_path="thermo_owner.c",
         stack=lambda: extract_int_literal("drivers/owners/thermo_owner.c",
             r'xTaskCreatePinnedToCore\(owner_task,\s*"thermo_owner",\s*(\d+)')),
    dict(name="telemetry_log", root="telemetry_log_task",
         stack=lambda: extract_int_literal("drivers/persist/telemetry_log.c",
             r'xTaskCreatePinnedToCoreWithCaps\(telemetry_log_task,\s*"telemetry_log",\s*(\d+)')),
    dict(name="danger_mode", root="danger_mode_task",
         stack=lambda: extract_int_literal("drivers/safety/danger_mode.c",
             r'xTaskCreate\(danger_mode_task,\s*"danger_mode",\s*(\d+)')),
    dict(name="safety_owner_evt", root="hal_uart_esp_event_task",
         stack=lambda: extract_sdkconfig_macro("drivers/safety/safety_link.c",
             r'uart_owner_init\(&link->owner,.*?UART_OWNER_STACK_SIZE',
             "CONFIG_KILNCTL_UART_OWNER_STACK_SIZE")),
    dict(name="safety_proto_rx", root="uart_protocol_rx_task",
         stack=lambda: extract_sdkconfig_macro("drivers/safety/safety_link.c",
             r'uart_protocol_init\(&link->proto,.*?UART_PROTOCOL_STACK_SIZE',
             "CONFIG_KILNCTL_UART_PROTOCOL_STACK_SIZE")),
    dict(name="safety_poll", root="safety_poll_task",
         historical_note="2026-09-04 panic: safety_poll's configASSERT was reached via a deep LVGL "
                          "call path that corrupted thermo_owner.c's s_slots[] -- see lvgl's note below.",
         stack=lambda: extract_local_macro("drivers/safety/safety_link.c",
             r'#define SAFETY_POLL_TASK_STACK\s+(\d+)',
             r'xTaskCreatePinnedToCoreWithCaps\(safety_poll_task,\s*"safety_poll",\s*SAFETY_POLL_TASK_STACK')),
    dict(name="lvgl", root="lvgl_port_task",
         historical_note="2026-09-04 panic: s_lvgl_task_stack (this task's own 8192 B stack) sits close "
                          "behind thermo_owner.c's s_slots[] in .bss; a reentrant lv_obj_invalidate()-in-"
                          "flush-callback path (pre-51e1ef5) could run this stack deep enough to corrupt it.",
         stack=lambda: extract_local_macro("drivers/ui/lvgl_port.c",
             r'static StackType_t s_lvgl_task_stack\[(\d+)\s*/\s*sizeof\(StackType_t\)\]',
             r'xTaskCreateStaticPinnedToCore\(\s*lvgl_port_task,\s*"lvgl",\s*sizeof\(s_lvgl_task_stack\)'),
         # lvgl_port_task's own body is thin -- practically all of its real
         # depth lives behind lv_timer_handler()'s internal function-pointer
         # dispatch (stack_budget_lib.has_unresolved_dispatch() confirms this
         # task is flagged), which this walk cannot follow at all. extra_roots
         # is now MECHANICALLY DISCOVERED (2026-09-24, lvgl_callback_discovery.py)
         # by scanning firmware/KilnFW/App for every lv_obj_add_event_cb/
         # lv_timer_create/lv_display_set_flush_cb/lv_indev_set_read_cb/
         # ui_confirm on_confirm+on_cancel registration site and resolving the
         # callback argument to a concrete symbol (including one level of
         # parameter-forwarding, e.g. ui_topbar.c's build_icon()/
         # build_icon_named() helpers, and through ui_confirm.c's own indirect
         # confirm_yes_cb/confirm_close_cb -> on_confirm/on_cancel hop) -- a
         # hand-kept list of 8 names used to cover only timer/flush/touch
         # callbacks and had NO lv_obj event callbacks at all, missing chains
         # like confirm_yes_cb -> ui_home_confirm_start_yes_cb ->
         # ui_home_do_start (measured by hand at 2512 B, invisible before this
         # change). See lvgl_callback_discovery.py's module docstring for the
         # scan's exact coverage, the FAIL-vs-NOTE decision for unresolved
         # names (bare unresolved identifier is FATAL, same severity as any
         # other extra_roots resolution failure below; a callback argument that
         # is a dynamic struct-field expression, not a bare name, is reported
         # as a NOTE and left as an accepted, pre-existing indirect-dispatch
         # gap -- architecturally the same class of gap lv_timer_handler()'s
         # own dispatch already leaves open, not a new one this change
         # introduces), and the vacuity floor (MIN_PLAUSIBLE_LVGL_CALLBACKS)
         # enforced in main() below. Measuring each discovered root as its own
         # path and adding the deepest onto this task's base still leaves
         # genuinely dynamic dispatch (LVGL's own internals, animations, the 4
         # noted struct-field forwards) unresolved, which is why this task
         # still reports INDETERMINATE, never a bare pass; see main().
         extra_roots=_LVGL_ROOTS),
    dict(name="screen_idle", root="screen_idle_task",
         stack=lambda: extract_int_literal("drivers/ui/screen_idle.c",
             r'xTaskCreatePinnedToCore\(screen_idle_task,\s*"screen_idle",\s*(\d+)')),
    dict(name="uart_owner_evt_task", root="hal_uart_esp_event_task",
         stack=lambda: extract_sdkconfig_macro("main_network_http.c",
             r'uart_owner_init\(&ctx->uart_owner,.*?UART_OWNER_STACK_SIZE',
             "CONFIG_KILNCTL_UART_OWNER_STACK_SIZE")),
    dict(name="uart_proto_rx", root="uart_protocol_rx_task",
         stack=lambda: extract_sdkconfig_macro("main_network_http.c",
             r'uart_protocol_init\(&ctx->uart_proto,.*?UART_PROTOCOL_STACK_SIZE',
             "CONFIG_KILNCTL_UART_PROTOCOL_STACK_SIZE")),
    dict(name="kiln_cfg_swap", root="swap_worker_task",
         # Added 2026-09-22 after a REAL hardware stack overflow
         # (docs/audits/kiln_cfg_swap_stack_overflow_2026-09-22.md):
         # "A stack overflow in task kiln_cfg_swap has been detected", coredump
         # confirmed via espcoredump against a matching archived ELF. This task
         # had `stack_margin_register()` (registration only, no depth ceiling)
         # since it was created, but no entry in this table -- swap_worker_task
         # directly calls kiln_cfg_swap_apply() and kiln_cfg_swap_boot_recover()
         # (both in persist/kiln_cfg_swap.c), so the plain root below already
         # reaches the whole deep path with no indirect-dispatch gap to model.
         stack=lambda: extract_local_macro("drivers/persist/kiln_cfg_swap_worker.c",
             r'#define SWAP_WORKER_STACK_BYTES\s+(\d+)',
             r'xTaskCreate\(swap_worker_task,\s*"kiln_cfg_swap",\s*SWAP_WORKER_STACK_BYTES')),
    dict(name="wifi_prov_owner", root="owner_task", expect_path="wifi_prov.c",
         # Registered 5cd11231 (2026-09-28): the ONE task that ever writes
         # s_wifi, created once in wifi_prov_start() and never torn down (a
         # stale check_stack_margin_registration.ps1 exemption -- "provisioning
         # -only command owner, torn down with the provisioning session" --
         # predated that; the task is actually long-lived). `root="owner_task"`
         # collides with kiln_io_owner.c/thermo_owner.c's own `owner_task`
         # symbol, so `expect_path` disambiguates the same way those two rows
         # do.
         stack=lambda: extract_int_literal("drivers/net/wifi_prov.c",
             r'xTaskCreatePinnedToCore\(owner_task,\s*"wifi_prov_owner",\s*(\d+)')),
]

# Measured 2026-09-09 against KilnCtrl.elf as built that day (the run that
# added this check) -- see module docstring "CEILINGS". Filled in below the
# table (rather than inline) so the baseline-capture pass that produced them
# is auditable as one block; retighten a value down if a fix legitimately
# shrinks it, never raise one to paper over a regression.
CEILING_BYTES = {
    # 2026-10-09 LONG-CALL RE-BASELINE. stack_budget_lib/the legacy parser now
    # resolve `l32r aN,<lit>` + `callx8 aN` long calls (all IRAM/ROM and
    # >512 KB flash calls), which the walk used to drop silently. Every figure
    # below that this change touched is the new measured total from a fresh
    # target build; older per-entry notes quoting smaller numbers are history.
    # The tiny tasks grew most: any ESP_LOG reaches esp_log -> ... ->
    # pvPortMalloc -> __assert_func through long calls (~1 KB, conservative:
    # the assert branch aborts, but it runs on the task's own stack).
    # Measured 2026-09-20 against a KilnCtrl.elf freshly built by
    # check_00_kilnfw_target_build.ps1 in a clean worktree, immediately after
    # registering this task (see TASKS["pico_auto_update"]'s own comment for
    # why: ota_http_check_interlocks() does SPI reads/zone snapshots normally
    # run on the 8 KB httpd stack, and this task's declared stack is 4096 B).
    # Ceiling 3104 B; the tool's own lower bound (indeterminate walk) reads
    # 692 B honest free (16.9% of 4096 B) on the declared stack -- not the
    # 992 B / 24.2% an earlier draft of this comment claimed. Not over
    # budget; reported here per the standing instruction not to bump a
    # stack size just because a checker was newly wired up.
    "pico_auto_update": 3104,
    "gpio_probe": 3376,
    "link_watchdog": 1168,
    # 3792 = 48 (bx_worker_task's own dispatch loop) + 3744 (the deepest of
    # the enumerated dispatch targets, safety_poll_pico_half_recapture_job --
    # see TASKS["bx_flash_worker"]'s extra_roots comment for why that set is
    # enumerable here and is NOT enumerable for lvgl). Measured 2026-09-16
    # against a KilnCtrl.elf freshly built by check_00_kilnfw_target_build.ps1.
    # The previous 48 was the loop alone: it ceilinged the dispatch, not the
    # work, so every job this task has ever run sat above an unmeasured, and
    # unmeasuring, tripwire. Honest free at this number: 6448 B (63.0%) of the
    # declared 10240 B. Still a LOWER BOUND (NVS/LittleFS internals dispatch
    # indirectly) and still reported INDETERMINATE, never a pass.
    # 2026-10-08: 3792 -> 3840. profiles_handle_message grew from 3744 to 3760 B
    # (walk total 3808 B) after unrelated profile-path commits; 16 B over. The
    # declared stack is 10240 B (BX_WORKER_STACK), so honest free is still 6432 B
    # (62.8%) -- the ceiling is a tripwire on growth, not a margin problem, and no
    # stack bytes change. (3840 was set against a 3808 B walk; that figure is stale.)
    # 2026-10-09: 6160 is the real measured walk total on dev tip (check_all_task_stack_budgets
    # --dump-ceilings), no headroom added: the profiles_handle_message dispatch root grew
    # 5408 -> 5424 B (readiness_gate_collect frame 256 -> 272 from dashboard_status_t growth).
    "bx_flash_worker": 6160,  # 2026-10-09: re-measured on dev tip (was 6144); ample headroom of 10240 B
    "info_uart_bridge": 3200,
    "io_uart_bridge": 3248,
    "safety_uart_bridge": 3312,
    "thermo_uart_bridge": 3152,
    "touch_uart_bridge": 3168,
    # Measured 2026-09-22 against a KilnCtrl.elf freshly built in worktree
    # C:\wt\swapstack_yrljuq (rebased onto origin/main f5a793ce), superseding
    # the same-day static-locals fix below. Review of that fix
    # (docs/audits/kiln_cfg_swap_stack_overflow_2026-09-22.md) found the
    # `static` aggregates (target_blob[896]x2, kiln_pkg_safety_t ~772B x2,
    # kiln_cfg_swap_pending_t ~1688B x2, live_blob[896]) cost ~8.5 kB of
    # PERMANENT internal .bss every boot on a board with a real DRAM-
    # exhaustion history (project_esp_internal_dram_exhaustion). Replaced
    # with one heap_caps_malloc(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT) scratch
    # struct allocated per job and freed before every return, inside a thin
    # wrapper around each of kiln_cfg_swap_apply()/finish_esp_done()/
    # kiln_cfg_swap_boot_recover() (still safe under the same one-job-at-a-
    # time serialization: depth-1 queue, is_busy() interlock, boot_recover
    # runs once before the job loop starts). Confirmed on a side-by-side
    # build of the pre-fix `static` source against the same origin/main base:
    # DIRAM .bss dropped from 102664 B to 94168 B (-8496 B), DIRAM total from
    # 199698 B to 191202 B; .data unchanged at 23367 B. Declared task stack
    # unchanged at 8192 B. New ceiling 4656 B (this walk's own measured
    # total, not headroom-padded) -- 3236 B honest free (39.5% of the
    # declared 8192 B, already net of UNMODELED_OVERHEAD_BYTES). Each
    # wrapper fails loud
    # (set_reason()/latch_boot_fault()) and returns before any state-
    # mutating step if the allocation fails, so a failed allocation can
    # never leave a partial swap.
    "kiln_cfg_swap": 5520,
    "autotune_engine": 2944,
    # profile_exec_wdt: this 2496 is a 2026-09-09 baseline capture used as a
    # regression tripwire -- NOT a measured worst case. The task's deep path
    # (heat_enable_reconcile -> send_enable -> safety_exchange -> the UART
    # send chain) was never entered on the run that produced it, and the
    # static walk reports the task INDETERMINATE because that chain ends in
    # unresolved indirect calls. Its declared size is 4096 (profile_executor_
    # start.c). Those three numbers answer three different questions and the
    # task's true worst case is unmeasured -- see that file's "THE THREE
    # NUMBERS, RECONCILED" comment before quoting any of them as safe.
    # Deliberately NOT raised to reconcile them: that is what this table's
    # own comment above calls papering over a regression.
    # 2026-10-10, safety link review F1: the watchdog loop now calls
    # heat_enable_note_pico_state() (K4 reconcile) before heat_enable_reconcile(),
    # measured 2688 -> 2704 B (26.7% of the 4096 B stack still free). Re-pinned.
    # firing review item 1 (guard9_prelock_check inlined in watchdog_task_entry): measured 2704 -> 2720 B (honest free 1076 B of 4096 B).
    # 2026-10-10, safety-link fix batch 2 (review MED-4): the declared stack
    # went 4096 -> 6144 B (profile_executor_start.c) because the heat_enable
    # reconcile/resend chain (send_enable -> safety_link_request_enable ->
    # safety_exchange -> uart send) runs routinely on this task now, and the
    # static walk reaches 2736 B through it (measured on the 2026-10-10 target
    # ELF; the 2720 B ceiling was 16 B short once note_pico_boot and
    # pause_with_reason joined the loop). Honest free at 6144 B: 3108 B
    # (50.6%); the same walk at the old 4096 B would leave 1060 B -- both far
    # above the 512 B margin the review asked for. NOT a live high-water
    # measurement (no bench run in this batch): the walk stays INDETERMINATE
    # on unresolved indirect calls, so a bench HWM after forcing a K4
    # re-request mid-firing is still the honest confirmation.
    "profile_exec_wdt": 2736,
    "ota_rollback_reboot": 2464,
    # Inherited from ota_rollback_reboot (same shape: announce-reboot send + hal_wdt_reboot); not measured -- never run on hardware.
    "recovery_boot": 2208,
    "ota_pico_rollback": 2896,
    # 2026-09-25 fix-then-push review: the previous 2736 ceiling here was
    # WRONG -- it was borrowed from ota_pico_rollback on the assumption the
    # two tasks were a comparable shape, but ota_pico_rollback's OWN resolved
    # depth is only 1888 B (see the walk's own report for that entry), so it
    # was never a valid stand-in either way. With ct_auto_zero_job wired in as
    # an extra_roots entry (this task's own trampoline, run_job(), dispatches
    # through http_async_job_fn_t, a function pointer the static walk cannot
    # follow on its own), this checker now measures the REAL resolved depth:
    # http_async_job_task (32 B) + ct_auto_zero_job's deepest known chain
    # (ct_auto_zero_job -> safety_cfg_write_apply_pairs -> apply_pairs_ex ->
    # safety_cfg_store_refetch_locked -> safety_link_get_config_page, a
    # 1024 B frame -> uart_protocol_send_broadcast -> frame_and_send, 3280 B)
    # = 3312 B total, against a 6144 B declared stack (raised from 4096 B in
    # the same review, after accounting for an ESP_LOG-through-uart_log_vprintf
    # path adding roughly another 1344 B (esp_log_write ~128 B +
    # uart_log_vprintf ~736 B + vsnprintf ~480 B) on top of this walk's own
    # ~3612 B estimate before that raise). Ceiling set to the number this walk
    # actually measures, not a hand-copied guess -- still INDETERMINATE (the
    # walk cannot follow every indirect call in this chain), so this is a
    # real lower bound, not a proven worst case.
    # 2026-09-28 A4 review, follow-up: backup_import_job's real resolved
    # depth (this checker's own re-measurement, not the earlier hand estimate
    # above) is 4528 B, over the 3760 B ceiling that review landed --
    # tightened here to the number the walk actually reports. All three
    # http_async_job_try_start() call sites (ct_auto_zero_job,
    # bench_preset_job, backup_import_job) were raised from a declared 6144 B
    # to 8192 B in the same change: adding the ~1344 B ESP_LOG-through-
    # uart_log_vprintf overhead (see the ct_auto_zero_job comment above) to
    # 4528 B gives about 5872 B of 8192 B, ~2.3 KB spare -- the old 6144 B
    # stack left only ~270 B, too tight given this is still an INDETERMINATE
    # lower bound (unresolved indirect calls in backup_import_apply()'s own
    # chain), not a proven worst case.
    # 2026-10-02 S7 follow-up: the writer-guard wiring (6bf98270) put
    # safety_cfg_writer_release() (48 B) on the trampoline's own walk, so the
    # measured total is 80 B own + 4496 B backup_import_job = 4576 B, still
    # about 2.3 KB under the 8192 B stack after the ESP_LOG overhead above.
    # 2026-10-05 WP9: the update_repo restore calls added to backup_import_apply()
    # (backup_import_update_repo(), NOINLINE) grew backup_import_apply_two_pass's own
    # frame by 16 B (1648 -> 1664 B; the helper's own frame is not on the deepest
    # path), so the measured total is 80 B own + 4512 B backup_import_job = 4592 B,
    # about 2.25 KB under the 8192 B stack after the ESP_LOG overhead above
    # (4528 B ESP_LOG-inclusive ceiling + ~1.4 KB of ESP_LOG frames = ~5.9 KB used;
    # an earlier revision of this note said 3.3 KB, which forgot that overhead).
    "http_async_job": 7632,  # 2026-10-09: re-measured on dev tip (was 7552); honest free well above 20%
    "recovery_exit": 1056,
    # 2026-10-09: 112 -> 192 B. Measured on a clean origin/main target build
    # (5ddf68d1): backlight_pwm_task 80 + hal_pwm_set_duty 32 + ledc_set_duty 48 +
    # _ledc_fade_hw_release 32 = 192 B of the 3072 B stack (84% free). The walk
    # now follows hal_pwm_set_duty into the IDF LEDC driver; the old 112 B only
    # covered task + hal_pwm_set_duty. Real, tiny, stack is ample: ceiling follows.
    "backlight_pwm": 1168,
    "i2c_owner_ns2009": 1584,
    "i2c_owner_sx1509": 1584,
    # 2026-10-10 K7 relay IO fixes: 1984 -> 2160 B. The deepest walk now runs
    # owner_task -> kiln_io_set_relay_mask -> kiln_io_reinit_locked ->
    # relays_to_inputs -> resync -> SX1509 write chain -> ESP_LOG -> __assert_func
    # (288 B). Measured on a target build; 1636 B (39.9%) of the 4096 B stack stays
    # free, so the stack is not raised (internal RAM floor, owner 2026-10-01).
    "kiln_io_owner": 2160,
    "thermo_owner": 1616,
    "telemetry_log": 3008,
    "danger_mode": 2256,
    "safety_owner_evt": 1184,
    "safety_proto_rx": 4576,
    # 3136 = 3104 (prior baseline) + 32. 2026-09-10: safety_cfg_store_
    # refetch_nonblocking() (safety_cfg_store.c) was made non-static/public
    # (safety_cfg_store.h) so the ceiling-reconcile writer path
    # (safety_cfg_http.c's confirm_commit_landed(), now callable with
    # nonblocking_refetch=true from safety_ceiling_sync.c, itself called
    # from safety_poll_task) can use the same non-blocking refetch
    # safety_cfg_store_maybe_refetch() already used, instead of the
    # forbidden blocking safety_cfg_store_refetch() (portMAX_DELAY) --
    # see safety_cfg_store.c:1488-1510's own "ONLY path safety_poll_task
    # may take" rule. Losing `static` on that function removed the
    # compiler's ability to inline it into safety_cfg_store_maybe_
    # refetch()'s call in the deepest measured path here, adding one new
    # 32 B call frame (safety_cfg_store_maybe_refetch ->
    # safety_cfg_store_refetch_nonblocking -> safety_cfg_store_refetch_
    # locked -> ...), not a growth in any function's own locals. Measured
    # against a freshly rebuilt KilnCtrl.elf (build_kilnfw, same day) with
    # 4756 B (58.1%) of the 8192 B stack still honestly free -- ordinary,
    # understood growth, not a regression to paper over.
    "safety_poll": 3360,
    # 4880 = 752 (lvgl_port_task's own deepest resolved path) + 4128
    # (ui_home_refresh_cb, the deepest of the extra_roots callbacks -- see
    # TASKS["lvgl"]'s comment), measured 2026-09-09 against KilnCtrl.elf as
    # rebuilt that day. Still a LOWER BOUND: this task remains INDETERMINATE
    # because lv_timer_handler()'s own internals (animations, other lv_obj
    # event callbacks, anything a future page adds) are not covered by
    # extra_roots and are not resolvable at all by this walk.
    #
    # Re-verified 2026-09-18 against a freshly rebuilt KilnCtrl.elf
    # (idf.py build, clean worktree at origin/main): own depth is still
    # 752 B and ui_home_refresh_cb is still the deepest extra_root at
    # 4128 B, so the total is UNCHANGED at 4880 B -- no code on this
    # task's measured paths grew since 2026-09-09. This pass also closed
    # a coverage gap found while re-verifying: 906170a9 ("Web auth section
    # 7 + LCD half of section 8") added ui_lcd_lock.c's own
    # lv_timer_create(tick_timer_cb, ...) -- a periodic UI timer of the
    # exact same shape as the five refresh_cb timers already enumerated
    # in extra_roots -- without adding it there, so it was silently
    # unmeasured (not merely INDETERMINATE: absent from the walk
    # entirely). Measured on its own: 928 B deepest resolved path
    # (tick_timer_cb -> ... -> lv_malloc_core, cut short by an unresolved
    # indirect call inside LVGL's own event-list cleanup), well under
    # ui_home_refresh_cb's 4128 B, so adding it to extra_roots (below)
    # does not change this ceiling. Honest free at 4880 B: 3012 B
    # (36.8% of the declared 8192 B) -- thin relative to some other
    # tasks in this table, but unchanged from the 2026-09-09 baseline,
    # and this is the task named in the 2026-09-04 panic post-mortem, so
    # thin margin here is flagged, not just noted: if a future page or
    # callback measurably deepens this task's real worst case, treat it
    # as a hazard needing review, not a routine ceiling bump.
    "lvgl": 7520,  # 2026-10-10: K7 kiln_io_lcd_dc now takes the io lock (+32 B, was 7488); 23.6% free
    # 3152 = 3008 (prior baseline, 2026-09-09) + 144, from bfa60679
    # ("Refuse to start a firing on a quarantined zones config, surface it
    # everywhere") closing the ota_rollback_esp() silent-default-PID-gains
    # hazard documented in CLAUDE.md. screen_idle.c itself is BYTE-IDENTICAL
    # since the 3008 baseline (verified: `git diff <3008-baseline>..HEAD --
    # .../ui/screen_idle.c` is empty) -- the growth is entirely in callees on
    # this task's own reachable path:
    #   +96 B in screen_idle_refresh_inputs's own frame (2224 vs 2128 B):
    #   its local `dashboard_status_t status;` grew because dashboard_http.h
    #   added 5 new fields (2 bool, 2 uint8_t, a char[96] reason string) to
    #   surface the quarantine fault -- no line in screen_idle.c changed,
    #   only the struct it embeds on its own stack.
    #   +48 B in dashboard_get_status's own frame (416 vs 464 B): a new
    #   local `zones_cfg_load_fault_t load_fault;` plus the
    #   `zones_config_get_load_fault()` call and an `snprintf()` into
    #   out->zones_config_load_fault_reason.
    # Verified 2026-09-16 against a real linked KilnCtrl.elf (`idf.py build`,
    # clean worktree at this commit's parent, backup_import.c:1284's
    # -Werror=format-truncation break already fixed by 7ad48c62 by then) via
    # this checker script itself: total 3152 B exactly, honest free 2692 B
    # of 6144 B (43.8%) after UNMODELED_OVERHEAD_BYTES. That is down from the
    # 3008-baseline's headroom, as expected for a straightforward size
    # increase -- this is legitimate, understood growth closing a real
    # safety hazard, not a regression to paper over, and not one to design
    # around by removing the surfaced fault.
    # 3504 = 3152 (prior baseline, above) + 352, from 6985c89b/da90fbd9
    # ("M13 second sweep: surface kiln_cfg_swap boot-recovery give-ups")
    # closing the silent-migration-loss hazard where an interrupted
    # two-processor kiln-config swap could leave the board alarmed with no
    # operator-visible reason. screen_idle.c itself is unchanged by that
    # commit pair -- the growth is again entirely in dashboard_http.h's
    # dashboard_status_t, which screen_idle_refresh_inputs holds as a plain
    # (non-pointer) local on its own stack:
    #   dashboard_status_t gained zones_config_migration_persist_fault
    #   (bool) + two uint8_t version fields, plus kiln_cfg_swap_boot_fault
    #   (bool) + int32_t target_id + a 200-byte char[] reason string copied
    #   verbatim from the latch -- no line in screen_idle.c changed, only
    #   the struct it embeds.
    # Verified 2026-09-16 against a real linked KilnCtrl.elf, freshly built
    # (`idf.py build`) in a clean worktree minted at origin/main == da90fbd9,
    # via this checker script itself: total 3504 B (a LOWER BOUND -- this
    # task remains INDETERMINATE, an unresolved indirect call outside
    # extra_roots' coverage), honest free 2340 B of 6144 B (38.1%) after
    # UNMODELED_OVERHEAD_BYTES. Legitimate, understood growth closing a real
    # safety-visibility hazard (the same one CLAUDE.md's boot_guard section
    # already documents as fixed), not a regression to paper over, and not
    # one to design around by shrinking the surfaced fault's own message.
    "screen_idle": 3904,  # 2026-10-09: re-measured on dev tip (was 3888)
    "uart_owner_evt_task": 1184,
    "uart_proto_rx": 4576,
    # Measured 2026-09-24 against a KilnCtrl.elf freshly built by
    # check_00_kilnfw_target_build.ps1 (this pass's own build, worktree
    # C:\wt\sweepstack_dmt6la rebased onto origin/main 4ddad119). Deepest
    # measured path: zone_sweep_task -> zone_sweep_unstage_k_ct (constprop)
    # -> zone_sweep_confirm_k_ct_landed (constprop) ->
    # safety_cfg_store_refetch -> safety_cfg_store_refetch_locked ->
    # safety_link_get_config_page -> uart_protocol_send_broadcast ->
    # frame_and_send -> hal_uart_send_blocking -> uart_write_bytes ->
    # uart_tx_all -> uart_enable_tx_write_fifo = 2896 B, honest free 900 B
    # (22.0%) of the declared 4096 B, after UNMODELED_OVERHEAD_BYTES. Still
    # a LOWER BOUND (INDETERMINATE -- an unresolved indirect call in this
    # graph), same caveat as every other task in this table. 2896 B is well
    # under the 4096 B declared stack, so this does not meet the "raise the
    # stack" bar on its own -- but 22.0% honest free for an INDETERMINATE
    # task that reaches into safety_cfg_store's NVS-backed refetch path is
    # thin margin, same class flagged (not bumped) for lvgl above; worth a
    # real hardware high-water-mark measurement before ruling this settled.
    "zone_sweep": 3888,
    # Measured 2026-09-29 against a KilnCtrl.elf freshly built by build_kilnfw
    # in a clean worktree (C:\wt\stacktbl_4yfec2) minted at origin/main, right
    # after adding this task's TASKS row (5cd11231 registered
    # stack_margin_register() for wifi_prov_owner but left it out of this
    # table -- see check_stack_task_table_consistency.ps1's finding). This
    # walk's own measured total, not headroom-padded: 1184 B of the declared
    # 4096 B (heap-allocated via xTaskCreatePinnedToCore, so this task costs
    # zero .dram0.bss), already net of UNMODELED_OVERHEAD_BYTES.
    "wifi_prov_owner": 2944,
}


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--dump-ceilings", action="store_true",
                     help="print a CEILING_BYTES dict literal for the measured totals instead of "
                          "grading against CEILING_BYTES (used to (re)capture the baseline table)")
    # Negative-test hooks: force one task's measured numbers to force a FAIL deterministically
    # without touching production code (used by the human negative test on a copy of the ELF).
    ap.add_argument("--force-ceiling", metavar="TASK=BYTES", action="append", default=[])
    ap.add_argument("--sdkconfig", default=None,
                     help="the sdkconfig that produced --elf. Normally inferred from --elf "
                          "(see use_sdkconfig_for_elf); REQUIRED for an ELF in elf_archive/, "
                          "whose producing config is not the project's live one.")
    args = ap.parse_args()

    forced_ceilings = {}
    for item in args.force_ceiling:
        k, _, v = item.partition("=")
        forced_ceilings[k] = int(v)

    sdk_path, sdk_origin = use_sdkconfig_for_elf(args.elf, args.sdkconfig)
    # Say out loud which config this run is grading against. A run must never
    # be able to quietly grade an ELF against a config that did not produce it.
    if sdk_path is None:
        print(f"check_all_task_stack_budgets: sdkconfig: {sdk_origin}")
    else:
        print(f"check_all_task_stack_budgets: sdkconfig: {sdk_path} ({sdk_origin})")

    if not os.path.isfile(args.elf):
        print("check_all_task_stack_budgets: SKIP: no ELF at " + args.elf)
        print("  Build KilnFW (build_kilnfw / idf.py build, or check_00_kilnfw_target_build.ps1) and "
              "re-run; unmeasured, not passing -- this is a SKIP, not a pass.")
        return 3

    objdump = lib.find_objdump()
    if not objdump:
        print("check_all_task_stack_budgets: SKIP: xtensa-esp32s3-elf-objdump not found "
              "(set XTENSA_OBJDUMP). Unmeasured, not passing.")
        return 3
    addr2line = lib.find_addr2line()

    try:
        parsed = lib.parse(objdump, args.elf)
    except lib.ElfParseError as exc:
        if exc.skip:
            print(f"check_all_task_stack_budgets: SKIP: {exc}")
            return 3
        print(f"check_all_task_stack_budgets: FAIL -- {exc}")
        return 1

    results = []
    errors = []
    excluded = []

    # lvgl extra_roots discovery report -- printed UNCONDITIONALLY per this
    # check's own requirement ("print the count found"), not only on failure,
    # so a silently-broken scan is visible on every green run too.
    print(f"check_all_task_stack_budgets: lvgl callback discovery: "
          f"{_LVGL_RAW_COUNT} registration site(s) found, "
          f"{len(_LVGL_ROOTS)} unique root(s) resolved, "
          f"{len(_LVGL_DISCOVERY_NOTES)} dynamic/unresolvable note(s), "
          f"{len(_LVGL_DISCOVERY_ERRORS)} resolution error(s)")
    for _note in _LVGL_DISCOVERY_NOTES:
        print(f"  NOTE: {_note}")
    if _LVGL_RAW_COUNT < MIN_PLAUSIBLE_LVGL_CALLBACKS:
        errors.append(
            f"lvgl callback discovery: only {_LVGL_RAW_COUNT} registration site(s) found "
            f"(floor {MIN_PLAUSIBLE_LVGL_CALLBACKS}) -- treated as a broken scan "
            f"(wrong directory, regex regression, etc.), not an empty UI; see "
            f"lvgl_callback_discovery.py")
    if _LVGL_DISCOVERY_ERRORS:
        for _err in _LVGL_DISCOVERY_ERRORS:
            errors.append(f"lvgl callback discovery: {_err}")

    # SOURCE-SIDE STRUCTURAL GUARD, run before any row is measured: no root may
    # be gated by something this table cannot describe. See
    # normalised_gate_violations() for why this is enforced rather than merely
    # commented.
    _tracked_roots = set()
    for _t in TASKS:
        if _t.get("root"):
            _tracked_roots.add(_t["root"])
        _extra_roots_spec = _t.get("extra_roots") or ()
        if callable(_extra_roots_spec):
            _extra_roots_spec = _extra_roots_spec()
        for _extra in _extra_roots_spec:
            _tracked_roots.add(_extra[0] if isinstance(_extra, (tuple, list)) else _extra)
    errors.extend(normalised_gate_violations(_tracked_roots))
    errors.extend(declared_edge_violations())
    for task in TASKS:
        tname = task["name"]
        # KCONFIG-GATED ROWS. A task whose whole body is #if CONFIG_X'd out is
        # legitimately absent from a build with X off -- failing on that would
        # make this check unrunnable in any clean worktree (which regenerates
        # sdkconfig from sdkconfig.defaults + Kconfig defaults, where
        # CONFIG_KILNCTL_ENABLE_GPIO_PROBE is n). But "absent because its
        # option is off" and "absent because somebody renamed or deleted it"
        # must stay distinguishable, so this is NOT a skip and NOT a blanket
        # "missing roots are fine":
        #   * option ON  -> measured exactly as any other row; a vanished root
        #                   is still a hard FAIL.
        #   * option OFF -> the root is REQUIRED to be absent, and that is
        #                   asserted here. If it resolves anyway, the ELF and
        #                   the sdkconfig disagree (stale build, or the #if no
        #                   longer matches the symbol this table names) and
        #                   that is a FAIL too.
        #   * sdkconfig unreadable -> FAIL, never "assume off".
        # Every row without a `kconfig` key keeps the original unconditional
        # behaviour: absent root == FAIL.
        kcfg = task.get("kconfig")
        if kcfg is not None:
            try:
                enabled = sdkconfig_bool(kcfg)
            except ValueError as e:
                errors.append(f"{tname}: cannot adjudicate {kcfg}: {e}")
                continue
            if not enabled:
                try:
                    lib.resolve_root(parsed, task["root"], args.elf, addr2line,
                                     task.get("expect_path"))
                except ValueError:
                    # SOURCE-SIDE VERIFICATION OF AN EXCLUDED ROW (2026-09-15).
                    # Absence from the ELF alone is one-sided evidence. With
                    # the option off -- which for gpio_probe is EVERY clean
                    # worktree and every default build -- a task that had been
                    # deleted or renamed outright in source produces exactly
                    # the same absence, and the previous version of this branch
                    # reported it as "confirmed absent" and exited 0. (Proved
                    # by deleting gpio_probe_task from gpio_probe.c: the check
                    # passed.) An `#if` that no longer matches the declared
                    # symbol was always caught, loudly and in the safe
                    # direction, by the disagreement FAIL just below -- plain
                    # deletion was the real hole.
                    #
                    # So an excluded row is excused from MEASURING the symbol
                    # in the ELF, never from knowing the code is still there:
                    # the row's own stack= extractor must still match its
                    # xTaskCreate* call site (which pins the task function name
                    # AND the stack literal), and the file must still gate
                    # something on the Kconfig symbol this row names. Both are
                    # pure source reads -- no measurement cost, and insensitive
                    # to whether the task was compiled in.
                    try:
                        task["stack"]()
                        verify_kconfig_guard(task["kconfig_src"], kcfg)
                    except (ValueError, OSError) as e:
                        errors.append(
                            f"{tname}: {kcfg} is off in {SDKCONFIG_PATH} and root symbol "
                            f"{task['root']!r} is correctly absent from the ELF -- but the "
                            f"SOURCE-side definition could not be confirmed: {e} "
                            "A Kconfig-gated row is excused from being measured in the ELF, "
                            "never from still existing in source; 'compiled out' and "
                            "'deleted' must not look the same to this check.")
                        continue
                    excluded.append((tname, kcfg))
                    continue
                errors.append(
                    f"{tname}: {kcfg} is not set in {SDKCONFIG_PATH}, so root symbol "
                    f"{task['root']!r} must not exist in this build -- but it resolves in "
                    f"{args.elf}. Either the ELF is stale with respect to that sdkconfig, or "
                    "the #if gating around that task no longer matches the Kconfig symbol "
                    "this table names.")
                continue
        try:
            root_addr = lib.resolve_root(parsed, task["root"], args.elf, addr2line,
                                          task.get("expect_path"))
        except ValueError as e:
            errors.append(f"{tname}: could not resolve root symbol {task['root']!r}: {e}")
            continue
        try:
            declared = task["stack"]()
        except ValueError as e:
            errors.append(f"{tname}: could not derive declared stack size from source: {e}")
            continue

        def _resolve_any(n):
            return lib.resolve_root(parsed, n, args.elf, addr2line)
        walk_parsed = lib.drop_worker_only_edges(parsed, _resolve_any)
        if task.get("declared_edges"):
            def _resolve_declared(n):
                return lib.resolve_root(parsed, n, args.elf, addr2line)
            try:
                walk_parsed = apply_declared_edges(parsed, _resolve_declared)
            except ValueError as e:
                errors.append(f"{tname}: cannot apply DECLARED_EDGES: {e}")
                continue
        own_total, path_addrs = lib.deepest(root_addr, walk_parsed)

        # Known callback entry points that also run ON this task's own stack
        # (registered with some library that dispatches to them by function
        # pointer -- see the "extra_roots" comment on TASKS["lvgl"]). Each is
        # measured as its own root and the DEEPEST one is added onto the
        # task's own depth: only one callback runs at a time, so they do not
        # stack on top of each other, but each one's frames stack on top of
        # whatever got the task to the dispatch point in the first place.
        extra_total = 0
        extra_label = None
        extra_errors = []
        # extra_roots may be a plain list or a zero-arg callable -- the
        # callable form (docs/HTTP_POST_OWNER_MIGRATION.md slice A2,
        # http_async_job's own row) lets a row's set of registered fns depend
        # on the ELF's OWN sdkconfig (bench_preset_job only exists when
        # CONFIG_KILNCTL_DEV_TOOLS=y, safety_cfg_http.c's #if around its
        # definition) without needing the stricter present/absent kconfig=
        # adjudication this file's whole-row KCONFIG-GATED TASKS handling
        # applies -- http_async_job itself is unconditionally present either
        # way (ct_auto_zero_job alone still gives it a real callee), so a
        # dev-tools-off ELF simply omits bench_preset_job from the candidates
        # measured here rather than failing on its absence.
        extra_roots_spec = task.get("extra_roots", [])
        if callable(extra_roots_spec):
            extra_roots_spec = extra_roots_spec()
        for extra_name, extra_path in extra_roots_spec:
            try:
                extra_addr = lib.resolve_root(parsed, extra_name, args.elf, addr2line, extra_path)
            except ValueError as e:
                extra_errors.append(f"{extra_name} ({extra_path}): {e}")
                continue
            d, _p = lib.deepest(extra_addr, walk_parsed)
            if d > extra_total:
                extra_total = d
                extra_label = extra_name
        if extra_errors:
            errors.append(f"{tname}: could not resolve declared extra_roots callback(s): "
                           + "; ".join(extra_errors))
            continue

        total = own_total + extra_total
        indirect = lib.has_unresolved_dispatch(root_addr, walk_parsed)
        rom_unknown = lib.bodyless_calls(root_addr, walk_parsed)
        overhead = UNMODELED_OVERHEAD_BYTES
        honest_free = declared - total - overhead
        results.append(dict(task=task, declared=declared, own_total=own_total, total=total,
                             path_addrs=path_addrs, root_addr=root_addr, overhead=overhead,
                             honest_free=honest_free, indirect=indirect, rom_unknown=rom_unknown,
                             extra_total=extra_total, extra_label=extra_label))

    if errors:
        print("check_all_task_stack_budgets: FAIL -- could not measure every registered task:")
        for e in errors:
            print(f"  {e}")
        print("  This check must measure something for EVERY task in its table before it can pass "
              "(never a silent partial pass) -- a renamed entry function or a call-site literal that "
              "no longer matches the extractor pattern needs this table updated, not ignored.")
        return 1

    if args.dump_ceilings:
        # Excluded rows appear too, commented, so the dump is a COMPLETE
        # picture of the table. Previously this returned before the
        # excluded-row report and a baseline regenerated with an option off
        # simply omitted that row with no trace -- self-correcting (a later
        # build with the option on FAILs with "no CEILING_BYTES entry") but a
        # foot-gun for whoever regenerates the table.
        print("CEILING_BYTES = {")
        for r in results:
            print(f'    "{r["task"]["name"]}": {r["total"]},')
        for tname, kcfg in excluded:
            print(f'    # "{tname}": EXCLUDED from this dump -- {kcfg} is off in '
                  f"{SDKCONFIG_PATH}, so it was not measured. Keep this row's existing "
                  "CEILING_BYTES value, or re-dump with that option on to capture it.")
        print("}")
        if excluded:
            print(f"# {len(excluded)} of {len(results) + len(excluded)} table row(s) are missing "
                  "above because their Kconfig option is off in this build -- this dump is NOT a "
                  "complete replacement for CEILING_BYTES.")
        return 0

    for tname, kcfg in excluded:
        print(f"-- {tname}: NOT MEASURED, compiled out of THIS build ({kcfg} is not set in "
              f"{SDKCONFIG_PATH}); its root symbol was confirmed absent from the ELF, which is "
              "exactly what that option being off should produce.")
    if excluded:
        print()

    print(f"{len(results)} registered tasks measured (objdump: {objdump})")
    print(f"unmodeled-overhead allowance applied to every task: {UNMODELED_OVERHEAD_BYTES} B "
          "(see module docstring \"OVERHEAD CONSTANT, STATED HONESTLY\" -- a conservative placeholder, "
          "not a live measurement, for any of these 28 tasks)")
    print()

    failed = []
    indeterminate = []
    for r in sorted(results, key=lambda r: r["honest_free"]):
        task = r["task"]
        tname = task["name"]
        # A task with no CEILING_BYTES entry must not silently fall back to
        # "honest-free-only" grading -- that is exactly the kind of quiet
        # degradation this table exists to prevent (a typo'd or newly-added
        # task name would otherwise measure forever without ever being
        # graded against a regression ceiling and nobody would notice).
        if tname in forced_ceilings:
            ceiling = forced_ceilings[tname]
        elif tname in CEILING_BYTES:
            ceiling = CEILING_BYTES[tname]
        else:
            errors.append(f"{tname}: no CEILING_BYTES entry -- every TASKS row must be graded "
                           "against a ceiling (run --dump-ceilings to capture one and add it)")
            continue
        note = f"  [{task['historical_note']}]" if "historical_note" in task else ""
        print(f"-- {tname} (root {task['root']}, declared {r['declared']} B) --{note}")
        running = 0
        for fn in [r["root_addr"]] + r["path_addrs"]:
            fsize = parsed.frames.get(fn, 0)
            running += fsize
            print(f"    {fsize:>6} B  {running:>6} B cumulative  {parsed.names.get(fn, hex(fn))}")
        if r["extra_total"]:
            print(f"    + {r['extra_total']:>6} B  deepest known dispatch-target callback "
                  f"({r['extra_label']}), added to own depth (see TASKS extra_roots)")
        pct = 100.0 * r["honest_free"] / r["declared"] if r["declared"] else 0.0
        print(f"    total {r['total']} B; ceiling {ceiling} B; "
              f"honest free {r['honest_free']} B ({pct:.1f}% of {r['declared']} B)")
        task_failed = False
        if r["total"] > ceiling:
            print(f"    FAIL: {r['total']} B exceeds the {ceiling} B ceiling for {tname}.")
            task_failed = True
        if r["honest_free"] < 0:
            print(f"    FAIL: honest free is negative ({r['honest_free']} B) once the "
                  f"{UNMODELED_OVERHEAD_BYTES} B unmodeled-overhead allowance is counted.")
            task_failed = True
        if r["indirect"]:
            # A confirmed lower bound is still worth printing (a lower bound
            # that already exceeds the ceiling or the declared stack is a
            # real, valid FAIL above), but a task flagged this way must NEVER
            # be reported as a clean pass: its true worst-case path continues
            # through a function pointer this walk cannot see past, so
            # "within budget" here means only "the KNOWN part is within
            # budget", not "this task is safe".
            if task_failed:
                print(f"    INDETERMINATE too: {tname}'s call graph contains an unresolved "
                      "indirect call (callx4/8/12) outside what extra_roots covers -- the FAIL "
                      "above is real (it is a lower bound), but the true worst case may be "
                      "worse still.")
            else:
                print(f"    INDETERMINATE: {tname}'s call graph contains an unresolved indirect "
                      f"call (callx4/8/12) this walk cannot follow -- {r['total']} B above is a "
                      "LOWER BOUND, not a measurement. Not reported as a pass.")
            indeterminate.append(tname)
        if task_failed:
            failed.append(tname)
        print()

    if errors:
        print("check_all_task_stack_budgets: FAIL -- could not grade every measured task:")
        for e in errors:
            print(f"  {e}")
        return 1

    if failed:
        print(f"check_all_task_stack_budgets: FAIL -- {len(failed)} of {len(results)} tasks over "
              f"budget: {', '.join(failed)}")
        print("  Fix by moving large locals off the named task's own stack (heap/static, per this "
              "codebase's established convention), not by enlarging the stack or raising the ceiling "
              "without a documented reason for accepting the new margin.")
        return 1

    # Review F3: WARNING only (never a failure): resolved ROM calls count 0 B.
    n_rom = sum(1 for r in results if r.get("rom_unknown"))
    rom_names = sorted({n for r in results for n in r.get("rom_unknown", ())})
    print(f"check_all_task_stack_budgets: WARNING count: {n_rom} of {len(results)} tasks reach "
          f"resolved ROM calls with unknown stack depth (counted 0 B): {', '.join(rom_names[:6])}")
    confident = len(results) - len(indeterminate)
    if indeterminate:
        print(f"check_all_task_stack_budgets: OK -- {confident} of {len(results)} tasks fully "
              f"measured and within budget; {len(indeterminate)} INDETERMINATE (known lower bound "
              f"only, within budget as far as this walk can see, true depth unresolved): "
              f"{', '.join(indeterminate)}.")
        print("  INDETERMINATE is not a pass claim for those tasks -- see the per-task notes above "
              "for what remains unresolved and why (unresolved indirect/function-pointer dispatch).")
    else:
        print(f"check_all_task_stack_budgets: OK -- all {len(results)} tasks measured and within budget.")
    if excluded:
        names = ", ".join("%s (%s)" % (t, k) for t, k in excluded)
        print(f"  {len(excluded)} table row(s) not measured because their Kconfig option is off in "
              f"this build: {names}. Each was verified ABSENT from the ELF, not merely unmeasured "
              "-- turn the option on to have it measured here.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
