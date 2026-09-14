# `control_get_zones` did not surface the identified plant model, 2026-09-13

## The gap, confirmed

`control_get_zones()`'s docstring (`tools/PcTools/src/kilnctrl/mcp_server_control.py`)
claimed to report "PID/model config". It did not: it rendered the UART
`ZoneConfig` (PID gains, cal offset, ramp/temp limits -- no model fields at
all, since `zone_cfg_t`'s model fields never crossed the UART CONTROL wire,
see `devices_control.py`'s `ZoneConfig` dataclass), plus (over HTTP
`GET /api/zones`) the coupling matrix and three HTTP-only fields. It never
read or rendered `model_k_dc`, `model_tau_s`, `model_dead_time_s`,
`tuning_valid`, `model_fit_temp_c`, or `model_fit_ambient_c`, even though all
six are present in the same `GET /api/zones` JSON body it already fetches
(`firmware/KilnFW/App/drivers/http/zones_http_get.c:339`,`:477`,`:504`). The
docstring overstated the tool's coverage.

Confirmed live 2026-09-13 via `kiln_call(name="control_get_zones")` before
the fix: output had a `coupling matrix` and `http-only fields` section but
nothing naming any model field.

This is exactly the class of gap `feedback_prioritize_mcp_improvements`
exists for: a separate review had to bypass this tool and read raw
`GET /api/zones` to discover `model_fit_temp_c` reads `-273.15` (the UNKNOWN
sentinel) on all three zones -- the operating-point recording added in
`5d3bc854` has never actually populated. `control_get_zones` should have
made that obvious on its own.

## Fix

Added `_describe_model_fields()` and wired it into `control_get_zones()`'s
output (new "plant model" section, ahead of the coupling matrix). Rewrote
the tool's docstring to describe what it actually renders now, including
naming this fix so a future reader can see the old text was wrong rather
than assuming today's docstring was always accurate.

### Sentinel rendering (the important part)

Read the authoritative sentinel from firmware source rather than assuming:

* `model_fit_temp_c` / `model_fit_ambient_c`: sentinel is
  `ZONE_MODEL_FIT_TEMP_UNKNOWN = -273.15f`
  (`firmware/KilnFW/App/drivers/persist/zones_config_accessors.h:83`,
  ZONES_CFG_VERSION 23->24). `-273.15` (absolute zero) is physically
  unreachable on a kiln, so it means "never recorded", not a temperature.
  `_read_zone_model_fit_temp_unknown_sentinel()` reads this constant live
  out of that header (same discipline as the existing
  `_read_coupling_use_measured_diag_k_dc_compiled_value()`), falling back to
  the literal only if the header can't be read/parsed. Rendered as
  `UNKNOWN (never recorded)`, never as a bare `-273.15`.
* `model_k_dc` / `model_tau_s` / `model_dead_time_s`: all-zero is the
  documented "no model identified" encoding
  (`zones_config_accessors.h`'s `zones_config_set_model()` doc comment:
  "Writing all zeros is legal and is how a caller clears a stale model").
  Rendered as `no model identified (all-zero sentinel)`, never as
  `K_dc=0.0000 C/duty`.
* `autotune_baseline_k_dc` (`zones_config_accessors.h:185`, ZONES_CFG_VERSION
  25->26): `0` = "no baseline recorded yet". **CORRECTED 2026-09-14, roadmap
  truth-up: this claim was wrong even at the time it was written for this
  field, though right for the general pattern.** `0dbd7c6d` ("Expose
  autotune_baseline_k_dc on GET /api/zones (was write-only)") added
  `APPEND("\"autotune_baseline_k_dc\":%.4f,", (double)z->autotune_baseline_k_dc);`
  to `firmware/KilnFW/App/drivers/http/zones_http_get.c:606` -- the field IS
  emitted by `GET /api/zones` as of that commit. It is also still read in
  `zones_http_post_parse.c` (echoed back unchanged on every POST so a
  whole-page-submit doesn't clobber it), same as described above. The
  PC-side `mcp_server_control.py` code (lines ~262-350 as of this note)
  already checks for the key's presence dynamically (`if
  "autotune_baseline_k_dc" not in z`) rather than hardcoding "not exposed",
  so it should already render correctly against current firmware -- that
  file is owned by another concurrent session's work and was not verified
  or edited here; flagging only that the "real firmware gap" premise in this
  audit is stale.

### Other tools in the same module

Scanned `mcp_server_control.py` for the same overstatement pattern.
`control_set_zone_model()`'s docstring ("Set a zone's feedforward thermal
model...") matches what it actually does (writes exactly k_dc/tau_s/
dead_time_s over the UART wire) -- no fix needed there. No other tool in
this module claims coverage of fields it doesn't render.

## Tests

New file `tools/PcTools/tests/test_mcp_server_control_model_fields.py`, 9
cases against mocked `_srv._control`/`zones_http_client` (no live board):
UNKNOWN-temp sentinel does not leak as `-273.15`; a real fit temperature
renders as a temperature; the all-zero "no model" sentinel does not render
as `K_dc=0.0000`; real model values render with units; both `tuning_valid`
states render; `autotune_baseline_k_dc` reports itself as not exposed;
missing fields degrade to `missing`/`unavailable` instead of crashing; an
end-to-end case through `control_get_zones()` itself; and a negative test
proving the primary UNKNOWN-sentinel assertion has teeth (a deliberately
broken renderer inline -- not a mutation of production code -- reproduces
`-273.15` leaking and the assertion catches it).

Also negative-tested the actual production code per
`feedback_negative_test_every_check`: temporarily short-circuited the
UNKNOWN-sentinel branch in `_describe_model_fields()` (added `False and` to
its guard), reran `test_mcp_server_control_model_fields.py`, confirmed
`test_unknown_fit_temp_sentinel_renders_as_UNKNOWN_not_a_number` failed with
`-273.15` visibly leaking into the rendered output, then restored the
source by hand (removed the `False and`) and reran to confirm all 9 tests
pass again. Pure Python, no separate build step to invalidate.

Full suite: `pytest tools/PcTools/tests` -- **2301 passed, 13 skipped**
(891s). No failures, no regressions.

## Live board readback (first real look at these fields)

Read via `kiln_call(name="control_get_zones")` against the live board,
2026-09-13, through the OLD (unrestarted) server code first
(`coupling matrix`/`http-only fields` sections only, no model fields --
confirming the gap), then the code fix was made and unit-tested. **The MCP
server restart to confirm the new output through a live call was DEFERRED**:
`profiles_get_exec_status` showed `state=1 ... dwelling=True` (segment 0/3,
`elapsed=2077s`, `dwell_remaining=623s`) -- another agent's heating run is
actively in progress, matching this session's standing instruction not to
restart the kilnctrl MCP server while a firing is active. Restarting
`kilnctrl` would drop the live UART/HTTP client state other in-flight tools
depend on and risks disrupting that run's monitoring.

Once the run completes (or a later session confirms none is active),
restart with `.\tools\PcTools\scripts\mcp_servers.ps1 restart` and re-run
`kiln_call(name="control_get_zones")` to confirm the new "plant model"
section renders live, and to get the actual live values -- as of this note
those numbers are still owed. The unit tests above exercise the exact code
path that will run against the live response; they are not a substitute for
the live confirmation, only a stand-in until the restart is safe.

**Follow-up required by this repo's standing MCP-server discipline**: do
not treat this fix as done until a restarted server's live
`control_get_zones()` output has actually been read back and the per-zone
model_k_dc/model_tau_s/model_dead_time_s/tuning_valid/model_fit_temp_c/
model_fit_ambient_c values are recorded here.
