# lib_kilnfw_ccache.ps1 -- the ONE place that decides how ccache is configured
# for check_00_kilnfw_target_build.ps1, and the ONE place its safety-relevant
# settings are asserted. Dot-sourced by that check and by
# check_kilnfw_ccache_no_stale.ps1, which proves the staleness argument below
# against the real ccache binary with exactly this configuration.
#
# WHY CCACHE WAS OFF, AND WHY THAT REASON DOES NOT HOLD (2026-10-08).
# 139debb5 forced CCACHE_DISABLE=1 on the stated grounds that ccache could
# "mask a broken translation unit (a stale hit from before the break, or from
# a tree another agent already 'fixed' locally without committing)". Neither
# can happen with the settings below. A ccache hit requires the SAME result
# key: the compiler (mtime+size), the compiler arguments that affect code
# generation, and the FULL PREPROCESSED OUTPUT of the TU -- i.e. the content of
# the source file and of every header the preprocessor actually pulled in, as
# the preprocessor resolved them on this run, plus every macro value. (Direct
# mode is deliberately OFF, see below.) A TU that was edited to break is different
# content, so it cannot hit an entry made before the break; a tree whose fix
# differs from yours is different content too. A failed compile (including a
# -Werror diagnostic) is never stored, so a failure cannot be cached away, and
# a stored hit replays the compiler's stderr, so warnings still print. The
# real cause of 9bc155ea reaching main was that NOTHING compiled the tree from
# clean (the shared main-tree build/ was never rebuilt) -- the per-tree build
# directory and mirror assertions in check_00 fixed that, independently of
# ccache.
#
# WHAT CCACHE CAN AND CANNOT CHANGE. ccache only runs when ninja has already
# decided to compile a TU; it never decides WHETHER to compile. So enabling it
# cannot make a build skip anything it would otherwise have rebuilt -- it can
# only substitute a stored object for a fresh compile of byte-identical
# inputs. The only ways that substitution could be wrong are configuration
# choices, and each one is pinned here and asserted before every build:
#   * direct_mode           FALSE. Direct mode keys a hit on the source file
#                           plus a manifest of the headers used LAST time, and
#                           ccache's own manual (Caveats) says it "fails to pick
#                           up new header files in some rare scenarios": a new
#                           header that would shadow an existing one earlier in
#                           the -I search path is not in the manifest. A fresh
#                           per-tree build directory (every new agent worktree)
#                           compiles every TU, so without ccache that case is
#                           always compiled correctly; with direct mode it could
#                           be a false hit. With direct mode off, ccache runs the
#                           real preprocessor every time and hashes its output,
#                           so header resolution is exactly the compiler's.
#                           depend_mode (same manifest idea) is off too.
#   * sloppiness            EMPTY. In particular never file_stat_matches (trusts
#                           a header's mtime+size instead of its content -- the
#                           exact robocopy same-size/same-mtime hazard check_00
#                           already guards its mirror against) and never
#                           time_macros (would let a TU using __DATE__/__TIME__,
#                           e.g. esp_app_desc, hit with an old timestamp).
#   * ignore_options        EMPTY (an ignored option is not hashed).
#   * extra hashing knobs   depend_mode, recache, read_only*, hard_link,
#                           file_clone, remote_storage, prefix_command* all at
#                           their safe defaults; no remote cache.
#   * compiler_check        mtime (the ESP-IDF toolchain lives in a versioned
#                           directory, esp-15.2.0_<date>, so a toolchain change
#                           is a different compiler file with its own mtime).
#   * base_dir = C:\wt, hash_dir = false. These are what let a NEW agent
#     worktree (each gets its own C:\wt\checkbuild_<hex>) hit objects another
#     tree compiled from the same bytes: absolute paths under C:\wt are
#     rewritten relative to the build directory before hashing, so identical
#     source at a different C:\wt path produces the same key. Measured
#     2026-10-08: the ccache input text for readiness_http.c compiled in two
#     different checkbuild_<hex> trees differed ONLY in a genuinely different
#     header line. The dependency file ninja reads (-MD) is produced from the
#     same relative paths, so it names only relative paths and the shared
#     toolchain/IDF absolute paths, never another tree's directory -- which is
#     what keeps ninja's own dirty tracking pointed at THIS tree's headers
#     after a cross-tree hit (asserted by check_kilnfw_ccache_no_stale.ps1).
#     The documented cost of hash_dir=false: a hit's DWARF DW_AT_comp_dir names
#     the build directory of whichever tree compiled it first. Code and data
#     are identical; only that debug string differs, and this check's ELF is
#     graded by objdump/nm (stack budgets, duplicate symbols), never used to
#     symbolize a crash (that is elf_archive, fed by flash_firmware's own
#     build).
#
# Every setting comes from the environment this helper builds (every inherited
# CCACHE_* variable is cleared first) and CCACHE_CONFIGPATH points at a file
# that is not expected to exist, so neither a user's ccache.conf nor a stray
# `ccache -M` can change the configuration. Assert-KilnfwCcacheConfig then
# re-reads the EFFECTIVE configuration from ccache itself (`ccache -p`) and
# fails on any deviation, including any value whose origin is a config file.

$script:KilnfwCcacheDefaultDir = "C:\wt\.ccache"
$script:KilnfwCcacheBaseDir = "C:\wt"

