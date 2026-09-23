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

`kilnctrl` registers 181 tools and `kicad` 86. Published as MCP
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
| `build_kilnfw(target, jobs, skip_saftyfw)` | kilnctrl | sources the Espressif PowerShell profile; `jobs>0` calls ninja directly because idf.py rejects `-- -j N`. **2026-09-20:** for a `build`/`reconfigure` target it now builds SaftyFW first (via `build_saftyfw()`) and aborts before starting the KilnFW build if that fails, reporting both build reports -- the KilnFW application build `EMBED_FILES`s both SaftyFW slot images (`docs/PICO_AUTO_UPDATE_PLAN.md`) and needs a fresh pair present in `firmware/SaftyFW/build/`. Pass `skip_saftyfw=True` to opt out (e.g. a caller that just ran `build_saftyfw()` itself); `fullclean` and other non-build targets never trigger it. |
| `build_saftyfw(jobs)` | kilnctrl | ninja in `firmware/SaftyFW/build` |
| `build_saftyfw_host_tests()` | kilnctrl | off-target MSVC unit tests |
| `run_pctools_tests(pattern)` | kilnctrl | the pytest suite |
| `bench_test_run(suite, cases, dry_run, allow_heat, ap_password, tag, host)` | kilnctrl | standardized bench regression testing (docs/BENCH_TEST_SYSTEM_PLAN.md); Wave 0 only runs read-only cases -- calls existing tool functions in-process, never a second MCP server or hardware directly |
| `bench_test_list(suite)` | kilnctrl | lists known suites, or one suite's case ids/descriptions and whether each has a judge function implemented yet |
| `bench_test_last(n)` | kilnctrl | the most recent run(s)' `summary.json`, read back from `logs/bench_test/` |

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

## Default host resolution (`kilnctrl.host_resolve`)

Every `*_http_client.py` used to hardcode its own `*_AP_DEFAULT_HOST =
"192.168.4.1"` (the board's softAP fallback address) as the default `host`
argument -- correct only for a board that has never joined a LAN, and wrong
for a board already provisioned onto the bench LAN (e.g. 192.168.1.156),
which cost one session a `/24` sweep to rediscover. `kilnctrl/host_resolve.py`
centralizes the default in one resolution order: the `KILNCTL_HOST`
environment variable if set, else the last host any client actually got a
response from (persisted in the same gitignored `settings.json` the GUI
already uses for the last serial port and OpenOCD path, via
`kilnctrl.settings.get_last_host`/`set_last_host`), else `192.168.4.1`
unchanged. Every module's `*_AP_DEFAULT_HOST` constant now calls
`host_resolve.resolve_default_host()` instead of hardcoding the literal, and
`http_auth.urlopen()` -- the seam nearly every client's request already goes
through -- calls `host_resolve.record_host_seen()` after any request that
gets a real HTTP response (success or a non-401 error), so the cache updates
itself from ordinary tool use with no extra wiring, including inside
`flash_firmware()`'s own post-flash verification (which calls
`partition_http_client.get_partitions()`, itself routed through
`http_auth.urlopen`). An explicit `host=` argument, or `flash_firmware()`'s
own ordered candidate list, is untouched by any of this and always wins.
Tests: `tools/PcTools/tests/test_host_resolve.py`.

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
   `KilnFW/flash_provenance.json` (a sibling of `build/` since 2026-09-15,
   not inside it -- see `elf_archive.kiln_provenance_path()`), so "what was actually on the board
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

**2026-09-20: the provenance report also names the embedded Pico (SaftyFW)
image identity.** Since the ESP application now embeds both SaftyFW slot
images (`docs/PICO_AUTO_UPDATE_PLAN.md`), `flash_firmware()`'s provenance
note gains one more line reading the `saftyfw_image_identity_t` record(s)
found by scanning the app binary about to be flashed (same scanning parser
as `check_embedded_pico_image_fresh.ps1`,
`kilnctrl/pico_image_freshness.py`) -- the commit and dirty flag the
embedded Pico image was built from, or a plain statement that no record was
found (a KilnFW build predating this feature, or one built without the
embedding wired up). This is read-only and purely informational: it never
blocks or changes a flash, it only puts the Pico expectation on the same
record as the rest of the flash's provenance.

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

