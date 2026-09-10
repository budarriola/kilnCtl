# MCP servers

Three MCP servers serve this repo. Two of them — `kilnctrl` and
`kicad` — are this project's own and were rebuilt around two decisions worth
understanding before using them.

| server     | transport | port | endpoint | what it drives                                      |
|------------|-----------|------|----------|-----------------------------------------------------|
| `kilnctrl` | HTTP      | 8767 | `/mcp`   | the main board (ESP32-S3) + RP2040 safety processor  |
| `kicad`    | HTTP      | 8766 | `/`      | the KiCad project, via the `mykicadMcp` submodule    |
| `pdf-mcp`  | stdio     | —    | —        | datasheet reading                                    |

`pdf-mcp` is untouched: it is a third-party server with a small tool surface and
no hardware to hold open, so neither decision below buys anything there.

Port 8765 is not free: it belongs to `kilnctrl`'s own `link_hub`
(`tools/PcTools/src/kilnctrl/link_hub.py`), which is how the GUI and the MCP
server share one physical serial port.

## Starting and stopping

```powershell
.\tools\PcTools\scripts\mcp_servers.ps1 start      # both; no-op if already up
.\tools\PcTools\scripts\mcp_servers.ps1 status     # what is listening
.\tools\PcTools\scripts\mcp_servers.ps1 restart    # after editing server code
.\tools\PcTools\scripts\mcp_servers.ps1 stop  -Server kilnctrl   # or kicad
```

In the editor these are the **MCP Start / Stop / Restart / Status** buttons in
the status bar, defined in `kilnCtl.code-workspace` (not `/.vscode/`, which is
gitignored).

Two things start them without being asked, because an HTTP MCP entry simply
fails to connect if nothing is listening:

* a `SessionStart` hook in `.claude/settings.json`, which covers every session
  including a plain terminal one
* the `MCP: Start servers` task in the workspace file, on folder open

Both call the same script, and the script is idempotent — it polls `/health`
first and does nothing if the servers are already up, so neither one restarts a
server that is mid-session on a serial link.

`.claude/settings.json` also lists both in `enabledMcpjsonServers`, so a
new session connects without stopping to ask for approval.

`tools/mykicadMcp/start_mcp_http_server.ps1` still starts the KiCad server on its own,
for anyone using that submodule outside this repo. Inside the repo the script
above is the one to use — same server, plus stop and status.

Two plain HTTP routes sit beside the MCP endpoint on each server:

* `GET /health` — liveness, pid, port, and the tool names it publishes
* `POST /shutdown` — graceful stop; the server releases its serial port first

Both are loopback-only and unauthenticated. Anything that can reach them can
already reach the MCP endpoint, which can flash firmware — a token on
`/shutdown` would protect nothing.

Stopping through `/shutdown` rather than killing the process matters: a COM port
left open by a dead process stays unusable on Windows until the device is
replugged.

### Stale-server self-announcing

Both `kilnctrl` and `kicad` are long-running, so each keeps serving whatever
code it started with — a source fix landed after the server came up is
invisible to it until a restart. That has twice sent debugging down the wrong
path: an MCP call reported a defect ("field not in the allowlist", say) that
the current source plainly did not have, because the process answering was
serving a build from before the fix landed.

