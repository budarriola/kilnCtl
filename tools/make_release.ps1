# make_release.ps1 -- build, package and (optionally) publish a kilnCtl GitHub release.
# Design: GITHUB_RELEASE_UPDATE_PLAN.md sections 6-8; how-to: docs/RELEASING.md.
#
# DEFAULT IS A DRY RUN: every gate runs, the images are collected, release.json and
# SHA256SUMS are written to logs\release\<tag>\ and validated, and the script prints
# what -Publish WOULD send. Nothing leaves the machine and no tag/release is created
# unless -Publish is given.
#
# Gates (each a hard refusal; none is skipped by -Publish):
#   * tag matches ^v\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$
#   * working tree is clean (git status --porcelain, logs/release excluded)
#   * HEAD equals origin/main (after a read-only `git fetch origin main`)
#   * the tag exists neither locally nor on origin (git ls-remote)
#   * KilnCtrl.bin <= 0x400000 (the 4 MB `app` partition; staging has its own partition)
# -DevDryRun downgrades the git gates to warnings so the packaging path can be
# exercised from a scratch worktree; it is refused together with -Publish.
#
# Build: unless -SkipBuild, runs the two standing target-build checks
# (firmware/KilnFW/App/test/check_00_kilnfw_target_build.ps1 and
# check_00_kilnfw_recovery_target_build.ps1). Those build in isolated C:\wt\checkbuild_*
# trees under the repo's build gate/lock and publish into this tree's build\ dirs, so
# idf.py is never invoked by hand here. -SkipBuild uses existing artifacts
# (-BuildDir / -RecoveryBin override the default locations).
#
# -Publish (token from env KILNCTL_GITHUB_TOKEN, never printed): POST a DRAFT release
# with target_commitish = the commit, upload every asset, re-download each one and
# compare sha256, and only then PATCH draft=false. Any mismatch leaves the draft in
# place (delete it by hand) and exits non-zero. -WhatIf prints the REST plan, calls nothing.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$Tag,
    [string]$Repo = "budarriola/kilnCtl",
    [ValidateSet("stable", "pre")][string]$Channel = "stable",
    [switch]$SkipBuild,
    [string]$BuildDir,
    [string]$RecoveryBin,
    [switch]$Publish,
    [switch]$DevDryRun,
    [switch]$LoadFunctionsOnly
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$SemverRe = '^v\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$'
$MaxAppSize = 0x400000