**Post-flash boot_guard counter reset is default-on (owner decision
2026-09-19).** `flash_firmware()` calls `POST /api/ota/esp/boot_guard_reset`
after post-flash verification confirms full, unambiguous success (never on a
raise, a WARNING, or `verify=False`) -- see
`docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md`. This used
to require an explicit `ap_password` argument; it is now attempted whenever a
credential is available at all: an explicit `ap_password` still wins if
passed, otherwise it falls back to the `KILNCTL_AP_PASSWORD` environment
variable -- the board's **AP Wi-Fi password**, distinct from and never equal
to the web admin password (`web_auth_store.c:157`), since
`POST /api/ota/esp/boot_guard_reset` verifies its HMAC keyed on the AP
password specifically. If it is not set, the tool result reports the
reset was skipped for lack of credentials rather than saying nothing. Pass
`reset_boot_guard=False` to opt out unconditionally. The password is never
logged or echoed, and the result always names the counter's before/after
values (or the skip reason).

**Data-partition erase during a commission reflash (owner decision
2026-09-21).** `flash_firmware()` takes `erase_partitions: list[str] = None`
plus a required `confirm_erase: bool = False` gate. This exists for one
specific case first: the board's web-auth admin record has an unknown
password, and `web_auth_store.c:18-31`'s `kiln_auth` namespace lives in the
DEFAULT `nvs` partition (`partitions.csv`'s `nvs,data,nvs,0x9000,0x6000`
row) -- so resetting that record means erasing that partition, not guessing
or brute-forcing a credential. No tool erased any data partition before
this: a hand `flash erase_sector` once wiped the WHOLE chip (see this
module's header comment), which is exactly the failure mode this parameter
is built to avoid repeating.

Each requested name is resolved fresh from `<kiln_fw_root>/partitions.csv`
(the same parser `_resolve_app_flash_target()` already uses, so a
`kiln_fw_root` worktree override's own table is what is consulted, never a
hardcoded offset) and refused -- before OpenOCD is touched at all -- unless
it is BOTH in the allowlist `ERASABLE_DATA_PARTITIONS` (`nvs`, `kiln_nvs`,
`wifi_nvs`, `profiles_nvs`, `cfg`) and actually present in that CSV.
`app`/`recovery`/`otadata`/`bootloader`/the partition table/`coredump` can
never be named here -- requesting one of those, or any name outside the
allowlist, refuses immediately, naming the offending partition.
`confirm_erase=True` must be passed alongside `erase_partitions`; omitting
it refuses too, naming every requested partition, so an erase can never
happen as a side effect of a call that only meant to flash firmware.

Each resolved partition gets a 0xFF-filled file (erased flash's read-back
value) written to a temp directory, sized to exactly that partition's
`size`, and appended to the SAME OpenOCD session as the app image -- as its
own `program_esp <file> <offset> verify` line, after the app image's write
and before the session's final `reset exit` -- never a separate session,
and never a bare `flash erase_sector`. The temp directory is removed after
the session ends regardless of outcome. The result names each erased
partition's name/offset/size and the write's verify outcome, and this is
persisted to `flash_provenance.json` under `erased_partitions` -- an erase
is never silent, and a failed flash still records what erase was attempted
rather than dropping it from the record.

Erasing `nvs` destroys: the web-auth admin record for BOTH roles
(`web_auth_store.c`'s `kiln_auth` namespace -- the LCD PIN and the web admin
password both revert to unset/first-run), the auth policy stored alongside
it (auth reverts to OFF until reconfigured), any pre-2026-08-13 legacy
remnants still stored in that namespace, and the Wi-Fi driver's own
`nvs.net80211` data plus PHY calibration data that the ESP-IDF Wi-Fi/RF
stack also keeps in this same default `nvs` partition -- both are
regenerated automatically (a fresh scan/associate and a fresh calibration
pass) and are not a credential, so this is a cosmetic one-time delay, not a
config loss. It does NOT touch: Wi-Fi
credentials (`wifi_nvs`), zones/profiles config (`kiln_nvs`/`profiles_nvs`),
boot_guard or crash_report state (also `kiln_nvs`), or the `cfg` LittleFS
partition's own data -- each of those is erased only if separately named in
`erase_partitions`.

## Building from a clean worktree for `kiln_fw_root`

`flash_firmware(kiln_fw_root=...)` exists for exactly the "build from a clean
git worktree at HEAD" case (the main tree carries another session's WIP that
would ride along or trip the sensitive-dirty guard above). A fresh
`git worktree add` there hits two traps, both hit and fixed 2026-09-11:

