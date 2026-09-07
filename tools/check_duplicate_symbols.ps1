# check_duplicate_symbols.ps1 -- catches a class of bug the host test suite
# is structurally blind to: two different object files defining the same
# externally-linked (non-static) symbol.
#
# WHY THIS EXISTS. 2026-08-31, three firmware files were split into smaller
# ones on the same day (uart_bridge.c, zones_http.c, safety_link.c). Each
# split had to widen some previously-`static` helper to external linkage so
# a sibling file could call it. One of those widened helpers collided:
# zones_http_handlers.c's json_escape() turned out to be byte-for-byte the
# same name as dashboard_json.c's own json_escape(), and the real ESP-IDF
# link failed with a duplicate-symbol error. Fixed in e34bdf2 by renaming to
# zones_json_escape().
#
# The host test suite (firmware/KilnFW/App/test/) unity-compiles the drivers
# it needs into ONE translation unit per test executable -- #include-ing the
# .c files together, not compiling and linking them as separate objects. Two
# same-named external symbols never exist as separately-linked objects in
# that world, so this entire bug class -- and any future one shaped like it
# -- is invisible to the host suite no matter how good its coverage is. It
# only exists as a real defect at the actual ESP-IDF link step, which nobody
# runs on every change.
#
# WHAT THIS CHECKS. After a real `idf.py build` (via the build_kilnfw tool
# or run manually), walk every *.c.obj this project's own components
# produced, list each object's DEFINED, EXTERNALLY-LINKED (non-static)
# symbols with the toolchain's own `nm`, and flag any symbol name that comes
# up defined in more than one object file. That is precisely the shape of
# collision the real linker enforces and the host suite cannot see.
#
# SCOPE: our own code only, not the whole ESP-IDF tree. Duplicate symbols
# inside vendored/managed IDF components are not this project's problem to
# fix and would be pure noise (IDF ships plenty of legitimate same-named
# statics and per-target build variants). This check inspects only the
# object files under:
#   esp-idf/App/CMakeFiles/__idf_App.dir/          (firmware/KilnFW/App/*.c)
#   esp-idf/drivers/CMakeFiles/__idf_drivers.dir/   (firmware/KilnFW/App/drivers/**/*.c)
#   esp-idf/kilnlink/CMakeFiles/__idf_kilnlink.dir/ (firmware/CommonFW, our shared link code)
#   esp-idf/hwabstraction_esp/CMakeFiles/__idf_hwabstraction_esp.dir/
#                                                    (firmware/hwAbstraction/esp via the thin
#                                                     wrapper component at firmware/hwAbstraction/
#                                                     idf/hwabstraction_esp/CMakeLists.txt -- HAL
#                                                     Phase 1a's new idf_component_register --
#                                                     esp_spi_owner.c/owner_slot_pool.c/
#                                                     i2c_owner.c/uart_owner.c/uart_protocol.c,
#                                                     added when this component started
#                                                     producing its own linked objects; renamed
#                                                     from the bare "esp" component name to avoid
#                                                     colliding in ESP-IDF's flat component
#                                                     namespace)
# which are the four components this repository actually authors and edits.
#
# SYMBOL FILTERING, why type letters and not names. `nm --defined-only`'s
# second column is the symbol type: uppercase means external/global linkage,
# lowercase means the symbol is local to its object file (`static` in C, or
# a compiler-generated local). Only uppercase types can collide at link time
# at all -- two lowercase (static) symbols with the same name in different
# objects are completely legal and extremely common (every driver file has
# its own `static void handle_get(...)`), so they are excluded by
# construction, not by a name-based guess. Within the uppercase set this
# check additionally excludes:
#   - 'W'/'V' (weak/weak-object): a weak symbol is *defined* to tolerate
#     being seen in more than one object; the linker picks one and that is
#     not a defect. IDF's own headers emit these for default/overridable
#     hooks. As of this writing this project's own three components emit
#     none, but excluding the type is the principled rule, not "there are
#     none today so it doesn't matter".
#   - 'C' (common): old-style tentative definitions (`int x;` at file scope
#     with no initializer, seen more than once) legitimately merge at link
#     time; that is what COMMON exists for, not a collision.
# What remains -- B/D/R/T (global bss/data/rodata/text) -- are exactly the
# symbol kinds a real link fails on if defined twice, which is this check's
# whole reason to exist.
#
# ALLOWLIST: none. A clean baseline scan of all three components' current
# object files (155 objects) found ZERO uppercase-type, non-weak,
# non-common name collisions -- see the commit that added this script for
# that baseline capture. An empty allowlist is the honest result, not a
# missing one: this repo's rule (check_no_duplicate_crc.ps1's header) is
# that an allowlist entry needs an individually justified reason, and
# "nothing collides yet" is not a reason to pre-populate one. If a future
# split introduces a *legitimate* same-named external symbol (extremely
# unlikely for this project's own code, which has no reason to reuse a name
# across files on purpose), add it here with the same discipline as every
# other allowlist in this repo: named, dated, justified, and removed the
# moment it stops applying.
#
# NO-BUILD BEHAVIOR: this check SKIPS (exit 3, run_all_checks.ps1's reserved
# SKIP status, clearly labeled -- see that script's header for the contract)
# rather than failing when firmware/KilnFW/build/ or its object files don't
# exist yet. Reasoning: build_kilnfw is a separate, expensive step (a full
# ESP-IDF build) that this fast guard should not silently force on every run
# of run_all_checks.ps1 -- every existing check_*.ps1 here inspects source
# trees, not build output, and turning this one into a hard failure on a
# clean checkout would make run_all_checks.ps1 red on every fresh clone
# until someone builds firmware, for a check that has nothing to say yet.
# The skip message says exactly what to run to make it meaningful. It used
# to exit 0 -- indistinguishable from a real pass in the aggregate -- until
# docs/audits/check_independence_2026-09-07.md flagged that as the same
# "reports green with zero coverage" shape this repo has been burned by
# before; exit 3 makes run_all_checks.ps1 report it as SKIPPED, not PASS.
#
# Usage: powershell -File tools\check_duplicate_symbols.ps1
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "firmware\KilnFW\build"

