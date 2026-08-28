# Repository Layout — hardware / software split

> **Status:** executed 2026-08-16, no items outstanding · **Last reviewed:** 2026-08-28
> **Keep this file current.** Tick the checklist as steps complete, and record
> what actually happened rather than what was planned — the two differed in
> several places and the differences are the useful part.

This was a proposal; the move has now been made. The tree is split into
`hardware/`, `firmware/`, `tools/` and `docs/`. What remains is recorded in the
completion checklist at the bottom.

**Both `mykicadMcp/` and `pdfMcp/` moved under `tools/`, 2026-08-28.**

`pdfMcp/` first: its own running `pdf-mcp.exe` process meant a plain rename
failed with "Permission denied", so `tools/pdfMcp/` was made as a COPY, not a
move, while the old process kept running against the original `pdfMcp/` at
the repo root. `.mcp.json`'s `pdf-mcp` entry (and its source template,
`templates/mcp.json.in`) now point at `tools/pdfMcp/.venv/Scripts/pdf-mcp.exe`;
a fresh session picks that up, the old process exits with the session that
had it open, and the stale root-level `pdfMcp/` (untracked, gitignored, safe
to delete once nothing holds it open) can be removed then.

`mykicadMcp/` as its own dedicated pass, per the plan this section used to
describe: the `kicad` server was stopped first (`mcp_servers.ps1 stop -Server
kicad`), then a `git mv` of the whole directory hit the exact same
"Permission denied" the original hardware/firmware split ran into on
`KilnFW/`/`UnitTest/UnitTestFw/` (see "What the move taught" below) — worked
around the same documented way, pre-creating `tools/mykicadMcp/` and moving
children individually. One thing didn't move: a stale, abandoned
`.claude/worktrees/` directory inside the old `mykicadMcp/`, left over from
unrelated agent sessions weeks earlier — harmless debris, not part of the
submodule's tracked content, left behind at the old path rather than forced.
A first attempt at re-registering the moved directory with git silently added
it as 43 individual file blobs instead of one `160000` gitlink entry — caught
by checking `git ls-files -s`, not assumed to have worked because `git add`
didn't complain. Root cause: the submodule's own `.git` file still pointed at
`../.git/modules/mykicadMcp` (correct one level down from the repo root, now
wrong two levels down), and its `core.worktree` in
`.git/modules/mykicadMcp/config` still pointed at the old absolute path —
both fixed before re-adding. `.gitmodules`, `mcp_servers.ps1`, and 8 of the 9
`.claude/settings.json` allowlist entries were updated one at a time (the
9th, a bare `../mykicadMcp/...` entry, had no recoverable original working
directory to translate against and was left to simply stop matching, rather
than guess). Verified: `git submodule status` resolves all three submodules,
the `kicad` server restarted clean from the new path and answered a real
`kicad_call`, and the submodule's own 110-test suite passed unchanged.

### What the move taught that the plan did not anticipate

- **Directory renames failed with "Permission denied"** on `KilnFW/`,
  `KilnFW/pc_tools/` and `UnitTest/UnitTestFw/` even with nothing obviously
  holding them. Moving the *children* into a pre-created destination worked
  every time. Git records the result identically, so this is a workaround, not
  a compromise.
- **`.gitignore` had to be re-rooted in the move commit, not the one after.**
  Its rules were anchored (`/mainBoard/…`, `/ThermocoupleBoard/…`), so they
  stopped matching the instant the directories moved and every KiCad backup
  archive appeared as untracked. Committing the move first would have added
  them. The replacements are unanchored.
- **Twenty-three files under `mainBoard/` are stored with CRLF** while the rest
  of the repo is LF. Re-adding them under `core.autocrlf=true` rewrote every
  line of five schematics. Their original blobs were restored with
  `git update-index` so the commit is a rename and nothing more. This latent
  inconsistency is still there and will resurface on the next edit to those
  files.
- **3D model paths inside 17 `.kicad_mod` files were absolute** and pointed at
  the old locations — not in the blocker list, and a real regression caused by
  the move. Repointed at the new absolute paths, which restores exactly the
  prior behaviour. Making them portable still needs a KiCad path variable.
- **The `PcTools` venv's editable install broke.** `_editable_impl_kilnctrl.pth`
  held the old absolute source path, so `import kilnctrl` failed. Repointed by
  hand; a `uv sync` is the clean fix.