1. **The `lvgl` submodule is not checked out in a fresh worktree.**
   `git worktree add` does not initialize submodules on its own, and `idf.py
   build` fails with `Failed to resolve component 'lvgl' required by
   component 'drivers'` -- a plausible-looking but wrong lead (it reads like
   a missing/renamed component, not a missing submodule checkout). Fix:
   `git submodule update --init firmware/KilnFW/components/lvgl` before the
   first build in the new worktree.
2. **`sdkconfig` is gitignored**, so a fresh worktree has none, and `idf.py
   build` silently defaults to plain `esp32` (`-- IDF_TARGET not set, using
   default target: esp32`) instead of this board's `esp32s3`. The failure
   this produces is a confusing one: the build proceeds a long way (dependency
   resolution, most of the component list) before dying deep in a target-only
   header (`temperature_sensor.h: 'TEMPERATURE_SENSOR_CLK_SRC_DEFAULT'
   undeclared`) -- nothing in the error names "esp32 vs esp32s3" directly.
   This is the same "gitignored config hides a mismatch" class as
   `feedback_gitignored_config_hides_mismatch` (that one was the FT6336U
   touch panel; this is the build target). Fix: after any `fullclean` or a
   from-scratch worktree, run `idf.py -C <worktree>/firmware/KilnFW
   set-target esp32s3` explicitly before `build` -- do not rely on a stale
   `sdkconfig` or the tool's own default.

## Git workflow guards (`tools/worktree_mint.ps1`, `tools/push_verify.ps1`, `tools/commit_guard.ps1`)

Three small PowerShell tools under `tools/` close three recurring, expensive
failure modes seen repeatedly in this project's development workflow (each
with real damage on record -- see each script's own header for the incidents
it exists to prevent). None of them assert anything standing about the
repository's current state, so none is wired into `run_all_checks.ps1` --
they are invoked by hand at the workflow moment they apply.

**`tools/worktree_mint.ps1`** -- mint or remove a short-lived worktree at
`origin/main` under `C:\wt\`. `C:\wt\` is a flat namespace shared by every
concurrent session on this machine, and two constraints have bitten
repeatedly: the path must be SHORT (a nested `.claude/worktrees/...` path
overflows the MSVC command line building SaftyFW host tests) and the name
must be UNIQUE (generic names collide between live sessions).

```powershell
powershell -ExecutionPolicy Bypass -File tools\worktree_mint.ps1 -Label myfeature
    # fetches origin, creates C:\wt\myfeature_<random> at origin/main,
    # refuses rather than reusing an existing directory, prints
    # "WORKTREE: <path>"

powershell -ExecutionPolicy Bypass -File tools\worktree_mint.ps1 -Remove -Path C:\wt\myfeature_ab12cd
    # removes cleanly; refuses if the worktree has uncommitted changes
    # (tracked or untracked) unless -Force is also passed
