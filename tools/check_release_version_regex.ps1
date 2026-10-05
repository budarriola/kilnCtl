# check_release_version_regex.ps1 -- keeps gen_build_info.cmake's FW_RELEASE_VERSION validation
# in agreement with update_tag_valid() (update_url.c).
#
# Why: KILNCTL_RELEASE_VERSION is validated in CMake before it is baked into build_info.h, and the
# firmware validates the same tag again at run time. The two used to disagree (CMake refused a
# prerelease containing '-', "v1.2.3-rc-1", which update_tag_valid() accepts), so a legitimate
# release build silently reported an empty running version. CMake cannot call C, so the CMake side
# is a regex pair (_RV_TAG_REGEX positive, _RV_REJECT_REGEXES negatives) plus a 32-character cap.
#
# This check evaluates that regex pair, as written in the cmake file, against a table of tags with
# the expected verdict, and requires every tag of the table to appear in the host test
# test_update_url.c with the same verdict (accepted tags in a TEST_CHECK(update_tag_valid(...)),
# refused tags in the badtag[] table), so the C side is pinned to the same table. Change either
# side and one of the two fails.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_release_version_regex.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cmakePath = Join-Path $root 'firmware/KilnFW/App/drivers/gen_build_info.cmake'
$testPath = Join-Path $root 'firmware/KilnFW/App/test/test_update_url.c'

function Get-CmakeString([string]$text, [string]$name) {
    $m = [regex]::Match($text, '(?m)^\s*set\(' + [regex]::Escape($name) + '\s+"(.*)"\)\s*$')
    if (-not $m.Success) { throw "gen_build_info.cmake: no set($name `"...`") line found" }
    # CMake quoted-argument escapes: \\ is one backslash.
    return $m.Groups[1].Value.Replace('\\', '\')
}

$cmake = Get-Content -Raw -LiteralPath $cmakePath
$posRx = Get-CmakeString $cmake '_RV_TAG_REGEX'
$negRx = (Get-CmakeString $cmake '_RV_REJECT_REGEXES') -split ';'
if ($cmake -notmatch '_rv_len GREATER 32') { throw 'gen_build_info.cmake: the 32-character cap is gone' }

function Test-CmakeAccepts([string]$tag) {
    if ($tag.Length -gt 32) { return $false }
    if ($tag -cnotmatch $posRx) { return $false }
    foreach ($n in $negRx) { if ($tag -cmatch $n) { return $false } }
    return $true
}

# tag, expected verdict of update_tag_valid() == expected verdict of the CMake side.
$table = @(
    ,@('v1.0.0', $true)
    ,@('v10.20.30-rc.1', $true)
    ,@('v1.2.3-rc-1', $true)
    ,@('v1.2.3-alpha-beta.2', $true)
    ,@('v1.0.0-rc-01', $true)
    ,@('v123456789.0.0', $true)
    ,@('1.0.0', $false)
    ,@('v1.0', $false)
    ,@('v1.0.0+build', $false)
    ,@('v1.0.0/x', $false)
    ,@('V1.0.0', $false)
    ,@('v1.0.0-a%', $false)
    ,@('v01.0.0', $false)
    ,@('vv1.0.0', $false)
    ,@('v1.0.0-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', $false)
    ,@('v1.2.3-01', $false)
    ,@('v1.2.3-rc.01', $false)
    ,@('v1.2.3-.', $false)
    ,@('v1.2.3-rc..1', $false)
    ,@('v1234567890.0.0', $false)
    ,@('v1.2.3-', $false)
)

$test = Get-Content -Raw -LiteralPath $testPath
$bad = New-Object System.Collections.Generic.List[string]
foreach ($row in $table) {
    $tag = $row[0]
    $want = $row[1]
    if ((Test-CmakeAccepts $tag) -ne $want) {
        $bad.Add("cmake: '$tag' should be " + $(if ($want) { 'accepted' } else { 'refused' }) + ' (as update_tag_valid does)')
    }
    if ($test.IndexOf('"' + $tag + '"') -lt 0) {
        $bad.Add("host test test_update_url.c does not pin the tag '$tag' (add it to the tag tests)")
    }
}
if ($bad.Count -gt 0) {
    Write-Host 'RELEASE VERSION REGEX CHECK FAILED:' -ForegroundColor Red
    foreach ($b in $bad) { Write-Host "  $b" -ForegroundColor Red }
    throw "$($bad.Count) disagreement(s) between gen_build_info.cmake and update_tag_valid()"
}
Write-Host "Release version regex check passed: $($table.Count) tags agree between gen_build_info.cmake and the update_tag_valid() host test."
exit 0
