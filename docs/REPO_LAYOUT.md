# Repository Layout — hardware / software split

> **Status:** executed 2026-08-16, two items outstanding · **Last reviewed:** 2026-08-16
> **Keep this file current.** Tick the checklist as steps complete, and record
> what actually happened rather than what was planned — the two differed in
> several places and the differences are the useful part.

This was a proposal; the move has now been made. The tree is split into
`hardware/`, `firmware/`, `tools/` and `docs/`. What remains is recorded in the
completion checklist at the bottom.

**Still outstanding:** `mykicadMcp/` and `pdfMcp/` are at the repo root rather
than under `tools/`. Both had running MCP server processes holding the
directories open, and the process could not be stopped from the session doing
the move. Move them after a restart, with `git mv` for `mykicadMcp` because it
is a submodule.

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
|  \- PcTools/                    <- was KilnFW/pc_tools/ - GUI + MCP for BOTH processors
|- docs/                          <- system-level, spans both halves
|  |- SYSTEM_ARCHITECTURE.md      <- TODO
|  |- SAFETY_CASE.md              <- TODO
|  \- REPO_LAYOUT.md              <- this file
|- mykicadMcp/                    <- submodule, still to move under tools/
|- pdfMcp/                        <- local-only, still to move under tools/
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

### B2. Two submodules change path — PARTLY FIXED

`.gitmodules` needed `mainBoard/parts/TFT35-SPI` →
`hardware/mainBoard/parts/TFT35-SPI`, which `git mv` did automatically, and
`mykicadMcp` → `tools/mykicadMcp`, which is **still outstanding** because the
directory could not be moved.

```powershell
git mv mykicadMcp tools/mykicadMcp
# edit .gitmodules paths
git submodule sync
git submodule update --init --recursive
```

Doing this with a plain directory move instead of `git mv` leaves a broken
gitlink that looks fine until someone clones fresh.

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
- [ ] `mykicadMcp/` and `pdfMcp/` moved under `tools/` — **blocked**, see the top of this file

**Commit 3 — cleanup**
- [ ] B4: `hardware/mainBoard/kiln.net` regenerated or deleted
- [ ] Root `README.md` written
- [ ] `docs/SYSTEM_ARCHITECTURE.md` and `docs/SAFETY_CASE.md` stubbed

**Verification**
- [ ] All four KiCad projects open with no missing symbols or footprints —
      **not yet done, and this is the one that catches B1 silently**
- [ ] **Fresh `git clone` into a scratch dir, `mainBoard` opens there**
- [ ] `idf.py build` succeeds. The move invalidates `firmware/KilnFW/build/`,
      whose CMake cache holds the old absolute path — expect to `idf.py fullclean` first
- [x] `import kilnctrl` works and both derived paths resolve, after repointing
      the venv's editable-install `.pth`
- [x] `PcTools/selfcheck.py` runs. **16 checks fail — and failed identically on the
      pre-move source**, so they are pre-existing and unrelated to the move.
      They are not "all passing" as `PROJECT_STATUS.md` claims; that claim is stale
- [ ] `CommonFW` builds under xtensa, arm-none-eabi and MSVC — nothing to build yet
- [ ] MCP server resolves board paths; `.claude/settings*.json` checked
- [x] No tracked file still refers to a pre-move path, except `.claude/settings.json`
      and `.mcp.json`, which correctly point at the not-yet-moved `mykicadMcp/`
