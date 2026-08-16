<#
.SYNOPSIS
    One-time setup for a fresh clone of kilnCtl.

.DESCRIPTION
    Everything in this repository is committed with portable paths. The parts
    that cannot be portable -- where ESP-IDF is installed, which clangd and
    OpenOCD binaries to use, which COM ports the boards are on -- are discovered
    here and written into generated files that are gitignored.

    Run from the repository root:

        powershell -ExecutionPolicy Bypass -File tools/setup.ps1

    Windows PowerShell 5.1 is enough; pwsh 7 also works. Add -WhatIf to see
    what it would write without writing it.

    Re-running is safe and is the right move after an ESP-IDF upgrade: the
    discovered paths carry a toolchain version in them and go stale.

.PARAMETER IdfPath
    ESP-IDF checkout. Defaults to $env:IDF_PATH, then a search of common
    install locations.

.PARAMETER EspToolsRoot
    Where the ESP-IDF installer put clangd / OpenOCD / the xtensa toolchain.
    Defaults to $env:IDF_TOOLS_PATH, then C:\Espressif, then ~\.espressif.

.PARAMETER SkipVenv
    Do not create or sync the PcTools virtual environment.

.PARAMETER WhatIf
    Report what would be written without writing anything.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string] $IdfPath,
    [string] $EspToolsRoot,
    [switch] $SkipVenv
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot
try {

$script:Problems = @()
function Warn([string]$m) { $script:Problems += $m; Write-Host "  ! $m" -ForegroundColor Yellow }
function Ok  ([string]$m) { Write-Host "  + $m" -ForegroundColor Green }
function Step([string]$m) { Write-Host "`n== $m" -ForegroundColor Cyan }

# ---------------------------------------------------------------- discovery --

function Find-FirstDir([string[]]$Candidates) {
    foreach ($c in $Candidates) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Container)) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return $null
}

