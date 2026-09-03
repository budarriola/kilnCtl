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
    ("ui_", "ui_test"),
    ("safety_", "safety"),
    ("ota_", "ota"),
    ("gpio_probe_", "gpio"),
    ("pico_gpio_", "gpio"),
    ("wifi_", "wifi"),
    ("control_", "control"),
    ("profiles_", "profiles"),
    ("autotune_", "autotune"),
    ("adaptive_tune_", "adaptive_tune"),
    ("codec_", "codec"),
    ("debug_", "debug"),
    ("saleae_", "saleae"),
    ("build_", "build"),
    ("run_", "build"),
    ("log_analyze", "analysis"),
)

GROUP_OVERRIDES = {
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
    # Whole-board introspection.
    "get_board_state": "system",
    "get_pin_config": "system",
    "get_fw_version": "system",
    "get_stack_margin": "system",
    "get_heap_status": "system",
    "get_watchdog_panic_disabled": "system",
    "set_watchdog_panic_disabled": "system",
    # OpenOCD, shared by both processors.
    "flash_firmware": "debug",
    "kill_openocd_sessions": "debug",
    "set_openocd_path": "debug",
    "get_openocd_status": "debug",
    "debug_check_partition_table": "debug",
    # LVGL page interaction.
    "list_buttons": "ui",
    "press_button": "ui",
    # Known-good config presets -- a consistent starting point for tests.
    "list_config_presets": "presets",
    "load_config_preset": "presets",
    "factory_default_then_load_preset": "presets",
}

#: Extra search tokens for tools whose names hide what they are for.
KEYWORDS = {
    "thermo_read": ("temperature", "celsius", "max31856", "hot"),
    "thermo_read_faults": ("open", "circuit", "broken", "thermocouple"),
    "io_set_relay": ("sx1509", "switch", "output", "coil"),
    "io_all_relays_off": ("panic", "stop", "safe", "everything"),
    # "what is the safety processor doing" is the standard opening question, and
    # every safety_* tool matches the word "safety" equally -- these are what
    # break the tie towards the one that just reports.
    "safety_get_status": ("rp2040", "guard", "trip", "interlock", "estop",
                          "doing", "state", "overview", "processor"),
    "safety_clear_trip": ("reset", "latch", "unlatch"),
    "safety_get_link_stats": ("uart", "isolated", "frames", "dropped"),
    "get_board_state": ("snapshot", "everything", "overview", "dump"),
    "get_stack_margin": ("freertos", "watermark", "overflow", "task"),
    "get_heap_status": ("dram", "psram", "spiram", "memory", "malloc", "fragmentation",
                        "internal", "dma", "free", "min_free", "exhaustion"),
    "get_device_log": ("console", "printf", "esp_logx", "serial", "tail"),
    "debug_program": ("flash", "swd", "jtag", "elf", "burn", "openocd"),
    "debug_read_symbol": ("variable", "global", "inspect", "elf", "nm"),
    "debug_read_registers": ("pc", "sp", "primask", "core", "cpu"),
    "flash_firmware": ("esp32", "jtag", "openocd", "program"),
    "debug_check_partition_table": ("partitions.csv", "on-chip", "verify",
                                    "confirm", "gen_esp32part", "flash layout"),
    "saleae_capture": ("logic", "analyzer", "trace", "waveform", "timing"),
    "touch_inject": ("tap", "press", "click", "simulate", "screen"),
    "press_button": ("lvgl", "ui", "tap", "screen", "page"),
    "profiles_start": ("firing", "ramp", "soak", "cone", "schedule"),
    "autotune_start": ("pid", "tuning", "relay", "ziegler"),
    "adaptive_tune_get_status": ("k_dc", "gain", "learned", "dwell", "coupled",
                                 "coupling", "ki", "integral", "refusal", "observations"),
    "adaptive_tune_set_enabled": ("opt", "opt-in", "toggle", "enable", "disable",
                                  "learn", "continuous"),
    "ramp_assist_get_enabled": ("ramp", "assist", "stretch", "dwell", "credit", "cone",
                                "heat-work", "flag", "toggle", "pid", "tuning"),
    "ramp_assist_set_enabled": ("ramp", "assist", "stretch", "dwell", "credit", "cone",
                                "heat-work", "pin", "toggle", "enable", "disable"),
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
    "load_config_preset": ("bench", "fixture", "known", "good", "default", "consistent", "zones"),
    "factory_default_then_load_preset": ("factory", "reset", "consistent", "baseline", "bench", "fixture"),
    "ui_list_scripts": ("regression", "script", "test", "lcd", "web", "json"),
    "ui_run_script": ("regression", "script", "click", "tap-target", "wait", "assert", "lcd", "web"),
    "ui_step": ("regression", "click", "tap-target", "wait", "assert", "debug", "single", "step"),
    "log_analyze": ("firing", "tuning", "autotune", "windowed", "overshoot", "undershoot",
                     "settle", "iae", "fopdt", "refit", "compare", "saturation", "jsonl",
                     "trace", "poll", "capture", "report"),
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
