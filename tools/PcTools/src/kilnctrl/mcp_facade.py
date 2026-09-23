"""Taxonomy for kilnctrl's search facade: groups, synonyms, recipes.

Kept out of ``mcp_server.py`` because it is data about the tools, not tools.
The mapping is what makes ``kiln_find("temperature")`` land on ``thermo_read``
rather than on whatever happens to say "read" in its docstring, so it is worth
reading as a table.

Two things carry the search:

``GROUP_PREFIXES``/``GROUP_OVERRIDES``
    Longest-prefix match on the tool name, with an exact-name table on top for
    the tools whose names predate any prefix convention (``connect``,
    ``flash_firmware``, ``get_board_state``). Groups are how ``kiln_help``
    presents 200 tools in 20 lines.

``SYNONYMS``
    Query token -> extra tokens to score with. This is the bridge between the
    words a caller uses and the words the code uses: nobody types "thermo" when
    they want a temperature, and nobody types "sx1509" when they want a relay.
"""

from __future__ import annotations

#: Longest match wins, so ("gpio_probe_", ...) beats a bare ("gpio_", ...).
GROUP_PREFIXES = (
    ("thermo_", "thermo"),
    ("io_", "io"),
    ("expander_", "io"),
    ("touch_", "touch"),
    ("fixture_", "fixture"),
    ("ui_", "ui_test"),
    ("safety_", "safety"),
    ("ota_", "ota"),
    ("gpio_probe_", "gpio"),
    ("pico_gpio_", "gpio"),
    ("wifi_", "wifi"),
    ("control_", "control"),
    ("profiles_", "profiles"),
    ("profile_live_", "profiles"),
    ("autotune_", "autotune"),
    ("adaptive_tune_", "adaptive_tune"),
    ("codec_", "codec"),
    ("debug_", "debug"),
    ("saleae_", "saleae"),
    ("build_", "build"),
    ("run_", "build"),
    ("log_analyze", "analysis"),
    ("coupled_ident_", "coupled_ident"),
    ("plant_sim_", "plant_sim"),
    ("zone_current_sweep_", "zones"),
    ("bench_test_", "bench_test"),
)

GROUP_OVERRIDES = {
    # Single-tool HARDWARE.md wiring test -- keep it in "gpio" rather than
    # growing a one-tool "coordinated" group.
    "coordinated_gpio_test": "gpio",
    # The UART link itself: nothing else works until this group does.
    "list_serial_ports": "link",
    "connect": "link",
    "disconnect": "link",
    "link_status": "link",
    "restart_uart": "link",
    "close_server": "link",
    # Firmware's forwarded ESP_LOGx output.
    "get_device_log": "log",
    "get_device_log_json": "log",
    "fetch_event_log": "log",
    # Whole-board introspection.
    "get_board_state": "system",
    "get_pin_config": "system",
    "get_fw_version": "system",
    "get_stack_margin": "system",
    "check_task_liveness": "system",
    "get_heap_status": "system",
    "crash_report_ack": "system",
    "boot_guard_get": "system",
    "crash_report_clear": "system",
    "kiln_configs_quarantine_clear": "system",
    "kiln_config_apply": "system",
    "web_auth_setup": "system",
    "web_auth_logout": "system",
    "get_cfgfs_status": "system",
    "cfgfs_format": "system",
    "get_watchdog_panic_disabled": "system",
    "set_watchdog_panic_disabled": "system",
    "get_readiness": "system",
    "nvs_list_keys": "system",
    # OpenOCD, shared by both processors.
    "flash_firmware": "debug",
    "kill_openocd_sessions": "debug",
    "set_openocd_path": "debug",
    "get_openocd_status": "debug",
    "debug_check_partition_table": "debug",
    "find_crash_elf": "debug",
    "find_crash_elf_for_coredump": "debug",
    "find_safty_crash_elf": "debug",
    "read_esp_coredump": "debug",
    "sw_reset_esp": "ota",
    # LVGL page interaction, plus the web UI's own served-page structure.
    "list_buttons": "ui",
    "press_button": "ui",
    "board_page_structure": "ui",
    # Searching the repository itself -- no board, no link, no hardware.
    "repo_grep": "search",
    # Known-good config presets -- a consistent starting point for tests.
    "list_config_presets": "presets",
    "load_config_preset": "presets",
    "capability_preflight_check": "presets",
    "factory_default_then_load_preset": "presets",
    # Best-effort file-only config version conversion -- never touches a board.
    "convert_config": "presets",
}

