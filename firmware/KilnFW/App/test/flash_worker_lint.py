#!/usr/bin/env python3
"""flash_worker_lint.py -- direct flash/NVS write calls, outside the
allowlist below, have panicked real hardware three times: a PSRAM-stacked
task's NVS write asserted esp_task_stack_is_sane_cache_disabled() (see
kiln_cfg_store.c's/safety_cfg_store.c's caller_stack_is_external() guard
comments and DRAM_PSRAM_PLAN.md section 7.2), and a handler already running
ON the flash-safe worker deadlocked the whole board when it tried to
dispatch a SECOND flash-safe call through the normal path (uart_bridge_ext.c
commit 7c47683, see that file's "RE-ENTRANCY" comment). Host tests cannot
see either bug -- the host build stubs nvs_set_*()/nvs_commit() as no-ops
with no stack or re-entrancy model at all (see App/test/stubs/nvs.h) -- so
this is a static grep-based lint, not a test.

THE SANCTIONED PATTERN (read uart_bridge_ext.c's own header comment and
kiln_cfg_store.c's nvs_save_store()/caller_stack_is_external() for the full
story before changing this allowlist):

  1. Dispatch the write onto the single shared flash-safe worker task via
     uart_bridge_ext_run_on_flash_worker() (internal-SRAM stack, so a
     PSRAM-stacked caller is never the one doing the write), checking
     uart_bridge_ext_is_on_flash_worker() first so a caller already ON that
     worker does the write inline instead of deadlocking itself waiting for
     its own job to drain the queue it is blocking.
  2. OR guard the write with a local caller_stack_is_external() check (the
     same predicate, copied into each file rather than shared, per those
     files' own comments) so a PSRAM-stacked caller is refused loudly
     instead of taking the board down -- used by files whose own callers
     are expected to have already dispatched onto the worker (or reached
     this code from app_main's own internal-SRAM-stack task before the
     scheduler introduced any other caller).
  3. OR run ONLY from app_main's own task, once, before the scheduler starts
     any other task that could contend for the worker or run on a PSRAM
     stack -- the same "no concurrency yet" argument boot_guard.c/
     crash_report.c/ota_record.c/profiles_builtin.c/ramp_assist_cfg.c/
     time_sync.c/touch_cal_store.c/unit_pref.c/watchdog_cfg.c/wifi_prov.c/
     zones_config_store.c/ota_http.c each make in their own init-time
     comments.

A file not on the allowlist below that starts calling nvs_set_*()/
nvs_commit()/esp_partition_write()/esp_partition_erase_range() directly has,
by definition, not been through that reasoning -- it fails this lint naming
the exact file:line, rather than shipping a fourth hardware incident.

Usage: python flash_worker_lint.py [drivers_dir]
Exit 0: clean. Exit 1: violation(s) found (printed as file:line).
"""
import re
import sys
from pathlib import Path