## Why bother

Before the move, the root mixed five unrelated kinds of thing at the same level:

```
mainBoard/  ThermocoupleBoard/  SaftyThermocoupleBoard/  UnitTest/   <- KiCad projects
parts/  datasheets/  ltspice/                                        <- hardware support
KilnFW/  SaftyFW/  CommonFW/                                         <- firmware
KilnFW/pc_tools/                                                     <- PC tooling, misfiled
mykicadMcp/  pdfMcp/                                                 <- dev tooling
buy_list_aggregate.md  master_buy_list.md  ALTERNATE_MOUSER_PARTS.md
MOUSER_LINK_REFACTOR.md  buy_list_mouser.csv                         <- sourcing
```

Three concrete costs, not just tidiness:

- **`UnitTest/` held both a KiCad project and firmware** (`UnitTest/UnitTestFw/`),
  so it was filed under neither.
- **`parts/` is shared by every board** but lived beside them rather than above
  them, which is why two library tables ended up pointing at it with **absolute
  paths** (see blocker B1).
- With `SaftyFW/` and `CommonFW/` added there were three firmware projects, two
  of which must agree on a wire protocol, and no obvious home for the
  cross-cutting documents that describe the *system* rather than either half.
- **`KilnFW/pc_tools/` was filed under one firmware but serves both.** It
  already contained `safety.py`, a page and MCP tools for a processor whose
  firmware lives in a different directory. See
  [`../tools/PcTools/TODO.md`](../tools/PcTools/TODO.md).

## The layout

```
kilnCtl/
|- hardware/
|  |- mainBoard/                  <- unchanged contents
|  |- ThermocoupleBoard/
|  |- SaftyThermocoupleBoard/
|  |- UnitTestFixture/            <- the .kicad_* files from UnitTest/
|  |- lib/                        <- was parts/  (shared symbols, footprints, 3D)
|  |- datasheets/
|  |- simulation/                 <- was ltspice/
|  \- sourcing/                   <- buy lists, Mouser CSV/MD
|- firmware/
|  |- CommonFW/                   <- shared link contract + codecs, linked by both
|  |- KilnFW/                     <- ESP32-S3 main controller
|  |- SaftyFW/                    <- RP2040 safety processor
|  \- UnitTestFw/                 <- from UnitTest/UnitTestFw/
|- tools/
|  |- PcTools/                    <- was KilnFW/pc_tools/ - GUI + MCP for BOTH processors
|  |- mykicadMcp/                 <- submodule, moved 2026-08-28
|  \- pdfMcp/                     <- moved 2026-08-28; a copy, see the note above
|- docs/                          <- system-level, spans both halves
|  |- SYSTEM_ARCHITECTURE.md      <- TODO
|  |- SAFETY_CASE.md              <- TODO
|  \- REPO_LAYOUT.md              <- this file
|- CLAUDE.md
|- ROADMAP.md
\- README.md                      <- TODO: there is still no root README
```

### The `docs/` directory is the part that earns its keep

Right now the isolated-link contract is described in **`firmware/KilnFW/docs/SAFETY_LINK.md`**
— inside one of the two firmwares that implement it. That is exactly how it
came to describe the wire direction backwards without anyone noticing for
months: it reads as an ESP implementation note rather than as a contract with a
second party.

`firmware/CommonFW/` is the first half of the answer: the link contract now lives with
the code that implements it once, owned by neither firmware. A system-level
`docs/` is the other half — the two-processor safety case and the
hardware/firmware interface belong there, read before touching either side.

---

## Working from the repository root

**From 2026-08-16 the editor and Claude are opened at `kilnCtl/`, not at an
individual firmware folder.** This is now an assumption the tooling depends on,
and it changes what is left to do.

### What it makes correct

- The two `.vscode/settings.json` files were changed to
  `${workspaceFolder}/firmware/…` during the move. `${workspaceFolder}` is the
  *opened* folder, so those are right with the root open and wrong if someone
  opens `firmware/KilnFW` directly. That is now the supported way round.
- `.mcp.json` uses root-relative paths throughout, including
  `-C firmware/KilnFW` for the ESP-IDF server.
- `CLAUDE.md` at the root is loaded automatically, and its paths are now written
  relative to the root.

