# check_route_tier_coverage.ps1 -- fails when a route exists in code with no
# tier assigned in route_tier_table.h (docs/WEB_AUTH_PLAN.md section 1).
#
# Fail-closed by design: an unclassified route is NOT allowed to default to
# OPEN. This check does not itself enforce anything at runtime (that is
# kiln_http_register(), plan item 5) -- it only guarantees the classification
# table stays complete, so a route added later cannot silently ship
# unprotected. Same shape as check_uri_handler_cap.ps1: comment-stripped
# `.uri = "..."` scan under App/drivers, a Resolve-DriverFile fail-loud
# lookup, same blindness floor idiom.
param(
    [string]$DriversDir,
    [string]$TableFile
)
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
if ($DriversDir) {
    $driversDir = (Resolve-Path $DriversDir).Path
} else {
    $driversDir = Join-Path $root "..\firmware\KilnFW\App\drivers"
    $driversDir = (Resolve-Path $driversDir).Path
}

function Resolve-DriverFile {
    param([string]$DriversDir, [string]$BaseName)
    $found = Get-ChildItem -Path $DriversDir -Filter $BaseName -File -Recurse
    if ($found.Count -eq 0) {
        throw "check_route_tier_coverage.ps1: expected file '$BaseName' not found anywhere under $DriversDir -- has it moved or been renamed? Update this script's path."
    }
    if ($found.Count -gt 1) {
        $paths = ($found | ForEach-Object { $_.FullName }) -join ", "
        throw "check_route_tier_coverage.ps1: '$BaseName' matched more than one file under $DriversDir ($paths) -- this script cannot tell which one is authoritative. Disambiguate."
    }
    return $found[0].FullName
}

function Get-CodeOnlyLines {
    param([string]$Path)
    $inBlockComment = $false
    $lines = Get-Content -Path $Path
    $result = @()
    foreach ($line in $lines) {
        $code = $line
        if ($inBlockComment) {
            $endIdx = $code.IndexOf("*/")
            if ($endIdx -ge 0) {
                $code = $code.Substring($endIdx + 2)
                $inBlockComment = $false
            } else {
                $result += ""
                continue
            }
        }
        $lineCommentIdx = $code.IndexOf("//")
        if ($lineCommentIdx -ge 0) {
            $code = $code.Substring(0, $lineCommentIdx)
        }
        while ($true) {
            $startIdx = $code.IndexOf("/*")
            if ($startIdx -lt 0) { break }
            $endIdx = $code.IndexOf("*/", $startIdx)
            if ($endIdx -ge 0) {
                $code = $code.Substring(0, $startIdx) + $code.Substring($endIdx + 2)
            } else {
                $code = $code.Substring(0, $startIdx)
                $inBlockComment = $true
                break
            }
        }
        $result += $code
    }
    return $result
}

# Pull every registered (uri, method) pair out of one comment-stripped file.
# httpd_uri_t literals in this tree are always struct initializers with
# `.uri = "...", .method = HTTP_..., .handler = ...` fields, possibly spread
# across several lines -- so this scans field-by-field across the whole
# comment-stripped file text (joined with newlines) rather than line-by-line,
# and pairs each `.uri` with the NEXT `.method` that follows it (the same
# ordering httpd_uri_t initializers always use in this codebase: uri then
# method then handler).
function Get-RegisteredRoutes {
    param([string]$Path)
    $codeLines = Get-CodeOnlyLines -Path $Path
    $text = [string]::Join("`n", $codeLines)
    $uriPattern = '\.uri\s*=\s*"([^"]*)"'
    $methodPattern = '\.method\s*=\s*(HTTP_[A-Za-z0-9_]+)'
    $uriMatches = [regex]::Matches($text, $uriPattern)
    $routes = New-Object System.Collections.Generic.List[object]
    foreach ($m in $uriMatches) {
        $searchFrom = $m.Index + $m.Length
        $window = $text.Substring($searchFrom, [Math]::Min(400, $text.Length - $searchFrom))
        $mm = [regex]::Match($window, $methodPattern)
        if ($mm.Success) {
            $routes.Add([PSCustomObject]@{ Uri = $m.Groups[1].Value; Method = $mm.Groups[1].Value; File = (Split-Path $Path -Leaf) })
        } else {
            $routes.Add([PSCustomObject]@{ Uri = $m.Groups[1].Value; Method = $null; File = (Split-Path $Path -Leaf) })
        }
    }
    return $routes
}

