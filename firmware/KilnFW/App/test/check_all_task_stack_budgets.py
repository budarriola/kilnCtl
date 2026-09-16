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

REPO_ROOT = lib.REPO_ROOT
DEFAULT_ELF = lib.DEFAULT_ELF
DEFAULT_SDKCONFIG = lib.DEFAULT_SDKCONFIG

APP_DIR = os.path.join(REPO_ROOT, "firmware", "KilnFW", "App")

UNMODELED_OVERHEAD_BYTES = 300  # see module docstring "OVERHEAD CONSTANT, STATED HONESTLY"


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
    cands = _sdkconfig_candidates(elf)
    for path, origin in cands:
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
# TASKS: one row per stack_margin_register() call site with no dedicated
# checker of its own. `root` is the task's own C entry function (the first
# argument to whichever xTaskCreate* family function creates it -- NOT the
# stack_margin_register() name string, which is only a label). `expect_path`
# disambiguates a root symbol name that is not unique in the ELF (currently
# only `owner_task`, shared verbatim between kiln_io_owner.c and
# thermo_owner.c -- see stack_budget_lib.py's docstring).
# ---------------------------------------------------------------------------
TASKS = [
    dict(name="boot_button", root="boot_button_task",
         stack=lambda: extract_int_literal("drivers/bridge/boot_button.c",
             r'xTaskCreate\(boot_button_task,\s*"boot_button",\s*(\d+)')),
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
    dict(name="bx_flash_worker", root="bx_worker_task",
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
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge_touch.c",
             r'xTaskCreatePinnedToCoreWithCaps\(touch_bridge_task,\s*"touch_uart_bridge",\s*(\d+)')),
    dict(name="autotune_engine", root="task_entry",
         stack=lambda: extract_int_literal("drivers/control/autotune_engine.c",
             r'xTaskCreatePinnedToCoreWithCaps\(task_entry,\s*"autotune_engine",\s*(\d+)')),
    dict(name="profile_exec_wdt", root="watchdog_task_entry",
         stack=lambda: extract_int_literal("drivers/control/profile_executor_start.c",
             r'xTaskCreatePinnedToCore\(watchdog_task_entry,\s*"profile_exec_wdt",\s*(\d+)')),
    dict(name="ota_rollback_reboot", root="ota_rollback_reboot_task",
         stack=lambda: extract_int_literal("drivers/http/ota_http_esp.c",
             r'xTaskCreate\(ota_rollback_reboot_task,\s*"ota_rollback_reboot",\s*(\d+)')),
    dict(name="ota_pico_rollback", root="ota_pico_rollback_task",
         stack=lambda: extract_int_literal("drivers/http/ota_http_pico.c",
             r'xTaskCreate\(ota_pico_rollback_task,\s*"ota_pico_rollback",\s*(\d+)')),
    dict(name="recovery_exit", root="ota_recovery_exit_reboot_task",
         stack=lambda: extract_int_literal("drivers/http/ota_http_recovery.c",
             r'xTaskCreate\(ota_recovery_exit_reboot_task,\s*"recovery_exit_reboot",\s*(\d+)')),
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
         # task is flagged), which this walk cannot follow at all. These are
         # the KNOWN callback entry points lv_timer_handler() invokes that
         # way and that this codebase registers -- the display flush
         # callback, the touch indev read callback, and every page's
         # lv_timer_create() refresh callback (see grep for lv_timer_create
         # across drivers/ui/ui_page_*.c). Measuring each as its OWN root and
         # adding the deepest of them onto this task's base turns "752 B
         # measured, real depth invisible" into a real, source-derived lower
         # bound instead -- still not a full measurement (LVGL's own
         # internals -- animations, other registered lv_obj event callbacks,
         # anything a future page adds -- stay unresolved, which is why this
         # task still reports INDETERMINATE, never a bare pass; see
         # check_all_task_stack_budgets.py's main()).
         extra_roots=[
             ("ili9488_flush_cb", "lvgl_port.c"),
             ("touch_read_cb", "lvgl_port.c"),
             ("refresh_cb", "ui_page_diagnostics.c"),
             ("ui_home_refresh_cb", "ui_page_home.c"),
             ("refresh_cb", "ui_page_network.c"),
             ("refresh_cb", "ui_page_network_manage.c"),
             ("refresh_cb", "ui_page_temperature.c"),
         ]),
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
]