$componentDirs = @(
    "esp-idf\App\CMakeFiles\__idf_App.dir",
    "esp-idf\drivers\CMakeFiles\__idf_drivers.dir",
    "esp-idf\kilnlink\CMakeFiles\__idf_kilnlink.dir",
    "esp-idf\hwabstraction_esp\CMakeFiles\__idf_hwabstraction_esp.dir"
)

if (-not (Test-Path $buildDir)) {
    Write-Host "SKIP: no firmware/KilnFW/build/ found -- run the build_kilnfw tool (or 'idf.py build' in firmware/KilnFW) first." -ForegroundColor Yellow
    Write-Host "      This check inspects real build output; it has nothing to check on a clean checkout."
    exit 3
}

# Each component's .c.obj tree mirrors its source tree's relative layout
# 1:1 (verified: drivers.dir/owners/uart_protocol.c.obj <-> App/drivers/
# owners/uart_protocol.c). Used below to drop STALE objects -- .c.obj files
# left behind by a previous build whose source .c no longer exists at that
# path, which is exactly what a file move without an intervening clean
# build leaves lying around. HAL Phase 1a moved esp_spi_owner.c/
# owner_slot_pool.c/i2c_owner.c/uart_owner.c/uart_protocol.c OUT of
# App/drivers/owners/ entirely (afbaa6e), but firmware/KilnFW/build/ is
# generated, gitignored, per-machine output that a source-only move commit
# cannot touch -- so a build directory from before that move still has
# owners/*.c.obj for those five files sitting right next to the NEW esp/
# component's objects for the very same symbols, reading as a duplicate-
# symbol failure that has never existed in any single real link (nobody
# links stale + fresh objects together; a real `idf.py build` recompiles
# drivers/ and simply stops producing those five objects there). Comparing
# each .c.obj's source path against the real tree, not just re-running nm
# on whatever files happen to be sitting in build/, is what tells stale
# build output apart from an actual same-symbol-two-live-objects collision.
$componentSourceRoots = @{
    "esp-idf\App\CMakeFiles\__idf_App.dir"          = "firmware\KilnFW\App"
    "esp-idf\drivers\CMakeFiles\__idf_drivers.dir"   = "firmware\KilnFW\App\drivers"
    "esp-idf\kilnlink\CMakeFiles\__idf_kilnlink.dir" = "firmware\CommonFW"
    "esp-idf\hwabstraction_esp\CMakeFiles\__idf_hwabstraction_esp.dir" = "firmware\hwAbstraction\esp"
}

