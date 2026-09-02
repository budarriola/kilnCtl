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

`kilnctrl` registers 134 tools and `kicad` 86. Published as MCP
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
| `kilnctrl` | ~20,160 tokens | ~697 | 96.5% |
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
