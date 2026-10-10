# Shared host-test executable runner with a per-executable timeout.
# Dot-source, then: $code = Invoke-HostTestProcess -Name n -ExePath p -TimeoutSec 300
# Output is captured to files and echoed afterwards (same text, now after the run).
# On timeout: kills the whole process tree, prints "FAIL <name>: timed out after N s"
# plus the captured output tail, and returns 124 (nonzero) so callers count a failure.
function Invoke-HostTestProcess {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$ExePath,
        [int]$TimeoutSec = 300
    )
    $tag = [guid]::NewGuid().ToString('N')
    $outF = Join-Path ([System.IO.Path]::GetTempPath()) "hostrun_$tag.out"
    $errF = Join-Path ([System.IO.Path]::GetTempPath()) "hostrun_$tag.err"
    $nullIn = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "hostrun_$tag.in")
    [System.IO.File]::WriteAllText($nullIn, "")
    try {
        $p = Start-Process -FilePath $ExePath -PassThru -NoNewWindow `
            -RedirectStandardInput $nullIn -RedirectStandardOutput $outF -RedirectStandardError $errF
        $null = $p.Handle  # keep the handle so ExitCode stays readable
        $done = $p.WaitForExit([int]([Math]::Min([int64]$TimeoutSec * 1000, [int]::MaxValue)))
        if (-not $done) {
            & taskkill.exe /T /F /PID $p.Id *> $null
            $null = $p.WaitForExit(10000)
        }
        $lines = @()
        foreach ($f in @($outF, $errF)) {
            if (Test-Path -LiteralPath $f) { $lines += @(Get-Content -LiteralPath $f) }
        }
        if ($done) {
            $lines | ForEach-Object { Write-Host $_ }
            return $p.ExitCode
        }
        Write-Host "FAIL ${Name}: timed out after $TimeoutSec s"
        Write-Host "---- output tail ($Name) ----"
        $lines | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        # -1 collides with callers' "did not run" sentinel; report the timeout separately (B-LOW-1).
        $global:HostTestTimedOut = @($global:HostTestTimedOut) + $Name
        return 124
    } finally {
        Remove-Item -LiteralPath $outF, $errF, $nullIn -Force -ErrorAction SilentlyContinue
    }
}
