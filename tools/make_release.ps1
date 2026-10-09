# make_release.ps1 -- build, package and (optionally) publish a kilnCtl GitHub release.
# Design: GITHUB_RELEASE_UPDATE_PLAN.md sections 6-8; how-to: docs/RELEASING.md.
#
# DEFAULT IS A DRY RUN: every gate runs, the images are collected, release.json and
# SHA256SUMS are written to logs\release\<tag>\ and validated, and the script prints
# what -Publish WOULD send. Nothing leaves the machine and no tag/release is created
# unless -Publish is given.
#
# Gates (each a hard refusal; none is skipped by -Publish):
#   * -Publish is refused with -SkipBuild / -BuildDir / -RecoveryBin (provenance: the
#     binaries shipped must be the ones this script just built from the stamped commit)
#   * tag matches ^v\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$
#   * working tree is clean (git status --porcelain, logs/release excluded)
#   * HEAD is the commit annotated tag <tag> peels to, that tag is on origin, and it is the origin/release tip (tools/release_merge.ps1)
#   * KilnCtrl.bin <= 0x400000: deliberately the planned post-split app size (see
#     docs/GITHUB_RELEASE_UPDATE_PLAN.md WP2), which is now also the actual app partition size
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
# Release gates: docs/release_gates.json (tools/release_gates.py) lists the stable gates, each
# open or pass with evidence. Every run prints their status. -Publish of a STABLE tag is refused
# while any gate is open unless -AllowOpenGates (prints them loudly); a pre-release tag
# (vX.Y.Z-pre.N, accepted by update_semver.c and release_manifest.py) may publish with open
# gates but still prints them. -GatesFile overrides the path (tests).
#
# Release notes: -NotesFile <path> becomes the release body verbatim. Without it the body is
# generated: git log --oneline <previous semver tag merged into HEAD>..HEAD, or the last 50
# commits if there is none. The body is also written to logs\release\<tag>.notes.md for review.
#
# Bench evidence: after the app binary is located, `release_gates.py bench-evidence --app-bin` requires a
# full, passing, <= 7 day old run of suites ota, lcd, safety on exactly that build. Treated like an open
# gate: -Publish of a STABLE tag refuses without it unless -AllowOpenGates; dry runs and pre-release tags
# print the result as a WARNING. -BenchLogsDir overrides the logs location.
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
    [string]$NotesFile,
    [string]$GatesFile,
    [switch]$AllowOpenGates,
    [string]$BenchLogsDir,       # bench_test run logs (default <repo>\logs\bench_test; gitignored, so pass the main tree's in a release worktree)
    [switch]$LoadFunctionsOnly,
    [int]$DramCeilingBytes = 0   # test override for the .dram0.bss gate (0 = checker default)
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
# Invoke-ReleasePublish calls Invoke-RestMethod and Get-AssetToFile by plain name so a
# test can shadow them (tools/check_release_manifest.ps1 does; it also exercises the real
# Get-AssetToFile against a local redirect server).

function Get-ReleaseApiHeaders([string]$Token, [string]$Accept = "application/vnd.github+json") {
    return @{ Authorization = "Bearer $Token"; Accept = $Accept; "X-GitHub-Api-Version" = "2022-11-28" }
}

