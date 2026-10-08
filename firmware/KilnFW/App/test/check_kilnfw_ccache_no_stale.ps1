# check_kilnfw_ccache_no_stale.ps1 -- proves, against the real ccache binary
# and the real ESP32-S3 compiler, that the ccache configuration
# check_00_kilnfw_target_build.ps1 builds with (lib_kilnfw_ccache.ps1) cannot
# hand back a stale object.
#
# check_00 ran with CCACHE_DISABLE=1 from 139debb5 (2026-09-09) to 2026-10-08
# on the fear that ccache could mask a broken translation unit. It now runs
# with ccache ON (shared cache, base_dir C:\wt so a fresh agent worktree reuses
# objects another tree compiled). lib_kilnfw_ccache.ps1 argues why that is
# safe; this check is the argument's evidence, re-run on every suite run, so a
# ccache upgrade or a configuration edit that breaks it turns the suite red.
#
# What it does, in a throwaway pair of trees under C:\wt (so base_dir applies)
# with a throwaway cache directory (so the shared cache neither helps nor
# hurts), using Enable-KilnfwCcache's exact environment:
#   1. tree A compiles a TU                         -> must MISS
#   2. tree B (byte-identical copy, other path)      -> must HIT, same object,
#      and B's dependency file must not name tree A (ninja would otherwise
#      track the wrong tree's headers) -- the non-vacuity step: if nothing
#      ever hits, every "must miss" below proves nothing
#   3. one byte of the .c changed                    -> MISS, object changes,
#      and equals a compile with ccache disabled
#   4. a header changed, same size, mtime restored   -> MISS, object changes
#   5. a new header shadowing the old one earlier in the -I path
#                                                    -> MISS, object changes
#      (the direct-mode caveat in ccache's manual; why direct mode is off)
#   6. a -D that changes code                        -> MISS, object changes
#   7. an optimisation flag change                   -> MISS
#   8. a TU that fails -Werror, compiled twice       -> fails BOTH times,
#      never a hit (a failure is never cached)
#   9. everything restored to tree A's bytes         -> HIT, tree A's object
# and then asserts the effective configuration (Get-KilnfwCcacheConfigProblems).
# All failures are collected and reported together.
#
# Negative tests (2026-10-08, restored by hand afterwards): turning direct mode
# back on in lib_kilnfw_ccache.ps1 (CCACHE_NODIRECT removed, expected
# direct_mode "true") made step 5 report a stale HIT, and adding
# CCACHE_SLOPPINESS=file_stat_matches made the configuration assertion fail.
#
# Exit codes: 0 PASS, 1 FAIL, 3 SKIP (no ESP-IDF xtensa toolchain or ccache on
# this machine). Not marked checkcache-able: it runs a toolchain compile.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "lib_kilnfw_ccache.ps1")

$failures = New-Object System.Collections.Generic.List[string]
function Add-Failure([string]$msg) { $script:failures.Add($msg); Write-Host "  FAIL: $msg" }

$gcc = $null
$gccCmd = Get-Command "xtensa-esp32s3-elf-gcc" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($gccCmd) { $gcc = $gccCmd.Source }
if (-not $gcc) {
    $found = @(Get-ChildItem -Path "C:\Espressif\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin\xtensa-esp32s3-elf-gcc.exe" -ErrorAction SilentlyContinue |
        Sort-Object { $_.Directory.Parent.Parent.Name } -Descending)
    if ($found.Count -gt 0) { $gcc = $found[0].FullName }
}
$ccache = Get-KilnfwCcacheExe
if (-not $gcc) { Write-Host "SKIP: no xtensa-esp32s3-elf-gcc (ESP-IDF toolchain) found"; exit 3 }
if (-not $ccache) { Write-Host "SKIP: no ccache found (PATH or C:\Espressif\tools\ccache)"; exit 3 }
Write-Host "compiler: $gcc"
Write-Host "ccache:   $ccache"

$root = Join-Path "C:\wt" ("ccache_selftest_" + [guid]::NewGuid().ToString("N").Substring(0, 10))
$treeA = Join-Path $root "treeA"
$treeB = Join-Path $root "treeB"
$cacheDir = Join-Path $root "cache"
$past = [datetime]::Now.AddHours(-2)

function Write-Src([string]$path, [string]$text) {
    $dir = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [System.IO.File]::WriteAllText($path, $text.Replace("`r`n", "`n"))
    # Old enough that ccache's "file modified too recently" guard never
    # suppresses a hit or a direct-mode lookup -- otherwise the negative test
    # (direct mode on) could pass for the wrong reason.
    [System.IO.File]::SetLastWriteTime($path, $past)
}

