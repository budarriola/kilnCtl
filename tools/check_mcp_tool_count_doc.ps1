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

Write-Host "check_mcp_tool_count_doc: OK ($realCount tools, both docs agree)"
exit 0
