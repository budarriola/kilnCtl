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
        $paths = ($found | ForEach-Object { $_.FullName }) -join ", "
        throw "_drivers_layout: '$BaseName' matched more than one file under $DriversDir ($paths) -- cannot tell which one is the real file."
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