function Get-TieredKeys {
    param([string]$Path)
    $codeLines = Get-CodeOnlyLines -Path $Path
    $text = [string]::Join("`n", $codeLines)
    $rowPattern = 'ROUTE_TIER\(\s*"([^"]*)"\s*,\s*(HTTP_[A-Za-z0-9_]+)\s*,\s*(ROUTE_TIER_[A-Za-z0-9_]+)\s*\)'
    $rowMatches = [regex]::Matches($text, $rowPattern)
    $keys = @{}
    foreach ($m in $rowMatches) {
        $key = "$($m.Groups[2].Value) $($m.Groups[1].Value)"
        $keys[$key] = $m.Groups[3].Value
    }
    return $keys
}

# Production entry point, called directly by the negative test (dot-sourced)
# against synthetic files -- do not reimplement this logic anywhere else.
function Invoke-RouteTierCoverageScan {
    param(
        [Parameter(Mandatory)][string]$DriversDir,
        [Parameter(Mandatory)][string]$TableFile,
        [int]$BlindnessFloor = 80
    )
    $sourceFiles = Get-ChildItem -Path $DriversDir -Filter "*.c" -File -Recurse
    if ($sourceFiles.Count -lt 5) {
        throw "check_route_tier_coverage.ps1: only $($sourceFiles.Count) .c file(s) found under $DriversDir -- has the routing code moved? Update this script's target directory."
    }

    $allRoutes = New-Object System.Collections.Generic.List[object]
    foreach ($f in $sourceFiles) {
        foreach ($r in (Get-RegisteredRoutes -Path $f.FullName)) {
            $allRoutes.Add($r)
        }
    }

    if ($allRoutes.Count -lt $BlindnessFloor) {
        throw "check_route_tier_coverage.ps1: only counted $($allRoutes.Count) total registered routes under $DriversDir, which is implausibly low ($BlindnessFloor+ expected) -- the registration style has probably changed and this script has gone blind. Update its pattern before trusting its result."
    }

    $tieredKeys = Get-TieredKeys -Path $TableFile
    if ($tieredKeys.Count -lt $BlindnessFloor) {
        throw "check_route_tier_coverage.ps1: only found $($tieredKeys.Count) ROUTE_TIER(...) row(s) in $TableFile, which is implausibly low ($BlindnessFloor+ expected) -- has the table's macro shape changed? Update this script's pattern before trusting its result."
    }

    $missing = New-Object System.Collections.Generic.List[object]
    $seen = New-Object System.Collections.Generic.HashSet[string]
    foreach ($r in $allRoutes) {
        if ($null -eq $r.Method) {
            # Fail-closed: a route this scanner cannot even parse a method
            # for is treated as an offender too, never silently skipped.
            $missing.Add([PSCustomObject]@{ Uri = $r.Uri; Method = "<unparsed>"; File = $r.File })
            continue
        }
        $key = "$($r.Method) $($r.Uri)"
        if (-not $tieredKeys.ContainsKey($key)) {
            if ($seen.Add($key)) {
                $missing.Add([PSCustomObject]@{ Uri = $r.Uri; Method = $r.Method; File = $r.File })
            }
        }
    }

    return [PSCustomObject]@{
        TotalRoutes  = $allRoutes.Count
        TotalTiered  = $tieredKeys.Count
        Missing      = $missing
    }
}

# Only run when invoked directly (not dot-sourced by the negative test).
if ($MyInvocation.InvocationName -ne '.') {
    if (-not $TableFile) {
        $TableFile = Resolve-DriverFile -DriversDir $driversDir -BaseName "route_tier_table.h"
    } elseif (-not (Test-Path $TableFile)) {
        throw "check_route_tier_coverage.ps1: -TableFile '$TableFile' does not exist."
    } else {
        $TableFile = (Resolve-Path $TableFile).Path
    }

    $result = Invoke-RouteTierCoverageScan -DriversDir $driversDir -TableFile $TableFile

    Write-Host "Route tier coverage check: $($result.TotalRoutes) registered route(s) under $driversDir, $($result.TotalTiered) tiered row(s) in $TableFile."

    if ($result.Missing.Count -gt 0) {
        Write-Host "ROUTE TIER COVERAGE CHECK FAILED: $($result.Missing.Count) route(s) registered with no tier assignment (fail-closed -- these would default to ADMIN-and-blocked, never OPEN, but the enforcement point cannot even do that until this table names them):" -ForegroundColor Red
        foreach ($m in $result.Missing) {
            Write-Host "    $($m.Method) $($m.Uri)  (in $($m.File))" -ForegroundColor Red
        }
        throw "$($result.Missing.Count) route(s) have no ROUTE_TIER(...) entry in $TableFile. Add one for each route named above before merging -- an unclassified route must never ship."
    }

    Write-Host "Route tier coverage check passed: every registered route has a tier."
    exit 0
}