function Get-AssetToFile([string]$Token, [string]$AssetApiUrl, [string]$OutFile) {
    # Two hops, done by hand with HttpClient (AllowAutoRedirect off) so the behavior does
    # not depend on how Windows PowerShell 5.1 surfaces a 302 as an exception:
    #   1. GET the API asset URL with the token + Accept: application/octet-stream
    #      -> 3xx with a Location (signed blob URL).
    #   2. GET Location WITHOUT the token, streamed to $OutFile.
    # A direct 200 on hop 1 (no redirect) is streamed too. Anything else throws, which
    # leaves the draft release unpublished.
    Add-Type -AssemblyName System.Net.Http
    $handler = New-Object System.Net.Http.HttpClientHandler
    $handler.AllowAutoRedirect = $false
    $client = New-Object System.Net.Http.HttpClient($handler)
    $client.Timeout = [TimeSpan]::FromMinutes(10)
    $resp = $null; $resp2 = $null
    try {
        $req = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Get, $AssetApiUrl)
        $req.Headers.TryAddWithoutValidation("Authorization", "Bearer $Token") | Out-Null
        $req.Headers.TryAddWithoutValidation("Accept", "application/octet-stream") | Out-Null
        $req.Headers.TryAddWithoutValidation("X-GitHub-Api-Version", "2022-11-28") | Out-Null
        $req.Headers.TryAddWithoutValidation("User-Agent", "kilnctl-make-release") | Out-Null
        $resp = $client.SendAsync($req, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
        $code = [int]$resp.StatusCode
        $final = $resp
        if ($code -in 301, 302, 303, 307, 308) {
            $loc = $resp.Headers.Location
            if (-not $loc) { throw "asset download: HTTP $code without a Location header" }
            if (-not $loc.IsAbsoluteUri) { $loc = New-Object System.Uri((New-Object System.Uri($AssetApiUrl)), $loc) }
            $req2 = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Get, $loc)
            $req2.Headers.TryAddWithoutValidation("User-Agent", "kilnctl-make-release") | Out-Null
            $resp2 = $client.SendAsync($req2, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
            $final = $resp2
        }
        if (-not $final.IsSuccessStatusCode) { throw "asset download: HTTP $([int]$final.StatusCode)" }
        $inS = $final.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $outS = [System.IO.File]::Create($OutFile)
        try { $inS.CopyTo($outS) } finally { $outS.Dispose(); $inS.Dispose() }
    } finally {
        if ($resp2) { $resp2.Dispose() }
        if ($resp) { $resp.Dispose() }
        $client.Dispose()
    }
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

function Test-DramBssBudget([string]$Python, [string]$Elf, [int]$Ceiling = 0) {
    # Reuses the standing checker (no second parser). Exit 0 = pass; 1 = over; 3 = unmeasured SKIP -- both refuse.
    $checker = Join-Path $repoRoot "firmware\KilnFW\App\test\check_kilnfw_dram_bss_budget.py"
    $a = @($checker, "--elf", $Elf)
    if ($Ceiling -gt 0) { $a += @("--ceiling-bytes", "$Ceiling") }
    & $Python @a
    if ($LASTEXITCODE -ne 0) { Fail ".dram0.bss budget check failed or could not measure (exit $LASTEXITCODE) on $Elf; a release needs a measured pass." }
}

if ($LoadFunctionsOnly) { return }

# ---------------------------------------------------------------- gates
if (-not $Tag -or $Tag -cnotmatch $SemverRe) { Fail "tag '$Tag' is not semver (vMAJOR.MINOR.PATCH[-pre])." }
if ($Publish -and ($SkipBuild -or $BuildDir -or $RecoveryBin)) {
    Fail "-Publish cannot be combined with -SkipBuild/-BuildDir/-RecoveryBin: the release is stamped with HEAD's commit, so the shipped binaries must be built here, from that commit."
}
if ($Publish -and $DevDryRun) { Fail "-DevDryRun and -Publish are mutually exclusive." }
if ($Repo -notmatch '^[A-Za-z0-9._-]{1,39}/[A-Za-z0-9._-]{1,100}$') { Fail "repo '$Repo' is not owner/name." }

function Gate([string]$msg) {
    if ($DevDryRun) { Write-Host "WARNING (DevDryRun): $msg" -ForegroundColor Yellow } else { Fail $msg }
}

. (Join-Path $PSScriptRoot "lib_pctools_python.ps1")
$python = Resolve-PcToolsPython -RepoRoot $repoRoot
if (-not $python) { Fail "no python (tools\PcTools\.venv, KILNCTL_PCTOOLS_PYTHON, main tree venv or PATH); release_manifest.py needs one." }
if ($NotesFile -and -not (Test-Path -LiteralPath $NotesFile -PathType Leaf)) { Fail "-NotesFile $NotesFile does not exist." }
if ($NotesFile -and ((Get-Item -LiteralPath $NotesFile).Length -eq 0)) { Fail "-NotesFile $NotesFile is empty." }
if (-not $GatesFile) { $GatesFile = Join-Path $repoRoot "docs\release_gates.json" }
$gatesTool = Join-Path $PSScriptRoot "release_gates.py"
# Gate status is printed on every run; the refusal (stable tag, open gates, no -AllowOpenGates)
# applies only to -Publish. A dry run of a stable tag therefore never fails on gates.
$gateArgs = @($gatesTool, "check", "--file", $GatesFile, "--tag", $Tag)
if ($AllowOpenGates -or -not $Publish) { $gateArgs += "--allow-open" }
& $python @gateArgs
if ($LASTEXITCODE -ne 0) { Fail "release gates not satisfied (see above)." }

$dirty = @(& git -C $repoRoot status --porcelain -- . ":(exclude)logs/release")
if ($LASTEXITCODE -ne 0) { Fail "git status failed." }
if ($dirty.Count -gt 0) { Gate "working tree is dirty ($($dirty.Count) change(s), first: $($dirty[0])); build releases from a clean worktree (tools\worktree_mint.ps1)." }

$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
# Releases are built from the TAGGED RELEASE COMMIT (tools/release_merge.ps1 -Push puts the single-parent
# commit M on origin/release and the annotated tag on M), not from origin/main: HEAD must be the commit
# the tag peels to, and that commit must be the origin/release tip.
& git -C $repoRoot fetch --quiet origin release --tags
if ($LASTEXITCODE -ne 0) { Fail "git fetch origin release --tags failed." }
$tagCommit = (& git -C $repoRoot rev-parse --verify --quiet "refs/tags/$Tag^{commit}")
if ($LASTEXITCODE -ne 0 -or -not $tagCommit) { Fail "tag $Tag does not exist locally; run tools\release_merge.ps1 -Commit <main commit> -Tag $Tag -Push first." }
$tagCommit = $tagCommit.Trim()
$remoteTag = @(& git -C $repoRoot ls-remote --tags origin "refs/tags/$Tag^{}")
if ($LASTEXITCODE -ne 0) { Fail "git ls-remote --tags origin failed (cannot prove the tag is on origin)." }
if ($remoteTag.Count -eq 0 -or ($remoteTag[0] -split '\s+')[0] -ne $tagCommit) { Gate "tag $Tag is not on origin at $($tagCommit.Substring(0,12))." }
if ($commit -ne $tagCommit) { Gate "HEAD $($commit.Substring(0,12)) is not the commit tag $Tag peels to ($($tagCommit.Substring(0,12))); check out the tagged release commit." }
$relTip = (& git -C $repoRoot rev-parse --verify --quiet "refs/remotes/origin/release^{commit}")
if ($LASTEXITCODE -ne 0 -or -not $relTip -or $relTip.Trim() -ne $tagCommit) { Gate "tag $Tag is not the origin/release tip." }
# Release body: resolved before the (long) build so a bad -NotesFile or a git failure costs nothing.
if ($NotesFile) {
    $bodyBase = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $NotesFile).Path)
    $bodySuffix = ""
} else {
    $notesTmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kilnrel_notes_" + [guid]::NewGuid().ToString("N") + ".md")
    & $python $gatesTool notes --root $repoRoot --tag $Tag --out $notesTmp
    if ($LASTEXITCODE -ne 0) { Fail "release_gates.py notes failed (see above)." }
    $bodyBase = [System.IO.File]::ReadAllText($notesTmp)
    Remove-Item -LiteralPath $notesTmp -Force -ErrorAction SilentlyContinue
    $bodySuffix = "`n`nCommit: {0}`nzones_cfg_version: {1}`n"
}

