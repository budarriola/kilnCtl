"""Taxonomy for kilnsim's search facade: groups, synonyms, recipes.

Same shape as ``kilnctrl.mcp_facade`` -- see that module's header for why the
mapping lives beside the server rather than inside it. The vocabulary differs
because the two servers answer to different callers: kilnctrl is asked to *do*
things to a board, kilnsim is asked to *lie* to one convincingly.
"""

from __future__ import annotations

GROUP_PREFIXES = (
    ("sim_", "sim"),
    ("tc_", "tc"),
    ("ct_", "ct"),
    ("relay_", "relay"),
    ("io_", "io"),
    ("estop_", "io"),
    ("dut_power_", "io"),
    ("fault_", "fault"),
    ("build_", "build"),
    ("run_", "scenario"),
    ("get_test_", "scenario"),
)

GROUP_OVERRIDES = {
    "sim_connect": "link",
    "sim_disconnect": "link",
    "sim_raw_command": "link",
    "run_pctools_tests": "build",
    "run_repo_checks": "build",
}

KEYWORDS = {
    "sim_connect": ("attach", "open", "port", "fixture", "mock"),
    "sim_get_state": ("telemetry", "snapshot", "everything", "dump"),
    "sim_reset": ("fresh", "clean", "restart", "zero"),
    "sim_load_preset": ("fast_test", "small_kiln", "three_zone", "stress"),
    "sim_set_timescale": ("speed", "faster", "accelerate", "time"),
    "sim_set_seed": ("prng", "random", "deterministic", "repeatable"),
    "tc_get_regs": ("max31856", "shadow", "truth", "lied", "register"),
    "tc_inject_fault": ("open", "short", "drift", "stuck", "noise", "break"),
    "tc_set_mode": ("manual", "model", "override", "force"),
    "ct_set_amps": ("current", "transformer", "amps", "load"),
    "ct_set_distortion": ("clipping", "dropout", "dc_offset", "harmonics"),
    "relay_get_edges": ("timing", "log", "history", "when", "transition"),
    "estop_set": ("emergency", "loop", "open", "interlock", "safety"),
    "dut_power_set": ("j18", "main", "12v", "power", "cycle"),
    "dut_power_safety_set": ("j19", "safety", "12v", "power", "cycle"),
    "fault_schedule": ("arm", "trigger", "later", "timed", "inject"),
    "fault_fire_now": ("force", "immediate", "trigger", "bypass"),
    "run_test_scenario": ("yaml", "regression", "expectations", "verdict", "suite"),
    "get_test_report": ("verdict", "result", "pass", "fail", "report"),
    "sim_reboot_bootloader": ("uf2", "bootsel", "reflash", "usb"),
    "build_simfw": ("compile", "elf", "ninja", "cmake"),
    "build_simfw_host_tests": ("unit", "msvc", "offtarget", "sim"),
}

SYNONYMS = {
    "temperature": ("tc", "thermocouple", "max31856"),
    "thermocouple": ("tc", "max31856"),
    "current": ("ct", "amps", "transformer"),
    "amps": ("ct",),
    "emergency": ("estop", "io"),
    "interlock": ("estop", "io"),
    "power": ("dut", "relay"),
    "scenario": ("run", "test", "yaml"),
    "regression": ("run", "test", "scenario"),
    "inject": ("fault", "tc"),
    "break": ("fault", "inject"),
    "fail": ("fault", "inject"),
    "telemetry": ("state",),
    "snapshot": ("state",),
    "speed": ("timescale",),
    "faster": ("timescale",),
    "random": ("seed",),
    "flash": ("build", "bootloader"),
    "compile": ("build",),
    "kiln": ("model", "preset", "zone"),
    "heat": ("model", "zone", "preset"),
}

RECIPES = """\
  no fixture attached:    simfw_call(name="sim_connect", args={"mock":true})
  attach the real rig:    simfw_call(name="sim_connect")
  fresh deterministic run: simfw_batch(calls=[{"name":"sim_reset"},{"name":"sim_set_seed","args":{"value":1}},{"name":"sim_set_timescale","args":{"value":10}}])
  telemetry snapshot:     simfw_call(name="sim_get_state")
  run a scenario file:    simfw_call(name="run_test_scenario", args={"path":"firmware/SimFW/scenarios/<name>.yaml"})
  then read the verdict:  simfw_call(name="get_test_report", args={"run_id":1})
  lie to one channel:     simfw_call(name="tc_set_mode", args={"channel":0,"mode":"manual","manual_temp":1200})
  what the DUT was told
  vs what is true:        simfw_call(name="tc_get_regs", args={"channel":0})
  break the E-stop loop:  simfw_call(name="estop_set", args={"state":"open"})
  power-cycle the DUT:    simfw_batch(calls=[{"name":"dut_power_set","args":{"state":"off"}},{"name":"dut_power_set","args":{"state":"on"}}])
  host tests, no rig:     simfw_call(name="build_simfw_host_tests")

Note: timescale and seed persist across runs -- set both explicitly at the start
of any run whose result you intend to compare against another.
"""

#: `sim_connect` stays published for the same reason kilnctrl keeps `connect`:
#: it is unconditionally the first call, including the `mock=true` form that
#: needs no hardware at all.
KEEP = ("sim_connect",)

TITLE = "SimFW bench fixture (RP2040) -- simulates the kiln's physical interfaces for the DUT"
LABEL = "SimFW fixture"
PREFIX = "simfw_"

#: 8765 link_hub, 8766 KiCad, 8767 kilnctrl -- this is the next free one.
DEFAULT_PORT = 8768