# The ccache executable: whatever is on PATH (after the ESP-IDF profile it is
# the IDF tools copy, which idf.py's rules.ninja launches by name), else the
# newest ESP-IDF tools install -- check_00 configures ccache before loading
# the profile, because the SaftyFW slot build runs first and must keep the
# non-IDF cmake/ninja it has always used.
function Get-KilnfwCcacheExe {
    $cmd = Get-Command "ccache" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    $found = @(Get-ChildItem -Path "C:\Espressif\tools\ccache\*\*\ccache.exe" -ErrorAction SilentlyContinue |
        Sort-Object { $_.Directory.Parent.Name } -Descending)
    if ($found.Count -gt 0) { return $found[0].FullName }
    return $null
}

# KILNCTL_CCACHE_DISABLE=1 comparison runs: clear every inherited CCACHE_*
# setting and make ccache a pure passthrough (the pre-2026-10-08 behaviour).
function Disable-KilnfwCcache {
    foreach ($item in @(Get-ChildItem Env: | Where-Object { $_.Name -like "CCACHE_*" })) {
        Remove-Item -LiteralPath ("Env:" + $item.Name) -ErrorAction SilentlyContinue
    }
    $env:CCACHE_DISABLE = "1"
}

function Enable-KilnfwCcache {
    param(
        [string]$CacheDir = $(if ($env:KILNCTL_CCACHE_DIR) { $env:KILNCTL_CCACHE_DIR } else { $script:KilnfwCcacheDefaultDir }),
        [string]$StatsLog = ""
    )
    foreach ($item in @(Get-ChildItem Env: | Where-Object { $_.Name -like "CCACHE_*" })) {
        Remove-Item -LiteralPath ("Env:" + $item.Name) -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path -LiteralPath $CacheDir)) {
        New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
    }
    $env:CCACHE_DIR = $CacheDir
    $env:CCACHE_CONFIGPATH = Join-Path $CacheDir "kilnctl-no-config-file-expected-here.conf"
    $env:CCACHE_BASEDIR = $script:KilnfwCcacheBaseDir
    $env:CCACHE_NOHASHDIR = "1"
    $env:CCACHE_NODIRECT = "1"
    $env:CCACHE_MAXSIZE = "5G"
    if ($StatsLog) { $env:CCACHE_STATSLOG = $StatsLog }
}

# Returns an array of problem strings; empty means the effective configuration
# is exactly the one the staleness argument above was made for.
function Get-KilnfwCcacheConfigProblems {
    param([Parameter(Mandatory = $true)][string]$CcacheExe)
    $expected = [ordered]@{
        "base_dir"                   = $script:KilnfwCcacheBaseDir
        "hash_dir"                   = "false"
        "sloppiness"                 = ""
        "ignore_options"             = ""
        "ignore_headers_in_manifest" = ""
        "compiler_check"             = "mtime"
        "depend_mode"                = "false"
        "direct_mode"                = "false"
        "recache"                    = "false"
        "read_only"                  = "false"
        "read_only_direct"           = "false"
        "hard_link"                  = "false"
        "file_clone"                 = "false"
        "remote_storage"             = ""
        "remote_only"                = "false"
        "prefix_command"             = ""
        "prefix_command_cpp"         = ""
        "extra_files_to_hash"        = ""
        "namespace"                  = ""
        "disable"                    = "false"
        "compiler"                   = ""
        "cpp_extension"              = ""
        "keep_comments_cpp"          = "false"
        "pch_external_checksum"      = "false"
    }
    $problems = @()
    $lines = @(& $CcacheExe -p 2>&1)
    if ($LASTEXITCODE -ne 0) {
        return @("'ccache -p' failed (exit $LASTEXITCODE): $($lines -join ' | ')")
    }
    $seen = @{}
    foreach ($line in $lines) {
        $s = "$line"
        if ($s -notmatch '^\((?<origin>[^)]*)\)\s+(?<key>[a-z_]+)\s+=\s?(?<val>.*)$') { continue }
        $origin = $Matches.origin; $key = $Matches.key; $val = $Matches.val.Trim()
        $seen[$key] = $val
        if ($origin -ne "default" -and $origin -ne "environment") {
            $problems += "ccache setting '$key' comes from '$origin', not from this helper's environment -- a config file is in effect"
        }
        if ($expected.Contains($key) -and $null -ne $expected[$key] -and $val -ne $expected[$key]) {
            $problems += "ccache setting '$key' is '$val', expected '$($expected[$key])'"
        }
    }
    foreach ($key in $expected.Keys) {
        if ($null -ne $expected[$key] -and -not $seen.ContainsKey($key)) {
            $problems += "ccache -p did not report '$key' -- cannot confirm it is safe (ccache version changed?)"
        }
    }
    if ($seen.Count -lt 20) {
        $problems += "ccache -p reported only $($seen.Count) settings -- output format not understood, refusing to treat the configuration as verified"
    }
    return $problems
}

# Counts per-compile results from a CCACHE_STATSLOG file written during one
# run. Returns a hashtable result-name -> count.
function Get-KilnfwCcacheStatsLogCounts {
    param([Parameter(Mandatory = $true)][string]$StatsLog)
    $counts = @{}
    if (-not (Test-Path -LiteralPath $StatsLog)) { return $counts }
    foreach ($line in [System.IO.File]::ReadAllLines($StatsLog)) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith("#")) { continue }
        if ($counts.ContainsKey($t)) { $counts[$t]++ } else { $counts[$t] = 1 }
    }
    return $counts
}