# Toolchain directories carry a version in the name, so glob and take the
# newest rather than pinning a version that will be wrong after an upgrade.
function Find-NewestUnder([string]$Root, [string]$RelativeGlob, [string]$LeafGlob) {
    if (-not $Root -or -not (Test-Path -LiteralPath $Root)) { return $null }
    $hits = Get-ChildItem -Path (Join-Path $Root $RelativeGlob) -Filter $LeafGlob `
                          -Recurse -File -ErrorAction SilentlyContinue
    if (-not $hits) { return $null }
    return ($hits | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}

Step 'Locating ESP-IDF'

if (-not $IdfPath) {
    $IdfPath = Find-FirstDir @(
        $env:IDF_PATH,
        'C:\esp\v6.0.2\esp-idf',
        (Join-Path $env:USERPROFILE 'esp\esp-idf'),
        'C:\Espressif\frameworks\esp-idf'
    )
    if (-not $IdfPath) {
        # Last resort: any esp-idf under C:\esp, newest first.
        $guess = Get-ChildItem 'C:\esp' -Directory -ErrorAction SilentlyContinue |
                 ForEach-Object { Join-Path $_.FullName 'esp-idf' } |
                 Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($guess) { $IdfPath = $guess }
    }
}
if ($IdfPath) { Ok "IDF_PATH = $IdfPath" }
else { Warn 'ESP-IDF not found. Install it, or re-run with -IdfPath <dir>. KilnFW will not build.' }

if (-not $EspToolsRoot) {
    $EspToolsRoot = Find-FirstDir @(
        $env:IDF_TOOLS_PATH,
        'C:\Espressif',
        (Join-Path $env:USERPROFILE '.espressif')
    )
}
if ($EspToolsRoot) { Ok "IDF_TOOLS_PATH = $EspToolsRoot" }
else { Warn 'ESP-IDF tools directory not found. clangd, OpenOCD and the compiler path will be blank.' }

# The installer splits these across two roots on some machines: the frameworks
# and python env under ~\.espressif, the binaries under C:\Espressif.
$toolRoots = @($EspToolsRoot, 'C:\Espressif', (Join-Path $env:USERPROFILE '.espressif')) |
             Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique

Step 'Locating toolchain binaries'

$clangd = $null; $openocd = $null; $xtensaGcc = $null; $idfPython = $null
foreach ($r in $toolRoots) {
    if (-not $clangd)    { $clangd    = Find-NewestUnder $r 'tools\esp-clang'      'clangd.exe' }
    if (-not $openocd)   { $openocd   = Find-NewestUnder $r 'tools\openocd-esp32'  'openocd.exe' }
    if (-not $xtensaGcc) { $xtensaGcc = Find-NewestUnder $r 'tools\xtensa-esp-elf' 'xtensa-esp32s3-elf-gcc.exe' }
    if (-not $idfPython) { $idfPython = Find-NewestUnder $r 'python_env'           'python.exe' }
}

if ($clangd)    { Ok "clangd    = $clangd" }    else { Warn 'clangd not found; IntelliSense will fall back to the C/C++ extension.' }
if ($openocd)   { Ok "openocd   = $openocd" }   else { Warn 'openocd not found; the JTAG flash and debug tasks will not run.' }
if ($xtensaGcc) { Ok "xtensa-gcc = $xtensaGcc" } else { Warn 'xtensa toolchain not found.' }
if ($idfPython) { Ok "idf python = $idfPython" } else { Warn 'ESP-IDF python env not found; the ESP-IDF MCP server will not start.' }

$openocdScripts = $null
if ($openocd) {
    $openocdScripts = Join-Path (Split-Path -Parent (Split-Path -Parent $openocd)) 'share\openocd\scripts'
    if (-not (Test-Path -LiteralPath $openocdScripts)) { $openocdScripts = $null; Warn 'OpenOCD scripts directory not found next to the binary.' }
}

$idfPythonEnv = $null
if ($idfPython) { $idfPythonEnv = Split-Path -Parent (Split-Path -Parent $idfPython) }

# ------------------------------------------------------------ env variables --

Step 'Setting user environment variables'

$vars = [ordered]@{
    'IDF_PATH'                = $IdfPath
    'IDF_TOOLS_PATH'          = $EspToolsRoot
    'IDF_PYTHON_ENV_PATH'     = $idfPythonEnv
    'KILNCTL_CLANGD'          = $clangd
    'KILNCTL_OPENOCD'         = $openocd
    'KILNCTL_OPENOCD_SCRIPTS' = $openocdScripts
    'KILNCTL_XTENSA_GCC'      = $xtensaGcc
}
foreach ($k in $vars.Keys) {
    $v = $vars[$k]
    if (-not $v) { continue }
    if ($PSCmdlet.ShouldProcess("user environment variable $k", 'set')) {
        [Environment]::SetEnvironmentVariable($k, $v, 'User')
        Set-Item -Path "env:$k" -Value $v
    }
    Ok "$k"
}
Write-Host '  (VS Code and terminals must be restarted before these are visible)' -ForegroundColor DarkGray

# -------------------------------------------------------- generated configs --

Step 'Generating machine-specific config from templates'

function Expand-Template([string]$TemplatePath, [string]$OutPath, [hashtable]$Map) {
    if (-not (Test-Path -LiteralPath $TemplatePath)) { Warn "missing template: $TemplatePath"; return }
    $text = Get-Content -LiteralPath $TemplatePath -Raw
    foreach ($k in $Map.Keys) {
        # JSON needs backslashes doubled.
        $escaped = ($Map[$k] -replace '\\', '\\')
        $text = $text.Replace("@@$k@@", $escaped)
    }
    $unresolved = [regex]::Matches($text, '@@([A-Z_]+)@@') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique
    if ($unresolved) { Warn "$OutPath still has unresolved placeholders: $($unresolved -join ', ')" }
    if ($PSCmdlet.ShouldProcess($OutPath, 'write')) {
        $dir = Split-Path -Parent $OutPath
        if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
        Set-Content -LiteralPath $OutPath -Value $text -Encoding utf8 -NoNewline
    }
    Ok $OutPath
}

# Windows PowerShell 5.1 has no null-coalescing operator, and this script has to
# run on the stock shell rather than requiring pwsh 7.
function Get-OrEmpty($v) { if ($v) { return $v } else { return '' } }

$map = @{
    'IDF_PATH'                = (Get-OrEmpty $IdfPath)
    'IDF_TOOLS_PATH'          = (Get-OrEmpty $EspToolsRoot)
    'IDF_PYTHON_ENV_PATH'     = (Get-OrEmpty $idfPythonEnv)
    'IDF_PYTHON'              = (Get-OrEmpty $idfPython)
    'CLANGD'                  = (Get-OrEmpty $clangd)
    'OPENOCD'                 = (Get-OrEmpty $openocd)
    'OPENOCD_SCRIPTS'         = (Get-OrEmpty $openocdScripts)
    'XTENSA_GCC'              = (Get-OrEmpty $xtensaGcc)
}

Expand-Template 'templates/mcp.json.in'                          '.mcp.json'                                                  $map
Expand-Template 'templates/KilnFW.settings.json.in'              'firmware/KilnFW/.vscode/settings.json'                       $map
Expand-Template 'templates/KilnFW.c_cpp_properties.json.in'      'firmware/KilnFW/.vscode/c_cpp_properties.json'               $map
Expand-Template 'templates/KilnFW.tasks.json.in'                 'firmware/KilnFW/.vscode/tasks.json'                          $map
Expand-Template 'templates/UnitTestFw.settings.json.in'          'firmware/UnitTestFw/UnitTest/.vscode/settings.json'          $map
Expand-Template 'templates/UnitTestFw.c_cpp_properties.json.in'  'firmware/UnitTestFw/UnitTest/.vscode/c_cpp_properties.json'  $map
Expand-Template 'templates/UnitTestFwOuter.settings.json.in'     'firmware/UnitTestFw/.vscode/settings.json'                   $map

# ------------------------------------------------------------- submodules ----

Step 'Submodules'

# Check the outcome, not the exit code. `git submodule update --init` silently
# SKIPS any gitlink with no .gitmodules entry and still exits 0 -- which is
# exactly what happened when four Claude Code agent worktrees were accidentally
# committed as gitlinks: this step reported success while `git submodule status`
# failed outright. An exit code that cannot distinguish "did the work" from
# "found nothing to do" is not a check.
if ($PSCmdlet.ShouldProcess('git submodules', 'init and update')) {
    git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { Warn 'git submodule update returned non-zero.' }
}

# The verification below is read-only, so it runs even under -WhatIf. That is
# deliberate: a dry run should still be able to tell you what is broken.
if ($true) {
    # Every path declared in .gitmodules must exist, be non-empty, and resolve
    # to a commit.
    $declared = @()
    if (Test-Path .gitmodules) {
        $declared = Select-String -Path .gitmodules -Pattern '^\s*path\s*=\s*(.+?)\s*$' |
                    ForEach-Object { $_.Matches[0].Groups[1].Value }
    }
    if (-not $declared) {
        Warn 'No submodule paths declared in .gitmodules -- expected at least two.'
    }
    foreach ($sm in $declared) {
        if (-not (Test-Path -LiteralPath $sm)) { Warn "submodule missing: $sm"; continue }
        if (-not (Get-ChildItem -LiteralPath $sm -Force -ErrorAction SilentlyContinue)) {
            Warn "submodule directory is empty (not checked out): $sm"; continue
        }
        git -C $sm rev-parse HEAD *> $null
        if ($LASTEXITCODE -ne 0) { Warn "submodule has no resolvable HEAD: $sm" }
        else { Ok "submodule $sm" }
    }

    # A tracked gitlink with no .gitmodules entry breaks `git submodule status`
    # for everyone, so name it here rather than letting it surface later.
    $links = git ls-files -s | Where-Object { $_ -match '^160000' } |
             ForEach-Object { ($_ -split "`t", 2)[1] }
    foreach ($l in $links) {
        if ($declared -notcontains $l) {
            Warn "tracked gitlink with no .gitmodules entry: $l -- this breaks 'git submodule status'. Untrack it with: git rm --cached '$l'"
        }
    }
}