# ---- allowlist --------------------------------------------------------
# Seeded from current reality (2026-09-04 audit: every drivers/*.c file
# that calls nvs_set_*()/nvs_commit()/esp_partition_write()/
# esp_partition_erase_range() today). Adding a file here is not "fixing a
# lint failure" -- it is asserting, with the one-line justification below,
# that the new call site follows one of the three sanctioned patterns
# above. Say which pattern and why in the comment, the same way every
# existing entry does.
ALLOWLIST = {
    # Pattern 1 (worker dispatch): save_kibase_job() runs via
    # uart_bridge_ext_run_on_flash_worker(), checking
    # uart_bridge_ext_is_on_flash_worker() first for the re-entrant case
    # (autotune_handle_accept() already on the worker).
    "adaptive_tune.c",
    # Pattern 3 (init-time only): boot_guard_record_boot() is called once
    # from app_main's own task before the scheduler starts any other task.
    "boot_guard.c",
    # Pattern 3 (init-time only): crash_report_save() runs from the panic/
    # boot path, before normal task concurrency exists.
    "crash_report.c",
    # Pattern 3 (init-time only): display_power_cfg_set() runs from
    # settings_http.c's POST /api/settings/display_power handler, on that
    # handler's own internal-SRAM-stack httpd task -- same story as
    # unit_pref.c/zones_config_store.c's identical entries below.
    "display_power_cfg.c",
    # Pattern 2 (local caller_stack_is_external() guard): nvs_save_store().
    "kiln_cfg_store.c",
    # Pattern 3 (init-time / recovery path): ota_http.c's pico-firmware
    # esp_partition_erase_range()/esp_partition_write() calls run from the
    # single-threaded OTA apply sequence, not a PSRAM-stacked handler task.
    "ota_http.c",
    # Same call sites as ota_http.c above -- ota_pico_do_stage() (and its
    # esp_partition_erase_range()/esp_partition_write() calls) moved here
    # verbatim in the 2026-09-04 ota_http.c split (a6ab73b); still called
    # only from ota_pico_post_handler()'s single-threaded httpd handler, not
    # a PSRAM-stacked task.
    "ota_http_pico.c",
    # Pattern 3 (init-time only): ota_record_save() runs once from
    # app_main's boot-time OTA-verify sequence.
    "ota_record.c",
    # Pattern 2 (local caller_stack_is_external() guard), see this file's
    # own comment mirroring kiln_cfg_store.c's.
    "profile_executor_firing_stats.c",
    # Pattern 3 (init-time only): profiles_builtin_init() runs once from
    # app_main before the scheduler starts any other task.
    "profiles_builtin.c",
    # Pattern 2 (local caller_stack_is_external() guard), added after the
    # "earlier pass treated this file as always-internal-stack" incident --
    # see this file's own comment.
    "profiles_http.c",
    # Pattern 3 (init-time only): ramp_assist_cfg loads/saves run from
    # app_main's own task at boot before other tasks exist.
    "ramp_assist_cfg.c",
    # Pattern 2 (local caller_stack_is_external() guard), added when the
    # guard was introduced -- see this file's own comment; also called once
    # from app_main's own task before the scheduler starts.
    "relay_cycles.c",
    # Pattern 2 (local caller_stack_is_external() guard), same story as
    # relay_cycles.c -- see this file's own comment.
    "run_state.c",
    # Pattern 2 (local caller_stack_is_external() guard); dispatches
    # through uart_bridge_ext_run_on_flash_worker() otherwise -- see this
    # file's own comment.
    "safety_cfg_store.c",
    # Pattern 3 (init-time only): time zone save runs from the settings
    # HTTP handler's own internal-SRAM-stack httpd task, no PSRAM stack
    # involved in this handler's call chain.
    "time_sync.c",
    # Pattern 3 (init-time only): touch calibration is saved once from the
    # commissioning flow's own internal-SRAM-stack task.
    "touch_cal_store.c",
    # Pattern 3 (init-time only): unit preference save runs from the
    # settings HTTP handler's internal-SRAM-stack httpd task.
    "unit_pref.c",
    # Pattern 3 (init-time only): watchdog config save runs once from
    # app_main's own task before the scheduler starts.
    "watchdog_cfg.c",
    # Pattern 3 (init-time only): wifi_prov_start() deliberately stays a
    # plain call from app_main's own task -- see this file's own comment.
    "wifi_prov.c",
    # NOT actually init-time-only, unlike the entry above: nvs_save_*()
    # here (nvs_save_saved_nets()/nvs_save_mode()/nvs_save_ap_ssid()/
    # nvs_save_ap_password()/nvs_save_ip_config()) run from command
    # handlers in wifi_prov_api.c, on every add/forget-network, mode, AP-
    # identity and IP-mode change -- moved verbatim from wifi_prov.c's own
    # command handlers in the 2026-09-04 split (f9341e9), where they
    # already worked the same way. Safe because every one of them, and
    # every caller in wifi_prov_api.c/wifi_prov_link.c, runs exclusively on
    # owner_task() (see wifi_prov.c's DEADLOCK RULE comment and
    # wifi_prov.c:399's xTaskCreatePinnedToCore(owner_task, ...) -- a
    # plain, non-WithCaps create, so its stack is internal SRAM, never
    # PSRAM), which is wifi_prov's single serializing writer task, not a
    # PSRAM-stacked httpd/handler task.
    "wifi_prov_nvs.c",
    # Pattern 3 (init-time only): zones/relay-name/zone-normals config
    # loads/saves run from the settings HTTP handler's own
    # internal-SRAM-stack httpd task.
    "zones_config_store.c",
}

WRITE_CALL_RE = re.compile(
    r"\b(nvs_set_\w+|nvs_commit|esp_partition_write(?:_raw)?|esp_partition_erase_range)\s*\("
)

# Matches this file's own definitions/declarations of the sanctioned
# dispatch wrappers so a match inside uart_bridge_ext.c itself (which never
# calls nvs_*()/esp_partition_*() directly -- it only dispatches to them)
# is never a false hit in the first place; kept as a comment, not code,
# since nothing here currently needs it -- uart_bridge_ext.c has zero
# nvs_*/esp_partition_* call sites of its own (verified 2026-09-04).


def scan_file(path: Path):
    violations = []
    text = path.read_text(encoding="utf-8", errors="replace")
    in_block_comment = False
    for lineno, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line
        # Strip block comments (best-effort, line-oriented -- good enough
        # for this repo's style, which never straddles a call across a
        # /* */ boundary).
        if in_block_comment:
            end = line.find("*/")
            if end == -1:
                continue
            line = line[end + 2:]
            in_block_comment = False
        start = line.find("/*")
        if start != -1:
            end = line.find("*/", start + 2)
            if end == -1:
                in_block_comment = True
                line = line[:start]
            else:
                line = line[:start] + line[end + 2:]
        # Strip line comments.
        cpos = line.find("//")
        if cpos != -1:
            line = line[:cpos]
        if WRITE_CALL_RE.search(line):
            violations.append((lineno, raw_line.strip()))
    return violations


def main(argv):
    default_drivers = Path(__file__).resolve().parent.parent / "drivers"
    drivers_dir = Path(argv[1]) if len(argv) > 1 else default_drivers
    if not drivers_dir.is_dir():
        print(f"flash_worker_lint: drivers dir not found: {drivers_dir}", file=sys.stderr)
        return 1

    all_violations = []
    for c_file in sorted(drivers_dir.glob("*.c")):
        if c_file.name in ALLOWLIST:
            continue
        for lineno, text in scan_file(c_file):
            all_violations.append(f"{c_file.relative_to(drivers_dir.parent)}:{lineno}: {text}")

    if all_violations:
        print("FLASH WORKER LINT: direct flash/NVS write(s) outside the allowlist:")
        for v in all_violations:
            print(f"  {v}")
        print("")
        print("See flash_worker_lint.py's header comment for the three sanctioned")
        print("patterns and how to add a justified allowlist entry.")
        return 1

    print(f"flash_worker_lint: clean ({len(list(drivers_dir.glob('*.c')))} driver files scanned, "
          f"{len(ALLOWLIST)} allowlisted)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
