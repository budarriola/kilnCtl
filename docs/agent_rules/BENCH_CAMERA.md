# Bench LCD camera

Split out of CLAUDE.md 2026-10-14; history in docs/CLAUDE_HISTORY.md.

## LCD camera

`tools/PcTools/scripts/capture_lcd.ps1` grabs the bench webcam frame of the LCD (`-Full` for
uncropped; `-Frames N` keeps the sharpest of N after settling; default crop `X=158 Y=52 W=894
H=616`). Judge colors by numeric pixel sampling, never by eye or theme constants:
`sample_lcd_region.ps1 -Image full.jpg -X -Y -W -H` (also prints a bezel reference; always use
one). Current panel corners (`lcd_sampler.py` `FRAME_CORNERS`): TL (179,68), TR (1045,125), BL
(177,627), BR (981,662); the camera drifts, so re-measure with a `-Full` capture when a color
check reads INCONCLUSIVE. ffmpeg exit `-5` = another process holds the C920. Detail:
`docs/COMMISSIONING_LCD_RUNBOOK.md`. LCD pages must not scroll (320x480).