# ------------------------------------------------------------------ venvs ----

if (-not $SkipVenv) {
    Step 'PcTools virtual environment'
    $uv = (Get-Command uv -ErrorAction SilentlyContinue)
    if (-not $uv) {
        Warn 'uv is not on PATH. Install it (https://docs.astral.sh/uv/) then re-run, or create the venv by hand.'
    } else {
        if ($PSCmdlet.ShouldProcess('tools/PcTools', 'uv sync')) {
            uv sync --project tools/PcTools
            if ($LASTEXITCODE -ne 0) { Warn 'uv sync failed for tools/PcTools.' }
        }

        # Same principle: prove the package imports rather than trusting the
        # sync's exit code. This is also what catches a stale editable-install
        # .pth left behind by a directory move.
        $venvPy = Join-Path $RepoRoot 'tools\PcTools\.venv\Scripts\python.exe'
        if (-not (Test-Path -LiteralPath $venvPy)) {
            Warn 'tools/PcTools/.venv/Scripts/python.exe not found after sync.'
        } else {
            & $venvPy -c 'import kilnctrl' *> $null
            if ($LASTEXITCODE -ne 0) {
                Warn "tools/PcTools synced but 'import kilnctrl' fails. A stale editable-install path is the usual cause; 'uv sync --reinstall --project tools/PcTools' fixes it."
            } else { Ok 'tools/PcTools synced, import kilnctrl works' }
        }
    }
}

# ------------------------------------------------------------------ KiCad ----

Step 'KiCad library paths'
$fpTable = 'hardware/mainBoard/fp-lib-table'
if (Test-Path $fpTable) {
    $bad = Select-String -Path $fpTable, 'hardware/mainBoard/sym-lib-table' -Pattern '"[A-Za-z]:[\\/]' -ErrorAction SilentlyContinue
    if ($bad) { Warn 'A mainBoard library table has an absolute path again. It must be ${KIPRJMOD}-relative -- see docs/REPO_LAYOUT.md B1.' }
    else { Ok 'mainBoard library tables are project-relative' }
}

# ----------------------------------------------------------------- summary ---

Write-Host ''
if ($script:Problems.Count -eq 0) {
    Write-Host 'Setup complete. Restart VS Code, then open kilnCtl.code-workspace.' -ForegroundColor Green
} else {
    Write-Host "Setup finished with $($script:Problems.Count) thing(s) needing attention:" -ForegroundColor Yellow
    $script:Problems | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    Write-Host 'Nothing above is fatal to editing the repository; each one disables a specific tool.' -ForegroundColor DarkGray
}
Write-Host 'Serial ports are deliberately not configured here -- PcTools discovers them, and the' -ForegroundColor DarkGray
Write-Host 'ESP-IDF extension prompts. See docs/SETUP.md.' -ForegroundColor DarkGray

} finally { Pop-Location }