#: Extra search tokens for tools whose names hide what they are for.
KEYWORDS = {
    "thermo_read": ("temperature", "celsius", "max31856", "hot"),
    "thermo_read_faults": ("open", "circuit", "broken", "thermocouple"),
    "io_set_relay": ("sx1509", "switch", "output", "coil"),
    "io_all_relays_off": ("panic", "stop", "safe", "everything"),
    "fixture_set_relay": ("unittestfixture", "pcf8575", "expander", "bench", "short", "open"),
    "fixture_all_off": ("unittestfixture", "pcf8575", "panic", "stop", "safe"),
    "fixture_get_relays": ("unittestfixture", "pcf8575", "read", "state"),
    "fixture_list_relays": ("unittestfixture", "pcf8575", "names"),
    # "what is the safety processor doing" is the standard opening question, and
    # every safety_* tool matches the word "safety" equally -- these are what
    # break the tie towards the one that just reports.
    "safety_get_status": ("rp2040", "guard", "trip", "interlock", "estop",
                          "doing", "state", "overview", "processor"),
    "safety_clear_trip": ("reset", "latch", "unlatch"),
    "safety_get_link_stats": ("uart", "isolated", "frames", "dropped"),
    "safety_get_diag": ("rp2040", "pico", "diag", "boot reason", "reset reason",
                        "watchdog", "brownout", "power-on", "reboot", "guard state",
                        "warn mask", "trip mask", "push_context"),
    "safety_capture_ct_counts": ("current sense", "adc", "raw counts", "noise floor",
                                "ct_counts", "oversample", "csv", "mean", "std",
                                "sample rate"),
    "safety_get_fw_version": ("rp2040", "pico", "commit", "build", "version",
                              "boot_id", "config_version", "config_crc",
                              "commissioned"),
    "safety_get_commissioning": ("abs_max_temp_c", "max_rate_c_per_min", "overtemp", "ceiling",
                                "armed", "dormant", "commissioning", "commissioned",
                                "commissioning_get_handler", "config crc", "stale",
                                "thresholds", "ct_installed", "i_normal_a", "tc_source",
                                "read", "inspect", "verify"),
    "safety_get_ct_cal_raw": ("ct calibration", "current transformer", "k_ct_v_per_a",
                             "zero_counts", "gain", "ct_installed", "ct_topology",
                             "i_present_a", "raw", "uncalibrated", "commissioning",
                             "amps per volt", "verify calibration", "bench check"),
    "safety_get_ct_cal": ("legacy", "ct_cal", "correction table", "gain", "offset"),
    "safety_get_param": ("get_param", "config_reference", "param_id", "diagnostic",
                         "config ram-integrity", "unsupported", "not wired",
                         "single parameter", "commissioning"),
    "safety_set_rate_guard": ("s8", "rate of rise", "max_rate_c_per_min", "rate_window_s",
                             "commission", "dormant", "grace", "armed", "write", "confirm"),
    "safety_set_commissioning_fields": ("commissioning", "commission", "ct_installed",
                                        "ct_topology", "abs_max_temp_c", "tc_source",
                                        "generic", "arbitrary", "write", "flash", "grace",
                                        "armed", "confirm", "readback", "verify", "flash the safety processor"),
    "safety_get_rate_guard": ("s8", "rate of rise", "max_rate_c_per_min", "rate_window_s",
                             "dormant", "armed", "read", "inspect"),
    "get_board_state": ("snapshot", "everything", "overview", "dump"),
    "get_stack_margin": ("freertos", "watermark", "overflow", "task"),
    "check_task_liveness": ("task not running", "task dead", "task missing", "did not start",
                            "task creation failed", "xtaskcreate", "alive", "not running",
                            "required tasks", "expected tasks", "silently failed to start"),
    "get_heap_status": ("dram", "psram", "spiram", "memory", "malloc", "fragmentation",
                        "internal", "dma", "free", "min_free", "exhaustion",
                        "display flush", "flush time", "thermocouple read latency",
                        "display_flush_us", "thermo_read_us", "timing"),
    "crash_report_ack": ("crash", "panic", "acknowledge", "ack", "dismiss", "unacknowledged",
                         "coredump", "exception", "backtrace", "reviewed", "clear the banner"),
    "boot_guard_get": ("boot guard", "boot_guard", "recovery mode", "recovery_mode",
                       "boot_count", "recovery counter", "api/boot_guard",
                       "boot_guard_reset", "flash footgun"),
    "crash_report_clear": ("crash", "panic", "clear", "erase", "coredump", "erase coredump",
                           "reflash", "allow_unacknowledged", "delete crash record",
                           "free the coredump partition"),
    "kiln_configs_quarantine_clear": ("kiln_configs", "quarantine", "quarantined", "wrong size",
                                      "corrupt", "corrupted", "discard", "kiln config store",
                                      "saved configs", "factory_reset alternative", "confirm_discard"),
    "kiln_config_apply": ("kiln_configs", "apply", "load config", "select config",
                         "hardware differs", "x-kiln-ack-hardware-differs", "ack_hardware_differs",
                         "428", "precondition required", "kiln_cfg_swap", "apply_status",
                         "diverged", "confirm"),
    "get_readiness": ("readiness", "commissioning checklist", "commissioning", "estop_verified",
                      "checklist", "not_done", "cannot_yet", "deliberately_off",
                      "ready to fire", "fix_url", "api/readiness"),
    "nvs_list_keys": ("nvs", "nvs_entry_find", "nvs_entry_info", "key names", "namespace",
                      "nvs.net80211", "wifi driver storage", "factory_reset audit",
                      "api/nvs/keys", "partition", "kiln_auth forbidden", "esp_wifi_restore"),
    "web_auth_setup": ("web auth", "bootstrap", "bootstrap_password", "admin password",
                       "set password", "enable web auth", "login", "credential",
                       "kilnctl_web_username", "kilnctl_web_password", "security page",
                       "settings/security", "first boot", "administrator"),
    "web_auth_logout": ("web auth", "logout", "log out", "end session", "sign out",
                        "api/auth/logout", "session cookie", "kiln_sid"),
    "get_device_log": ("console", "printf", "esp_logx", "serial", "tail"),
    "fetch_event_log": ("event log", "event_log", "flash log", "log_store", "firing history",
                        "autotune history", "run started", "run faulted", "binary log",
                        "api/logs", "decode"),
    "get_cfgfs_status": ("filesystem", "littlefs", "cfg partition", "cfg_fs", "mounted",
                         "capacity", "free space", "used bytes", "file list",
                         "dual-write", "dual write", "zones_rev", "tmp", "stale",
                         "corruption", "zones.json", "prefs.json"),
    "cfgfs_format": ("format", "erase", "wipe", "cfg partition", "cfg_fs", "littlefs",
                     "format_confirm", "format_pending", "auto-format", "confirm",
                     "factory-reset", "destructive", "reformat"),
    "debug_program": ("flash", "swd", "jtag", "elf", "burn", "openocd"),
    "debug_read_symbol": ("variable", "global", "inspect", "elf", "nm"),
    "debug_read_registers": ("pc", "sp", "primask", "core", "cpu"),
    "flash_firmware": ("esp32", "jtag", "openocd", "program"),
    "debug_check_partition_table": ("partitions.csv", "on-chip", "verify",
                                    "confirm", "gen_esp32part", "flash layout"),
    "find_crash_elf": ("symbolize", "backtrace", "coredump", "panic", "elf_archive",
                       "crash", "fw_build", "which elf", "matching build"),
    "find_crash_elf_for_coredump": ("symbolize", "backtrace", "coredump", "panic",
                                    "elf_archive", "crash", "stored coredump",
                                    "archived coredump", "sha256", "content-based",
                                    "which elf", "permanently unsymbolizable"),
    "find_safty_crash_elf": ("symbolize", "backtrace", "panic", "rp2040", "pico",
                             "saftyfw", "elf_archive", "commit", "which elf"),
    "read_esp_coredump": ("coredump", "core dump", "panic", "crash", "partition",
                          "esp_partition_read", "chunk", "http", "no jtag",
                          "read-only", "espcoredump", "fetch", "download"),
    "sw_reset_esp": ("reboot", "reset both", "restart", "grace window",
                     "config_store", "commission while armed"),
    "saleae_capture": ("logic", "analyzer", "trace", "waveform", "timing"),
    "saleae_decode_kilnlink": ("kilnlink", "frame", "decode", "uart", "link",
                               "safety link", "resync", "crc", "timeline"),
    "touch_inject": ("tap", "press", "click", "simulate", "screen"),
    "press_button": ("lvgl", "ui", "tap", "screen", "page"),
    "profiles_start": ("firing", "ramp", "soak", "cone", "schedule"),
    "profile_live_get": ("live edit", "working copy", "in-flight", "mid-firing"),
    "profile_live_fork": ("live edit", "working copy", "in-flight", "mid-firing"),
    "profile_live_edit": ("live edit", "working copy", "in-flight", "mid-firing", "edit segments"),
    "profile_live_decide": ("live edit", "discard", "save as", "overwrite", "working copy"),
    "autotune_start": ("pid", "tuning", "relay", "ziegler"),
    "adaptive_tune_get_status": ("k_dc", "gain", "learned", "dwell", "coupled",
                                 "coupling", "ki", "integral", "refusal", "observations"),
    "adaptive_tune_set_enabled": ("opt", "opt-in", "toggle", "enable", "disable",
                                  "learn", "continuous"),
    "ramp_assist_get_enabled": ("ramp", "assist", "stretch", "dwell", "credit", "cone",
                                "heat-work", "flag", "toggle", "pid", "tuning"),
    "ramp_assist_set_enabled": ("ramp", "assist", "stretch", "dwell", "credit", "cone",
                                "heat-work", "pin", "toggle", "enable", "disable"),
    "zone_current_sweep_start": ("current", "sweep", "measure", "measurement", "ct",
                                 "clamp", "amps", "amperage", "normal current",
                                 "i_normal", "zone_normals_set", "commissioning",
                                 "nameplate", "k_ct", "energize", "relay", "calibrate"),
    "zone_current_sweep_status": ("current", "sweep", "progress", "unmeasured",
                                  "noise floor", "ct map", "k_ct", "nameplate",
                                  "i_normal_pushed_mask", "summed_unmeasured_mask"),
    "zone_current_sweep_abort": ("current", "sweep", "stop", "cancel", "abort",
                                 "de-energize", "relay off"),
    "adaptive_tune_revert": ("undo", "rollback", "restore", "previous", "one-click"),
    "control_set_zone_pid": ("kp", "ki", "kd", "gains", "loop"),
    "wifi_add_network": ("provision", "credentials", "ssid", "join"),
    "ota_update_esp": ("over", "air", "upload", "firmware", "http"),
    "codec_decode_frame": ("wire", "bytes", "protocol", "parse", "hex"),
    "build_kilnfw": ("esp32", "idf", "compile", "ninja"),
    "build_saftyfw_host_tests": ("unit", "msvc", "offtarget", "pytest"),
    "run_pctools_tests": ("pytest", "unit", "python", "regression"),
    "run_repo_checks": ("lint", "guard", "invariant", "ci", "grep", "audit"),
    "list_config_presets": ("bench", "fixture", "known", "good", "default", "json"),
    "convert_config": ("migrate", "migration", "version", "upgrade", "downgrade", "backup",
                        "profile", "blob", "zones", "schema", "lossy"),
    "load_config_preset": ("bench", "fixture", "known", "good", "default", "consistent", "zones"),
    "capability_preflight_check": ("preflight", "capability", "endpoint", "missing", "fatal",
                                   "benign", "reflash", "firmware", "version", "campaign",
                                   "unattended", "ramp_assist", "no such endpoint"),
    "coordinated_gpio_test": ("gpio", "pin", "wiring", "crossing", "swd", "hardware.md",
                              "safety link", "uart", "cable", "cross-check", "isolated link",
                              "esp", "pico", "verify wiring"),
    "factory_default_then_load_preset": ("factory", "reset", "consistent", "baseline", "bench", "fixture"),
    "bench_test_run": ("standardized", "test", "suite", "smoke", "nightly", "full", "regression",
                        "run all tests", "self-test", "verify the board", "log"),
    "bench_test_list": ("standardized", "test", "suite", "catalogue", "case", "registry"),
    "bench_test_last": ("standardized", "test", "suite", "history", "last run", "summary"),
    "ui_list_scripts": ("regression", "script", "test", "lcd", "web", "json"),
    "ui_run_script": ("regression", "script", "click", "tap-target", "wait", "assert", "lcd", "web"),
    "ui_step": ("regression", "click", "tap-target", "wait", "assert", "debug", "single", "step"),
    "log_analyze": ("firing", "tuning", "autotune", "windowed", "overshoot", "undershoot",
                     "settle", "iae", "fopdt", "refit", "compare", "saturation", "jsonl",
                     "trace", "poll", "capture", "report"),
    "coupled_ident_report": ("coupling matrix", "fit", "score", "nonlinearity",
                            "self-check", "dwell", "joint", "settled", "jsonl",
                            "profile_exec", "capture", "k_dc", "gain"),
    "coupled_ident_single_zone": ("coupling matrix", "single-zone", "excitation",
                                 "assemble", "k_dc", "gain", "one zone at a time"),
    "coupled_ident_settle_audit": ("dwell", "settle", "drifting", "oscillating",
                                   "steady state", "duty", "dc-gain", "coupid6",
                                   "steady-state check", "trust"),
    "repo_grep": ("grep", "search", "ripgrep", "rg", "find", "look for", "source",
                  "code", "pattern", "regex", "string", "symbol", "occurrences",
                  "which files", "where", "mention", "mentions", "timeout", "deadline",
                  "time limit", "repository", "repo", "tree", "files", "count",
                  "context", "glob", "case", "text"),
    "board_page_structure": ("web", "page", "html", "http", "web ui", "dashboard",
                             "settings", "settings page", "zones page", "kiln graphic",
                             "graphic", "svg", "symbol", "symbols", "icon", "icons",
                             "ids", "element ids", "declare", "declares", "css",
                             "class", "prefix", "gzip", "gzipped", "size", "bytes",
                             "structure", "inventory", "present", "complete",
                             "freshly flashed", "check"),
    "plant_sim_compare": ("simulation", "model", "kp", "ki", "kd", "gains",
                          "climb", "integral floor", "prediction", "coupled",
                          "compare against capture"),
}