# Matched by BASENAME against everything real under the component's source
# root, not by reconstructing the object's exact relative path -- kilnlink's
# component directory encodes its (out-of-component-tree) absolute source
# paths into its .obj directory names (CMake's usual scheme for a source
# file living outside the component dir), so an exact relative-path
# rebuild breaks for it. A basename match is the same "agnostic to exactly
# where under the tree it lives" contract this repo already uses elsewhere
# (resolve_driver_file / Resolve-DriverFile) and is sufficient here: this
# loop only needs to tell "some real .c by this name still exists in this
# component's source tree" from "nothing does any more, this is leftover
# build output" -- not to prove it is the same file byte-for-byte.
$sourceBasenamesByRoot = @{}
foreach ($sourceRoot in ($componentSourceRoots.Values | Select-Object -Unique)) {
    $sourceFull = Join-Path $repoRoot $sourceRoot
    $names = @{}
    if (Test-Path $sourceFull) {
        foreach ($f in (Get-ChildItem -Path $sourceFull -Recurse -File -Filter "*.c" -ErrorAction SilentlyContinue)) {
            $names[$f.Name] = $true
        }
    }
    $sourceBasenamesByRoot[$sourceRoot] = $names
}

$objFiles = @()
$staleObjFiles = @()
foreach ($c in $componentDirs) {
    $full = Join-Path $buildDir $c
    if (Test-Path $full) {
        $candidates = @(Get-ChildItem -Path $full -Recurse -File -Filter "*.c.obj" -ErrorAction SilentlyContinue)
        $sourceRoot = $componentSourceRoots[$c]
        $knownNames = $sourceBasenamesByRoot[$sourceRoot]
        foreach ($obj in $candidates) {
            $expectedBaseName = $obj.Name -replace '\.obj$', ''
            if ($sourceRoot -and $knownNames -and -not $knownNames.ContainsKey($expectedBaseName)) {
                $staleObjFiles += [pscustomobject]@{ Obj = $obj; ExpectedBaseName = $expectedBaseName; SourceRoot = $sourceRoot }
            } else {
                $objFiles += $obj
            }
        }
    }
}

if ($staleObjFiles.Count -gt 0) {
    Write-Host "NOTE: ignoring $($staleObjFiles.Count) stale .c.obj file(s) whose source no longer exists anywhere under its component's source tree (leftover from a build directory predating a file move -- re-run build_kilnfw to clean these up):" -ForegroundColor Yellow
    foreach ($s in $staleObjFiles) {
        Write-Host "        $($s.Obj.FullName.Substring($buildDir.Length + 1)) -- no $($s.ExpectedBaseName) found under $($s.SourceRoot)"
    }
}

if ($objFiles.Count -eq 0) {
    Write-Host "SKIP: firmware/KilnFW/build/ exists but none of this project's own component object directories were found:" -ForegroundColor Yellow
    foreach ($c in $componentDirs) { Write-Host "        $c" }
    Write-Host "      Run the build_kilnfw tool to produce them, then re-run this check."
    exit 3
}

