# Repository Layout — proposed hardware / software split

> **Status:** proposed, not executed · **Last reviewed:** 2026-08-16
> **Keep this file current.** If the tree moves, this file moves with it and
> stops being a proposal. Tick the checklist as steps complete.

Planning doc. **Nothing here has been moved.** This proposes a reorganisation
of the `kilnCtl` tree into hardware and software halves, lists the things that
will actually break when it happens (there are several, and two are silent),
and sequences the move so `git log --follow` survives it.

## Why bother

The root currently mixes five unrelated kinds of thing at the same level:

```
mainBoard/  ThermocoupleBoard/  SaftyThermocoupleBoard/  UnitTest/   ← KiCad projects
parts/  datasheets/  ltspice/                                       ← hardware support
KilnFW/  SaftyFW/  CommonFW/                                       ← firmware
KilnFW/pc_tools/                                                    ← PC tooling, misfiled
mykicadMcp/  pdfMcp/                                                ← dev tooling
buy_list_aggregate.md  master_buy_list.md  ALTERNATE_MOUSER_PARTS.md
MOUSER_LINK_REFACTOR.md  buy_list_mouser.csv                        ← sourcing
```

Three concrete costs, not just tidiness:

- **`UnitTest/` holds both a KiCad project and firmware** (`UnitTest/UnitTestFw/`),
  so it is filed under neither.
- **`parts/` is shared by every board** but lives beside them rather than above
  them, which is why two library tables ended up pointing at it with **absolute
  paths** (see blocker B1).
- With `SaftyFW/` and `CommonFW/` added there are now three firmware projects,
  two of which must agree on a wire protocol, and no obvious home for the
  cross-cutting documents that describe the *system* rather than either half.
- **`KilnFW/pc_tools/` is filed under one firmware but serves both.** It already
  contains `safety.py`, a page and MCP tools for a processor whose firmware
  lives in a different directory. See [`PcTools/README.md`](PcTools/README.md).

## Proposed layout

```
kilnCtl/
├─ hardware/
│  ├─ mainBoard/                  ← unchanged contents
│  ├─ ThermocoupleBoard/
│  ├─ SaftyThermocoupleBoard/
│  ├─ UnitTestFixture/            ← the .kicad_* files from UnitTest/
│  ├─ lib/                        ← was parts/   (shared symbols, footprints, 3D)
│  ├─ datasheets/
│  ├─ simulation/                 ← was ltspice/
│  └─ sourcing/                   ← buy lists, Mouser CSV/MD
├─ firmware/
│  ├─ CommonFW/                   ← shared link contract + codecs, linked by both
│  ├─ KilnFW/                     ← ESP32-S3 main controller
│  ├─ SaftyFW/                    ← RP2040 safety processor
│  └─ UnitTestFw/                 ← from UnitTest/UnitTestFw/
├─ tools/
│  ├─ PcTools/                    ← was KilnFW/pc_tools/ — GUI + MCP for BOTH processors
│  ├─ mykicadMcp/                 ← submodule
│  └─ pdfMcp/                     ← local-only, .venv is gitignored
├─ docs/                          ← NEW: system-level, spans both halves
│  ├─ SYSTEM_ARCHITECTURE.md
│  ├─ SAFETY_CASE.md
│  └─ REPO_LAYOUT.md              ← this file moves here
├─ CLAUDE.md
└─ README.md                      ← NEW: currently there is no root README
```

### The `docs/` directory is the part that earns its keep

Right now the isolated-link contract is described in **`KilnFW/docs/SAFETY_LINK.md`**
— inside one of the two firmwares that implement it. That is exactly how it
came to describe the wire direction backwards without anyone noticing for
months: it reads as an ESP implementation note rather than as a contract with a
second party.

`CommonFW/` is the first half of the answer: the link contract now lives with
the code that implements it once, owned by neither firmware. A system-level
`docs/` is the other half — the two-processor safety case and the
hardware/firmware interface belong there, read before touching either side.

---

## Blockers — fix these as part of the move, not after

### B1. Absolute paths in two KiCad library tables · **silent breakage**

```
mainBoard/fp-lib-table:4
  (lib (name "parts") (uri "C:/Users/budar/OneDrive/Desktop/kilnCtl/parts") …)

mainBoard/sym-lib-table:4
  (lib (name "MAXM17572AMC+T") (uri "C:/Users/budar/OneDrive/Desktop/kilnCtl/parts/MAXM17572AMC+T.kicad_sym") …)
```

These break the moment `parts/` becomes `hardware/lib/`, and they are already
broken for anyone who is not this user on this machine.

**Fix: a KiCad path variable, not a relative path.** Define `KILNCTL_LIB` in
*Preferences → Configure Paths* and use `${KILNCTL_LIB}/…`. A relative
`${KIPRJMOD}/../lib` would also work but re-breaks if a board folder ever moves
a level; the variable survives any layout.