#: Query word -> tokens to also score against. One-way on purpose: expanding
#: "relay" to "io" helps, but expanding every "io" query to "relay" would drag
#: the digital-I/O tools down every relay search.
SYNONYMS = {
    "temperature": ("thermo", "thermocouple"),
    "temp": ("thermo", "thermocouple"),
    "thermocouple": ("thermo", "max31856"),
    "hot": ("thermo", "temperature"),
    "relay": ("io", "expander", "sx1509"),
    "flash": ("program", "debug", "openocd", "firmware"),
    "program": ("debug", "flash", "openocd"),
    "jtag": ("debug", "openocd"),
    "swd": ("debug", "openocd", "pico"),
    "estop": ("safety", "trip", "interlock"),
    "interlock": ("safety",),
    "trip": ("safety",),
    "rp2040": ("safety", "pico"),
    "pid": ("control", "autotune"),
    "tuning": ("autotune", "control"),
    "adaptive": ("adaptive_tune",),
    "learning": ("adaptive_tune",),
    "continuous": ("adaptive_tune",),
    "firing": ("profiles", "ramp", "soak"),
    "kiln": ("profiles", "control"),
    "network": ("wifi",),
    "scope": ("saleae", "logic"),
    "analyzer": ("saleae",),
    "console": ("log", "device"),
    "tail": ("log",),
    "update": ("ota",),
    "version": ("fw", "firmware"),
    "compile": ("build",),
    "test": ("build", "tests"),
    "pin": ("gpio", "config"),
    "memory": ("debug", "read"),
    "register": ("reg",),
    "grep": ("repo_grep", "search"),
    "ripgrep": ("repo_grep", "search", "grep"),
    "search": ("repo_grep", "grep"),
    "html": ("page", "web"),
    "svg": ("page", "graphic", "symbol"),
    "graphic": ("page", "web"),
    "preset": ("presets",),
    "default": ("presets",),
    "baseline": ("presets",),
}

