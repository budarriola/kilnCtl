# check_c_files_in_cmakelists.ps1 -- every source file under
# firmware/KilnFW/App/**.c and firmware/SaftyFW/src/**.c must be named in the
# CMakeLists.txt that actually builds the target, or explicitly allowlisted
# with a stated reason.
#
# WHY THIS EXISTS. Real instance, 2026-08-28: src/tasks/tick_timing.c was
# added to firmware/SaftyFW/test/build_host_tests.ps1 (the host-test file
# list) but NOT to firmware/SaftyFW/CMakeLists.txt's SAFTYFW_APP_SOURCES (the
# real target's file list). The host suite compiled and linked it happily --
# host_tests.ps1 globs/lists its own sources independently of the firmware
# CMakeLists -- so every host test passed while the real TARGET LINK failed
# with an undefined reference the moment anything called into it. The file
# read correctly, the tests that covered it read correctly, and the gap was
# invisible to anyone not doing a real device build. A second near-miss of
# the exact same shape happened the same week. Host tests structurally cannot
# see this class: their own build script is a second, independently
# maintained list, so a file present in one and absent from the other is
# exactly the kind of drift neither list can catch by itself.
#
# WHAT THIS CHECKS. For each of the two firmwares below, every real *.c file
# under its source tree (excluding test/ and build/ output) must appear,
# quoted or bare, in the CMakeLists.txt file(s) that register that firmware's
# actual build target -- NOT the host-test build script, which is a
# different, independently-maintained list this bug already proved is not a
# substitute. A file that is neither found in the target's CMakeLists nor on
# the allowlist below is a failure.
#
#   - firmware/KilnFW/App/*.c              -> firmware/KilnFW/App/CMakeLists.txt
#   - firmware/KilnFW/App/drivers/**/*.c   -> firmware/KilnFW/App/drivers/CMakeLists.txt
#   - firmware/SaftyFW/src/**/*.c          -> firmware/SaftyFW/CMakeLists.txt
#     (SAFTYFW_APP_SOURCES; bootloader/ is a separate executable with its own
#     CMakeLists.txt and is out of scope here, same as this check's own
#     charter: firmware/SaftyFW/src/, not the whole firmware tree)
#
# CMakeLists.txt source lists in this repo appear in two different, both
# legal, CMake syntaxes: double-quoted (App/CMakeLists.txt's
# `SRCS "main.c" "monitor_task.c"`, App/drivers/CMakeLists.txt's
# `idf_component_register(SRCS "MAX31856.c" ... "espInterfaces/uart_owner.c"`)
# and bare, one-per-line (SaftyFW's `set(SAFTYFW_APP_SOURCES` block: plain
# `src/tasks/relay_owner.c` with no quotes at all, valid CMake list syntax).
# This script matches both shapes rather than assuming one, because assuming
# quoting is exactly the kind of narrow pattern that goes blind the moment a
# list is reformatted -- see check_uri_handler_cap.ps1's header for the same
# lesson learned about pattern narrowness.
#
# Comments are stripped first (line comments only -- CMake has no block
# comment syntax) so a file merely MENTIONED in a comment (this project's own
# CMakeLists comments name files constantly) is not counted as "in the
# build".
#
# EXPLICIT OPT-OUT. A file that genuinely should not compile into the real
# target -- a host-only test double, a frozen reference copy kept purely for
# byte-identity proof, etc. -- goes on the allowlist below WITH a reason,
# same pattern as check_no_duplicate_crc.ps1's one allowlisted duplicate.
# There is nothing on it today; it exists so a legitimate future exclusion
# has somewhere to go that isn't "quietly don't run this check on that file".
#
# Usage: powershell -File tools\check_c_files_in_cmakelists.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot

# Comment stripper -- CMake only has line comments (#), so this is simpler
# than the C/C++ block-comment strippers duplicated elsewhere in this repo's
# checks, but kept as its own function for the same reason those are: no
# shared PowerShell module mechanism exists here to import it from.
function Get-CMakeCodeOnlyLines {
    param([string]$Path)
    $result = @()
    foreach ($line in (Get-Content -Path $Path)) {
        $idx = $line.IndexOf('#')
        if ($idx -ge 0) {
            $result += $line.Substring(0, $idx)
        } else {
            $result += $line
        }
    }
    return $result
}

# Pulls every *.c path this CMakeLists.txt names, quoted or bare, forward
# slashes only, no leading/trailing whitespace. Both syntaxes appear in this
# repo (see header above) so both are matched in one pass.
function Get-CMakeCSourceRefs {
    param([string]$Path)
    $codeLines = Get-CMakeCodeOnlyLines -Path $Path
    $refs = @()
    foreach ($line in $codeLines) {
        # Quoted: "some/relative/path.c"
        foreach ($m in [regex]::Matches($line, '"([A-Za-z0-9_./\\-]+\.c)"')) {
            $refs += ($m.Groups[1].Value -replace '\\', '/')
        }
        # Bare, one-per-line list entry: optional leading whitespace, a
        # relative path ending .c, nothing else on the line.
        if ($line -match '^\s*([A-Za-z0-9_][A-Za-z0-9_./\\-]*\.c)\s*$') {
            $refs += ($Matches[1] -replace '\\', '/')
        }
    }
    return ($refs | Sort-Object -Unique)
}

