# lib_push_verify_walk.ps1 -- descendant walk for push_verify.ps1's no-job-membership kill fallback.
# Returns the descendants of $RootId from one process snapshot, parent-first (breadth-first), as
# objects { Id; Created }. A child is kept only if its CreationDate is at or after ITS OWN parent's
# (a reused PID's older children fail that test at every depth); the root gets ~1 ms slack.
# Tested by check_push_verify.ps1 with synthetic snapshots.
function Get-PvDescendantOrder {
    param($Snap, [int]$RootId, [datetime]$RootCreated)
    $created = @{ $RootId = $RootCreated.AddMilliseconds(-1) }
    $keep = @{}; $out = New-Object System.Collections.ArrayList; $frontier = @($RootId)
    while ($frontier.Count -gt 0) {
        $next = @()
        foreach ($pp in $frontier) {
            foreach ($c in ($Snap | Where-Object { [int]$_.ParentProcessId -eq $pp -and -not $keep.ContainsKey([int]$_.ProcessId) })) {
                $cid = [int]$c.ProcessId
                if ($cid -ne $RootId -and $c.CreationDate -and $c.CreationDate -ge $created[$pp]) {
                    $keep[$cid] = $true; $created[$cid] = $c.CreationDate
                    [void]$out.Add([pscustomobject]@{ Id = $cid; Created = $c.CreationDate }); $next += $cid
                }
            }
        }
        $frontier = $next
    }
    return ,$out
}

# R2: a snapshot CreationDate and an opened handle's StartTime must agree (PID reuse gives a far later time).
function Test-PvStartMatch {
    param([datetime]$Actual, [datetime]$Expected)
    return ([Math]::Abs(($Actual - $Expected).TotalMilliseconds) -le 100)
}
