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
were derived from a specific hardware DRAM_PSRAM_PLAN.md capture; no
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
  * Indirect calls (`callx8` through a function pointer) are not followed.
  * Recursion is cut at the first repeat rather than unrolled.
  * ISR/window-overflow spill beyond UNMODELED_OVERHEAD_BYTES is not modelled.
Both make this an UNDER-estimate of true worst-case depth, never an over-
estimate: anything this reports as too deep genuinely is too deep.

EXIT CODES: 0 OK, 1 FAIL (at least one task over ceiling or honest-negative),
3 SKIP (no ELF / no objdump -- never claims success without measuring).
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


def _sdkconfig_value(key):
    if not _SDKCONFIG_CACHE:
        if os.path.isfile(DEFAULT_SDKCONFIG):
            for line in open(DEFAULT_SDKCONFIG, encoding="utf-8", errors="replace"):
                if "=" in line:
                    k, _, v = line.strip().partition("=")
                    _SDKCONFIG_CACHE[k] = v
    if key not in _SDKCONFIG_CACHE:
        raise ValueError(f"{key} not found in {DEFAULT_SDKCONFIG}")
    return int(_SDKCONFIG_CACHE[key])


def extract_sdkconfig_macro(rel_path, usage_pattern, sdkconfig_key):
    """UART_OWNER_STACK_SIZE / UART_PROTOCOL_STACK_SIZE resolve through
    settings.h to a CONFIG_KILNCTL_* sdkconfig value. Confirm the call site
    references the macro, then read the real configured value out of
    sdkconfig -- never hand-copied."""
    text = _read(rel_path)
    if not re.search(usage_pattern, text, re.DOTALL):
        raise ValueError(f"call site does not reference the expected macro in {rel_path}: {usage_pattern!r}")
    return _sdkconfig_value(sdkconfig_key)


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
    dict(name="boot_button", root="boot_button_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/bridge/boot_button.c",
             r'xTaskCreate\(boot_button_task,\s*"boot_button",\s*(\d+)')),
    dict(name="gpio_probe", root="gpio_probe_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/bridge/gpio_probe.c",
             r'xTaskCreatePinnedToCoreWithCaps\(gpio_probe_task,\s*"gpio_probe",\s*(\d+)')),
    dict(name="link_watchdog", root="link_watchdog_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge.c",
             r'xTaskCreatePinnedToCoreWithCaps\(link_watchdog_task,\s*"link_watchdog",\s*(\d+)')),
    dict(name="bx_flash_worker", root="bx_worker_task", ceiling=None,
         stack=lambda: extract_local_macro("drivers/bridge/uart_bridge_ext.c",
             r'#define BX_WORKER_STACK\s+(\d+)',
             r'xTaskCreatePinnedToCore\(bx_worker_task,\s*"bx_flash_worker",\s*BX_WORKER_STACK')),
    dict(name="info_uart_bridge", root="info_bridge_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge_info.c",
             r'xTaskCreatePinnedToCoreWithCaps\(info_bridge_task,\s*"info_uart_bridge",\s*(\d+)')),
    dict(name="io_uart_bridge", root="io_bridge_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge_io.c",
             r'xTaskCreatePinnedToCoreWithCaps\(io_bridge_task,\s*"io_uart_bridge",\s*(\d+)')),
    dict(name="safety_uart_bridge", root="safety_bridge_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge_safety.c",
             r'xTaskCreatePinnedToCoreWithCaps\(safety_bridge_task,\s*"safety_uart_bridge",\s*(\d+)')),
    dict(name="thermo_uart_bridge", root="thermo_bridge_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge_thermo.c",
             r'xTaskCreatePinnedToCoreWithCaps\(thermo_bridge_task,\s*"thermo_uart_bridge",\s*(\d+)')),
    dict(name="touch_uart_bridge", root="touch_bridge_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/bridge/uart_bridge_touch.c",
             r'xTaskCreatePinnedToCoreWithCaps\(touch_bridge_task,\s*"touch_uart_bridge",\s*(\d+)')),
    dict(name="autotune_engine", root="task_entry", ceiling=None,
         stack=lambda: extract_int_literal("drivers/control/autotune_engine.c",
             r'xTaskCreatePinnedToCoreWithCaps\(task_entry,\s*"autotune_engine",\s*(\d+)')),
    dict(name="profile_exec_wdt", root="watchdog_task_entry", ceiling=None,
         stack=lambda: extract_int_literal("drivers/control/profile_executor_start.c",
             r'xTaskCreatePinnedToCore\(watchdog_task_entry,\s*"profile_exec_wdt",\s*(\d+)')),
    dict(name="ota_rollback_reboot", root="ota_rollback_reboot_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/http/ota_http_esp.c",
             r'xTaskCreate\(ota_rollback_reboot_task,\s*"ota_rollback_reboot",\s*(\d+)')),
    dict(name="ota_pico_rollback", root="ota_pico_rollback_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/http/ota_http_pico.c",
             r'xTaskCreate\(ota_pico_rollback_task,\s*"ota_pico_rollback",\s*(\d+)')),
    dict(name="recovery_exit", root="ota_recovery_exit_reboot_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/http/ota_http_recovery.c",
             r'xTaskCreate\(ota_recovery_exit_reboot_task,\s*"recovery_exit_reboot",\s*(\d+)')),
    dict(name="backlight_pwm", root="backlight_pwm_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/hw/backlight_pwm.c",
             r'xTaskCreate\(backlight_pwm_task,\s*"backlight_pwm",\s*(\d+)')),
    dict(name="i2c_owner_ns2009", root="i2c_owner_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/hw/NS2009.c",
             r'i2c_owner_init\(&t->owner,\s*bus,\s*8,\s*5,\s*(\d+)')),
    dict(name="i2c_owner_sx1509", root="i2c_owner_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/hw/SX1509.c",
             r'i2c_owner_init\(&e->owner,\s*bus,\s*8,\s*5,\s*(\d+)')),
    dict(name="kiln_io_owner", root="owner_task", expect_path="kiln_io_owner.c", ceiling=None,
         stack=lambda: extract_int_literal("drivers/owners/kiln_io_owner.c",
             r'xTaskCreatePinnedToCore\(owner_task,\s*"kiln_io_owner",\s*(\d+)')),
    dict(name="thermo_owner", root="owner_task", expect_path="thermo_owner.c", ceiling=None,
         stack=lambda: extract_int_literal("drivers/owners/thermo_owner.c",
             r'xTaskCreatePinnedToCore\(owner_task,\s*"thermo_owner",\s*(\d+)')),
    dict(name="telemetry_log", root="telemetry_log_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/persist/telemetry_log.c",
             r'xTaskCreatePinnedToCoreWithCaps\(telemetry_log_task,\s*"telemetry_log",\s*(\d+)')),
    dict(name="danger_mode", root="danger_mode_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/safety/danger_mode.c",
             r'xTaskCreate\(danger_mode_task,\s*"danger_mode",\s*(\d+)')),
    dict(name="safety_owner_evt", root="hal_uart_esp_event_task", ceiling=None,
         stack=lambda: extract_sdkconfig_macro("drivers/safety/safety_link.c",
             r'uart_owner_init\(&link->owner,.*?UART_OWNER_STACK_SIZE',
             "CONFIG_KILNCTL_UART_OWNER_STACK_SIZE")),
    dict(name="safety_proto_rx", root="uart_protocol_rx_task", ceiling=None,
         stack=lambda: extract_sdkconfig_macro("drivers/safety/safety_link.c",
             r'uart_protocol_init\(&link->proto,.*?UART_PROTOCOL_STACK_SIZE',
             "CONFIG_KILNCTL_UART_PROTOCOL_STACK_SIZE")),
    dict(name="safety_poll", root="safety_poll_task", ceiling=None,
         historical_note="2026-09-04 panic: safety_poll's configASSERT was reached via a deep LVGL "
                          "call path that corrupted thermo_owner.c's s_slots[] -- see lvgl's note below.",
         stack=lambda: extract_local_macro("drivers/safety/safety_link.c",
             r'#define SAFETY_POLL_TASK_STACK\s+(\d+)',
             r'xTaskCreatePinnedToCoreWithCaps\(safety_poll_task,\s*"safety_poll",\s*SAFETY_POLL_TASK_STACK')),
    dict(name="lvgl", root="lvgl_port_task", ceiling=None,
         historical_note="2026-09-04 panic: s_lvgl_task_stack (this task's own 8192 B stack) sits close "
                          "behind thermo_owner.c's s_slots[] in .bss; a reentrant lv_obj_invalidate()-in-"
                          "flush-callback path (pre-51e1ef5) could run this stack deep enough to corrupt it.",
         stack=lambda: extract_local_macro("drivers/ui/lvgl_port.c",
             r'static StackType_t s_lvgl_task_stack\[(\d+)\s*/\s*sizeof\(StackType_t\)\]',
             r'xTaskCreateStaticPinnedToCore\(\s*lvgl_port_task,\s*"lvgl",\s*sizeof\(s_lvgl_task_stack\)')),
    dict(name="screen_idle", root="screen_idle_task", ceiling=None,
         stack=lambda: extract_int_literal("drivers/ui/screen_idle.c",
             r'xTaskCreatePinnedToCore\(screen_idle_task,\s*"screen_idle",\s*(\d+)')),
    dict(name="uart_owner_evt_task", root="hal_uart_esp_event_task", ceiling=None,
         stack=lambda: extract_sdkconfig_macro("main_network_http.c",
             r'uart_owner_init\(&ctx->uart_owner,.*?UART_OWNER_STACK_SIZE',
             "CONFIG_KILNCTL_UART_OWNER_STACK_SIZE")),
    dict(name="uart_proto_rx", root="uart_protocol_rx_task", ceiling=None,
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
    "bx_flash_worker": 48,
    "info_uart_bridge": 2208,
    "io_uart_bridge": 2256,
    "safety_uart_bridge": 2912,
    "thermo_uart_bridge": 2160,
    "touch_uart_bridge": 2176,
    "autotune_engine": 2944,
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
    "safety_poll": 3104,
    "lvgl": 752,
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
    args = ap.parse_args()

    forced_ceilings = {}
    for item in args.force_ceiling:
        k, _, v = item.partition("=")
        forced_ceilings[k] = int(v)

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

    parsed = lib.parse(objdump, args.elf)

    results = []
    errors = []
    for task in TASKS:
        tname = task["name"]
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

        total, path_addrs = lib.deepest(root_addr, parsed)
        overhead = UNMODELED_OVERHEAD_BYTES
        honest_free = declared - total - overhead
        results.append(dict(task=task, declared=declared, total=total, path_addrs=path_addrs,
                             root_addr=root_addr, overhead=overhead, honest_free=honest_free))

    if errors:
        print("check_all_task_stack_budgets: FAIL -- could not measure every registered task:")
        for e in errors:
            print(f"  {e}")
        print("  This check must measure something for EVERY task in its table before it can pass "
              "(never a silent partial pass) -- a renamed entry function or a call-site literal that "
              "no longer matches the extractor pattern needs this table updated, not ignored.")
        return 1

    if args.dump_ceilings:
        print("CEILING_BYTES = {")
        for r in results:
            print(f'    "{r["task"]["name"]}": {r["total"]},')
        print("}")
        return 0

    print(f"{len(results)} registered tasks measured (objdump: {objdump})")
    print(f"unmodeled-overhead allowance applied to every task: {UNMODELED_OVERHEAD_BYTES} B "
          "(see module docstring \"OVERHEAD CONSTANT, STATED HONESTLY\" -- a conservative placeholder, "
          "not a live measurement, for any of these 28 tasks)")
    print()

    failed = []
    for r in sorted(results, key=lambda r: r["honest_free"]):
        task = r["task"]
        tname = task["name"]
        ceiling = forced_ceilings.get(tname, CEILING_BYTES.get(tname))
        note = f"  [{task['historical_note']}]" if "historical_note" in task else ""
        print(f"-- {tname} (root {task['root']}, declared {r['declared']} B) --{note}")
        running = 0
        for fn in [r["root_addr"]] + r["path_addrs"]:
            fsize = parsed.frames.get(fn, 0)
            running += fsize
            print(f"    {fsize:>6} B  {running:>6} B cumulative  {parsed.names.get(fn, hex(fn))}")
        pct = 100.0 * r["honest_free"] / r["declared"] if r["declared"] else 0.0
        print(f"    total {r['total']} B; ceiling {ceiling if ceiling is not None else 'UNSET'} B; "
              f"honest free {r['honest_free']} B ({pct:.1f}% of {r['declared']} B)")
        task_failed = False
        if ceiling is not None and r["total"] > ceiling:
            print(f"    FAIL: {r['total']} B exceeds the {ceiling} B ceiling for {tname}.")
            task_failed = True
        if r["honest_free"] < 0:
            print(f"    FAIL: honest free is negative ({r['honest_free']} B) once the "
                  f"{UNMODELED_OVERHEAD_BYTES} B unmodeled-overhead allowance is counted.")
            task_failed = True
        if task_failed:
            failed.append(tname)
        print()

    if failed:
        print(f"check_all_task_stack_budgets: FAIL -- {len(failed)} of {len(results)} tasks over "
              f"budget: {', '.join(failed)}")
        print("  Fix by moving large locals off the named task's own stack (heap/static, per this "
              "codebase's established convention), not by enlarging the stack or raising the ceiling "
              "without a documented reason for accepting the new margin.")
        return 1

    print(f"check_all_task_stack_budgets: OK -- all {len(results)} tasks measured and within budget.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
