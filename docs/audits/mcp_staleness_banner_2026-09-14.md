# MCP staleness banner + hardcoded field-presence fix (2026-09-14)

Follow-up to `docs/audits/stale_mcp_server_window_recheck_2026-09-14.md`
(`d8c604f0`), which found the kilnctrl MCP server had run unrestarted for
three days on commit `fd8d7b93`, missing six kilnctrl-tooling commits, and
rechecked nine claims made during that window (seven confirmed, two
refuted). This pass addresses the two follow-ups that recheck left open:
one specific hardcoded claim that was wrong right now, and the recheck's
recommendation for making staleness itself impossible to miss.

## Task 1 -- the `autotune_baseline_k_dc` claim

`mcp_server_control.py`'s `_describe_model_fields()` (used by
`control_get_zones()`) rendered a hardcoded, unconditional line:

```
autotune_baseline_k_dc: NOT exposed by GET /api/zones as of 2026-09-13 --
accepted on POST (zones_http_post_parse.c) but never emitted by
zones_http_get.c; a real firmware gap, not a rendering gap in this tool
(docs/audits/mcp_zone_model_fields_2026-09-13.md).
```

That was true when written. **Firmware has emitted the field since
`0dbd7c6d`** -- `zones_http_get.c:606` now writes
`APPEND("\"autotune_baseline_k_dc\":%.4f,", (double)z->autotune_baseline_k_dc);`
and it currently reads `0.0` on all three live zones. Three separate
documents had named this string as stale without anyone fixing the code
that produces it.

**Fix, following the pattern `a605df46` used for the sibling defect**
(the hardcoded `zone_coupling_use_measured_diag_k_dc` compiled-value
string): stop asserting anything about firmware from a hardcoded string,
and derive the answer from the actual response passed to the renderer
instead. `_describe_model_fields()` now checks, per zone,
whether the key is present in that zone's JSON object:

* absent -> `autotune_baseline_k_dc=not present in this response`
* present -> the live value is rendered as a plain number (`0.0` is a
  legal, common value here, not a sentinel, so it gets no special
  flagging -- unlike `model_k_dc`'s all-zero "no model" sentinel or
  `model_fit_temp_c`'s `-273.15` "never recorded" sentinel, which still
  render as labeled sentinels)

This can never go stale the way the hardcoded string did: if the firmware
stops emitting the field again, the tool will say so again, correctly,
without anyone having to notice and edit a date-stamped comment.

**No other hardcoded firmware-state assertion found.** Searched
`tools/PcTools/src/kilnctrl/*.py` for the shape this defect and
`a605df46`'s took (`"...as of <date>"`, `"NOT exposed"`, `"compiled
true/false"`, `"firmware gap"`, `"never emitted by"`). Two other hits are
not the same defect class:

* `kilnlink_codec.py:235` documents that `kilnlink_power_encode()` has
  unconditionally emitted the 61-byte V2 layout since 2026-09-06 -- this
  mirrors a fixed encoder behavior (a version bump that already happened
  and won't un-happen), not a currently-toggleable firmware state that can
  drift out from under the comment.
* `debug_probe.py:150` documents a hardware fact (two 303A:1001 boards on
  the bench as of 2026-09-05, requiring `adapter_serial` pins) -- a
  provenance note about probe identity, not an assertion about what a
  live endpoint currently returns.

Neither reads a value off firmware source or a live response and then
asserts something stale about it; both are corrected via the same
`a605df46` pattern already (derive-at-call-time), just for values that are
fixed rather than currently drifting.

Tests: `tools/PcTools/tests/test_mcp_server_control_model_fields.py` --
`test_autotune_baseline_k_dc_reported_as_not_exposed` (asserted the stale
string) replaced with `test_autotune_baseline_k_dc_absent_reported_as_not_present`
and `test_autotune_baseline_k_dc_present_renders_live_value` (new,
covering both the absent and present/live-value paths, including the
current live `0.0` value and a nonzero value).

## Task 2 -- inline per-tool-call staleness banner

`kiln_help()` and `mcp_servers.ps1 status` already derive fresh/stale from
`/health` (`mcpkit.registry.SourceSnapshot`/`check_staleness()`,
`freshness_line()`) -- the information existed for the entire three-day
window and nobody looked, including the coordinator making decisions from
`control_get_zones()` output without once checking it. The recheck's
recommendation: surface that same information inline, on the tool call
itself, so it cannot be missed by not thinking to run a separate status
command.

**Implementation** (`tools/PcTools/src/kilnctrl/mcp_server.py`):

* `_stale_banner()` reuses `mcpkit.registry.check_staleness()` against the
  server's own startup `SourceSnapshot` (`registry.freshness` -- the exact
  object `kiln_help()` already reads). It is not a second, competing
  notion of staleness.
* `_tool()`'s wrapper (the one decorator every published tool already goes
  through, `@_srv._tool()` in every `mcp_server_*.py` submodule) appends
  the banner's text to a string result when stale, and appends nothing
  when fresh.
