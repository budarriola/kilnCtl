<#
.SYNOPSIS
  Print mean RGB for a named pixel region of an LCD capture, plus a bezel
  (off-screen) reference sample, so agents don't have to re-derive the
  ffmpeg numeric-sampling pipeline from CLAUDE.md each time.

.DESCRIPTION
  Wraps the crop/scale/rawvideo/od pipeline documented in CLAUDE.md
  ("Judge colors by numeric pixel sampling, never by eye"). Takes an image
  already captured by capture_lcd.ps1 (full-frame or cropped) and reports
  the mean RGB over a small region, plus the same for a bezel reference
  region so a reading can be checked against "definitely off" as a sanity
  floor.

.PARAMETER Image
  Path to the captured JPEG (from capture_lcd.ps1).

.PARAMETER X, -Y
  Top-left corner of the region to sample, in the image's own pixel space
  (i.e. relative to a -Full frame's 1280x720, or to a cropped/scaled
  frame's own dimensions -- match Image to how you're specifying X/Y).

.PARAMETER W, -H
  Size of the region to sample and average over (default 6x6). Kept small
  on purpose -- this is meant to read one icon/glyph/swatch, not blend
  across a gradient or multiple UI elements.

.PARAMETER BezelX, -BezelY
  Off-screen reference region (default 100,100 on a -Full 1280x720 frame --
  adjust if the camera framing changes). Sampled at the same W/H.

.PARAMETER Json
  Emit machine-readable JSON instead of the default text lines, for callers
  like the bench-test harness's lcd_sampler.py. Backwards compatible: the
  plain-text output is unchanged when this switch is omitted.
  Shape: {"region":{"X":,"Y":,"W":,"H":,"R":,"G":,"B":},
          "bezel":{"X":,"Y":,"W":,"H":,"R":,"G":,"B":} | null}

.EXAMPLE
  .\sample_lcd_region.ps1 -Image full.jpg -X 500 -Y 650 -W 8 -H 8
.EXAMPLE
  .\sample_lcd_region.ps1 -Image full.jpg -X 500 -Y 650 -W 8 -H 8 -Json
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][int]$X,
    [Parameter(Mandatory = $true)][int]$Y,
    [int]$W = 6,
    [int]$H = 6,
    [int]$BezelX = 100,
    [int]$BezelY = 100,
    [switch]$SkipBezel,
    [switch]$Json
)

$ErrorActionPreference = "Stop"

$ffmpeg = "C:\ffmpeg\ffmpeg.exe"
if (-not (Test-Path $ffmpeg)) {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($null -eq $cmd) { throw "ffmpeg not found at $ffmpeg and not on PATH." }
    $ffmpeg = $cmd.Source
}
if (-not (Test-Path $Image)) { throw "Image not found: $Image" }

function Sample-Region([string]$path, [int]$cx, [int]$cy, [int]$cw, [int]$ch) {
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        $args = @(
            "-hide_banner", "-loglevel", "error", "-y",
            "-i", $path,
            "-vf", "crop=${cw}:${ch}:${cx}:${cy},scale=1:1",
            "-f", "rawvideo", "-pix_fmt", "rgb24", $tmp
        )
        & $ffmpeg @args | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed sampling ($cx,$cy,$cw,$ch) with exit code $LASTEXITCODE" }
        $bytes = [System.IO.File]::ReadAllBytes($tmp)
        if ($bytes.Length -lt 3) { throw "ffmpeg wrote no pixel data for ($cx,$cy,$cw,$ch) -- region may be out of frame." }
        return @{ R = $bytes[0]; G = $bytes[1]; B = $bytes[2] }
    } finally {
        Remove-Item $tmp -ErrorAction SilentlyContinue
    }
}

$region = Sample-Region -path $Image -cx $X -cy $Y -cw $W -ch $H
$bezel = $null
if (-not $SkipBezel) {
    $bezel = Sample-Region -path $Image -cx $BezelX -cy $BezelY -cw $W -ch $H
}

if ($Json) {
    $out = [ordered]@{
        region = [ordered]@{ X = $X; Y = $Y; W = $W; H = $H; R = $region.R; G = $region.G; B = $region.B }
        bezel  = $null
    }
    if ($null -ne $bezel) {
        $out.bezel = [ordered]@{ X = $BezelX; Y = $BezelY; W = $W; H = $H; R = $bezel.R; G = $bezel.G; B = $bezel.B }
    }
    Write-Output ($out | ConvertTo-Json -Compress)
} else {
    Write-Output ("region  ({0},{1},{2}x{3}): RGB({4},{5},{6})" -f $X, $Y, $W, $H, $region.R, $region.G, $region.B)
    if ($null -ne $bezel) {
        Write-Output ("bezel   ({0},{1},{2}x{3}): RGB({4},{5},{6})" -f $BezelX, $BezelY, $W, $H, $bezel.R, $bezel.G, $bezel.B)
    }
}
