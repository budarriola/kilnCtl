# checkcache: ok
# check_page_js_tests.ps1 -- runs every plain-node page test under
# firmware/KilnFW/App/test/*.js as a standing check, not just something a
# session has to remember to run by hand.
#
# firmware/KilnFW/App/test/ holds two kinds of .js file: the "test_*.js"
# family (one Node-only DOM-less unit test per page/widget, following the
# regex-parser pattern documented at the top of test_zones_type_toggle.js --
# no jsdom, no browser, exit 0 on all-pass / 1 otherwise) plus two library
# files that are not tests at all: lint_pages.js (a separate lint tool,
# already enforced on its own by check_lint_pages.ps1, and it exits nonzero
# on a DIFFERENT contract -- a lint finding, not a test assertion) and
# _drivers_layout.js (a shared helper `require()`d by the others, with no
# assertions of its own to run). Every test_*.js file was run by hand while
# writing this check (2026-09-20) and every one of them ran clean under a
# bare `node <file>` with no Chrome/CDP or other bench dependency -- none are
# excluded for needing a bench/browser today. If a future test genuinely
# needs one, exclude it here with an explicit comment naming the reason,
# never silently.
#
# Exit: 0 pass, 1 fail, 3 SKIP (node not installed).

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$testDir = Join-Path $repo 'firmware\KilnFW\App\test'

if (-not (Test-Path $testDir)) {
    Write-Output "FAIL: test directory not found at $testDir"
    exit 1
}

$node = Get-Command node -ErrorAction SilentlyContinue
if ($null -eq $node) {
    Write-Output "SKIP: node is not installed, so the page .js tests cannot be run."
    exit 3
}

# Exclusions, each with an explicit reason -- never exclude a file just
# because it wasn't run yet.
$excludeNames = @(
    'lint_pages.js'       # a separate lint tool (own contract/exit codes), already
                          # enforced by check_lint_pages.ps1 -- not a test_*.js unit test.
    '_drivers_layout.js'  # shared require()d helper, no assertions of its own to run.
)

$files = Get-ChildItem -Path $testDir -Filter '*.js' -File |
    Where-Object { $excludeNames -notcontains $_.Name } |
    Sort-Object Name

if ($files.Count -eq 0) {
    Write-Output "FAIL: no page .js tests found under $testDir"
    exit 1
}

$failed = @()
$passCount = 0

foreach ($f in $files) {
    # Run through cmd /c with 2>&1 merged into stdout, captured as plain text.
    # A passing test that merely writes to stderr (e.g. console.error) must
    # not abort this check -- native stderr output inside PowerShell's own
    # pipeline throws a NativeCommandError and stops the whole script under
    # $ErrorActionPreference = 'Stop'. Routing through cmd /c avoids that:
    # PowerShell never sees the child's stderr as its own error stream.
    $out = & cmd /c "node `"$($f.FullName)`" 2>&1"
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        Write-Output "PASS: $($f.Name)"
        $passCount++
    } else {
        Write-Output "FAIL: $($f.Name) (exit $code)"
        if ($out) {
            Write-Output ($out | Out-String)
        }
        $failed += $f.Name
    }
}

Write-Output ""
Write-Output "$passCount/$($files.Count) page .js tests passed"

if ($failed.Count -gt 0) {
    Write-Output "FAILED: $($failed -join ', ')"
    exit 1
}

exit 0