`mainBoard/fp-lib-table`'s other entry already uses `${KIPRJMOD}` correctly and
needs no change.

### B2. Two submodules change path

`.gitmodules` has `mykicadMcp` → `tools/mykicadMcp`, and
`mainBoard/parts/TFT35-SPI` → `hardware/mainBoard/parts/TFT35-SPI`.

```powershell
git mv mykicadMcp tools/mykicadMcp
# edit .gitmodules paths
git submodule sync
git submodule update --init --recursive
```

Doing this with a plain directory move instead of `git mv` leaves a broken
gitlink that looks fine until someone clones fresh.

### B3. `CLAUDE.md` hard-codes ~30 paths

Every `mainBoard/…`, `mykicadMcp/…` and `parts/…` reference needs updating,
including the PowerShell examples. Do it in the follow-up commit (see
sequencing), not the move commit.

### B4. `mainBoard/kiln.net` is stale and actively misleading

Dated `2026-07-19`, its `(source)` still points at the pre-move
`kilnCtl\kiln.kicad_sch`, and it disagrees with the current schematic in at
least three places — wrong op-amp part and values in the current-sense stage,
the safety MAX31856 on the wrong board, and two Pico GPIOs swapped.

**This is not a tidiness item.** It is the most probable origin of the reversed
optocoupler directions in `KilnFW/docs/SAFETY_LINK.md`, which would have cost
real bench time. Regenerate it from the current schematic or delete it; do not
leave both a stale netlist and a correct schematic in the tree.
See `SaftyFW/docs/HARDWARE.md` §10.

### B5. Working tree must be clean first

`git status` currently shows four modified `.claude/worktrees/agent-*` entries
and an untracked `UnitTest/UnitTestFw/UnitTest/nul`.

That `nul` file is a Windows artifact — something redirected to `/dev/null` in a
shell that does not have one, creating a file named `nul`. Delete it and add
`nul` to `.gitignore`; on Windows it is awkward to remove once it exists
(`Remove-Item -LiteralPath .\nul`).

**Do not start a bulk `git mv` from a dirty tree.** Commit or stash the
worktree changes first.

### B6. Do not reorganise while `SaftyFW` is being written

The move touches every path in the tree. Doing it mid-implementation guarantees
a painful merge. **Either do it before phase 1 of `SaftyFW/TODO.md`, or after
phase 8.** Before is better — the paths in the new documents are then correct
from the start.

---

## Sequencing

Three commits, in this order. The split is what keeps history readable.

**Commit 1 — pure moves, zero content edits.**

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

**Commit 2 — path fixes.** B1, B2, B3, plus `.gitignore` (the `/mainBoard/…`
and `/pdfMcp/…` rules all need re-rooting) and the `docs/` cross-links in
`SaftyFW`'s documents.

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
- [ ] `grep -rn "kilnCtl/\(mainBoard\|parts\|mykicadMcp\)" --include=*.md --include=*.json .`
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
- [ ] Working tree clean; worktree changes committed or stashed (B5)
- [ ] `UnitTest/UnitTestFw/UnitTest/nul` deleted and `nul` added to `.gitignore`
- [ ] Timing agreed: **before** `SaftyFW/TODO.md` phase 1, or after phase 8 (B6)

**Commit 1 — pure moves**
- [ ] `hardware/`, `firmware/`, `tools/`, `docs/` created
- [ ] All moves done with `git mv`, **no content edits in this commit**
- [ ] `git log --follow` verified across the boundary on a sample file

**Commit 2 — path fixes**
- [ ] B1: `KILNCTL_LIB` path variable defined; both library tables de-absolutised
- [ ] B2: `.gitmodules` updated, `git submodule sync` run, both submodules resolve
- [ ] B3: `CLAUDE.md` paths updated (including the `pc_tools` references)
- [ ] `PcTools` moved out of `KilnFW/`; `.gitignore` re-rooted for its `logs/` and `.venv/`
- [ ] `.gitignore` rules re-rooted
- [ ] Cross-links in `SaftyFW/` and `CommonFW/` docs updated

**Commit 3 — cleanup**
- [ ] B4: `mainBoard/kiln.net` regenerated or deleted
- [ ] Root `README.md` written
- [ ] `docs/SYSTEM_ARCHITECTURE.md` and `docs/SAFETY_CASE.md` stubbed

**Verification**
- [ ] All four KiCad projects open with no missing symbols or footprints
- [ ] **Fresh `git clone` into a scratch dir, `mainBoard` opens there** — the only test that catches B1/B2
- [ ] `idf.py build` succeeds
- [ ] `CommonFW` builds under xtensa, arm-none-eabi and MSVC
- [ ] MCP server resolves board paths; `.claude/settings*.json` checked
- [ ] `grep -rn "kilnCtl/\(mainBoard\|parts\|mykicadMcp\)" --include=*.md --include=*.json .` returns nothing
