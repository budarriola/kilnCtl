<#
.SYNOPSIS
  Capture a photo of the kiln controller's LCD from the bench webcam and crop
  to the panel's active area.

.DESCRIPTION
  The bench camera (Logitech C920) is aimed at the board's LCD and does not
  move, so the panel's location in frame is fixed. This script grabs one or
  more frames with ffmpeg's dshow input and crops to the screen, giving a
  small image suitable for checking rendering (colors, inversion, brightness,
  what page is showing) without a human at the bench.

  Keep the resolution low. These images are read by an assistant, and a
  1280x720 frame costs several times the tokens of the default 640x360 crop
  for no diagnostic gain.

  ffmpeg's dshow input exposes only format/resolution/fps device options
  (`-list_options true`) -- it does NOT expose the C920's IAMCameraControl/
  IAMVideoProcAmp properties (exposure, gain, focus), confirmed on this
  machine 2026-09-24. The only ffmpeg-level route to those is
  `-show_video_device_dialog true`, which pops the driver's own modal
  property page and blocks for a human -- not usable from an unattended
  script. A DirectShow property-page helper (e.g. Python's `pygrabber`)
  could set them without that dialog, but it is a new dependency this repo
  does not carry (never pip/uv into the shared `tools/PcTools/.venv` --
  install it in a throwaway venv first if this is ever pursued). Absent
  that, this script compensates by capturing multiple frames after a short
  auto-exposure warm-up and keeping the sharpest one (a Laplacian edge
  convolution's mean absolute response, via ffmpeg's own
  `convolution`+`signalstats` filters -- no new dependency). This does not
  fix an overexposed scene (a burned-out capture stays burned out whichever
  frame is picked), but it does discard motion-/focus-blurred frames from a
  capture that started before auto-exposure/auto-focus had settled, which is
  what "blurred" reports traced back to when checked by hand.

  The crop rectangle was re-measured from a full-frame capture on 2026-09-24
  (previously 2026-09-19) after the LCD/camera geometry drifted again (see
  CLAUDE.md's "Camera aim" note). If the camera or board is ever moved again,
  re-run with -Full to get an uncropped frame, re-measure by numeric pixel
  sampling (never by eye) with sample_lcd_region.ps1, and update the defaults
  here.

.PARAMETER Out
  Output image path. Defaults to lcd.jpg in the current directory.

.PARAMETER Full
  Skip cropping and save the whole camera frame. Use this to re-measure the
  crop rectangle after the camera moves.

.PARAMETER Width
  Width to scale the cropped image down to (default 640). Height follows the
  panel's 3:2 aspect automatically.

.PARAMETER Frames
  Number of candidate frames to capture after the warm-up, scored by a
  sharpness metric, keeping the sharpest. Default 5. Pass 1 to restore the
  old single-frame behaviour (no scoring, no temp files).

.PARAMETER WarmupSeconds
  Seconds to let the camera stream run (auto-exposure/auto-focus settling)
  before any candidate frame is captured. Default 1.0.

.EXAMPLE
  .\capture_lcd.ps1 -Out shot.jpg
.EXAMPLE
  .\capture_lcd.ps1 -Full -Out frame.jpg
.EXAMPLE
  .\capture_lcd.ps1 -Out shot.jpg -Frames 1
#>
[CmdletBinding()]
param(
    [string]$Out = "lcd.jpg",
    [switch]$Full,
    [int]$Width = 640,
    [int]$Frames = 5,
    [double]$WarmupSeconds = 1.0,
    [string]$Device = "HD Pro Webcam C920",
    # Active-area rectangle of the LCD within a 1280x720 frame, re-measured
    # 2026-09-24 by numeric pixel sampling (luminance/color edge scans against
    # the black bezel) after LCD-01 found the 2026-09-19 crop stale -- the
    # panel had moved left and up again (left edge from ~298-323 to ~158-172,
    # right edge from ~1145-1147 to ~983-1044), and the geometry is a
    # perspective skew, not a simple rotation: the right edge measures
    # shorter than the left and slants, while the top edge is longer than
    # the bottom. Corners: top-left (160, 52), top-right
    # (1044, 116), bottom-left (161, 645), bottom-right (983, 624). This
    # rectangle is the smallest axis-aligned box that contains all four
    # corners of the screen content -- it includes a few pixels of bezel on
    # some sides rather than clipping any UI, since losing content is worse
    # than a small margin.
    # Grown 2026-09-24 (round 4, LCD-01 corner re-derivation) from W=886
    # H=593 to W=894 H=616, origin unchanged: lcd_sampler.py's re-derived
    # FRAME_CORNERS put the panel's bottom-right corner at (981, 662) and
    # top-right at (1045, 125), outside the old box (bottom edge 645, right
    # edge 1044). The new box ends at x=1052, y=668 -- all four round-4
    # corners plus a few pixels of bezel; every value stays even, since an
    # odd crop offset/size on this 4:2:2 source makes ffmpeg refuse.
    # (Superseded 2026-09-19 rectangle, kept for history: X=296 Y=58 W=853
    # H=578, corners left 298 (top)/323 (bottom), right ~1145-1147, top 60
    # (right)/86 (left), bottom 617 (right)/635 (left).)
    [int]$CropX = 158,
    [int]$CropY = 52,
    [int]$CropW = 894,
    [int]$CropH = 616
)

$ErrorActionPreference = "Stop"

if ($Frames -lt 1) { throw "-Frames must be >= 1" }

$ffmpeg = "C:\ffmpeg\ffmpeg.exe"
if (-not (Test-Path $ffmpeg)) {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($null -eq $cmd) { throw "ffmpeg not found at $ffmpeg and not on PATH." }
    $ffmpeg = $cmd.Source
}

# The C920 hands out stale/transitional frames right after the stream opens
# (previous screen contents, and auto-exposure/auto-focus still converging),
# so discard an initial run of frames covering both the old fixed 4-frame
# skip and -WarmupSeconds of wall-clock settling time (at the device's ~30
# fps capture rate), whichever asks for more frames. Without this the image
# can show the previous contents of the screen, or a blurred/still-adjusting
# one -- which, when the point of the capture is to confirm a flash landed or
# judge rendering, is exactly the wrong failure mode.
$warmupFrames = [Math]::Max(4, [Math]::Ceiling($WarmupSeconds * 30))

if ($Frames -eq 1) {
    # Old single-frame behaviour, unchanged: no scoring, no temp files.
    $filters = @("select=gte(n\,$warmupFrames)")
    if (-not $Full) {
        $filters += "crop=${CropW}:${CropH}:${CropX}:${CropY}"
        $filters += "scale=${Width}:-2"
    }
    $filterChain = $filters -join ","

    $ffArgs = @(
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
    & $ffmpeg @ffArgs
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed with exit code $LASTEXITCODE" }
    if (-not (Test-Path $Out)) { throw "ffmpeg reported success but $Out was not written." }
    $size = (Get-Item $Out).Length
    Write-Output "wrote $Out ($size bytes) [single frame, no scoring]"
    return
}

$tempDir = Join-Path ([IO.Path]::GetTempPath()) ("capture_lcd_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
    # Capture $Frames candidate frames, one per subsequent camera frame, after
    # discarding the warm-up run.
    $pattern = Join-Path $tempDir "cand_%03d.jpg"
    $captureFilterChain = "select=gte(n\,$warmupFrames)"
    $ffArgs = @(
        "-hide_banner", "-loglevel", "error",
        "-f", "dshow",
        "-video_size", "1280x720",
        "-i", "video=$Device",
        "-vf", $captureFilterChain,
        "-fps_mode", "vfr",
        "-frames:v", $Frames,
        "-q:v", "6",
        "-y", $pattern
    )
    & $ffmpeg @ffArgs
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed capturing candidate frames with exit code $LASTEXITCODE" }

    $candidates = @(Get-ChildItem -Path $tempDir -Filter "cand_*.jpg" | Sort-Object Name)
    if ($candidates.Count -eq 0) { throw "ffmpeg reported success but no candidate frames were written." }

    # Score each candidate's sharpness on the panel content (crop first, so a
    # sharp bezel doesn't win over a blurred screen) using a Laplacian edge
    # convolution's mean absolute response (signalstats YAVG on the
    # convolved, grayscale image) -- higher means more high-frequency detail,
    # i.e. sharper focus. No new dependency: this is stock ffmpeg filters.
    $scoreCropFilter = if ($Full) { $null } else { "crop=${CropW}:${CropH}:${CropX}:${CropY}" }
    $best = $null
    $bestIndex = -1
    $bestMetric = [double]::NegativeInfinity
    for ($i = 0; $i -lt $candidates.Count; $i++) {
        $f = $candidates[$i]
        $scoreFilters = @()
        if ($scoreCropFilter) { $scoreFilters += $scoreCropFilter }
        $scoreFilters += "format=gray"
        $scoreFilters += "convolution='0 -1 0 -1 4 -1 0 -1 0'"
        $scoreFilters += "signalstats"
        $scoreFilters += "metadata=print"
        $scoreChain = $scoreFilters -join ","

        $scoreArgs = @(
            "-hide_banner", "-loglevel", "info",
            "-i", $f.FullName,
            "-vf", $scoreChain,
            "-f", "null", "-"
        )
        # ffmpeg writes its metadata=print output to stderr; PowerShell 5.1
        # turns a native command's stderr lines into NativeCommandError
        # records if merged with 2>&1 directly, so redirect stderr to a file
        # instead and read it back as plain text.
        $scoreErrFile = Join-Path $tempDir "score_$i.err"
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            & $ffmpeg @scoreArgs 2> $scoreErrFile | Out-Null
        }
        finally {
            $ErrorActionPreference = $prevEap
        }
        $scoreOutput = Get-Content -Path $scoreErrFile -ErrorAction SilentlyContinue
        $metric = $null
        foreach ($line in $scoreOutput) {
            if ($line -match "lavfi\.signalstats\.YAVG=([0-9.eE+-]+)") {
                $metric = [double]$Matches[1]
                break
            }
        }
        if ($null -eq $metric) {
            Write-Warning "could not score candidate $($f.Name); skipping"
            continue
        }
        if ($metric -gt $bestMetric) {
            $bestMetric = $metric
            $bestIndex = $i
            $best = $f
        }
    }

    if ($null -eq $best) { throw "no candidate frame could be scored for sharpness." }

    $finalFilters = @()
    if (-not $Full) {
        $finalFilters += "crop=${CropW}:${CropH}:${CropX}:${CropY}"
        $finalFilters += "scale=${Width}:-2"
    }
    if ($finalFilters.Count -gt 0) {
        $finalFilterChain = $finalFilters -join ","
        $finalArgs = @(
            "-hide_banner", "-loglevel", "error",
            "-i", $best.FullName,
            "-vf", $finalFilterChain,
            "-y", $Out
        )
        & $ffmpeg @finalArgs
        if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed cropping the chosen frame with exit code $LASTEXITCODE" }
    }
    else {
        Copy-Item -Path $best.FullName -Destination $Out -Force
    }

    if (-not (Test-Path $Out)) { throw "chosen frame was not written to $Out." }
    $size = (Get-Item $Out).Length
    Write-Output "wrote $Out ($size bytes) [chose candidate $($bestIndex + 1) of $($candidates.Count), sharpness metric $bestMetric]"
}
finally {
    if (Test-Path $tempDir) {
        Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
