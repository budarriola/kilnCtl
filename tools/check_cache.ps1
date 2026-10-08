# check_cache.ps1 -- machine-wide, per-content cache of check PASS results for
# tools/run_all_checks.ps1. Dot-source it; it defines functions only.
#
# PROBLEM (2026-10-07): many agent sessions each run `run_all_checks.ps1 -Fast`
# on byte-identical content (same origin/main, or a worktree that differs only
# in commit message) and every one re-runs ~150 identical checks.
#
# KEY. sha256 of: schema | repo-relative check path | git TREE hash of HEAD |
# run mode (fast|full) | environment fingerprint. The TREE hash (not the commit
# hash) is used so two commits with identical content share entries.
# FINGERPRINT. PowerShell version, git version, OS version, ESP-IDF
# (IDF_PATH + its version.txt), MSVC (vswhere installationVersion /
# VCToolsVersion), the system python version, the PcTools venv python version,
# node version, and every KILNCTL_* environment variable (name and value)
# except cache controls, credentials and gate tuning. Any difference is a miss.
#
# WHEN THE CACHE IS USED AT ALL. Only when the working tree is exactly HEAD:
# `git status --porcelain --untracked-files=normal` is empty (so no modified or
# staged tracked file, no untracked file that is not gitignored) and no tracked
# file carries the assume-unchanged / skip-worktree / sparse bits that would
# let `git status` miss a modification. Anything else (including any git
# error) disables the cache for the whole run: when unsure, miss.
#
# WHAT IS STORED. PASS only, never FAIL / SKIP / SKIP-FAST / BUSY. Entries are
# collected in memory during the run and written once at the end, after
# re-verifying that the tree is STILL clean and at the same tree hash (a check
# that dirtied the tree, or a concurrent edit, means nothing is stored).
#
# OPT-IN. A check is cacheable only if its file carries a line
#       # checkcache: ok
# The marker asserts: "my result is a pure function of the git-tracked content
# of the tree plus the tool versions in the fingerprint". It must NOT be added
# to a check that reads/writes the live board, network, MCP servers, the clock
# or other external state, a build/ output, an installed-package state (the
# PcTools venv packages), a toolchain compile/link, or a build gate/lock.
# See docs/agent_rules/COMMON.md "Check result cache" for the excluded groups.
#
# CONTROLS. KILNCTL_CHECKCACHE=0 or run_all_checks.ps1 -NoCache disables it.
# Entries expire after 7 days (checked on read; also pruned). The directory is
# capped (entry count and bytes), oldest first. Directory: C:\wt\.checkcache,
# override KILNCTL_CHECKCACHE_DIR (the unit test uses this).
#
# STORE LAYOUT. One JSON file per entry, <key>.json, written to a temp file in
# the same directory and then moved into place with File.Move, which refuses to
# overwrite: the first writer wins and a reader can never see a partial file.

$script:CheckCacheSchema = 1
$script:CheckCacheMaxEntries = 4000
$script:CheckCacheMaxBytes = 64MB

function Get-CheckCacheDir {
    $d = $env:KILNCTL_CHECKCACHE_DIR
    if ([string]::IsNullOrWhiteSpace($d)) { $d = "C:\wt\.checkcache" }
    return $d
}

function Get-CheckCacheSha256 {
    param([Parameter(Mandatory = $true)][string]$Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Text))
    } finally { $sha.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString("x2") }) -join "")
}

function Get-CheckCacheToolVersion {
    param([string]$Exe, [string[]]$ToolArgs = @("--version"))
    if ([string]::IsNullOrWhiteSpace($Exe) -or -not (Get-Command $Exe -ErrorAction SilentlyContinue)) { return "absent" }
    try {
        $o = & $Exe @ToolArgs 2>&1 | Out-String
        return ($o.Trim() -replace '\s+', ' ')
    } catch { return "error" }
}