```

As of 2026-09-19 the mint step also runs `git submodule update --init
--recursive` in the new worktree by default (`-NoSubmodules` opts out).
`git worktree add` never initializes submodules on its own, so a fresh mint
used to leave `firmware/KilnFW/components/lvgl` and `tools/mykicadMcp` as
empty directories -- failing every KilnFW target build
(`check_00_kilnfw_target_build.ps1` / its `_recovery_` sibling, "Failed to
resolve component 'lvgl'") and both mykicadMcp-dependent checks
(`check_mcp_facade_coverage.ps1`, `check_mykicad_golden_suite_runs.ps1`)
until someone ran that command by hand. A submodule-init failure is printed
loudly but never fails the mint itself -- the worktree is still handed back.

`check_00_kilnfw_recovery_target_build.ps1` separately picked up the same
per-tree stale-directory prune that `check_00_kilnfw_target_build.ps1`
already had (2026-09-19): it reuses one persistent
`C:\wt\checkbuild_recovery_<hash-of-tree-path>` directory per invoking tree
(good for incremental build speed) but, before this fix, never cleaned one up
once its owning tree (a removed agent worktree) was gone -- 29 accumulated,
~4.8 GB. It now writes the same kind of `.checkbuild_source` ownership marker
and prunes a stale directory only once its named owner no longer exists on
disk and its per-tree build lock is not currently held, exactly mirroring the
main check's existing logic. Separately, `check_00_kilnfw_target_build.ps1`'s
own "am I the main worktree" test used to ask the invoking tree's own `git
worktree list` whether it was the first (therefore "main") entry -- true for
ANY standalone clone of its own accord, so an unrelated private repo copy
could claim the shared, pre-warmed `C:\wt\checkbuild` directory and mix its
source into the real main tree's build (a bogus undefined-reference link
failure was traced to exactly this). It now compares against one fixed,
known absolute path for the real shared tree instead of a self-reported
claim, so a distinct source tree always gets its own hash-tagged directory
and never shares a mirror with another tree.

**Stale cached build config self-heals (2026-09-19).** A reviewer found that
`check_00_kilnfw_target_build.ps1`'s persistent per-tree checkbuild directory
(`C:\wt\checkbuild_<hash>`, above) could carry a `build/` configured against
an OLDER sdkconfig than the one just copied in from the invoking tree, with
nothing comparing the two -- every downstream ELF-grading check then graded a
binary built against the wrong config (observed: `CONFIG_KILNCTL_ENABLE_GPIO_PROBE`
cached `n` while the copied sdkconfig said `y`). Both `check_00_kilnfw_target_build.ps1`
and `check_00_kilnfw_recovery_target_build.ps1` now hash the config that
actually governs the build (`sdkconfig` for the main target, `sdkconfig.defaults`
for recovery) and compare it against a marker file left by the last build that
used this checkbuild directory (`build\.sdkconfig_built.sha256` /
`build\.sdkconfig_defaults_built.sha256`). A mismatch (or no marker, on an
already-configured `build/`) runs `idf.py reconfigure` before building, printing
both the previous and current hash; the marker is rewritten only after the
freshness check passes, so a build that fails never reports a false "known
good against this hash." The PASS line itself now names the sdkconfig hash the
graded ELF was built from, e.g. `PASS: KilnFW target build succeeded, ...
(built against sdkconfig hash D72984E5...)`, so a later reader/check does not
have to trust that the checkbuild directory happened to be current -- it can
compare that hash against the invoking tree's own `sdkconfig`. Net effect:
**a stale checkbuild directory is no longer something an agent needs to
hand-delete before trusting a stack-margin or other ELF-derived measurement --
the check now detects and corrects it itself.** `check_00_saftyfw_target_build.ps1`
needed no equivalent change: it already runs `cmake .` (an unconditional
reconfigure) on every invocation, never trusting a cached configure across runs.

**`tools/push_verify.ps1`** -- verify a commit actually landed on
`origin/main`, in one unambiguous verdict line. This project has produced
four false "landed" reports from two specific causes: (1) running the
ancestry check backwards -- `git merge-base --is-ancestor origin/main HEAD`
asks "is origin/main an ancestor of my branch", which succeeds even for a
commit stranded on an unpushed local branch, not `git merge-base
--is-ancestor <mine> origin/main`, the question that actually matters; and
(2) trusting `$?` after a native `git push` in PowerShell 5.1, which is set
to `$false` on any command that wrote to stderr -- and `git push`'s own
progress banner does that on a successful push. This script uses the correct
argument order and reads only `$LASTEXITCODE`, never `$?`, never push output.

```powershell
powershell -ExecutionPolicy Bypass -File tools\push_verify.ps1 -Commit <hash> [-Branch origin/main]
    # prints "VERDICT: LANDED -- ..." or "VERDICT: NOT LANDED -- ...",
    # and on NOT LANDED also names the local branch(es) the commit IS
    # reachable from, if any (the actual common root cause)
```

**`tools/commit_guard.ps1`** -- guard a commit against the stale-working-copy
trap before it happens. `git commit -o <path>` (and `--amend` without a
pathspec) commits the WORKING COPY of a path whole, not your edit
specifically; in a tree several sessions edit concurrently, a stale working
copy silently reverts everyone else's changes to that file. This has
happened twice for real here: a stale doc commit reverted 113 lines of
another session's work, and a bare `--amend` pushed a 1067-line revert of
live work. The script compares `git hash-object <path>` against
`git rev-parse origin/main:<path>` for each path about to be committed, shows
the diff, reports insertion/deletion counts, and refuses by default until
the caller passes `-Confirm`. An optional `-ExpectedMaxLines` per path flags
any path whose actual insertion+deletion count exceeds what the caller
declared -- that count was the available tell in the real 113-line incident
and was read past unlooked-at.

```powershell
powershell -ExecutionPolicy Bypass -File tools\commit_guard.ps1 -Path CLAUDE.md -ExpectedMaxLines 40
    # refuses (exit 1) unless -Confirm is also passed, or the path is
    # unchanged/new vs origin/main; also refuses if actual changed lines
    # exceed the declared budget, regardless of -Confirm
```

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
