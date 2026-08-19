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
| `templates/UnitTestFw.settings.json.in` | `firmware/UnitTestFw/UnitTest/.vscode/settings.json` |
| `templates/UnitTestFw.c_cpp_properties.json.in` | `firmware/UnitTestFw/UnitTest/.vscode/c_cpp_properties.json` |
| `templates/UnitTestFwOuter.settings.json.in` | `firmware/UnitTestFw/.vscode/settings.json` |

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

## Verifying the clone

```powershell
# Both submodules resolve. A non-zero exit here means a gitlink is tracked
# without a .gitmodules entry -- setup.ps1 now detects and names that case.
git submodule status

# KilnFW builds
idf.py -C firmware/KilnFW build

# PcTools imports and its self-check runs
uv run --project tools/PcTools python tools/PcTools/selfcheck.py
```

`selfcheck.py` currently reports **16 failing checks**. They fail identically on
the pre-reorganisation source, so they are pre-existing and unrelated to setup —
see `docs/REPO_LAYOUT.md`. Treat a count other than 16 as the signal.

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
| `git submodule status` exits 128 | A gitlink is tracked with no `.gitmodules` entry. `git rm --cached <path>` it. This happened once already, when four Claude Code agent worktrees were swept in by a `git add -A` |

## Completion checklist

- [ ] Script run on a **second machine**, or at least a scratch clone, and the
      generated files checked. Nothing here is proven until it has run somewhere
      other than where it was written
- [ ] `pico-sdk` discovery added once `SaftyFW` has something to build
- [ ] A non-Windows path, if this ever needs to build anywhere else. Every
      toolchain path here is a Windows one today
