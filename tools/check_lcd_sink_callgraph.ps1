# check_lcd_sink_callgraph.ps1 -- wrapper for check_lcd_sink_callgraph.py (REVIEW_LCDFX9 HIGH-2).
# Reads relocations of the built KilnFW objects; SKIP-FAST (exit 3) when there is no build.
param([string]$BuildDir)
$ErrorActionPreference = "Stop"
$py = Join-Path $PSScriptRoot "check_lcd_sink_callgraph.py"
$argv = @($py)
if ($BuildDir) { $argv += @("--build-dir", $BuildDir) }
& python @argv
exit $LASTEXITCODE
