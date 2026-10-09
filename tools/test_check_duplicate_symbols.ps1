# test_check_duplicate_symbols.ps1 -- scratch-dir test for check_duplicate_symbols.ps1's manifest selection.
# Builds a fake build/ under the temp dir (KILNCTL_DUPSYM_BUILD_DIR hook), never touches firmware/KilnFW/build.
#   A: stale build.ninja (covers nothing) + matching obj_manifest.txt -> exit 0, manifest used (no silent narrowing)
#   B: neither source matches the on-disk objects                       -> exit 1, fails loud
#   C: only a matching build.ninja                                      -> exit 0
# -ScriptUnderTest lets negtest.ps1 point this at a mutated copy (must live in tools\).
# checkcache: ok
param([string]$ScriptUnderTest)
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $ScriptUnderTest) { $ScriptUnderTest = Join-Path $here 'check_duplicate_symbols.ps1' }
$fail = 0
function Check($ok, $msg) { if ($ok) { Write-Host "PASS: $msg" } else { Write-Host "FAIL: $msg"; $script:fail++ } }

function New-Scratch {
    $d = Join-Path ([IO.Path]::GetTempPath()) ("dupsym_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $objDir = Join-Path $d "esp-idf\drivers\CMakeFiles\__idf_drivers.dir\http"
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    [IO.File]::WriteAllText((Join-Path $objDir "zones_http_pid.c.obj"), "x")
    return $d
}
function Run-Check($dir) {
    $env:KILNCTL_DUPSYM_BUILD_DIR = $dir
    $fakeNm = Join-Path $dir "fake_nm.cmd"
    Set-Content -LiteralPath $fakeNm -Value "@echo off"
    $env:KILNCTL_DUPSYM_NM = $fakeNm
    try { $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $ScriptUnderTest 2>&1 | Out-String; return @{ Code = $LASTEXITCODE; Out = $out } }
    finally { Remove-Item Env:\KILNCTL_DUPSYM_BUILD_DIR, Env:\KILNCTL_DUPSYM_NM -ErrorAction SilentlyContinue }
}
$real = "esp-idf/drivers/CMakeFiles/__idf_drivers.dir/http/zones_http_pid.c.obj"
$other = "esp-idf/drivers/CMakeFiles/__idf_drivers.dir/http/not_here.c.obj"

$d = New-Scratch
try {
    Set-Content -LiteralPath (Join-Path $d "build.ninja") -Value "build ${other}: C_COMPILER__x`n"
    Start-Sleep -Milliseconds 50
    Set-Content -LiteralPath (Join-Path $d "obj_manifest.txt") -Value $real
    # make build.ninja the NEWER file, so "newest wins" alone would pick the stale one
    (Get-Item (Join-Path $d "build.ninja")).LastWriteTimeUtc = [DateTime]::UtcNow.AddMinutes(5)
    $r = Run-Check $d
    Check ($r.Code -eq 0) "A: stale build.ninja does not outrank the matching manifest (exit $($r.Code))"
    Check ($r.Out -match 'using obj_manifest\.txt') "A: output says the manifest was used"
    Check ($r.Out -notmatch 'ignoring 1 \.c\.obj') "A: the on-disk object was not dropped"
} finally { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue }

$d = New-Scratch
try {
    Set-Content -LiteralPath (Join-Path $d "build.ninja") -Value "build ${other}: C_COMPILER__x`n"
    Set-Content -LiteralPath (Join-Path $d "obj_manifest.txt") -Value $other
    $r = Run-Check $d
    Check ($r.Code -eq 1) "B: neither source matches -> fails loud, exit 1 (exit $($r.Code))"
    Check ($r.Out -match 'no object manifest matches') "B: message names the problem"
} finally { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue }

$d = New-Scratch
try {
    Set-Content -LiteralPath (Join-Path $d "build.ninja") -Value "build ${real}: C_COMPILER__x`n"
    $r = Run-Check $d
    Check ($r.Code -eq 0) "C: matching build.ninja alone -> exit 0 (exit $($r.Code))"
} finally { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue }

if ($fail -gt 0) { Write-Host "$fail check(s) FAILED"; exit 1 }
Write-Host "all passed"; exit 0