$tuText = @"
#include "cfg.h"
#ifdef KILN_BUMP
#define KILN_EXTRA 100
#else
#define KILN_EXTRA 0
#endif
int kiln_value(void) { return KILN_CFG_VALUE + KILN_EXTRA + 1; }
"@
$hdrText = "#define KILN_CFG_VALUE 41`n"

foreach ($t in @($treeA, $treeB)) {
    Write-Src (Join-Path $t "src\t.c") $tuText
    Write-Src (Join-Path $t "inc\cfg.h") $hdrText
    New-Item -ItemType Directory -Force -Path (Join-Path $t "inc_hi") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $t "build") | Out-Null
}

$savedLocation = Get-Location
# Compiles <tree>\src\t.c in <tree>\build exactly the way ninja does (absolute
# source/-I paths, -MD dependency file) and returns result, exit code, object
# bytes and dependency-file text. -NoCcache compiles with CCACHE_DISABLE=1.
function Invoke-TestCompile([string]$tree, [string[]]$extra = @(), [switch]$NoCcache) {
    $build = Join-Path $tree "build"
    $obj = Join-Path $build "t.c.obj"
    foreach ($f in @($obj, "$obj.d")) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } }
    $statsLog = Join-Path $root ("stats_" + [guid]::NewGuid().ToString("N") + ".log")
    $ccArgs = @("-O2", "-Wall", "-Werror", "-mlongcalls") + $extra + @(
        "-I", (Join-Path $tree "inc_hi"), "-I", (Join-Path $tree "inc"),
        "-MD", "-MT", "t.c.obj", "-MF", "t.c.obj.d",
        "-o", "t.c.obj", "-c", (Join-Path $tree "src\t.c"))
    Enable-KilnfwCcache -CacheDir $cacheDir -StatsLog $statsLog
    if ($NoCcache) { $env:CCACHE_DISABLE = "1" }
    Push-Location $build
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"   # a compiler diagnostic on stderr is data here, not a script error
    try {
        $out = & $ccache $gcc @ccArgs 2>&1
        $rc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedEap
        Pop-Location
        Remove-Item Env:CCACHE_DISABLE -ErrorAction SilentlyContinue
    }
    $lines = @()
    if (Test-Path -LiteralPath $statsLog) { $lines = @([System.IO.File]::ReadAllLines($statsLog) | ForEach-Object { $_.Trim() }) }
    $result = "none"
    if ($lines -contains "direct_cache_hit") { $result = "hit(direct)" }
    elseif ($lines -contains "preprocessed_cache_hit") { $result = "hit" }
    elseif ($lines -contains "cache_miss") { $result = "miss" }
    elseif ($lines -contains "compile_failed") { $result = "failed" }
    elseif ($lines.Count -gt 0) { $result = (@($lines | Where-Object { $_ -and -not $_.StartsWith("#") }) -join ",") }
    $bytes = $null
    if (Test-Path -LiteralPath $obj) { $bytes = [System.IO.File]::ReadAllBytes($obj) }
    $dep = ""
    if (Test-Path -LiteralPath "$obj.d") { $dep = [System.IO.File]::ReadAllText("$obj.d") }
    return [pscustomobject]@{ Result = $result; Rc = $rc; Obj = $bytes; Dep = $dep; Out = ($out -join "`n") }
}

function Test-SameBytes($a, $b) {
    if ($null -eq $a -or $null -eq $b) { return $false }
    if ($a.Length -ne $b.Length) { return $false }
    for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { return $false } }
    return $true
}

function Expect([string]$step, $r, [string]$want) {
    $isHit = $r.Result -like "hit*"
    $ok = (($want -eq "hit") -and $isHit -and $r.Rc -eq 0) -or (($want -eq "miss") -and $r.Result -eq "miss" -and $r.Rc -eq 0)
    if ($ok) { Write-Host "  ok   $step -> $($r.Result)" }
    else { Add-Failure "$step expected $want, got '$($r.Result)' (exit $($r.Rc)) $($r.Out)" }
    return $ok
}