# ---------------------------------------------------------------- build / collect
if (-not $SkipBuild) {
    # The build embeds the tag as FW_RELEASE_VERSION (gen_build_info.cmake) so the board can tell
    # a downgrade from an upgrade when it fetches a release itself.
    $env:KILNCTL_RELEASE_VERSION = $Tag
    try {
        foreach ($s in @("firmware\KilnFW\App\test\check_00_kilnfw_target_build.ps1",
                         "firmware\KilnFW\App\test\check_00_kilnfw_recovery_target_build.ps1")) {
            Write-Host "building: $s"
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoRoot $s)
            if ($LASTEXITCODE -ne 0) { Fail "$s exited $LASTEXITCODE (a SKIP, exit 3, is also a refusal: a release needs real binaries)." }
        }
    } finally {
        # Never leak the release tag into later builds in this process or its children.
        Remove-Item Env:\KILNCTL_RELEASE_VERSION -ErrorAction SilentlyContinue
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
if ($appSize -gt $MaxAppSize) { Fail "KilnCtrl.bin is $appSize bytes, over the $MaxAppSize (0x400000) gate (planned post-split app size, GITHUB_RELEASE_UPDATE_PLAN.md WP2)." }
Test-DramBssBudget $python $elf $DramCeilingBytes

$outDir = Join-Path $repoRoot "logs\release\$Tag"
if (Test-Path -LiteralPath $outDir) { Fail "$outDir already exists; remove it (or pick a new tag) so no stale asset is published." }
New-Item -ItemType Directory -Path $outDir | Out-Null

$elfZip = Join-Path $outDir "KilnCtrl-$Tag.elf.zip"
Compress-Archive -LiteralPath $elf -DestinationPath $elfZip -CompressionLevel Optimal

# ---------------------------------------------------------------- manifest
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

$notesOut = Join-Path $repoRoot "logs\release\$Tag.notes.md"
$body = $bodyBase
if ($bodySuffix) { $body = $bodyBase.TrimEnd() + ($bodySuffix -f $manifest.commit, $manifest.compat.zones_cfg_version) }
Set-Content -LiteralPath $notesOut -Value $body -Encoding UTF8
Write-Host ""
Write-Host "Release body ($(if ($NotesFile) { "from -NotesFile" } else { "generated" }), $($body.Length) chars) written to $notesOut"
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
