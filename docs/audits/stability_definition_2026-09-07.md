# Stability definition and soak procedure (2026-09-07)

## Why this exists

The owner's stated sequence is: current-sensor commissioning, then confirm
the board is **stable**, then start the filesystem work (`docs/
FILESYSTEM_PLAN.md`). "Stable" had no threshold or evidence trail behind it
-- it could not actually be demonstrated, only asserted. This document
defines it in measurable terms and points at the script
(`tools/PcTools/scripts/stability_soak.py`) that gathers the evidence.

## What "stable" means here, concretely

All of the following, over the soak window (see "How long" below):

| Metric | Source | Threshold | Rationale |
|---|---|---|---|
| Unacknowledged crash report | `GET /api/crash_report` (via `get_heap_status`) | none, ever | The 2026-08-31 incident ran 5 hours unnoticed before this check existed; a crash any time in the window is an automatic FAIL, not something to average away. |
| ESP reset_reason | same call | never one of `panic/exception`, `panic`, `exception`, `watchdog`, `brownout` (`UNCLEAN_RESET_REASONS`) at any sample | An unclean reset seen mid-soak means the board rebooted uncleanly during the window, even if it now looks fine. |
| `heap_internal.free` | `GET /api/status` | stays above **36000 B**, and does not trend down over the run | 11.9 kB free is the *documented, measured* point where HTTP sockets started resetting (`docs/bench_snapshots/2026-09-04.md`, `docs/FILESYSTEM_PLAN.md`). The floor here is 3x that with a round number -- a working margin, not the failure point itself. This is evidence-derived, not guessed. |
| `heap_internal.min_free` | same | reported, not gated | Low-water mark since boot; useful context for the free-trend reading, not a separate pass/fail line (it can only go down, so gating on it would fail every long-lived board). |
| Per-task stack margin | `GET_STACK_MARGIN` (UART) | every alive task reports `StackMarginLevel.OK` at every sample | The firmware already classifies LOW/CRITICAL against each task's *configured* stack size -- that classification already *is* "above its registered minimum," not a separate percentage invented for this doc. **Caveat, and it is a real one:** a soak run with no firing active is an idle-only exercise. The idle-baseline finding (`project_idle_stack_baseline_is_a_floor`) applies directly -- both LOW-priority tasks have deep paths that never run idle, so an idle PASS here is a *floor*, not proof the same tasks are fine under a firing's deep paths. Run the soak with `--firing-in-progress` for real coverage; see below. |
| Safety-link counters (`crc_errors`, `timeouts`, `broadcast_dropped`) | `GET_LINK_STATS`/`GET_DIAG` (UART) | flat (no increase) over the run | These have never been observed to self-heal; any climb during the window is real loss happening now, not residue from before the soak started. |
| Safety warn/trip mask | `GET_DIAG` (UART) | no NEW bit vs. the first sample of the run | S5 (no safety thermocouple fitted) is a known, already-armed warning on this board (CLAUDE.md / `docs/SAFETY_CASE.md` H2 row) -- it is expected to already be set at sample 0 and is not re-flagged every cycle. A bit that turns on *during* the run that was not on at the start is what actually matters. |
| ESP build identity (`commit`/`built`/`dirty`) | `GET_FW_VERSION` (UART) | unchanged for the whole run | A change mid-run means the ESP rebooted onto different code; every earlier "OK" sample in that run is then about a different build than the later ones and the run should be treated as two runs, not one. |
| Pico build identity + `boot_id` | Safety `GET_FW_VERSION` (UART) | unchanged for the whole run | Same reasoning, other processor. `boot_id` changing is the RP2040-side signal of "it rebooted" independent of build identity (`CommonFW/docs/LINK_PROTOCOL.md`'s `boot_id_changed` handling), so it is checked even when commit/built happen to look the same. |
| `profiles_get_exec_status().fault_guard` | UART | `0` at every sample | Any nonzero value means the executor itself is reporting a fault condition during the window. |
| httpd health (indirect) | HTTP call success/latency per sample | every `GET /api/status` succeeds; no sustained latency growth | There is currently **no direct httpd open-socket-count metric exposed by the firmware** (checked `dashboard_http.c`'s served JSON and the diagnostics/timing endpoint -- neither carries one). The soak's own periodic HTTP GET is used as a proxy: a leak or wedge (`project_httpd_wedge_is_dram`, `project_http_resets_acceptmbox`) shows up here as growing latency or outright failures long before it would show up any other way this tooling can see today. This is a known gap, not a solved one -- see "What this does NOT cover" below. |

A run is **PASS** only if every metric above holds for the *entire* window,
not just at the final sample -- the soak script checks every sample, and
also computes a trend (first-vs-last, >5% move counts as a direction) for
the two metrics where a trend matters more than any single reading
(`heap_internal.free`, stack min headroom%).

## How to run it

```powershell
uv run --project tools/PcTools python tools/PcTools/scripts/stability_soak.py --duration 3600 --interval 60
```

- `--duration` in seconds (default 3600 = 1 hour).
- `--interval` in seconds between samples (default 60).
- `--out PATH.csv` to control where the CSV lands (default: timestamped file
  in the current directory).
- `--host IP` to skip Wi-Fi-status autodetection of the dashboard host.
- `--firing-in-progress`: **required** to soak while a profile is actively
  running. Without it, the script refuses to start if
  `profiles_get_exec_status()` reports RUNNING/PAUSED -- an idle soak and a
  loaded soak answer different questions, and silently mixing them would
  make a PASS mean less than it claims.

The script is strictly read-only: no relay write, no config write, no reset,
no OTA, no firing control call. It queries only `GET_*`/`GET_STATUS`-shaped
UART commands and `GET /api/status` / `GET /api/crash_report` over HTTP.

### Minimum defensible soak duration

**30 minutes idle, minimum; 1 hour idle is the standard run; a firing-length
run (however long the next real firing takes) for the loaded case.**

Reasoning: the failure modes this soak exists to catch are not
instantaneous --
- The DRAM exhaustion pattern (`project_esp_internal_dram_exhaustion`) and
  the httpd wedge (`project_httpd_wedge_is_dram`) both accumulated over
  sustained operation, not on connect.
- `crc_errors`/`timeouts` drift is a rate; a handful of samples cannot
  distinguish "flat" from "climbing slowly" -- `TREND_MIN_SAMPLES = 5` in
  the script is deliberately conservative for exactly this reason, and at
  the default 60s interval, 5 samples is only 5 minutes, so 30+ minutes at
  that interval gives real margin above the minimum the trend check needs.
- The stack-margin idle-vs-firing gap (above) cannot be closed by a longer
  *idle* run at all -- only a run with `--firing-in-progress` during an
  actual firing exercises the deep paths. So "stable" in the full sense
  this board needs requires **both** an idle soak and at least one
  firing-time soak; a long idle-only soak is necessary but not sufficient.

## What each failure mode looks like in the CSV

- **Crash / unclean reboot:** `unacknowledged_crash` flips to `1`, or
  `esp_reset_reason` shows `panic`, `watchdog`, etc. partway through the
  file; `esp_uptime_s` also resets to a small number at the same row.
- **DRAM exhaustion:** `heap_internal_free` trending down across rows,
  eventually approaching or crossing 36000 B (well before it would hit the
  documented 11.9 kB failure point). `http_ok` flipping to `0` or
  `http_latency_ms` spiking on later rows is the secondary symptom, matching
  how the original 2026-08-xx incidents actually presented (see
  `project_esp_internal_dram_exhaustion.md`).
- **Stack overflow risk:** `stack_worst_level` moves from `OK` to `LOW` (or
  `CRITICAL`) on some row, with `stack_worst_task` naming which task.
  Remember: seeing `OK` for an entire *idle* run does not clear a task that
  only runs its deep path during a firing (`profile_executor`,
  `autotune_engine`, LVGL flush-adjacent paths) -- that requires the
  `--firing-in-progress` run.
- **Safety link degrading:** `crc_errors`, `timeouts`, or
  `broadcast_dropped` increasing row-over-row instead of holding constant.
  The summary line prints the delta across the whole run for exactly this.
- **New safety condition:** `warn_mask` or `trip_mask` gaining a bit not
  present in the first row -- printed inline as `PROBLEM: new safety warn
  bits since baseline: 0x..` at the row where it first appears.
- **Build/boot inconsistency:** `esp_commit`/`esp_built` or
  `pico_commit`/`pico_built`/`pico_boot_id` changing value between two rows
  -- means a processor rebooted mid-soak, onto a possibly different build.
- **httpd degradation (proxy):** `http_ok` going to `0` on some rows, or
  `http_latency_ms` growing across the run without recovering. This is an
  indirect signal, not a direct socket-count metric -- see the table above.

## What stability evidence exists TODAY, versus what a soak run would add

**Today, before any soak run:**
- The board has been reflashed repeatedly during this session (per this
  session's own history), so **current uptime is short** -- this is a fact
  to report plainly, not to paper over. A short uptime is not itself
  evidence of instability, but it also means none of the trend-based
  metrics above (heap trend, counter drift) have had time to show anything
  either way yet.
- `get_heap_status`/`get_stack_margin`/safety status are all live-queryable
  right now and returned clean single-point readings during this pass's
  smoke test (see below) -- i.e., the board is not *currently* crashed,
  faulted, or reporting a new safety condition. That is a snapshot, not a
  soak.
- One stack-margin finding came out of the smoke test itself (see below)
  and is already a real, present-tense data point, not hypothetical.
- Known, already-accepted conditions (not instability): S5 warn (no safety
  thermocouple fitted), CT-gated guards S3/S4/S9/S11/S14 structurally inert
  (no CTs on this board configuration as of the relevant note), Pico
  firmware attached and answering (contrary to a stale docstring comment in
  `safety.py` claiming otherwise -- that comment predates the Pico
  firmware landing and is now wrong; not corrected here since fixing stale
  comments is out of this task's scope).

**What a soak run adds that a snapshot cannot:**
- Whether heap free / stack headroom / safety counters hold flat over
  real time, versus a single instant that could be a lucky read right
  before a slide.
- Whether either processor reboots unexpectedly under sustained operation
  (build-identity / boot_id checks are meaningless against a single
  sample).
- Under `--firing-in-progress`, whether the deep-path tasks that never run
  idle stay within their configured stack sizes under real thermal/PWM
  load -- the single biggest gap between "looks fine on the bench" and
  "actually fine," per this board's own failure history (PWM chopping
  disarming guards, the httpd near-overflow under real load, and the two
  stack-overflow bricking incidents were all load-dependent, not
  idle-visible).

**Smoke-test finding from this pass (2 samples, ~5s, idle, real board):**
the soak script's first live run flagged `stack margin LOW on task
'system_uart_bridge'` on every sample (25.6% headroom at the time). This is
exactly the kind of finding the soak exists to surface plainly rather than
leave buried in a one-off snapshot -- it is real, present-tense, and should
be looked at before this board is declared stable, independent of any
longer soak. It was not investigated further here since this task's scope
is the definition and the tooling, not chasing this particular finding.

## What this does NOT cover

- No direct httpd open-socket / accept-queue metric exists in the firmware
  today (see the table above) -- the HTTP-call-success proxy is the best
  available signal, not a replacement for one.
- No CT-based current sensing is fitted as of this writing beyond the
  single summed-heater CT (`project_ct_sensor_on_gpio28`), so several
  safety guards (S3/S4/S9/S11/S14) cannot be exercised by any soak run --
  a soak reporting PASS says nothing about those guards' behavior.
- This soak does not itself start, stop, or wait for a firing; a
  `--firing-in-progress` run must be launched by a human (or a separately
  authorized process) once a firing is already underway.