# Locate the toolchain's nm rather than assuming a fixed path -- the ESP-IDF
# tools install (under the user's .espressif directory) is versioned and its
# exact folder name changes across toolchain updates.
$nmCandidates = Get-ChildItem -Path "$env:USERPROFILE\.espressif\tools\xtensa-esp-elf" -Recurse -File -Filter "xtensa-esp32s3-elf-nm.exe" -ErrorAction SilentlyContinue
if (-not $nmCandidates -or $nmCandidates.Count -eq 0) {
    Write-Host "SKIP: could not locate xtensa-esp32s3-elf-nm.exe under $env:USERPROFILE\.espressif\tools\xtensa-esp-elf -- is the ESP-IDF toolchain installed?" -ForegroundColor Yellow
    exit 3
}
$nm = ($nmCandidates | Sort-Object FullName -Descending | Select-Object -First 1).FullName

# Allowlist of individually justified duplicate external symbols. See
# header. Empty today -- a real allowlist entry, if one is ever needed,
# looks like: @{ Symbol = 'foo'; Reason = '...' }
$allowlist = @()
$allowlistNames = $allowlist | ForEach-Object { $_.Symbol }

# symbol name -> list of object file relative paths that define it
$definers = @{}

foreach ($obj in $objFiles) {
    $out = & $nm "--defined-only" $obj.FullName 2>$null
    $rel = $obj.FullName.Substring($buildDir.Length + 1) -replace '\\', '/'
    foreach ($line in $out) {
        # nm --defined-only output: "<addr> <type> <name>"
        $parts = $line -split '\s+'
        if ($parts.Count -lt 3) { continue }
        $type = $parts[1]
        $name = $parts[2]

        # Only uppercase, externally-linked types can collide at link time.
        # Exclude weak (W/V) and common (C) -- see header.
        #
        # NOTE: PowerShell's -match/-eq are CASE-INSENSITIVE by default, so a
        # naive '^[A-Z]$'/-eq here would also match nm's lowercase (static)
        # types -- 't' would pass a '^[A-Z]$' -match check exactly like 'T'
        # does. Every comparison below MUST use the case-sensitive operators
        # (-cmatch/-ceq), or this filter silently stops filtering anything.
        if ($type -cnotmatch '^[A-Z]$') { continue }
        if ($type -ceq 'W' -or $type -ceq 'V' -or $type -ceq 'C') { continue }

        if (-not $definers.ContainsKey($name)) {
            $definers[$name] = @()
        }
        $definers[$name] += $rel
    }
}

$violations = @{}
foreach ($name in $definers.Keys) {
    $files = $definers[$name] | Select-Object -Unique
    if ($files.Count -gt 1 -and ($allowlistNames -cnotcontains $name)) {
        $violations[$name] = $files
    }
}

if ($violations.Count -gt 0) {
    Write-Host "DUPLICATE SYMBOL CHECK FAILED:" -ForegroundColor Red
    Write-Host "  The following externally-linked symbol(s) are defined in more than one" -ForegroundColor Red
    Write-Host "  object file among this project's own components. A real ESP-IDF link will" -ForegroundColor Red
    Write-Host "  fail (or silently pick one definition) on this -- rename one of them." -ForegroundColor Red
    foreach ($name in ($violations.Keys | Sort-Object)) {
        Write-Host ""
        Write-Host "  $name" -ForegroundColor Red
        foreach ($f in $violations[$name]) {
            Write-Host "      $f"
        }
    }
    Write-Host ""
    throw "$($violations.Count) duplicate externally-linked symbol(s) found -- see tools\check_duplicate_symbols.ps1's header"
}

Write-Host "Duplicate symbol check passed: $($objFiles.Count) object file(s) across App/drivers/kilnlink/esp, no externally-linked symbol defined more than once."
if ($allowlist.Count -gt 0) {
    Write-Host "  ($($allowlist.Count) allowlisted duplicate(s) -- see script header)"
}

exit 0
