# check_mcp_tool_count_doc.ps1 -- the kilnctrl MCP tool count quoted in
# CLAUDE.md and docs/MCP_SERVERS.md must match the real number of registered
# tools, and the two docs must agree with each other.
#
# WHY THIS EXISTS. Two agents counted kilnctrl's tools differently on the same
# day: docs/MCP_SERVERS.md (145, commit f588d3d) came from summing kiln_help()'s
# per-group counts; CLAUDE.md (146, commit 6fc578e) came from
# `grep -c '@_srv._tool()' tools/PcTools/src/kilnctrl/mcp_server*.py`. The grep
# count was wrong by one: mcp_server_actions.py:71 has the literal text
# "@_srv._tool()" inside a `#` COMMENT (explaining the decorator pattern to a
# reader), which a bare grep -c cannot distinguish from a real decorator line.
# 145 is correct. This check recomputes the real count the same way and
# compares it against both docs so this specific drift can't silently recur,
# and so the two docs can't quietly diverge from each other again either.
#
# WHAT THIS CHECKS. Counts lines matching '^\s*@_srv\._tool\(\)' (anchored at
# the start of the (trimmed) line, so a comment containing the same text
# anywhere but the start does not count -- this is what the naive grep -c
# got wrong) across tools/PcTools/src/kilnctrl/mcp_server*.py. Then extracts
# the integer immediately preceding "tools for `kilnctrl`" in CLAUDE.md and
# immediately preceding "tools and `kicad`" in docs/MCP_SERVERS.md (the
# "`kilnctrl` registers N tools" sentence), and fails if either doc's number
# does not equal the real count.
#
# WHAT THIS DOES NOT CATCH. It does not check the `kicad` count, kiln_help()'s
# own live per-group total, or any other document that might quote this
# number. Narrow and mechanical on purpose -- see check_doc_citations.ps1's
# header for why a check like this stays narrow rather than growing a general
# prose-vs-reality parser.
#
# 2026-09-07: this check was blind to a whole registration path and passed
# while the docs said 146 and the live server actually carried 151.
# tools/PcTools/src/mcpkit/workbench.py registers build/test-runner tools
# (build_kilnfw, build_saftyfw, build_saftyfw_host_tests, run_pctools_tests,
# run_repo_checks) by calling `tool()(fn)` from `workbench.attach(_tool,
# ("dut", "common"))` in mcp_server.py -- a plain function call, not a
# `@_srv._tool()` decorator line, so the line-anchored regex above could never
# see them (this mechanism predates both wrong doc counts, per
# `29ce9970`). The fix below adds those 5 by reading workbench.py's `BUNDLES`
# table and the bundle names actually passed to `attach(...)` in
# mcp_server.py, rather than assuming a fixed number -- so a future bundle
# addition/removal is still caught.

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

$serverFiles = Get-ChildItem -Path (Join-Path $repoRoot "tools\PcTools\src\kilnctrl") -Filter "mcp_server*.py" -File
if (-not $serverFiles) {
    Write-Error "check_mcp_tool_count_doc: no tools/PcTools/src/kilnctrl/mcp_server*.py files found"
    exit 1
}

$realCount = 0
foreach ($f in $serverFiles) {
    $lines = Get-Content -LiteralPath $f.FullName
    foreach ($line in $lines) {
        if ($line.TrimStart() -match '^@_srv\._tool\(\)\s*$') {
            $realCount++
        }
    }
}

# Tools attached dynamically via workbench.attach(_tool, (bundle, ...)) --
# `tool()(fn)` calls, not `@_srv._tool()` decorator lines, so the scan above
# never sees them. Read which bundle names mcp_server.py actually attaches,
# then count the entries in those bundles from workbench.py's BUNDLES table.
$mcpServerPyPath = Join-Path $repoRoot "tools\PcTools\src\kilnctrl\mcp_server.py"
$workbenchPyPath = Join-Path $repoRoot "tools\PcTools\src\mcpkit\workbench.py"
$mcpServerPy = Get-Content -LiteralPath $mcpServerPyPath -Raw
$workbenchPy = Get-Content -LiteralPath $workbenchPyPath -Raw

$attachMatch = [regex]::Match($mcpServerPy, 'workbench\.attach\(\s*\w+\s*,\s*\(([^)]*)\)\s*\)')
if (-not $attachMatch.Success) {
    Write-Error "check_mcp_tool_count_doc: could not find 'workbench.attach(_tool, (...))' call in mcp_server.py"
    exit 1
}
$attachedBundles = [regex]::Matches($attachMatch.Groups[1].Value, '"([^"]+)"|''([^'']+)''') |
    ForEach-Object { if ($_.Groups[1].Success) { $_.Groups[1].Value } else { $_.Groups[2].Value } }

$bundlesMatch = [regex]::Match($workbenchPy, 'BUNDLES:.*?=\s*\{(.*)\n\}', 'Singleline')
if (-not $bundlesMatch.Success) {
    Write-Error "check_mcp_tool_count_doc: could not find BUNDLES table in workbench.py"
    exit 1
}
$bundlesBody = $bundlesMatch.Groups[1].Value

$attachedCount = 0
foreach ($bundleName in $attachedBundles) {
    $bundleMatch = [regex]::Match($bundlesBody, "`"$bundleName`"\s*:\s*\{([^}]*)\}", 'Singleline')
    if (-not $bundleMatch.Success) {
        Write-Error "check_mcp_tool_count_doc: attached bundle '$bundleName' not found in workbench.py BUNDLES"
        exit 1
    }
    $entryMatches = [regex]::Matches($bundleMatch.Groups[1].Value, '^\s*"[^"]+"\s*:', 'Multiline')
    $attachedCount += $entryMatches.Count
}
$realCount += $attachedCount

$failed = $false

$claudeMdPath = Join-Path $repoRoot "CLAUDE.md"
$claudeMd = Get-Content -LiteralPath $claudeMdPath -Raw
if ($claudeMd -match '(\d+)\s+tools for `kilnctrl`') {
    $claudeCount = [int]$Matches[1]
    if ($claudeCount -ne $realCount) {
        Write-Error "check_mcp_tool_count_doc: CLAUDE.md says $claudeCount kilnctrl tools, actual registered count is $realCount"
        $failed = $true
    }
} else {
    Write-Error "check_mcp_tool_count_doc: could not find 'N tools for `kilnctrl`' phrase in CLAUDE.md"
    $failed = $true
}

$mcpDocPath = Join-Path $repoRoot "docs\MCP_SERVERS.md"
$mcpDoc = Get-Content -LiteralPath $mcpDocPath -Raw
if ($mcpDoc -match '`kilnctrl` registers (\d+) tools') {
    $docCount = [int]$Matches[1]
    if ($docCount -ne $realCount) {
        Write-Error "check_mcp_tool_count_doc: docs/MCP_SERVERS.md says $docCount kilnctrl tools, actual registered count is $realCount"
        $failed = $true
    }
} else {
    Write-Error "check_mcp_tool_count_doc: could not find '`kilnctrl` registers N tools' phrase in docs/MCP_SERVERS.md"
    $failed = $true
}

if ($failed) {
    exit 1
}

Write-Host "check_mcp_tool_count_doc: OK ($realCount tools: $($realCount - $attachedCount) decorated + $attachedCount workbench-attached, both docs agree)"
exit 0