# Tool versions cannot change within one process, and probing them (python,
# node, vswhere, git) costs about a second, so memoize per python path.
$script:CheckCacheToolLines = @{}
function Get-CheckCacheToolLines {
    param([string]$PcToolsPython = "")
    if ($script:CheckCacheToolLines.ContainsKey($PcToolsPython)) { return $script:CheckCacheToolLines[$PcToolsPython] }
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("ps=" + $PSVersionTable.PSVersion.ToString())
    $lines.Add("os=" + [Environment]::OSVersion.Version.ToString())
    $lines.Add("git=" + (Get-CheckCacheToolVersion -Exe "git"))

    $idf = $env:IDF_PATH
    $idfVer = "none"
    if (-not [string]::IsNullOrWhiteSpace($idf)) {
        $vf = Join-Path $idf "version.txt"
        $idfVer = if (Test-Path -LiteralPath $vf) { (Get-Content -LiteralPath $vf -Raw).Trim() } else { "no-version-txt" }
    }
    $lines.Add("idf=$idf|$idfVer|$($env:IDF_TOOLS_PATH)")

    $msvc = "none"
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        try { $msvc = ((& $vswhere -latest -products * -property installationVersion 2>$null) | Out-String).Trim() } catch { $msvc = "vswhere-error" }
    }
    $lines.Add("msvc=$msvc|$($env:VCToolsVersion)")

    $lines.Add("python=" + (Get-CheckCacheToolVersion -Exe "python"))
    $venv = "none"
    if (-not [string]::IsNullOrWhiteSpace($PcToolsPython) -and (Test-Path -LiteralPath $PcToolsPython)) {
        $venv = Get-CheckCacheToolVersion -Exe $PcToolsPython
    }
    $lines.Add("pctools_python=$venv")
    $lines.Add("node=" + (Get-CheckCacheToolVersion -Exe "node"))
    $script:CheckCacheToolLines[$PcToolsPython] = $lines
    return $lines
}

function Get-CheckCacheFingerprint {
    param([string]$PcToolsPython = "")
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($l in (Get-CheckCacheToolLines -PcToolsPython $PcToolsPython)) { $lines.Add($l) }

    # Behaviour-changing env vars. Cache controls, credentials, session/gate
    # tuning are excluded: they cannot change what a source check decides, and
    # including them would make every agent's fingerprint differ.
    $skip = '^KILNCTL_(CHECKCACHE|.*PASSWORD|.*TOTP|.*SECRET|.*TOKEN|.*USERNAME|BUILD_GATE|LIGHT_GATE|HEAVY_GATE|PUSHED_STAMP_DIR|WEB_)'
    $envLines = Get-ChildItem Env: | Where-Object { $_.Name -like 'KILNCTL_*' -and $_.Name -notmatch $skip } |
        Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }
    foreach ($e in $envLines) { $lines.Add("env:$e") }
    return (Get-CheckCacheSha256 -Text ($lines -join "`n"))
}

# Returns the context object. Enabled=$false with a Reason when the cache must
# not be used for this run.
function Initialize-CheckCache {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [switch]$Fast,
        [switch]$NoCache,
        [string]$PcToolsPython = "",
        [int]$MaxAgeDays = 7
    )
    $ctx = [PSCustomObject]@{
        Enabled     = $false
        Reason      = ""
        RepoRoot    = $RepoRoot
        Dir         = Get-CheckCacheDir
        Tree        = ""
        Mode        = $(if ($Fast) { "fast" } else { "full" })
        Fingerprint = ""
        MaxAgeDays  = $MaxAgeDays
        Worktree    = (Split-Path -Leaf $RepoRoot)
        Pending     = (New-Object System.Collections.ArrayList)
        Hits        = (New-Object System.Collections.ArrayList)
        Markers     = @{}
    }
    if ($NoCache) { $ctx.Reason = "-NoCache"; return $ctx }
    if ($env:KILNCTL_CHECKCACHE -eq "0") { $ctx.Reason = "KILNCTL_CHECKCACHE=0"; return $ctx }

    $state = Get-CheckCacheTreeState -RepoRoot $RepoRoot
    if (-not $state.Clean) { $ctx.Reason = $state.Reason; return $ctx }
    $ctx.Tree = $state.Tree
    $ctx.Fingerprint = Get-CheckCacheFingerprint -PcToolsPython $PcToolsPython
    $ctx.Enabled = $true
    return $ctx
}