# Measured 2026-09-09 against KilnCtrl.elf as built that day (the run that
# added this check) -- see module docstring "CEILINGS". Filled in below the
# table (rather than inline) so the baseline-capture pass that produced them
# is auditable as one block; retighten a value down if a fix legitimately
# shrinks it, never raise one to paper over a regression.
CEILING_BYTES = {
    "boot_button": 1552,
    "gpio_probe": 3376,
    "link_watchdog": 160,
    # 3792 = 48 (bx_worker_task's own dispatch loop) + 3744 (the deepest of
    # the enumerated dispatch targets, safety_poll_pico_half_recapture_job --
    # see TASKS["bx_flash_worker"]'s extra_roots comment for why that set is
    # enumerable here and is NOT enumerable for lvgl). Measured 2026-09-16
    # against a KilnCtrl.elf freshly built by check_00_kilnfw_target_build.ps1.
    # The previous 48 was the loop alone: it ceilinged the dispatch, not the
    # work, so every job this task has ever run sat above an unmeasured, and
    # unmeasuring, tripwire. Honest free at this number: 4100 B (50.0%) of the
    # declared 8192 B. Still a LOWER BOUND (NVS/LittleFS internals dispatch
    # indirectly) and still reported INDETERMINATE, never a pass.
    "bx_flash_worker": 3792,
    "info_uart_bridge": 2208,
    "io_uart_bridge": 2256,
    "safety_uart_bridge": 2912,
    "thermo_uart_bridge": 2160,
    "touch_uart_bridge": 2176,
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
    "profile_exec_wdt": 2496,
    "ota_rollback_reboot": 1216,
    "ota_pico_rollback": 2736,
    "recovery_exit": 80,
    "backlight_pwm": 112,
    "i2c_owner_ns2009": 144,
    "i2c_owner_sx1509": 144,
    "kiln_io_owner": 720,
    "thermo_owner": 608,
    "telemetry_log": 2560,
    "danger_mode": 2112,
    "safety_owner_evt": 176,
    "safety_proto_rx": 3584,
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
    "safety_poll": 3136,
    # 4880 = 752 (lvgl_port_task's own deepest resolved path) + 4128
    # (ui_home_refresh_cb, the deepest of the extra_roots callbacks -- see
    # TASKS["lvgl"]'s comment), measured 2026-09-09 against KilnCtrl.elf as
    # rebuilt that day. Still a LOWER BOUND: this task remains INDETERMINATE
    # because lv_timer_handler()'s own internals (animations, other lv_obj
    # event callbacks, anything a future page adds) are not covered by
    # extra_roots and are not resolvable at all by this walk.
    
    "lvgl": 4880,
    "screen_idle": 3008,
    "uart_owner_evt_task": 176,
    "uart_proto_rx": 3584,
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

    # SOURCE-SIDE STRUCTURAL GUARD, run before any row is measured: no root may
    # be gated by something this table cannot describe. See
    # normalised_gate_violations() for why this is enforced rather than merely
    # commented.
    _tracked_roots = set()
    for _t in TASKS:
        if _t.get("root"):
            _tracked_roots.add(_t["root"])
        for _extra in (_t.get("extra_roots") or ()):
            _tracked_roots.add(_extra[0] if isinstance(_extra, (tuple, list)) else _extra)
    errors.extend(normalised_gate_violations(_tracked_roots))
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

        own_total, path_addrs = lib.deepest(root_addr, parsed)

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
        for extra_name, extra_path in task.get("extra_roots", []):
            try:
                extra_addr = lib.resolve_root(parsed, extra_name, args.elf, addr2line, extra_path)
            except ValueError as e:
                extra_errors.append(f"{extra_name} ({extra_path}): {e}")
                continue
            d, _p = lib.deepest(extra_addr, parsed)
            if d > extra_total:
                extra_total = d
                extra_label = extra_name
        if extra_errors:
            errors.append(f"{tname}: could not resolve declared extra_roots callback(s): "
                           + "; ".join(extra_errors))
            continue

        total = own_total + extra_total
        indirect = lib.has_unresolved_dispatch(root_addr, parsed)
        overhead = UNMODELED_OVERHEAD_BYTES
        honest_free = declared - total - overhead
        results.append(dict(task=task, declared=declared, own_total=own_total, total=total,
                             path_addrs=path_addrs, root_addr=root_addr, overhead=overhead,
                             honest_free=honest_free, indirect=indirect,
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
