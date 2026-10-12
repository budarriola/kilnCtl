# Verdict logic for build_host_tests.ps1's summary. The executable-count
# MISMATCH check runs whenever the run is not -Only, even when other
# executables failed: a silent dropout must not hide behind a red test.
# Tested by tools/check_host_test_verdict.ps1.
function Get-HostTestVerdict {
    param([int]$Built, [int]$Expected, [int]$BuildFailures, [int]$RunFailures, [bool]$OnlyRun)
    # Built excludes build failures, so a compile break is not a dropout: compare Built + BuildFailures.
    $accounted = $Built + $BuildFailures
    $mismatch = ($accounted -ne $Expected)
    $text = ""
    if ($mismatch) {
        $text = ("MISMATCH: $Built built + $BuildFailures build failures = $accounted but $Expected were expected -- " +
            "at least one was silently never attempted (not a build failure, not a run failure). Refusing to " +
            "report success.")
    }
    $code = 0
    if ($BuildFailures -gt 0 -or $RunFailures -gt 0 -or $mismatch) { $code = 1 }
    if ($OnlyRun -and $Built -ne $Expected -and $code -eq 0) { $code = 1 }
    [pscustomobject]@{ Mismatch = $mismatch; MismatchText = $text; ExitCode = $code }
}