# Returns every real *.c file under $Dir, excluding test/ and build/ output,
# as paths relative to $Dir with forward slashes.
function Get-RealCFiles {
    param([string]$Dir)
    Get-ChildItem -Path $Dir -Recurse -File -Filter "*.c" |
        Where-Object {
            $_.FullName -notmatch '[\\/]test[\\/]' -and
            $_.FullName -notmatch '[\\/]build[\\/]'
        } |
        ForEach-Object {
            ($_.FullName.Substring($Dir.Length + 1) -replace '\\', '/')
        } |
        Sort-Object
}

# --- Allowlist: {RelPath = repo-root-relative real file; Reason = why it is
# intentionally absent from its target's CMakeLists.txt}. Empty today -- see
# header. RelPath is compared case-insensitively against the repo-root-
# relative path of every file this check considers missing. ---
$allowlist = @(
    # Example shape (not real): @{ RelPath = "firmware/KilnFW/App/drivers/foo.c"; Reason = "..." }
)

function Test-Allowlisted {
    param([string]$RelPath)
    foreach ($entry in $allowlist) {
        if ($entry.RelPath -ieq $RelPath) { return $true }
    }
    return $false
}

$violations = @()

# --- 1. firmware/KilnFW/App/*.c (top level only -- drivers/ is its own
#        component with its own CMakeLists.txt, handled separately below) ---
$appDir = Join-Path $root "firmware\KilnFW\App"
$appCMake = Join-Path $appDir "CMakeLists.txt"
if (-not (Test-Path $appCMake)) {
    throw "check_c_files_in_cmakelists: $appCMake not found -- has it moved? This check is now blind."
}
$appRefs = Get-CMakeCSourceRefs -Path $appCMake
Get-ChildItem -Path $appDir -File -Filter "*.c" | ForEach-Object {
    $name = $_.Name
    if ($appRefs -notcontains $name) {
        $rel = "firmware/KilnFW/App/$name"
        if (-not (Test-Allowlisted -RelPath $rel)) {
            $violations += "${rel}: not referenced by $appCMake"
        }
    }
}

# --- 2. firmware/KilnFW/App/drivers/**/*.c ---
$driversDir = Join-Path $appDir "drivers"
$driversCMake = Join-Path $driversDir "CMakeLists.txt"
if (-not (Test-Path $driversCMake)) {
    throw "check_c_files_in_cmakelists: $driversCMake not found -- has it moved? This check is now blind."
}
$driversRefs = Get-CMakeCSourceRefs -Path $driversCMake
$driversRealResolved = (Resolve-Path $driversDir).Path
foreach ($relFromDrivers in (Get-RealCFiles -Dir $driversRealResolved)) {
    if ($driversRefs -notcontains $relFromDrivers) {
        $rel = "firmware/KilnFW/App/drivers/$relFromDrivers"
        if (-not (Test-Allowlisted -RelPath $rel)) {
            $violations += "${rel}: not referenced by $driversCMake"
        }
    }
}

# --- 3. firmware/SaftyFW/src/**/*.c ---
$saftyRoot = Join-Path $root "firmware\SaftyFW"
$saftyCMake = Join-Path $saftyRoot "CMakeLists.txt"
$saftySrcDir = Join-Path $saftyRoot "src"
if (-not (Test-Path $saftyCMake)) {
    throw "check_c_files_in_cmakelists: $saftyCMake not found -- has it moved? This check is now blind."
}
if (-not (Test-Path $saftySrcDir)) {
    throw "check_c_files_in_cmakelists: $saftySrcDir not found -- has it moved? This check is now blind."
}
$saftyRefs = Get-CMakeCSourceRefs -Path $saftyCMake
$saftySrcResolved = (Resolve-Path $saftySrcDir).Path
foreach ($relFromSrc in (Get-RealCFiles -Dir $saftySrcResolved)) {
    $cmakeStyleRel = "src/$relFromSrc"
    if ($saftyRefs -notcontains $cmakeStyleRel) {
        $rel = "firmware/SaftyFW/src/$relFromSrc"
        if (-not (Test-Allowlisted -RelPath $rel)) {
            $violations += "${rel}: not referenced by $saftyCMake"
        }
    }
}

# Sanity floor: this check has parsed 90+ KilnFW driver sources and 40+
# SaftyFW sources on every run so far. If either reference list collapses to
# near-zero, the CMake syntax has probably changed shape and this script has
# gone blind rather than the tree having gone empty.
if ($appRefs.Count -lt 1 -or $driversRefs.Count -lt 50 -or $saftyRefs.Count -lt 30) {
    throw "check_c_files_in_cmakelists: parsed implausibly few source refs (App=$($appRefs.Count) drivers=$($driversRefs.Count) SaftyFW=$($saftyRefs.Count)) -- the CMakeLists source-list syntax has probably changed and this script's parser has gone blind. Update Get-CMakeCSourceRefs before trusting this result."
}

if ($violations.Count -gt 0) {
    Write-Host "C FILES IN CMAKELISTS CHECK FAILED:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $v" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A .c file with no CMakeLists.txt reference compiles fine in a host-test" -ForegroundColor Red
    Write-Host "  suite that lists its own sources independently, and only fails at the" -ForegroundColor Red
    Write-Host "  real target's link step -- see this script's header for the 2026-08-28" -ForegroundColor Red
    Write-Host "  tick_timing.c incident this check exists to catch." -ForegroundColor Red
    throw "$($violations.Count) source file(s) are not referenced by their target's CMakeLists.txt and are not on the allowlist"
}

Write-Host "C files in CMakeLists check passed: $($appRefs.Count) App/, $($driversRefs.Count) drivers/, $($saftyRefs.Count) SaftyFW/src/ refs found; every real source file accounted for ($($allowlist.Count) allowlisted)."
exit 0