### Fixed with a multi-root workspace file (2026-08-16)

`kilnCtl.code-workspace` at the root. **Open that file, not the folder.**

The deciding argument was not tidiness. The ESP-IDF extension treats a workspace
folder as one project and looks for a `CMakeLists.txt` in it; with the repository
root open as a single folder there is no project at the top level for it to
attach to. A multi-root workspace gives it `firmware/KilnFW` as a folder in its
own right while the root stays open alongside.

This has a consequence that is easy to get backwards: **in a multi-root
workspace, `${workspaceFolder}` inside `firmware/KilnFW/.vscode/settings.json`
means `firmware/KilnFW`, not the repository root.** The clangd
`--compile-commands-dir` arguments were changed to `${workspaceFolder}/firmware/…`
during the move, which was right for a single root and wrong here; they are back
to `${workspaceFolder}/build`. Cross-folder references use the
`${workspaceFolder:Name}` form instead.

Settings that belong to the whole repository live **in the workspace file**, not
in a root `.vscode/settings.json`, because `/.vscode/` is gitignored and the
workspace file is not. That is also why the GUI, MCP-server and selfcheck tasks
moved there: they invoke `tools/PcTools`, which is not inside any firmware.

Two stale things surfaced while doing this:

- `firmware/KilnFW/.vscode/tasks.json` still launched
  `${workspaceFolder}\pc_tools\.venv\Scripts\kilnctrl-gui.exe` — broken by the
  move and missed in the path-fix commit. Those two tasks are now in the
  workspace file, pointing at `${workspaceFolder:PcTools}`.
- The existing root `.vscode/settings.json` (untracked, gitignored) has an
  action button running `${workspaceFolder}\python\start_kicad_mcp_http_server.ps1`.
  There is no `python/` directory and has not been for some time. It is
  superseded by the workspace file and can be deleted.

### What was breaking before that

**VS Code reads `<root>/.vscode/`, not the nested ones.** `firmware/KilnFW/.vscode/`
and `firmware/UnitTestFw/UnitTest/.vscode/` hold clangd arguments, launch
configurations and tasks that will simply not apply any more. They are not
broken files — they are ignored ones, which is worse, because they still look
maintained.

- [x] `kilnCtl.code-workspace` written and committed; `/.vscode/` stays ignored
- [x] clangd `--compile-commands-dir` back to `${workspaceFolder}/build` in both
      firmware settings files — correct for folder scope in a multi-root workspace
- [x] ESP-IDF extension gets `firmware/KilnFW` as a workspace folder, which is
      what it needs; the MCP server already had `-C firmware/KilnFW`
- [x] `idf.py -C firmware/KilnFW …` and `uv run --project tools/PcTools …`
      documented in both READMEs, with the reason
- [x] The two `pc_tools` tasks in `KilnFW/.vscode/tasks.json` fixed and moved
- [ ] Delete the stale root `.vscode/settings.json` action button (local,
      untracked — the workspace file supersedes it)

### What it does not change

Anything that derives paths from `__file__` or `${KIPRJMOD}` — the `PcTools`
modules and the KiCad library tables — is independent of the working directory
by construction. That was worth doing for its own sake and it pays off here.

The independence this bought paid off directly: `mykicadMcp/`'s own move
(2026-08-28) needed no `__file__`-derived path fixed anywhere in `PcTools` or
the KiCad library tables — only `.mcp.json` (root-relative) and
`mcp_servers.ps1` needed their path updated, exactly as `pdfMcp/`'s did.

---

## Blockers — fix these as part of the move, not after

### B1. Absolute paths in two KiCad library tables · **silent breakage** — FIXED

```
hardware/mainBoard/fp-lib-table:4
  (lib (name "parts") (uri "C:/Users/budar/OneDrive/Desktop/kilnCtl/parts") …)

hardware/mainBoard/sym-lib-table:4
  (lib (name "MAXM17572AMC+T") (uri "C:/Users/budar/OneDrive/Desktop/kilnCtl/parts/MAXM17572AMC+T.kicad_sym") …)
```

These break the moment `parts/` becomes `hardware/lib/`, and they are already
broken for anyone who is not this user on this machine.