function Fail([string]$msg) {
    Write-Host ""
    Write-Host "REFUSED: $msg" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------- publish helpers
# Kept as functions that call Invoke-RestMethod / Invoke-WebRequest by plain name so a
# test can shadow both (tools/check_release_manifest.ps1 does exactly that).

function Get-ReleaseApiHeaders([string]$Token, [string]$Accept = "application/vnd.github+json") {
    return @{ Authorization = "Bearer $Token"; Accept = $Accept; "X-GitHub-Api-Version" = "2022-11-28" }
}

function Get-AssetToFile([string]$Token, [string]$AssetApiUrl, [string]$OutFile) {
    # The API asset URL 302s to a signed blob URL; do not forward the token there.
    $hdr = Get-ReleaseApiHeaders $Token "application/octet-stream"
    $resp = $null
    try {
        $resp = Invoke-WebRequest -Uri $AssetApiUrl -Headers $hdr -MaximumRedirection 0 -UseBasicParsing
    } catch {
        $r = $_.Exception.Response
        if ($r -and ([int]$r.StatusCode -in 301, 302, 303, 307, 308)) {
            $loc = [string]$r.Headers["Location"]
            if (-not $loc) { $loc = [string]$r.Headers.Location }
            Invoke-WebRequest -Uri $loc -OutFile $OutFile -UseBasicParsing | Out-Null
            return
        }
        throw
    }
    [System.IO.File]::WriteAllBytes($OutFile, [byte[]]$resp.Content)
}

function Invoke-ReleasePublish {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory)][string]$Token,
        [Parameter(Mandatory)][string]$Repo,
        [Parameter(Mandatory)][string]$Tag,
        [Parameter(Mandatory)][string]$Commit,
        [Parameter(Mandatory)][string]$Dir,
        [bool]$Prerelease = $false,
        [string]$Body = ""
    )
    $api = "https://api.github.com/repos/$Repo"
    $hdr = Get-ReleaseApiHeaders $Token
    $files = Get-ChildItem -LiteralPath $Dir -File | Where-Object { $_.Name -ne "publish.log" } | Sort-Object Name

    if (-not $PSCmdlet.ShouldProcess("$Repo $Tag", "create draft release, upload $($files.Count) asset(s), verify, publish")) {
        Write-Host "WhatIf: POST $api/releases (draft, target_commitish $Commit)"
        foreach ($f in $files) { Write-Host "WhatIf: upload $($f.Name) ($($f.Length) B), re-download, compare sha256" }
        Write-Host "WhatIf: PATCH $api/releases/<id> draft=false"
        return
    }

    $create = @{ tag_name = $Tag; target_commitish = $Commit; name = $Tag; body = $Body
                 draft = $true; prerelease = $Prerelease } | ConvertTo-Json
    $rel = Invoke-RestMethod -Method Post -Uri "$api/releases" -Headers $hdr -Body $create -ContentType "application/json"
    $id = $rel.id
    Write-Host "created draft release id $id"

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kilnrel_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        foreach ($f in $files) {
            $name = [uri]::EscapeDataString($f.Name)
            $asset = Invoke-RestMethod -Method Post -Uri "https://uploads.github.com/repos/$Repo/releases/$id/assets?name=$name" `
                -Headers $hdr -InFile $f.FullName -ContentType "application/octet-stream"
            $want = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $dl = Join-Path $tmp $f.Name
            Get-AssetToFile $Token $asset.url $dl
            $got = (Get-FileHash -LiteralPath $dl -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($got -ne $want) {
                throw "re-downloaded $($f.Name) sha256 $got differs from local $want; draft release $id left UNPUBLISHED, delete it by hand"
            }
            Write-Host "uploaded and verified $($f.Name)"
        }
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
    Invoke-RestMethod -Method Patch -Uri "$api/releases/$id" -Headers $hdr -Body (@{ draft = $false } | ConvertTo-Json) -ContentType "application/json" | Out-Null
    Write-Host "published $Tag : https://github.com/$Repo/releases/tag/$Tag"
}

if ($LoadFunctionsOnly) { return }

# ---------------------------------------------------------------- gates
if (-not $Tag -or $Tag -cnotmatch $SemverRe) { Fail "tag '$Tag' is not semver (vMAJOR.MINOR.PATCH[-pre])." }
if ($Publish -and $DevDryRun) { Fail "-DevDryRun and -Publish are mutually exclusive." }
if ($Repo -notmatch '^[A-Za-z0-9._-]{1,39}/[A-Za-z0-9._-]{1,100}$') { Fail "repo '$Repo' is not owner/name." }

function Gate([string]$msg) {
    if ($DevDryRun) { Write-Host "WARNING (DevDryRun): $msg" -ForegroundColor Yellow } else { Fail $msg }
}

$dirty = @(& git -C $repoRoot status --porcelain -- . ":(exclude)logs/release")
if ($LASTEXITCODE -ne 0) { Fail "git status failed." }
if ($dirty.Count -gt 0) { Gate "working tree is dirty ($($dirty.Count) change(s), first: $($dirty[0])); build releases from a clean worktree (tools\worktree_mint.ps1)." }

$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
& git -C $repoRoot fetch --quiet origin main
if ($LASTEXITCODE -ne 0) { Fail "git fetch origin main failed." }
$originMain = (& git -C $repoRoot rev-parse origin/main).Trim()
if ($commit -ne $originMain) { Gate "HEAD $($commit.Substring(0,12)) is not origin/main $($originMain.Substring(0,12))." }

if ((& git -C $repoRoot tag --list $Tag)) { Fail "tag $Tag already exists locally." }
$remoteTag = @(& git -C $repoRoot ls-remote --tags origin "refs/tags/$Tag")
if ($LASTEXITCODE -ne 0) { Fail "git ls-remote --tags origin failed (cannot prove the tag is free)." }
if ($remoteTag.Count -gt 0) { Fail "tag $Tag already exists on origin." }

# ---------------------------------------------------------------- build / collect
if (-not $SkipBuild) {
    foreach ($s in @("firmware\KilnFW\App\test\check_00_kilnfw_target_build.ps1",
                     "firmware\KilnFW\App\test\check_00_kilnfw_recovery_target_build.ps1")) {
        Write-Host "building: $s"
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoRoot $s)
        if ($LASTEXITCODE -ne 0) { Fail "$s exited $LASTEXITCODE (a SKIP, exit 3, is also a refusal: a release needs real binaries)." }
    }
}
if (-not $BuildDir)    { $BuildDir = Join-Path $repoRoot "firmware\KilnFW\build" }
if (-not $RecoveryBin) { $RecoveryBin = Join-Path $repoRoot "firmware\KilnFW_recovery\build\recovery.bin" }
$appBin = Join-Path $BuildDir "KilnCtrl.bin"
$elf = Join-Path $BuildDir "KilnCtrl.elf"
foreach ($p in @($appBin, $elf, $RecoveryBin)) {
    if (-not (Test-Path -LiteralPath $p)) { Fail "missing build artifact: $p" }
}
$appSize = (Get-Item -LiteralPath $appBin).Length
if ($appSize -gt $MaxAppSize) { Fail "KilnCtrl.bin is $appSize bytes, over the $MaxAppSize (0x400000) gate." }

$outDir = Join-Path $repoRoot "logs\release\$Tag"
if (Test-Path -LiteralPath $outDir) { Fail "$outDir already exists; remove it (or pick a new tag) so no stale asset is published." }
New-Item -ItemType Directory -Path $outDir | Out-Null

$elfZip = Join-Path $outDir "KilnCtrl-$Tag.elf.zip"
Compress-Archive -LiteralPath $elf -DestinationPath $elfZip -CompressionLevel Optimal

# ---------------------------------------------------------------- manifest
$python = $null
foreach ($cand in @((Join-Path $repoRoot "tools\PcTools\.venv\Scripts\python.exe"))) {
    if (Test-Path -LiteralPath $cand) { $python = $cand }
}
if (-not $python) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $cmd) { Fail "no python (tools\PcTools\.venv or PATH); release_manifest.py needs one." }
    $python = $cmd.Source
}
$genArgs = @((Join-Path $PSScriptRoot "release_manifest.py"), "generate", "--root", $repoRoot, "--out", $outDir,
             "--tag", $Tag, "--repo", $Repo, "--channel", $Channel, "--app", $appBin,
             "--recovery", $RecoveryBin, "--elf-zip", $elfZip, "--max-app-size", "$MaxAppSize")
if ($DevDryRun) {
    # The generator has no dev bypass for a dirty tree by design; a dry run in a scratch
    # worktree reports that and stops after the gates above, which already printed.
    Write-Host "DevDryRun: manifest generation still requires a clean tree; attempting."
}
& $python @genArgs
if ($LASTEXITCODE -ne 0) { Fail "release_manifest.py generate failed (see above)." }
& $python (Join-Path $PSScriptRoot "release_manifest.py") validate $outDir
if ($LASTEXITCODE -ne 0) { Fail "release_manifest.py validate failed." }

# ---------------------------------------------------------------- report / publish
$manifest = Get-Content -LiteralPath (Join-Path $outDir "release.json") -Raw | ConvertFrom-Json
Write-Host ""
Write-Host "Release $Tag ($Channel) of $Repo, commit $($manifest.commit)"
Write-Host "zones_cfg_version $($manifest.compat.zones_cfg_version), partitions_sha256 $($manifest.compat.partitions_sha256.Substring(0,16))..."
Get-ChildItem -LiteralPath $outDir -File | ForEach-Object { Write-Host ("  {0,-34} {1,10} B" -f $_.Name, $_.Length) }

$body = "kilnCtl $Tag`n`nCommit: $($manifest.commit)`nzones_cfg_version: $($manifest.compat.zones_cfg_version)`nSee release.json and SHA256SUMS."
if (-not $Publish) {
    Write-Host ""
    Write-Host "DRY RUN: nothing published. -Publish would POST a draft release (target_commitish $($manifest.commit)),"
    Write-Host "upload the files above, re-download and compare sha256, then PATCH draft=false."
    exit 0
}

$token = [Environment]::GetEnvironmentVariable("KILNCTL_GITHUB_TOKEN")
if (-not $token) { $token = [Environment]::GetEnvironmentVariable("KILNCTL_GITHUB_TOKEN", "User") }
if (-not $token) { Fail "KILNCTL_GITHUB_TOKEN is not set (fine-grained PAT, contents:write, this repo only)." }
try {
    Invoke-ReleasePublish -Token $token -Repo $Repo -Tag $Tag -Commit $manifest.commit -Dir $outDir `
        -Prerelease ($Channel -eq "pre") -Body $body
} catch {
    $m = $_.Exception.Message
    if ($token) { $m = $m.Replace($token, "***") }
    Fail "publish failed: $m"
}
exit 0
