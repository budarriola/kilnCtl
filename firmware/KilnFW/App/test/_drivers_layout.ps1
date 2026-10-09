# _drivers_layout.ps1 -- shared helper for App/test check_*.ps1 scripts that
# read specific firmware/KilnFW/App/drivers files by name.
#
# firmware/KilnFW/App/drivers/ is being split into layer subdirectories
# (drivers/<layer>/<name> -- common,hw,owners,control,safety,persist,net,
# http,ui,bridge,sim; see tools/drivers_reorg/mapping.csv). Several check
# scripts in this directory used to build a flat basename path with
# `Join-Path $driversRoot "<name>"`, which silently stops resolving the
# moment that file moves one layer deeper. Resolve-DriverFile below mirrors
# _drivers_layout.py's / _drivers_layout.js's contract and
# check_thermal_guard_input_producers.ps1's own local copy of this same
# function: recursive basename search under drivers/, unique match
# required, throws loudly (never returns a nonexistent/wrong/ambiguous path
# silently) on 0 or >1 match.
#
# Usage (dot-source from a check_*.ps1 in this same directory):
#   . (Join-Path $PSScriptRoot "_drivers_layout.ps1")
#   $driversRoot = Get-DriversRoot -TestDir $PSScriptRoot
#   $otaPath = Resolve-DriverFile -DriversDir $driversRoot -BaseName "ota_page.html"

function Get-DriversRoot {
    param([string]$TestDir)
    return Join-Path (Split-Path -Parent (Split-Path -Parent $TestDir)) "App\drivers"
}

function Resolve-DriverFile {
    param([string]$DriversDir, [string]$BaseName)
    $found = Get-ChildItem -Path $DriversDir -Filter $BaseName -File -Recurse
    if ($found.Count -eq 0) {
        throw "_drivers_layout: expected file '$BaseName' not found anywhere under $DriversDir -- has it moved or been renamed? This check is now blind, which is worse than the bug it looks for."
    }
    if ($found.Count -gt 1) {
        # An untracked in-flight draft with the same basename as the real,
        # git-tracked file (e.g. another session's WIP copy dropped straight
        # into drivers/) must not make every check ambiguous. Prefer the
        # tracked file(s); only throw if more than one TRACKED file shares
        # this basename -- that ambiguity is the real defect this check
        # exists to catch, and an untracked scratch copy is not it.
        $tracked = git -C $DriversDir ls-files . 2>$null
        if (-not $tracked) {
            $tracked = @()
        }
        $trackedFull = $tracked | ForEach-Object { (Resolve-Path -LiteralPath (Join-Path $DriversDir $_)).ProviderPath }
        $trackedMatches = $found | Where-Object { $trackedFull -contains $_.FullName }
        if ($trackedMatches.Count -eq 1) {
            return $trackedMatches[0].FullName
        }
        $paths = ($found | ForEach-Object { $_.FullName }) -join ", "
        if ($trackedMatches.Count -gt 1) {
            $trackedPaths = ($trackedMatches | ForEach-Object { $_.FullName }) -join ", "
            throw "_drivers_layout: '$BaseName' matched more than one git-tracked file under $DriversDir ($trackedPaths) -- cannot tell which one is the real file."
        }
        throw "_drivers_layout: '$BaseName' matched more than one file under $DriversDir ($paths), and none of them is git-tracked -- cannot tell which one is the real file."
    }
    return $found[0].FullName
}

# Recursively list every file under $DriversDir whose Name matches -Filter
# (e.g. "*_page.html") -- the direct replacement for a flat, non-recursive
# Get-ChildItem that would otherwise find nothing (or only the top layer)
# once drivers/ splits.
function Get-DriverFiles {
    param([string]$DriversDir, [string]$Filter)
    return @(Get-ChildItem -Path $DriversDir -Filter $Filter -File -Recurse)
}