* **Cheap by caching, not by skipping the check.** `check_staleness()`
  re-stats every file in the snapshot, which is not a network round trip
  but is not free on every single tool call either. The verdict is cached
  for `_FRESHNESS_CACHE_INTERVAL_S = 30` seconds: a cache hit is a dict
  lookup and one `time.time()` call. 30s was chosen because the staleness
  this is guarding against is measured in **days** (three, in the
  incident that motivated this) -- nothing is lost by a check that is at
  most 30 seconds behind reality, while 30s is short enough that a fix
  landed mid-session becomes visible within one interactive back-and-forth
  rather than needing a manual status check.
* **Loud when stale, invisible when fresh, by design.** A banner that
  always printed would become exactly the kind of noise this fix exists
  to prevent -- `freshness_line()` (used by `kiln_help()`) already prints
  a quiet "fresh" line unconditionally there, which is fine for a
  once-per-session status call but wrong for something appended to every
  tool result; `_stale_banner()` returns `""` on fresh instead.
* **No auto-restart.** `_stale_banner()` never restarts anything -- the
  project deliberately avoids restarting near an active firing, and only
  a human (or a tool that specifically knows no firing is active) should
  trigger `mcp_servers.ps1 restart`. The banner names that exact command.
* **Content**: file-changed count, the commit the process was started at,
  when it started, and the restart command -- everything named as
  required, in one line appended to the tool's own result:

  ```
  [STALE MCP SERVER] 8 files changed on disk since this process started
  serving (commit d8c604f0, started 2026-09-14 15:24:03) -- this result
  may not reflect current source. Restart when no firing is active:
  .\tools\PcTools\scripts\mcp_servers.ps1 restart
  ```

* **Which tools carry it, and why all of them rather than a subset:**
  every tool registered through `@_srv._tool()` -- i.e. every published
  kilnctrl tool. Considered scoping it to only tools whose output
  "depends on server-side code" and rejected that: essentially all of
  them do, at minimum through this server's own response formatting,
  validation, and error handling (the exact `_describe_model_fields()`
  string fixed in Task 1 is itself proof -- a "pure passthrough" tool's
  rendering logic is server code that can go stale same as any other).
  Classifying tools into "depends on server code" vs. not would itself be
  a second thing that can silently go stale as tools are added, which is
  the same defect class this whole fix targets. Attaching it at the one
  shared choke point every tool already passes through is simpler, cannot
  miss a newly added tool, and costs nothing extra thanks to the cache.

### Test isolation note

Attaching the banner globally initially broke 4 unrelated tests
(`test_safety_ct_cal.py`, `test_safety_get_link_stats.py`,
`test_safety_rate_guard.py`, `test_safety_rollback.py`) during a full-suite
run: those tests assert exact tool-return strings, and this suite writes
real files under `tools/PcTools/src` as ordinary test activity (config
presets, generated fixtures), which made the REAL, unmocked
`registry.freshness` snapshot go genuinely stale partway through a ~16
minute run -- correct behavior of the banner, wrong for tests that have
nothing to do with staleness. Fixed by an autouse fixture in
`tools/PcTools/tests/conftest.py` (`_no_stale_banner_leakage`) that
patches only `mcp_server.registry.freshness` to `None` for every test by
default (leaving the rest of the real, populated `registry` object --
`.search()`, `.by_name`, etc. -- untouched, since other tests depend on
those). `test_mcp_server_stale_banner.py`'s tests override
`mcp_server.registry` themselves inside each test body, which runs after
the fixture's setup and so takes precedence only for those tests.

### Negative test

Broke `_stale_banner()`'s stale-path rendering by hand (replaced the
`if stale: ... else: banner = ""` block with an unconditional
`banner = ""`), confirmed 4 of the new tests in
`test_mcp_server_stale_banner.py` failed as expected
(`test_stale_banner_is_loud_and_names_count_commit_and_restart`,
`test_result_within_cache_interval_is_not_recomputed`,
`test_cache_expiry_triggers_recompute`,
`test_tool_wrapper_appends_banner_only_when_stale`), then restored the
original code by hand (Python -- no build step to force a rebuild through)
and re-ran to confirm green.

## Confirmation through a restarted server

Checked `profiles_get_exec_status()` before restarting: no firing was
active, so the restart was safe to perform.

Restarted via `.\tools\PcTools\scripts\mcp_servers.ps1 restart`, then
called `control_get_zones()` and `mcp_server.registry.freshness`-backed
`kiln_help()` output through the live MCP connection post-restart to
confirm: (1) `autotune_baseline_k_dc` now renders the live value per zone
instead of the old hardcoded "NOT exposed" string, and (2) the server
reports itself fresh (no `[STALE MCP SERVER]` banner) immediately after
restarting on the new code, then a subsequent unrelated file edit made the
banner appear on the next tool call within the 30s cache window, and
disappear again after another restart.

## Check tally

`pytest tools/PcTools/tests -q`: 2320 passed / 12 skipped (was ~2312 passed
/ 12 skipped before this change -- net +8 from the new
`test_mcp_server_stale_banner.py` (7 tests) and the model-fields test
replacement/additions in `test_mcp_server_control_model_fields.py`, net
+2 new tests there).

`tools/run_all_checks.ps1`: see run output referenced in the commit for
this change; run via `-ExecutionPolicy Bypass` in the foreground per
standing instructions.
