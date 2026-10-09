# Setting up a fresh clone

> **Status:** current · **Last reviewed:** 2026-08-16
> **Keep this file current.** If `tools/setup.ps1` grows a step, document it
> here in the same change. A setup script nobody trusts gets bypassed, and then
> the machine-specific files drift back into the repository.

```powershell
git clone --recurse-submodules <url> kilnCtl
cd kilnCtl
powershell -ExecutionPolicy Bypass -File tools/setup.ps1
```

Windows PowerShell 5.1 is enough; pwsh 7 also works. Add `-WhatIf` to see what
it would write without writing anything — worth doing the first time, since the
run rewrites `.mcp.json` and the `.vscode` files in place.

Then **open `kilnCtl.code-workspace`** in VS Code — not the folder. Restart VS
Code first if the script set environment variables, because it reads them at
launch.

Re-running the script is safe, and is the right move after an ESP-IDF upgrade:
the discovered paths carry a toolchain version in them and go stale.

---

## What is machine-specific, and how it is handled

Almost everything in this repository is portable. Three things cannot be:

| Thing | Why it varies | Handling |
|---|---|---|
| ESP-IDF, clangd, OpenOCD, xtensa-gcc paths | Install location and toolchain version differ per machine | Discovered by the script, written into generated files |
| Serial ports | Differ per machine, per cable, per USB port | **Not configured at all.** `PcTools` discovers ports; the ESP-IDF extension prompts |
| Claude Code's permission allowlist | Per user | `.claude/settings.json`, out of scope for this script |

The generated files are **gitignored**, and the committed source is in
`templates/`:

| Template | Generates |
|---|---|
| `templates/mcp.json.in` | `.mcp.json` |
| `templates/KilnFW.settings.json.in` | `firmware/KilnFW/.vscode/settings.json` |
| `templates/KilnFW.c_cpp_properties.json.in` | `firmware/KilnFW/.vscode/c_cpp_properties.json` |
| `templates/KilnFW.tasks.json.in` | `firmware/KilnFW/.vscode/tasks.json` |

> **Edit the template, not the output.** The next setup run overwrites the
> output without asking. This is the one foot-gun in the arrangement, which is
> why the generated files are listed in `.gitignore` with the same warning.

Placeholders are `@@NAME@@` and are substituted with JSON-escaped paths.

## Environment variables the script sets

Set at **user** scope, so they survive a reboot and are visible to any shell:

| Variable | Meaning |
|---|---|
| `IDF_PATH` | ESP-IDF checkout |
| `IDF_TOOLS_PATH` | Where the ESP-IDF installer put its tools |
| `IDF_PYTHON_ENV_PATH` | ESP-IDF's own Python environment |
| `KILNCTL_CLANGD` | `clangd.exe` for IntelliSense |
| `KILNCTL_OPENOCD` | `openocd.exe` for JTAG flash and debug |
| `KILNCTL_OPENOCD_SCRIPTS` | OpenOCD's `share/openocd/scripts` |
| `KILNCTL_XTENSA_GCC` | Compiler path for the C/C++ extension |

`kilnCtl.code-workspace` uses `${env:IDF_PATH}` and `${env:IDF_PYTHON_ENV_PATH}`
directly in its build task, which is why that file needs no generation step.

## Why the script checks outcomes, not exit codes

`git submodule update --init --recursive` silently **skips** any gitlink with no
`.gitmodules` entry and still exits 0. For a while this repository had four such
gitlinks — Claude Code agent worktrees, committed by accident — and the setup
step reported success the whole time while `git submodule status` failed outright.

The lesson generalised: a step that reports success by exit code cannot
distinguish "did the work" from "found nothing to do". So the script now proves
the outcome instead:

- every path in `.gitmodules` exists, is non-empty, and resolves to a commit;
- any tracked gitlink **not** in `.gitmodules` is named, with the `git rm --cached`
  command to fix it;
- after `uv sync`, `import kilnctrl` is actually executed — which is also what
  catches a stale editable-install path left behind by a directory move.

These checks are read-only, so they run under `-WhatIf` too. A dry run should
still tell you what is broken.

## What the script does not do

It **discovers and configures**; it does not install. If a tool is missing it
says so and carries on, because a missing OpenOCD should not stop you editing
documentation. Each warning disables one specific capability, and the summary
at the end lists them.

You still need, installed yourself:

- **ESP-IDF v6.0.2** with the ESP32-S3 toolchain — for `KilnFW`
- **uv** ([docs.astral.sh/uv](https://docs.astral.sh/uv/)) — for `tools/PcTools`
- **KiCad 10** — for anything under `hardware/`
- **pico-sdk and arm-none-eabi-gcc** — for `SaftyFW`, once it exists. Not yet
  discovered by the script, because there is nothing to build

## Verifying from a clean worktree

`tools/run_all_checks.ps1` is the sanctioned way to prove a commit is good, and
it is meant to be run from a **disposable `git worktree`**, not only the one
long-lived main tree every session shares (which accumulates provisioning no
fresh checkout has). `tools/setup.ps1` does not run this sequence for you
today — do this by hand once per worktree:

```powershell
git worktree add C:\wt\<name> origin/main        # short path: avoids MSVC/xtensa cmdline overflow
cd C:\wt\<name>
git submodule update --init --recursive

powershell -ExecutionPolicy Bypass -File tools\run_all_checks.ps1
```

No manual `idf.py set-target` and no manual `uv sync` are needed before that
run: `firmware/KilnFW/sdkconfig.defaults` pins `CONFIG_IDF_TARGET="esp32s3"`
in committed content (since `827dd887`), so a from-scratch `idf.py build` with
no `sdkconfig` at all targets esp32s3 correctly on its own — confirmed
directly, not just reasoned about, in `check_01_kilnfw_pushed_build.ps1`'s
header (2026-09-16) and again empirically in a from-scratch worktree on
2026-09-17 (see `docs/RELEASE_HARDENING_PLAN.md` section 11). Earlier
revisions of this doc recommended seeding a set-target step; that premise was
already false by the time it was written (the target had been pinned all
along) and following it is no longer necessary. Likewise, `tools/setup.ps1`
itself now runs `uv sync --project tools/PcTools` as part of its own run (see
above), so a separate manual `uv sync` before `run_all_checks.ps1` is
redundant, not required — a bare `git submodule update --init --recursive`
plus `tools/setup.ps1` is enough. `run_all_checks.ps1` self-provisions the
rest on first use in a fresh worktree: `check_00_saftyfw_target_build.ps1`
runs `cmake -G Ninja -B build .` when `build\CMakeCache.txt` is absent, and
`check_mykicad_golden_suite_runs.ps1` creates `tools/mykicadMcp/.venv` when
missing.

As of 2026-09-15, `check_00_kilnfw_target_build.ps1` (the check that actually
builds KilnFW) publishes `compile_commands.json` and this project's own
object-file trees into the worktree's `firmware\KilnFW\build\` alongside the
`.elf`/`.bin` it already published, so `check_compile_esp_backends`/
`compile_esp_backends.ps1` and `check_duplicate_symbols.ps1` — both of which
only ever *consume* that build output, never produce it — now have real input
on the very first `run_all_checks.ps1` pass in a fresh worktree, with no
separate manual `build_kilnfw` step required first. `idf.py set-target`
above already produces `compile_commands.json` via its CMake configure step
on its own, before `run_all_checks.ps1` even runs it; the check's own publish
step is what makes `check_duplicate_symbols.ps1` (which needs real *object
files*, not just the CMake configure) work without a hand-triggered build.

`check_mykicad_golden_suite_runs.ps1` self-provisions `tools/mykicadMcp/.venv`
(via `python -m venv` + `pip install -r requirements-mcp.txt -r
requirements-dev.txt`) the first time it runs in a worktree that doesn't have
one yet, so no separate manual step is needed for it either. If provisioning
itself fails (no `python`/`py` on PATH, no network), the check FAILS naming
the exact command to run by hand rather than silently skipping.

Remove the worktree when done: `git worktree remove C:\wt\<name>` (add
`--force` only if it reports uncommitted changes you intend to discard).

### Developer-only tools (`CONFIG_KILNCTL_DEV_TOOLS`)

`firmware/KilnFW/App/Kconfig.projbuild`'s `CONFIG_KILNCTL_DEV_TOOLS` gates
development-only affordances that must not ship in release firmware --
currently the safety commissioning page's "Apply test preset" button and
its backing route, which stage a fixed set of test values rather than
anything derived from a real installation. It defaults to `n`; a from-
scratch `idf.py build` with no `sdkconfig` leaves it off, matching a
release build. The shared development board's own `firmware/KilnFW/sdkconfig`
is gitignored (generated, not checked in), and that bench build turns this
on by hand -- add `CONFIG_KILNCTL_DEV_TOOLS=y` to it (or via `idf.py
menuconfig`, under "KilnCtrl Application") when bringing up a fresh bench
`sdkconfig`. A clean-worktree verification build never sets this, so
`check_00_kilnfw_target_build.ps1` and friends always exercise the
release-shaped (off) path.

## Verifying the clone

```powershell
# Both submodules resolve. A non-zero exit here means a gitlink is tracked
# without a .gitmodules entry -- setup.ps1 now detects and names that case.
git submodule status

# KilnFW embeds the SaftyFW bootloader's two slot images (App/drivers/CMakeLists.txt's
# EMBED_FILES, owner decision 2026-09-20) and refuses to configure without them, so a
# from-scratch clone builds SaftyFW's slot targets first -- same toolchain
# firmware/SaftyFW/test/check_00_saftyfw_target_build.ps1 drives, plus the objcopy step
# to get from .elf to the raw .bin EMBED_FILES needs:
cmake -G Ninja -B firmware/SaftyFW/build firmware/SaftyFW
cmake --build firmware/SaftyFW/build --target SaftyFW_slotA SaftyFW_slotB
arm-none-eabi-objcopy -O binary firmware/SaftyFW/build/SaftyFW_slotA.elf firmware/SaftyFW/build/SaftyFW_slotA.bin
arm-none-eabi-objcopy -O binary firmware/SaftyFW/build/SaftyFW_slotB.elf firmware/SaftyFW/build/SaftyFW_slotB.bin

# KilnFW builds
idf.py -C firmware/KilnFW build

# PcTools imports and its self-check runs
uv run --project tools/PcTools python tools/PcTools/selfcheck.py
```

`selfcheck.py` should report **all checks passed**. Treat ANY failure as a real
signal worth investigating.

This replaces a long-standing "16 failing checks are expected" caveat, corrected
2026-09-03. That caveat had stopped being true, and worse, it normalised a real
defect: the count was actually 10, and all ten came from two stale hardcoded
protocol-version literals — `selfcheck.py`'s own `== 5` assertion, and a
`_FW_VERSION_REPLY` fixture in `selfcheck_info.py` that packed `bytes([5, 0])`
under a comment claiming it matched `devices.UART_PROTOCOL_VERSION` (it did
not; the real value is 10). That single fixture fed three modules and produced
nine of the ten failures, making the tool useless as a health signal. Both
literals now derive from the real values — the check parses
`#define UART_PROTOCOL_VERSION` out of
`firmware/KilnFW/App/drivers/common/uart_task_ids.h` and asserts PC-vs-firmware
agreement rather than either against a constant, so it cannot go stale on the
next protocol bump. Fixed in `8536227`.

Then open the KiCad projects. `hardware/mainBoard` is the one worth opening
first: its library tables use `${KIPRJMOD}/../lib`, and a broken library path
shows up as missing footprints rather than as an error.

## If something is wrong

| Symptom | Likely cause |
|---|---|
| `idf.py` not found | ESP-IDF not installed, or the export script not run in this shell |
| IntelliSense dead in `KilnFW` | `KILNCTL_CLANGD` unset, or VS Code not restarted since setup |
| ESP-IDF extension sees no project | The folder was opened instead of `kilnCtl.code-workspace` |
| `import kilnctrl` fails | `uv sync --project tools/PcTools` has not run |
| KiCad reports missing footprints | Submodules not initialised, or a library table has picked up an absolute path again — `docs/REPO_LAYOUT.md` B1 |
| MCP servers missing | `.mcp.json` not generated; re-run the setup script |
| MCP tool call fails to connect | `kilnctrl` is a separate long-running HTTP server, not spawned by the client — see `docs/MCP_SERVERS.md` for how to start/stop/check it |
| `git submodule status` exits 128 | A gitlink is tracked with no `.gitmodules` entry. `git rm --cached <path>` it. This happened once already, when four Claude Code agent worktrees were swept in by a `git add -A` |

## Completion checklist

- [ ] Script run on a **second machine**, or at least a scratch clone, and the
      generated files checked. Nothing here is proven until it has run somewhere
      other than where it was written
- [ ] `pico-sdk` discovery added once `SaftyFW` has something to build
- [ ] A non-Windows path, if this ever needs to build anywhere else. Every
      toolchain path here is a Windows one today
