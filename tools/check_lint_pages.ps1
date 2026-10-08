# checkcache: ok
# check_lint_pages.ps1 -- runs firmware/KilnFW/App/test/lint_pages.js as a
# guard, not just a manual tools/verify.ps1 step.
#
# lint_pages.js syntax-checks every KilnFW web page's inline <script>/<style>
# blocks and standalone .js/.css files, and separately guards against a page
# defining its own sha256/hmacSha256 or setting the X-Ota-Mac header directly
# instead of going through app.js's shared OTA helpers (see that file's own
# header for the two real shipped bugs that motivated it). It was previously
# invoked ONLY from tools/verify.ps1's "lint" stage, which is a manual,
# opt-in script -- tools/run_all_checks.ps1 discovers checks by globbing
# check_*.ps1, so lint_pages.js never actually ran as part of the enforced
# suite. This wrapper closes that gap; tools/verify.ps1's "lint" stage now
# calls this same script instead of invoking node directly a second time, so
# there is exactly one way this lint runs.
#
# Exit: 0 pass, 1 fail, 3 SKIP (node not installed).

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$lintScript = Join-Path $repo 'firmware\KilnFW\App\test\lint_pages.js'
$targetDir = Join-Path $repo 'firmware\KilnFW\App\drivers'

if (-not (Test-Path $lintScript)) {
    Write-Output "FAIL: lint_pages.js not found at $lintScript"
    exit 1
}
if (-not (Test-Path $targetDir)) {
    Write-Output "FAIL: lint target directory not found at $targetDir"
    exit 1
}

$node = Get-Command node -ErrorAction SilentlyContinue
if ($null -eq $node) {
    Write-Output "SKIP: node is not installed, so lint_pages.js cannot be run."
    exit 3
}

& node $lintScript $targetDir
$code = $LASTEXITCODE

if ($code -eq 0) {
    exit 0
}

Write-Output "FAIL: lint_pages.js reported problem(s) (exit $code) -- see output above for the offending file(s)."
exit 1
