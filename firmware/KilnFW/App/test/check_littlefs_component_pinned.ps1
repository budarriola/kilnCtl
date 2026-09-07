# check_littlefs_component_pinned.ps1 -- docs/FILESYSTEM_PLAN.md step 1.
#
# Step 1 of the SPIFFS->LittleFS track is deliberately tiny: add the
# `joltwallet/littlefs` managed component to App/idf_component.yml and
# confirm the registry fetch works, with nothing in App/drivers referencing
# it yet (that wiring is step 2). A build passing once proves the fetch
# worked at that moment; it proves nothing about the checked-in *source*
# staying correct afterward -- someone could revert the yml edit, or the
# component manager could silently resolve a different package, and a
# build several commits later would only fail (or worse, quietly resolve
# something else) with no localized diagnostic pointing at this step.
#
# This check reads the two source-controlled files that describe the
# dependency -- App/idf_component.yml (the declaration) and
# KilnFW/dependencies.lock (the resolved, hash-pinned result of the last
# successful `build_kilnfw`) -- and fails loudly, naming the missing/wrong
# piece, if either does not show `joltwallet/littlefs` present and
# consistent. It does not invoke the build itself (that is
# `build_kilnfw`'s job and takes minutes); it is the fast, always-on guard
# that the two files were not left to drift apart.
#
# Usage:
#   powershell -File firmware\KilnFW\App\test\check_littlefs_component_pinned.ps1
#   powershell -File firmware\KilnFW\App\test\check_littlefs_component_pinned.ps1 -YmlPath <path> -LockPath <path>
#
# -YmlPath / -LockPath exist so this script's own negative test can point
# it at a broken copy without touching the real files.

param(
    [string]$YmlPath,
    [string]$LockPath
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot = ...\firmware\KilnFW\App\test
$appRoot    = Split-Path -Parent $PSScriptRoot
$kilnfwRoot = Split-Path -Parent $appRoot

if (-not $YmlPath)  { $YmlPath  = Join-Path $appRoot "idf_component.yml" }
if (-not $LockPath) { $LockPath = Join-Path $kilnfwRoot "dependencies.lock" }

if (-not (Test-Path $YmlPath)) {
    throw "check_littlefs_component_pinned.ps1: $YmlPath not found"
}
if (-not (Test-Path $LockPath)) {
    throw "check_littlefs_component_pinned.ps1: $LockPath not found"
}

$errors = @()

$ymlLines = Get-Content -Path $YmlPath
$ymlMatch = $ymlLines | Where-Object { $_ -match '^\s*joltwallet/littlefs\s*:\s*"?\^?[\d.]+"?\s*$' }
if (-not $ymlMatch) {
    $errors += "$YmlPath does not declare 'joltwallet/littlefs' as a dependency (FILESYSTEM_PLAN.md step 1)"
}

$lockText = Get-Content -Path $LockPath -Raw
if ($lockText -notmatch 'joltwallet/littlefs\s*:') {
    $errors += "$LockPath has no 'joltwallet/littlefs:' entry -- run build_kilnfw to resolve it after editing idf_component.yml"
} elseif ($lockText -notmatch 'joltwallet/littlefs\s*:\s*\r?\n\s*component_hash:\s*[0-9a-f]{16,}') {
    $errors += "$LockPath has a 'joltwallet/littlefs:' entry with no component_hash recorded -- resolution looks incomplete"
}

if ($errors.Count -gt 0) {
    Write-Host "LITTLEFS COMPONENT PIN CHECK FAILED:" -ForegroundColor Red
    foreach ($e in $errors) {
        Write-Host "  $e" -ForegroundColor Red
    }
    throw "$($errors.Count) problem(s) found -- see docs/FILESYSTEM_PLAN.md step 1"
}

Write-Host "LittleFS component pin check passed: 'joltwallet/littlefs' declared in $YmlPath and hash-pinned in $LockPath."
exit 0