Each server now knows this about itself and says so instead of leaving it to
be discovered by accident. At startup it snapshots the mtime of every source
file under its own package tree (`mcpkit.registry.take_snapshot`, kept out of
the hot path — the file list is walked once, then only re-stat'd) along with
the `git rev-parse` commit it started at. From then on:

* **`kiln_help()` / `kicad_help()`** prepend one line to their output. Fresh,
  it is quiet — `server code fresh: started <time>, at commit <hash>` — so
  freshness is checkable at a glance. Stale, it is loud:
  `SERVER CODE IS STALE: N files changed since this process started (started
  <time>, at commit <hash>). Restart via mcp_servers.ps1 restart.`
* **`mcp_servers.ps1 status`** shows the same fresh/stale line per server,
  read off `/health` (which now carries `fresh`, `changed_files`,
  `started_at`, and `commit` whenever the server was started with a
  `source_root`) rather than re-deriving it in PowerShell.
* **`tools/PcTools/selfcheck.py`** exercises `take_snapshot`/`check_staleness`
  against a real temp directory as a smoke test that the two halves still
  agree when wired together; `tools/PcTools/tests/test_mcpkit_freshness.py`
  covers the comparison logic itself against an injected fake clock.

If either `help()` or `status` reports stale, restart:
`.\tools\PcTools\scripts\mcp_servers.ps1 restart` (or `kiln_call(name=
"close_server")` followed by `... start` for `kilnctrl` specifically). The
start command can exceed a 120s tool timeout and finish in the background;
re-check with `status` rather than assuming it failed.

### Restart checklist

1. **Preconditions — nothing is mid-build/mid-flash, nothing is firing.**
   `build_kilnfw`/`build_saftyfw_host_tests`/`flash_firmware` all serialize
   through the cross-process lock in `tools/PcTools/src/mcpkit/buildlock.py`;
   a live one holds a `*.lock` file under
   `%TEMP%\kilnctl-builds\locks\` (age vs. the 2100s stale timeout tells you
   if it's a live holder or an abandoned one — see that file's docstring).
   Tail the newest `tools/PcTools/logs/session_*.log` for recent
   `flash_firmware`/`build_kilnfw` activity as a second signal. Then call
   `safety_get_status` — restart only once it shows no active firing (heating
   disabled / relays off).
2. **Restart and confirm freshness.** `.\tools\PcTools\scripts\mcp_servers.ps1
   restart`, then confirm via either `kiln_help()` (top line reads `server
   code fresh: started <time>, at commit <hash>`, no `STALE` line) or
   `mcp_servers.ps1 status`, which shows the same fresh/stale line per server
   off `/health` — e.g. today, before restart: `STALE  12 file(s) changed
   since started 2026-09-06 00:38:32 (commit 484846d) -- restart to pick up
   the change`. Confirm the commit hash now matches current `HEAD`.
3. **Post-restart smoke test (kilnctrl):**
   - `kiln_find(query="reset reason")` returns `safety_get_diag` among the
     results (it's indexed under `"reset reason"`/`"boot reason"` in
     `tools/PcTools/src/kilnctrl/mcp_facade.py`).
   - `safety_get_diag` decodes the Pico's last-boot reason live
     (`tools/PcTools/src/kilnctrl/mcp_server_safety.py`).
   - `safety_get_status` again first — proceed to `debug_reset(peer="pico")`
     only once it shows no firing and relays off. Then verify the link comes
     back up in ~100 ms and `boot_id` changes.
   - `safety_get_rate_guard` reads back cleanly (paired with
     `safety_set_rate_guard` — don't call the setter as part of a smoke test).
4. **pdf-mcp — separate from the above, not part of `mcp_servers.ps1`.**
   `pdf-mcp` is launched by the editor via `.mcp.json`
   (`"command": "tools/pdfMcp/.venv/Scripts/pdf-mcp.exe"`), so picking up a
   rebuilt venv is an editor/session reload, not a `mcp_servers.ps1` action.
   The queued fix (CLAUDE.md, "Where to start" section further up in this
   repo's root docs): kill the six live `pdf-mcp.exe` processes, rebuild
   `tools/pdfMcp/.venv` from scratch (`pip install pdf-mcp==2.0.0`), then
   delete the stale root-level `pdfMcp/` copy that the current venv's shims
   still point at.

### Decision 1 — HTTP instead of stdio

A stdio server is spawned by, and dies with, whichever client launched it. For a
server that owns a *physical port* that is the wrong lifetime: reconnecting a
client power-cycles the link, two clients cannot share one board, and there is
no way to ask a running server how it is doing without going through the
protocol it is currently blocked on.

`--transport stdio` still works, for headless and CI runs that want a private,
disposable server with no port to collide on:

```powershell
tools\PcTools\.venv\Scripts\python.exe -m kilnctrl.mcp_server --transport stdio
```

### Decision 2 — a search facade instead of 220 published tools

`kilnctrl` registers 154 tools and `kicad` 86. Published as MCP
schemas that is roughly 20,000 tokens each for `kilnctrl` and `kicad`, spent in
*every* context window before the model has read a word of the request.

Each server now publishes six tools instead (seven for `kicad`). The rest stay registered, callable,
and importable — only their advertisement is withdrawn.

| tool | what it does |
|------|--------------|
| `<p>help()` | groups with counts, plus the recipes that actually get run on this bench |
| `<p>find(query, group, limit, detail)` | ranked signatures for a plain-language query |
| `<p>describe(names)` | full schemas for named tools, batched |
| `<p>call(name, args)` | invoke one |
| `<p>batch(calls, stop_on_error)` | invoke several in one round trip |
| the `KEEP` set | kept published — always the first call of a session |

`<p>` is `kiln_` for kilnctrl, `kicad_` for kicad. The
`KEEP` set is `connect` / (`inspect_kicad_project`,
`get_kicad_ipc_status`).

Measured manifest cost:

| server | before | after | saved |
|--------|--------|-------|-------|
| `kilnctrl` | ~21,000 tokens | ~697 | 96.7% |
| `kicad` | ~20,237 tokens | ~799 | 96.1% |

The obvious risk of a dispatcher is that indirection costs reliability — a model
has to guess a name it has never seen a schema for. Everything in
`tools/PcTools/src/mcpkit/registry.py` is built to pay that back:

* `find` returns a rendered signature *and* a paste-ready `call` line, so the
  next step needs no invention.
* A wrong name is answered with ranked suggestions, not an error.
* A wrong argument is answered with the tool's signature.
* Arguments are coerced rather than rejected: `"3"` for an int, `"true"` for a
  bool, `"0x1f"` for a register address, a JSON string for an object.
* `batch` reports per-step status and stops at the first failure, so a
  half-applied hardware sequence is visible rather than silent.

Search is a small weighted BM25-style index built once at startup over tool
names, groups, curated keywords, summaries and parameter names, with a synonym
table that bridges the words a caller uses to the words the code uses
(`temperature` → `thermo`, `relay` → `io`/`expander`, `swd` → `debug`). The
tables live in `kilnctrl/mcp_facade.py` and
`tools/mykicadMcp/kicad_facade.py`. Query stopwords ("what", "how", "the") are dropped
before scoring, so a question-shaped query ranks on its nouns.

### The KiCad server is plumbed differently

`kilnctrl` is an `MCPServer` (FastMCP) application, so
`mcpkit.registry.collapse()` withdraws its tools from the framework's tool
manager. `mykicadMcp` hand-rolls its own JSON-RPC loop over a plain
`{name: {description, inputSchema, handler}}` dict, so it uses
`collapse_table()` instead — same registry, same five facade implementations,
different adapter.

Because that submodule is published on its own and must work outside this
checkout, it cannot import `mcpkit`. It carries a byte-for-byte copy at
`tools/mykicadMcp/mcpkit_registry.py`. Edit the PcTools original and copy it over;
`tools/PcTools/tests/test_mcpkit_vendored_copy.py` fails if the two drift.

## Build and test tools

The PcTools server also carries the build steps this repo's agents were
re-deriving by hand every session (`mcpkit/workbench.py`, group `build`). The
KiCad server has no equivalent -- there is nothing to compile there:

| tool | server | notes |
|------|--------|-------|
| `build_kilnfw(target, jobs)` | kilnctrl | sources the Espressif PowerShell profile; `jobs>0` calls ninja directly because idf.py rejects `-- -j N` |
| `build_saftyfw(jobs)` | kilnctrl | ninja in `firmware/SaftyFW/build` |
| `build_saftyfw_host_tests()` | kilnctrl | off-target MSVC unit tests |
| `run_pctools_tests(pattern)` | kilnctrl | the pytest suite |

They run their PowerShell scripts through `subprocess`, deliberately. Those
scripts set `$ErrorActionPreference = "Stop"` and `vcvarsall.bat` writes a
benign `vswhere.exe` line to stderr; a PowerShell host that wraps native stderr
in ErrorRecords turns that into a terminating error before a single test runs,
which reads exactly like a regression that is not there.

Output is summarized, never echoed whole: exit status, the diagnostic lines, and
a path to the full log under the system temp directory.

Flashing is not in this table on purpose. It already exists as
`debug_program(peer=...)` with `esp` / `pico` peers, each pinned to the
right probe serial (`kilnctrl/debug_probe.py`).

**Dual reflash (both processors reset close together) trips S6a -- this is
correct, not a bug.** The Pico starts polling `mainFault` almost immediately
on its own reset; the ESP takes longer to reach `safety_link_init()` and
complete the FW_VERSION handshake, and correctly drives GPIO6 (asserted)
for that whole window per its own link-down policy (`safety_link_poll.c`).
See `docs/audits/s6a_startup_grace_revert_2026-09-07.md` -- a guard-side
suppression of this was tried and reverted as unsafe. Procedure:

1. Flash/reset both processors as needed.
2. Call `safety_get_status()` and confirm link is up and FW_VERSION has
   been exchanged (not just that the tool call succeeded).
3. Confirm the trip is this one, not something else: `trip_reason` /
   `trip_mask` should show only `SAFETY_TRIP_MAIN_FAULT` (`trip_reason 6`,
   `trip_mask` bit 5 = `0x0020` -- `trip_mask` is `1 << (trip_reason - 1)`,
   see CLAUDE.md's dual-reflash note; `0x0040` is bit 6, `trip_reason 7`
   `SAFETY_TRIP_LINK_DEAD`/S6b, a DIFFERENT guard)
   -- if any other bit is set, do not clear, investigate instead.
4. Only then call `safety_clear_trip()`. Clearing before the link is
   actually up just re-trips (`safety_guards_try_clear()` re-checks live
   inputs and refuses while the condition still holds).

## Flash provenance and the sensitive-dirty-file guard

`get_fw_version()`'s `tree: dirty` has always been a single bit -- true or
false, no list. On 2026-09-04 that bit hid a real incident: an agent
authorised only to build+flash `KilnCtrl.bin` for a display/watchdog
diagnosis picked up *another* session's uncommitted, in-progress
`zones_config_*` schema-migration edits from the shared working tree and
flashed them, running an unplanned schema migration against the live board
config. It happened to land correctly -- a good migration plus luck, not
process -- and left a second live consequence (a client/firmware field
mismatch on `/api/zones` that blocks `load_config_preset` for every preset).

`flash_firmware()` (`kilnctrl/mcp_server_flash.py`, guard logic in
`kilnctrl/flash_provenance.py`) now:

1. **Always records** `git status --porcelain` (unscoped -- the whole repo,
   not just KilnFW/CommonFW the way `stale_check.py`'s staleness comparison
   is scoped, because the risk is cross-session) and HEAD at the moment of
   the flash. This is reported in the tool result and persisted to
   `KilnFW/build/flash_provenance.json`, so "what was actually on the board
   at `<time>`" is answerable from disk later, not just from a chat
   transcript that may have scrolled away.
2. **Refuses only when the dirty set touches a narrow, named sensitive
   list** (`flash_provenance.SENSITIVE_PATTERNS`: `zones?_config`,
   `_migrat`, `safety_cfg`, `safety_link`, `kiln_cfg_store`, `schema`),
   naming the offending files. An ordinary dirty tree -- this project's
   normal state, since several sessions share one working tree by design --
   is never refused; only that named list gates the flash. Override with
   `allow_sensitive_dirty=True` after actually reviewing the named files.

Two other guard shapes were considered and rejected:

- **A blanket `allow_dirty` toggle on "any dirty file"** (default True =
  report-only, False = refuse): a default-refuse would trip on ordinary,
  unrelated dirty files within a day of shipping and get switched off
  permanently -- worse than not existing, and it would not have
  distinguished today's incident from routine work anyway.
- **Caller-declares-its-own-scope** ("I'm only touching display code,
  ignore the rest"): this trusts the caller to know the full uncommitted
  footprint of every session sharing the tree at that instant -- exactly
  the information the agent in the incident did not have. It would have
  declared "display code", the guard would have checked declared-vs-dirty
  and found no conflict, and the same flash would have gone out.

A fixed sensitive-path list catches the incident regardless of what the
caller believes it is doing, stays silent on the other dirty files that make
a blanket gate unworkable here, and is bypassable only by an explicit,
logged opt-in rather than a setting people learn to leave on. Extend
`SENSITIVE_PATTERNS` (not a broader directory match) if another
schema/migration/safety surface needs the same protection.

Unit-tested in `tests/test_flash_provenance.py` against synthetic
`git status --porcelain` output -- clean tree, dirty-but-benign, and the
exact incident's mixed dirty set (schema files plus an unrelated edit) --
with a required negative test that drops the `zones?_config` pattern and
confirms the assertions fail, naming the file that slipped through.

Two ESP32-S3 boards are now permanently on the bench (2026-09-05: the main
board and the UnitTestFixture), and both share USB VID:PID 303A:1001 on
their native USB-Serial-JTAG interface -- indistinguishable to OpenOCD's
`board/esp32s3-builtin.cfg` without an `adapter serial`. `flash_firmware()`
now pins `adapter serial` to the main board's USB serial number and refuses,
before calling OpenOCD at all, if that serial isn't currently enumerated
(naming whichever 303A:1001 serial(s) are seen instead); a parallel
`fixture_flash()` tool does the same pinned to the fixture's serial.
`serial_link.recommend_port()` (the main board's own port picker) and
`fixture.recommend_fixture_port()` got the same serial/VID:PID-anchored
identity check, since either picker returning the other board's port
misdirects UART traffic just as badly as an unpinned JTAG flash.

## Adding a tool

For `kilnctrl`, write it in the server module with the existing
`@_tool()` decorator; for `kicad`, add an entry to `self.tools` as before.
Registration did not change on either. Then, if the tool's name does not make it
findable, add a keyword row to that server's facade module
(`kilnctrl/mcp_facade.py`, `tools/mykicadMcp/kicad_facade.py`)
and, for `kicad`, a `GROUP_OVERRIDES` row so it does not land in a junk group.
That is the whole change; the facade picks it up at import.

A useful check after adding one: call `<p>find` with the question a person would
actually ask, and confirm the new tool comes back first. If it does not, that is
a missing keyword, not a search bug.