# Clean = the working tree is exactly HEAD's tree. Fails closed on any doubt.
function Get-CheckCacheTreeState {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $bad = { param($why) [PSCustomObject]@{ Clean = $false; Tree = ""; Reason = $why } }
    try {
        $tree = (& git -C $RepoRoot rev-parse "HEAD^{tree}" 2>$null | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or $tree -cnotmatch '^[0-9a-f]{40,64}$') { return (& $bad "cannot resolve HEAD tree") }
        $st = @(& git -C $RepoRoot status --porcelain --untracked-files=normal --ignore-submodules=none 2>$null)
        if ($LASTEXITCODE -ne 0) { return (& $bad "git status failed") }
        if ($st.Count -gt 0) { return (& $bad "working tree not clean ($($st.Count) change(s), e.g. $($st[0]))") }
        # assume-unchanged (lowercase tag) / skip-worktree (S) hide edits from status.
        $hidden = @(& git -C $RepoRoot ls-files -v 2>$null | Where-Object { $_ -cmatch '^[a-zS] ' })
        if ($LASTEXITCODE -ne 0) { return (& $bad "git ls-files -v failed") }
        if ($hidden.Count -gt 0) { return (& $bad "tracked files hidden from git status (assume-unchanged/skip-worktree): $($hidden[0])") }
        return [PSCustomObject]@{ Clean = $true; Tree = $tree; Reason = "" }
    } catch {
        return (& $bad "tree check threw: $($_.Exception.Message)")
    }
}

function Test-CheckCacheable {
    param($Ctx, [Parameter(Mandatory = $true)][string]$FullPath)
    if ($Ctx.Markers.ContainsKey($FullPath)) { return $Ctx.Markers[$FullPath] }
    $ok = $false
    try {
        $t = [System.IO.File]::ReadAllText($FullPath)
        $ok = [regex]::IsMatch($t, '(?m)^\s*(#|//)\s*checkcache:\s*ok\s*$')
    } catch { $ok = $false }
    $Ctx.Markers[$FullPath] = $ok
    return $ok
}

function Get-CheckCacheKey {
    param($Ctx, [Parameter(Mandatory = $true)][string]$Rel)
    return (Get-CheckCacheSha256 -Text ("v$script:CheckCacheSchema|$Rel|$($Ctx.Tree)|$($Ctx.Mode)|$($Ctx.Fingerprint)"))
}

# Returns the entry iff a valid, unexpired PASS exists for exactly this key.
function Find-CheckCacheHit {
    param($Ctx, [Parameter(Mandatory = $true)][string]$Rel)
    if (-not $Ctx.Enabled) { return $null }
    $full = Join-Path $Ctx.RepoRoot $Rel
    if (-not (Test-CheckCacheable -Ctx $Ctx -FullPath $full)) { return $null }
    $key = Get-CheckCacheKey -Ctx $Ctx -Rel $Rel
    $path = Join-Path $Ctx.Dir "$key.json"
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $e = $null
    try { $e = [System.IO.File]::ReadAllText($path) | ConvertFrom-Json -ErrorAction Stop } catch { $e = $null }
    $valid = $false
    if ($null -ne $e) {
        $valid = ($e.schema -eq $script:CheckCacheSchema) -and ($e.result -ceq "PASS") -and
                 ($e.key -ceq $key) -and ($e.check -ceq $Rel) -and ($e.tree -ceq $Ctx.Tree) -and
                 ($e.mode -ceq $Ctx.Mode) -and ($e.fingerprint -ceq $Ctx.Fingerprint)
    }
    $when = [DateTime]::MinValue
    if ($valid) {
        try { $when = [DateTime]::Parse([string]$e.time_utc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime() } catch { $valid = $false }
    }
    if ($valid -and (([DateTime]::UtcNow - $when).TotalDays -gt $Ctx.MaxAgeDays -or $when -gt [DateTime]::UtcNow.AddMinutes(5))) { $valid = $false }
    if (-not $valid) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        return $null
    }
    return [PSCustomObject]@{ When = $when; Worktree = [string]$e.worktree; DurationSec = [double]$e.duration_s; Entry = $e }
}