**Fixed with `${KIPRJMOD}/../lib`, not the path variable the plan called for.**
The plan preferred a `KILNCTL_LIB` variable defined in *Preferences → Configure
Paths* because it survives any future layout. It also has to be created by hand
in the KiCad GUI, and until someone does, the board opens with its libraries
missing — trading a break that is already fixed for one that waits on a manual
step nobody has been told to take. The relative form works the moment the file
is saved and needs nothing configured.

The trade is real: `${KIPRJMOD}/../lib` re-breaks if a board folder ever moves a
level. If that happens, or if the libraries are ever shared with another
project, switch to the variable then and document the *Configure Paths* step
alongside it.

`hardware/mainBoard/fp-lib-table`'s other entry already uses `${KIPRJMOD}` correctly and
needs no change.

### B2. Two submodules change path — FIXED

`.gitmodules` needed `mainBoard/parts/TFT35-SPI` →
`hardware/mainBoard/parts/TFT35-SPI`, which `git mv` did automatically, and
`mykicadMcp` → `tools/mykicadMcp`, done 2026-08-28 once the directory itself
could finally be moved (see the note near the top of this file for how —
a plain `git mv` hit the same "Permission denied" as the directories below,
worked around the same way, plus a submodule-specific gitdir/`core.worktree`
fix the plan below didn't anticipate).

```powershell
git mv mykicadMcp tools/mykicadMcp
# edit .gitmodules paths
git submodule sync
git submodule update --init --recursive
```

Doing this with a plain directory move instead of `git mv` leaves a broken
gitlink that looks fine until someone clones fresh — which is exactly why the
actual 2026-08-28 move went through `git rm --cached` + `git add` once the
files were relocated by hand, not a bare filesystem move.

### B3. `CLAUDE.md` hard-codes ~30 paths — FIXED

Every `hardware/mainBoard/…`, `mykicadMcp/…` and `parts/…` reference needs updating,
including the PowerShell examples. Do it in the follow-up commit (see
sequencing), not the move commit.

### B4. `hardware/mainBoard/kiln.net` is stale and actively misleading — OUTSTANDING

Dated `2026-07-19`, its `(source)` still points at the pre-move
`kilnCtl\kiln.kicad_sch`, and it disagrees with the current schematic in at
least three places — wrong op-amp part and values in the current-sense stage,
the safety MAX31856 on the wrong board, and two Pico GPIOs swapped.

**This is not a tidiness item.** It is the most probable origin of the reversed
optocoupler directions in `firmware/KilnFW/docs/SAFETY_LINK.md`, which would have cost
real bench time. Regenerate it from the current schematic or delete it; do not
leave both a stale netlist and a correct schematic in the tree.
See `firmware/SaftyFW/docs/HARDWARE.md` §10.

### B5. Working tree must be clean first — DONE

`git status` currently shows four modified `.claude/worktrees/agent-*` entries
and an untracked `firmware/UnitTestFw/UnitTest/nul`.

That `nul` file is a Windows artifact — something redirected to `/dev/null` in a
shell that does not have one, creating a file named `nul`. Delete it and add
`nul` to `.gitignore`; on Windows it is awkward to remove once it exists
(`Remove-Item -LiteralPath .\nul`).

**Do not start a bulk `git mv` from a dirty tree.** Commit or stash the
worktree changes first.

### B6. Do not reorganise while `SaftyFW` is being written — SATISFIED

The move touches every path in the tree. Doing it mid-implementation guarantees
a painful merge. **Either do it before phase 1 of `firmware/SaftyFW/TODO.md`, or after
phase 8.** Before is better — the paths in the new documents are then correct
from the start.

---

## Sequencing

Three commits, in this order. The split is what keeps history readable.

**Commit 1 — pure moves.** Done: `a382380`. Every file a byte-identical
rename, plus the two unavoidable exceptions noted above (`.gitignore`,
`.gitmodules`).

```powershell
New-Item -ItemType Directory hardware, firmware, tools, docs

git mv mainBoard ThermocoupleBoard SaftyThermocoupleBoard hardware/
git mv parts hardware/lib
git mv datasheets hardware/datasheets
git mv ltspice hardware/simulation

git mv KilnFW SaftyFW CommonFW firmware/
git mv UnitTest/UnitTestFw firmware/UnitTestFw
New-Item -ItemType Directory hardware/UnitTestFixture
git mv UnitTest/UnitTestFixture.kicad_pro hardware/UnitTestFixture/
# …the remaining UnitTestFixture.* and UnitTestFixture-backups
git mv mykicadMcp tools/mykicadMcp
git mv pdfMcp tools/pdfMcp

New-Item -ItemType Directory hardware/sourcing
git mv buy_list_aggregate.md master_buy_list.md ALTERNATE_MOUSER_PARTS.md `
       MOUSER_LINK_REFACTOR.md buy_list_mouser.csv hardware/sourcing/
git mv KilnFW/pc_tools tools/PcTools
git mv REPO_LAYOUT.md docs/
```

`git mv` only, nothing else, so every file is a clean 100 %-similarity rename
and `git log --follow` keeps working across the boundary.

**Commit 2 — path fixes.** B1, B2, B3, the `docs/` cross-links, the `.mcp.json`
ESP-IDF project path, both `.vscode` `compile-commands-dir` settings, the two
`PcTools` modules that derived paths from `__file__`, and the `.kicad_mod` 3D
model paths. `.gitignore` moved into commit 1 for the reason given above.

**Commit 3 — cleanup.** B4 (`kiln.net`), B5 (`nul`), and the new root
`README.md`.

## After the move — verify, do not assume

- [ ] Open **all four** KiCad projects; confirm no missing footprints or symbols.
      B1 fails *silently* until a board is opened on a machine without the old
      absolute path.
- [ ] `git submodule status` — both submodules present and at the right commit.
- [ ] Fresh `git clone` into a scratch directory and open `mainBoard` there.
      This is the only test that actually catches B1 and B2.
- [ ] `idf.py build` in `firmware/KilnFW` still succeeds.
- [ ] `PcTools/selfcheck.py` still passes; `python -c "import kilnctrl"` works.
- [ ] `CommonFW` still builds for all three toolchains, and both firmwares still
      find it (relative `add_subdirectory` / component path).
- [ ] The MCP server still resolves board paths — check
      `.claude/settings*.json` and any MCP config for hard-coded roots.
- [ ] `grep -rn "kilnCtl/\(hardware\mainBoard\|parts\|mykicadMcp\)" --include=*.md --include=*.json .`
      returns nothing.

## Deliberately not proposed

- **Splitting into separate repositories.** The firmware and the boards change
  together and share a wire protocol; one repo with a clear internal split is
  the right granularity. Two repos would just relocate the coupling into
  version pinning.
- **Renaming `SaftyFW`/`SaftyThermocoupleBoard`/`SaftyProcessor`.** The
  misspelling is consistent across schematic net names, sheet names, KiCad
  files, and now firmware. Correcting it in the tree while the schematic still
  says `GND_Safty` and `saftyRelay` would create a mismatch that costs more
  than the typo does. Fix it in the schematic first, or not at all.
- **Renaming the `kilnctrl` Python package.** It is the *system's* name, not the
  main board's. Renaming would churn every import and both console scripts for
  no gain. Only its directory moves.


---

## Completion checklist

**Prerequisites**
- [x] Working tree clean apart from four pre-existing `.claude/worktrees/agent-*`
      gitlinks, which are unrelated to the move and were left alone (B5)
- [x] `UnitTest/UnitTestFw/UnitTest/nul` deleted and `nul` added to `.gitignore`
- [x] Timing: done **before** `firmware/SaftyFW/TODO.md` phase 1, which is what B6 asked for

**Commit 1 — pure moves** (`a382380`)
- [x] `hardware/`, `firmware/`, `tools/`, `docs/` created
- [x] All 380 files recorded as 100 %-similarity renames; original blobs preserved
      for the 23 CRLF-stored files rather than letting autocrlf rewrite them
- [x] `git log --follow` verified across the boundary on `hardware/mainBoard/kiln.kicad_sch`
- [x] `.gitignore` re-rooted here rather than in commit 2 — see the note at the top

**Commit 2 — path fixes**
- [x] B1: both mainBoard library tables de-absolutised with `${KIPRJMOD}/../lib`.
      The `KILNCTL_LIB` variable was **not** used; reasoning is in B1 above
- [x] B2: `.gitmodules` updated by `git mv`, `git submodule sync` run, both submodules resolve
- [x] B3: `CLAUDE.md` paths updated, both slash styles, plus a layout summary
- [x] `PcTools` moved out of `KilnFW/`; its own nested `.gitignore` travelled with it
      and still covers `logs/` and `.venv/`
- [x] Cross-links in `firmware/SaftyFW/`, `firmware/CommonFW/` and `tools/PcTools/`
      docs updated; every relative markdown link in the repo now resolves
- [x] `.mcp.json` ESP-IDF `-C` path; both `.vscode` `compile-commands-dir` settings
- [x] `mcp_server._kiln_fw_root()` and `pinout_reference.HARDWARE_MD_PATH`, which
      both derived paths from `__file__` and silently pointed at `tools/`
- [x] 3D model paths in 17 `.kicad_mod` files repointed
- [x] `pdfMcp/` moved under `tools/` — 2026-08-28, as a copy (its own running
      process blocked a rename the same way `mykicadMcp/`'s does); `.mcp.json`
      updated. Old root-level copy cleans up on the next session restart
- [x] `mykicadMcp/` moved under `tools/` — 2026-08-28, as its own dedicated
      pass. See the top of this file for full details

**Working from the repository root** (see the section above)
- [ ] `kilnCtl.code-workspace` written, or a root `.vscode/` chosen instead
- [ ] Nested `.vscode/` settings merged or re-pointed — they are currently ignored, not broken
- [ ] ESP-IDF extension told the project is `firmware/KilnFW`
- [ ] `idf.py -C …` / `uv run --project …` documented in both READMEs

**Commit 3 — cleanup**
- [ ] B4: `hardware/mainBoard/kiln.net` regenerated or deleted
- [ ] Root `README.md` written
- [ ] `docs/SYSTEM_ARCHITECTURE.md` and `docs/SAFETY_CASE.md` stubbed

**Verification**
- [x] **Every KiCad file audited against its pre-move blob (2026-08-16).** All
      87 are byte-identical except the 19 deliberately edited (2 library tables,
      17 footprint 3D model paths), and those differ by exactly one line each —
      38 changed lines total, every one a `uri` or a `model` path. All four
      projects have their `.kicad_pro`/`.kicad_pcb`/`.kicad_sch`/`.kicad_prl`;
      every hierarchical sheet reference, library-table URI, project-local
      footprint and 3D model path resolves to a file that exists
- [x] **`mainBoard`, `ThermocoupleBoard` and `SaftyThermocoupleBoard` opened in
      KiCad (2026-08-16) with no changes made and no missing libraries.** This is
      the check that catches B1, and `mainBoard` is the project B1 applied to —
      `${KIPRJMOD}/../lib` resolves. KiCad rewrote the three `.kicad_pro` files
      byte-identically (mtime only) and took one backup, which `.gitignore`
      caught. Nothing new needed ignoring
- [ ] `UnitTestFixture` opened. Not yet done, and the lowest-risk of the four —
      it has no project library tables of its own and relies on KiCad's globals
- [x] **Fresh `git clone` into a scratch dir, `mainBoard` library paths resolve
      (2026-08-19).** Tested clone into temp directory; fp-lib-table and
      sym-lib-table both use `${KIPRJMOD}/../lib` correctly, resolving to
      `hardware/lib` which exists with all expected files. This confirms B1's
      fix works for fresh clones on a different machine/user path
- [ ] `idf.py build` succeeds. The move invalidates `firmware/KilnFW/build/`,
      whose CMake cache holds the old absolute path — expect to `idf.py fullclean` first
- [x] `import kilnctrl` works and both derived paths resolve, after repointing
      the venv's editable-install `.pth`
- [x] `PcTools/selfcheck.py` runs. **16 checks fail — and failed identically on the
      pre-move source**, so they are pre-existing and unrelated to the move.
      They are not "all passing" as `PROJECT_STATUS.md` claims; that claim is stale
- [ ] `CommonFW` builds under xtensa, arm-none-eabi and MSVC — nothing to build yet
- [ ] MCP server resolves board paths; `.claude/settings*.json` checked
- [x] No tracked file still refers to a pre-move path. `.claude/settings.json`
      and `.mcp.json`/`templates/mcp.json.in` were the two places that used to
      correctly point at `mykicadMcp/` at the repo root; both now point at
      `tools/mykicadMcp/` since the 2026-08-28 move
