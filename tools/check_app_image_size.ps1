# check_app_image_size.ps1 -- wrapper so run_all_checks.ps1 discovers the
# application-image size gate (tools/check_app_image_size.py): KilnCtrl.bin
# must fit the 4 MiB `app` partition (docs/GITHUB_RELEASE_UPDATE_PLAN.md
# section 3). Exit 0 PASS, 3 SKIP (no image built yet), else FAIL.
$ErrorActionPreference = "Stop"

$pyScript = Join-Path $PSScriptRoot "check_app_image_size.py"
if (-not (Test-Path $pyScript)) {
    throw "check_app_image_size.ps1: expected $pyScript not found -- has it moved?"
}

$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript @args
$code = $LASTEXITCODE
if ($code -eq 0) { exit 0 }
if ($code -eq 3) { exit 3 }
throw "check_app_image_size.py exited $code -- see its output above."