# Queue a result for storage. Only a PASS of a marked check is ever queued.
function Add-CheckCacheResult {
    param($Ctx, [Parameter(Mandatory = $true)][string]$Rel, [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Bucket,
          [double]$DurationSec = 0, [string]$OutputText = "")
    if (-not $Ctx.Enabled) { return $false }
    if ($Bucket -cne "pass") { return $false }
    if (-not (Test-CheckCacheable -Ctx $Ctx -FullPath (Join-Path $Ctx.RepoRoot $Rel))) { return $false }
    $tail = (($OutputText -split "`r?`n" | Select-Object -Last 15) -join "`n")
    if ($tail.Length -gt 2000) { $tail = $tail.Substring($tail.Length - 2000) }
    [void]$Ctx.Pending.Add([PSCustomObject]@{ Rel = $Rel; DurationSec = $DurationSec; Tail = $tail })
    return $true
}

# Atomic, first-writer-wins write of one entry. Returns $true if this call
# created the file.
function Write-CheckCacheEntry {
    param([Parameter(Mandatory = $true)][string]$Dir, [Parameter(Mandatory = $true)][string]$Key, [Parameter(Mandatory = $true)]$Entry)
    if (-not (Test-Path -LiteralPath $Dir)) { New-Item -ItemType Directory -Path $Dir -Force | Out-Null }
    $final = Join-Path $Dir "$Key.json"
    $tmp = Join-Path $Dir (".{0}.{1}.{2}.tmp" -f $Key, $PID, [guid]::NewGuid().ToString("N"))
    [System.IO.File]::WriteAllText($tmp, ($Entry | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false)))
    try {
        [System.IO.File]::Move($tmp, $final)
        return $true
    } catch [System.IO.IOException] {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        return $false
    }
}

# Flush queued PASSes after re-verifying the tree. Returns the number written.
function Save-CheckCache {
    param($Ctx)
    if (-not $Ctx.Enabled -or $Ctx.Pending.Count -eq 0) { return 0 }
    $now = Get-CheckCacheTreeState -RepoRoot $Ctx.RepoRoot
    if (-not $now.Clean -or $now.Tree -cne $Ctx.Tree) {
        Write-Host "check cache: tree changed or dirtied during the run -- storing nothing ($($now.Reason))" -ForegroundColor Yellow
        $Ctx.Pending.Clear()
        return 0
    }
    $n = 0
    try {
        foreach ($p in $Ctx.Pending) {
            $key = Get-CheckCacheKey -Ctx $Ctx -Rel $p.Rel
            $entry = [ordered]@{
                schema      = $script:CheckCacheSchema
                key         = $key
                result      = "PASS"
                check       = $p.Rel
                tree        = $Ctx.Tree
                mode        = $Ctx.Mode
                fingerprint = $Ctx.Fingerprint
                time_utc    = (Get-Date).ToUniversalTime().ToString("o")
                worktree    = $Ctx.Worktree
                host        = $env:COMPUTERNAME
                duration_s  = [Math]::Round($p.DurationSec, 2)
                output_tail = $p.Tail
            }
            if (Write-CheckCacheEntry -Dir $Ctx.Dir -Key $key -Entry $entry) { $n++ }
        }
        Invoke-CheckCachePrune -Dir $Ctx.Dir -MaxAgeDays $Ctx.MaxAgeDays
    } catch {
        # The cache is an optimisation: a store failure must never fail the run.
        Write-Host "check cache: store failed ($($_.Exception.Message)) -- continuing without it" -ForegroundColor Yellow
    }
    $Ctx.Pending.Clear()
    return $n
}

function Invoke-CheckCachePrune {
    param([Parameter(Mandatory = $true)][string]$Dir, [int]$MaxAgeDays = 7,
          [int]$MaxEntries = $script:CheckCacheMaxEntries, [long]$MaxBytes = $script:CheckCacheMaxBytes)
    if (-not (Test-Path -LiteralPath $Dir)) { return }
    $cut = (Get-Date).AddDays(-$MaxAgeDays)
    Get-ChildItem -LiteralPath $Dir -Filter "*.json" -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -lt $cut } | Remove-Item -Force -ErrorAction SilentlyContinue
    Get-ChildItem -LiteralPath $Dir -Filter "*.tmp" -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -lt (Get-Date).AddHours(-1) } | Remove-Item -Force -ErrorAction SilentlyContinue
    $files = @(Get-ChildItem -LiteralPath $Dir -Filter "*.json" -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime)
    $bytes = ($files | Measure-Object Length -Sum).Sum
    $i = 0
    while (($files.Count - $i) -gt $MaxEntries -or $bytes -gt $MaxBytes) {
        if ($i -ge $files.Count) { break }
        $bytes -= $files[$i].Length
        Remove-Item -LiteralPath $files[$i].FullName -Force -ErrorAction SilentlyContinue
        $i++
    }
}