try {
    Write-Host "1. tree A, first compile"
    $a1 = Invoke-TestCompile $treeA
    [void](Expect "tree A first compile" $a1 "miss")
    if ($null -eq $a1.Obj) { Add-Failure "tree A produced no object" }

    Write-Host "2. tree B, identical bytes at another C:\wt path"
    $b1 = Invoke-TestCompile $treeB
    [void](Expect "tree B identical copy" $b1 "hit")
    if (-not (Test-SameBytes $a1.Obj $b1.Obj)) { Add-Failure "tree B's hit object differs from tree A's" }
    $depNorm = $b1.Dep.Replace("\", "/").ToLowerInvariant()
    if ($depNorm.Contains($treeA.Replace("\", "/").ToLowerInvariant()) -or $depNorm.Contains("treea")) {
        Add-Failure "tree B's dependency file names tree A -- ninja would track another tree's headers: $($b1.Dep)"
    }
    if (-not $depNorm.Contains("cfg.h")) { Add-Failure "tree B's dependency file does not list cfg.h: $($b1.Dep)" }

    Write-Host "3. one byte of the .c changed"
    Write-Src (Join-Path $treeB "src\t.c") ($tuText.Replace("KILN_EXTRA + 1;", "KILN_EXTRA + 2;"))
    $b2 = Invoke-TestCompile $treeB
    [void](Expect "one-byte .c edit" $b2 "miss")
    if (Test-SameBytes $a1.Obj $b2.Obj) { Add-Failure "one-byte .c edit produced the OLD object" }
    $b2plain = Invoke-TestCompile $treeB -NoCcache
    if (-not (Test-SameBytes $b2.Obj $b2plain.Obj)) { Add-Failure "ccache object for the edited .c differs from a compile with ccache disabled" }
    Write-Src (Join-Path $treeB "src\t.c") $tuText

    Write-Host "4. header changed, same size, same mtime"
    $hdrB = Join-Path $treeB "inc\cfg.h"
    Write-Src $hdrB ($hdrText.Replace("41", "43"))
    if ((Get-Item -LiteralPath $hdrB).Length -ne [System.Text.Encoding]::ASCII.GetByteCount($hdrText)) { Add-Failure "test setup: edited header is not the same size" }
    $b3 = Invoke-TestCompile $treeB
    [void](Expect "same-size same-mtime header edit" $b3 "miss")
    if (Test-SameBytes $a1.Obj $b3.Obj) { Add-Failure "header edit produced the OLD object" }
    Write-Src $hdrB $hdrText

    Write-Host "5. new header shadowing cfg.h earlier in the -I path"
    $b4pre = Invoke-TestCompile $treeB
    [void](Expect "restored tree B before shadow test" $b4pre "hit")
    $shadow = Join-Path $treeB "inc_hi\cfg.h"
    Write-Src $shadow "#define KILN_CFG_VALUE 57`n"
    $b4 = Invoke-TestCompile $treeB
    [void](Expect "shadowing header added" $b4 "miss")
    if (Test-SameBytes $a1.Obj $b4.Obj) { Add-Failure "shadowing header produced the OLD object (stale hit)" }
    Remove-Item -LiteralPath $shadow -Force

    Write-Host "6. -D that changes code"
    $b5 = Invoke-TestCompile $treeB @("-DKILN_BUMP")
    [void](Expect "-DKILN_BUMP" $b5 "miss")
    if (Test-SameBytes $a1.Obj $b5.Obj) { Add-Failure "-DKILN_BUMP produced the OLD object" }

    Write-Host "7. optimisation flag change"
    $b6 = Invoke-TestCompile $treeB @("-O0")
    [void](Expect "-O0 appended" $b6 "miss")

    Write-Host "8. TU failing -Werror, twice"
    Write-Src (Join-Path $treeB "src\t.c") ($tuText + "int kiln_bad(void) { int unused_kiln_var; return 0; }`n")
    foreach ($n in 1, 2) {
        $bf = Invoke-TestCompile $treeB
        if ($bf.Rc -eq 0) { Add-Failure "failing TU compiled successfully on attempt $n (result '$($bf.Result)') -- a failure was masked" }
        elseif ($bf.Result -like "hit*") { Add-Failure "failing TU reported a cache hit on attempt $n" }
        else { Write-Host "  ok   failing TU attempt $n -> exit $($bf.Rc), $($bf.Result)" }
        if ($bf.Obj) { Add-Failure "failing TU left an object behind on attempt $n" }
    }
    Write-Src (Join-Path $treeB "src\t.c") $tuText

    Write-Host "9. everything restored to tree A's bytes"
    $b7 = Invoke-TestCompile $treeB
    [void](Expect "restored tree B" $b7 "hit")
    if (-not (Test-SameBytes $a1.Obj $b7.Obj)) { Add-Failure "restored tree B's object differs from tree A's" }

    Write-Host "10. effective configuration"
    Enable-KilnfwCcache -CacheDir $cacheDir
    foreach ($p in @(Get-KilnfwCcacheConfigProblems -CcacheExe $ccache)) { Add-Failure $p }
} finally {
    Set-Location $savedLocation
    foreach ($item in @(Get-ChildItem Env: | Where-Object { $_.Name -like "CCACHE_*" })) {
        Remove-Item -LiteralPath ("Env:" + $item.Name) -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "FAIL: $($failures.Count) problem(s) with the ccache configuration check_00_kilnfw_target_build.ps1 builds with:"
    foreach ($f in $failures) { Write-Host "  - $f" }
    exit 1
}
Write-Host "PASS: ccache (lib_kilnfw_ccache.ps1 configuration) missed and rebuilt on every content/flag change, hit only on identical input, never cached a failure."
exit 0
