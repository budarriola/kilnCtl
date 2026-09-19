<#
.SYNOPSIS
  Capture a photo of the kiln controller's LCD from the bench webcam and crop
  to the panel's active area.

.DESCRIPTION
  The bench camera (Logitech C920) is aimed at the board's LCD and does not
  move, so the panel's location in frame is fixed. This script grabs a single
  frame with ffmpeg's dshow input and crops to the screen, giving a small
  image suitable for checking rendering (colors, inversion, brightness,
  what page is showing) without a human at the bench.

  Keep the resolution low. These images are read by an assistant, and a
  1280x720 frame costs several times the tokens of the default 640x360 crop
  for no diagnostic gain.

  The crop rectangle was re-measured from a full-frame capture on 2026-09-19
  after the LCD was moved and put back (see CLAUDE.md's "Camera aim" note). If the
  camera or board is ever moved again, re-run with -Full to get an uncropped
  frame, re-measure by numeric pixel sampling (never by eye) with
  sample_lcd_region.ps1, and update the defaults here.

.PARAMETER Out
  Output image path. Defaults to lcd.jpg in the current directory.

.PARAMETER Full
  Skip cropping and save the whole camera frame. Use this to re-measure the
  crop rectangle after the camera moves.

.PARAMETER Width
  Width to scale the cropped image down to (default 640). Height follows the
  panel's 3:2 aspect automatically.

.EXAMPLE
  .\capture_lcd.ps1 -Out shot.jpg
.EXAMPLE
  .\capture_lcd.ps1 -Full -Out frame.jpg
#>
[CmdletBinding()]
param(
    [string]$Out = "lcd.jpg",
    [switch]$Full,
    [int]$Width = 640,
    [string]$Device = "HD Pro Webcam C920",
    # Active-area rectangle of the LCD within a 1280x720 frame, re-measured
    # 2026-09-19 by numeric pixel sampling (luminance edge scans against the
    # black-bezel reference) after the owner moved the LCD and put it back --
    # the panel shifted right ~195px and down ~46px from the 2026-09-10
    # numbers, far beyond the few-px tolerance, so the old crop clipped most
    # of the screen. The board now sits rotated a few degrees (its right side
    # is ~25px higher than its left): left edge runs 298 (top) to 323
    # (bottom), right edge ~1145-1147, top edge 60 (right) to 86 (left),
    # bottom edge 617 (right) to 635 (left). This rectangle is the smallest
    # axis-aligned box that contains all four corners of the screen content --
    # it includes a few pixels of bezel on some sides rather than clipping any
    # UI, since losing content is worse than a small margin.
    [int]$CropX = 296,
    [int]$CropY = 58,
    [int]$CropW = 853,
    [int]$CropH = 578
)

$ErrorActionPreference = "Stop"

$ffmpeg = "C:\ffmpeg\ffmpeg.exe"
if (-not (Test-Path $ffmpeg)) {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($null -eq $cmd) { throw "ffmpeg not found at $ffmpeg and not on PATH." }
    $ffmpeg = $cmd.Source
}

# The C920 hands out a stale frame immediately after the stream opens, so
# discard the first few and keep the last one. Without this the image can
# show the previous contents of the screen -- which, when the point of the
# capture is to confirm a flash landed, is exactly the wrong failure mode.
$filters = @("select=gte(n\,4)")
if (-not $Full) {
    $filters += "crop=${CropW}:${CropH}:${CropX}:${CropY}"
    $filters += "scale=${Width}:-2"
}
$filterChain = $filters -join ","

$args = @(
    "-hide_banner", "-loglevel", "error",
    "-f", "dshow",
    "-video_size", "1280x720",
    "-i", "video=$Device",
    "-vf", $filterChain,
    "-frames:v", "1",
    "-update", "1",
    "-q:v", "6",
    "-y", $Out
)

& $ffmpeg @args
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE" }
if (-not (Test-Path $Out)) { throw "ffmpeg reported success but $Out was not written." }

$size = (Get-Item $Out).Length
Write-Output "wrote $Out ($size bytes)"
