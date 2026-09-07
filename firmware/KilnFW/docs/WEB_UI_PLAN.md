# Web UI plan — zones page clean-up, Chart.js, display-power Save

Status: **closed 2026-09-06.** All four items done or decided; nothing open.

- §1 (info-glyph disclosure for zones-page prose/tables) — landed, `zones_page.html`'s
  `infoHtml()`/`<details class="info">`.
- §2 (per-group "same as zone N", schema v20→v21) — landed, `5672719`+`0126f24`. See
  ROADMAP.md's "Zones page clean-up" row for the two follow-on notes from that review
  (`timing_profile` inheritance narrowed; test provenance split across `09769f5a`/`51c084f9`).
- §3 (Chart.js) — assessed, **not adopted**: ~60 KB gz for zoom/pan the hand-rolled canvas
  graph already covers with a ~40-line pointer-drag window if ever wanted.
- §4 (display-power Save button) — already fixed 2026-09-04, confirmed flashed in `05087f0`.