#: The sequences that are actually run on this bench, spelled out so the first
#: call of a session does not have to be a search. Every one of these is a
#: paste-ready line.
RECIPES = """\
  link up, then confirm:  kiln_batch(calls=[{"name":"connect"},{"name":"link_status"},{"name":"get_fw_version"}])
  whole-board snapshot:   kiln_call(name="get_board_state")
  all thermocouples:      kiln_call(name="thermo_read")
  safety processor:       kiln_batch(calls=[{"name":"safety_get_status"},{"name":"safety_get_link_stats"}])
  tail the firmware log:  kiln_call(name="get_device_log", args={"n":80})
  flash the ESP32-S3:     kiln_call(name="flash_firmware")           (JTAG/OpenOCD, never esptool)
  flash the RP2040:       kiln_call(name="debug_program", args={"peer":"pico","confirm":true})
  read a firmware global: kiln_call(name="debug_read_symbol", args={"peer":"pico","symbol":"s_tc_type_verified"})
  everything off, now:    kiln_call(name="io_all_relays_off")
  host tests before hw:   kiln_call(name="build_saftyfw_host_tests")
"""

#: Stays directly published, schema and all. `connect` earns it because a fresh
#: session's first hardware act is always this one, and paying a search round
#: trip to discover it is pure overhead. Nothing else has cleared that bar.
KEEP = ("connect",)

TITLE = "kilnCtl main board (ESP32-S3) + RP2040 safety processor, over the hardened UART link"
LABEL = "kilnCtl board"
PREFIX = "kiln_"

#: 8765 is kilnctrl's own link_hub (link_hub.py HUB_PORT) and 8766 is the KiCad
#: MCP server; this is the next free one.
DEFAULT_PORT = 8767
