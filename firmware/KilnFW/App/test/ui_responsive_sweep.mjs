// ui_responsive_sweep.mjs -- Phase 0 of WEB_UI_RESPONSIVE.md ("a real
// test matrix"). Drives real headless Chrome over the Chrome DevTools
// Protocol (CDP) via a plain WebSocket, so no npm/pip install is required --
// this toolchain has neither Playwright nor Puppeteer available (see
// check_stop_bar_body_padding.ps1's header comment, written before this
// file existed: "This script cannot render a page (no browser/jsdom in this
// toolchain)"). That was true for a grep-based .ps1 check with no browser
// dependency declared; it stops being true here because Node 18+ ships a
// native `WebSocket` global and Chrome/Edge are already installed on this
// machine -- nothing new to fetch.
//
// Pages are served from a throwaway 127.0.0.1 static file server rooted at
// firmware/KilnFW/App/drivers/ (this process spawns and kills it -- see
// startStaticServer() below), NOT from the live board. Plain file:// URLs
// were tried first and do not work: every page loads its shared chrome via
// `<script defer src="/nav.js">` / `<script defer src="/app.js">` and
// `<link href="/theme.css">` -- an ABSOLUTE root path, correct on the
// board's real httpd, but under file:// "/nav.js" resolves to the
// filesystem root (e.g. C:/nav.js), not the HTML file's own directory. A
// file:// sweep silently never loads nav.js/app.js at all -- no topbar, no
// stop bar, nothing this plan's own bug list (sec 3) is actually about.
// §4 anticipated exactly this ("a local static serve of the pages"); a
// same-origin http://127.0.0.1 static server is what makes root-relative
// paths resolve the way the board does, with no board involved.
//
// Two independent reasons this must still never be the live board: (1) a
// firing is in progress and the board's HTTP server wedged under load once
// already -- this tool must never poll it; (2) Phase 0 has to work from
// source before Phase 1/2 land, and the board only ever runs one version of
// the UI at a time. Every fetch() the pages issue (app.js's heartbeat poll)
// targets a relative /api/... URL that this static server does not
// implement -- it 404s immediately with no board contacted, and the pages'
// own .catch() handlers already treat "no server" as "show the
// disconnected banner", which is exactly what should happen here too.
//
// What this DOES exercise: the real static DOM, real theme.css cascade,
// real nav.js topbar/menu injection, and real app.js stop-bar construction
// -- all of which run with no data from the board. What it CANNOT exercise:
// layout that only appears after live data populates a table (e.g. a long
// zone list, a fault history with many rows) or JS branches gated on a
// successful fetch. That is a real, stated limitation of this static
// approximation, not an oversight -- see the report this script's caller
// (the Phase 0 task) files alongside it.
//
// Usage:
//   node firmware/KilnFW/App/test/ui_responsive_sweep.mjs
//   node firmware/KilnFW/App/test/ui_responsive_sweep.mjs --pages foo_page.html,bar_page.html
//   node firmware/KilnFW/App/test/ui_responsive_sweep.mjs --dir <alt drivers dir> --widths 320,1920
//
// Exit code 0 iff every page passes every assertion at every width.

import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { existsSync } from 'node:fs';
import { readFile, mkdtemp, rm } from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const { resolveDriversDir, listDriverFiles, walkFiles } = require('./_drivers_layout.js');
const DEFAULT_DRIVERS_DIR = resolveDriversDir(__dirname);
const DEFAULT_WIDTHS = [320, 360, 390, 768, 1280, 1920];
const VIEWPORT_HEIGHT = 1400; // tall enough that vertical scroll never masks a horizontal-overflow bug
const MIN_TARGET_PX = 32; // profiles_page.html catalogue Use/Save bug measured 81x19 -- see WEB_UI_RESPONSIVE.md sec 3 item 3

function findChrome() {
  const candidates = [
    process.env.KC_SWEEP_CHROME,
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
  ].filter(Boolean);
  for (const c of candidates) {
    if (existsSync(c)) return c;
  }
  return null;
}

function parseArgs(argv) {
  const out = { dir: DEFAULT_DRIVERS_DIR, widths: DEFAULT_WIDTHS, pages: null, port: 9333, staticPort: 9334 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--dir') out.dir = path.resolve(argv[++i]);
    else if (a === '--widths') out.widths = argv[++i].split(',').map(s => parseInt(s.trim(), 10));
    else if (a === '--pages') out.pages = argv[++i].split(',').map(s => s.trim());
    else if (a === '--port') out.port = parseInt(argv[++i], 10);
    else if (a === '--static-port') out.staticPort = parseInt(argv[++i], 10);
  }
  return out;
}

// Minimal same-origin static file server rooted at `dir` -- exists solely so
// the pages' root-relative <script src="/nav.js"> etc. resolve the way they
// do against the board's real httpd (see the file header comment for why
// file:// cannot do this). 127.0.0.1 only, one throwaway port, killed in
// main()'s finally. Not a general-purpose server: just enough MIME/path
// handling for this driver's own file set (.html/.js/.css), a bare 404 for
// anything else (in particular /api/... -- deliberately unimplemented, see
// header comment) and directory-traversal is blocked by rejecting any
// resolved path that escapes `dir`.
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json' };
function startStaticServer(dir, port) {
  // The pages request every asset by a root-relative path (`/nav.js`,
  // `/theme.css`, ...), correct against the board's real httpd, which
  // serves a flat namespace. Once drivers/ splits into layer subdirs
  // (drivers/hw/, drivers/http/, ...) a literal `path.join(dir, rel)` no
  // longer finds anything not still sitting at the top of `dir`. Resolve
  // every request by BASENAME anywhere under `dir` instead -- the same
  // flat-namespace illusion the board's httpd already presents, and the
  // same recursive-basename contract every other layout-agnostic script in
  // this directory uses (see _drivers_layout.js). Directory traversal is
  // still blocked structurally: the lookup never touches the requested
  // path's directory component at all, only its basename.
  const server = createServer(async (req, res) => {
    try {
      const urlPath = decodeURIComponent(new URL(req.url, 'http://x').pathname);
      const basename = path.basename(urlPath === '/' ? '/main_page.html' : urlPath);
      const matches = walkFiles(dir).filter((p) => path.basename(p) === basename);
      if (matches.length !== 1) { res.writeHead(404); res.end('not found'); return; }
      const abs = matches[0];
      const ext = path.extname(abs);
      const body = await readFile(abs);
      res.writeHead(200, { 'Content-Type': MIME[ext] || 'application/octet-stream' });
      res.end(body);
    } catch {
      res.writeHead(404);
      res.end('not found');
    }
  });
  // The fixed default (9334) can already be held by a leftover process from a
  // previous run or a concurrent gate on this same machine -- that is an
  // environment fact, not a layout regression, and reporting it as a sweep
  // FAILURE would be exactly the false-failure class this driver's own
  // callers (check_ui_responsive_sweep.ps1, the regression suite) are meant
  // to avoid. On EADDRINUSE, retry once on an OS-assigned ephemeral port
  // (0) and hand the actual bound port back to the caller via
  // `server.address().port` -- any other listen error still rejects.
  return new Promise((resolve, reject) => {
    const onError = (err) => {
      if (err && err.code === 'EADDRINUSE') {
        server.removeListener('error', onError);
        server.once('error', reject);
        server.listen(0, '127.0.0.1', () => resolve(server));
      } else {
        reject(err);
      }
    };
    server.once('error', onError);
    server.listen(port, '127.0.0.1', () => resolve(server));
  });
}

// Chrome's --remote-debugging-port has no EADDRINUSE fallback of its own --
// unlike startStaticServer()'s http.Server above, a busy CDP port just makes
// Chrome silently pick something else (or refuse), leaving waitForPort()
// spinning until its own timeout with no clue why. Rather than let Chrome
// guess, check the requested port for availability the same way
// startStaticServer() does, and fall back to an OS-assigned ephemeral port
// on conflict -- so a leftover process (or a concurrent sweep on this same
// machine) holding 9333 is the same non-failure it already is for 9334.
function tryListen(port) {
  return new Promise((resolve, reject) => {
    const probe = createServer();
    probe.once('error', (err) => {
      probe.close();
      reject(err);
    });
    probe.once('listening', () => {
      const bound = probe.address().port;
      probe.close(() => resolve(bound));
    });
    probe.listen(port, '127.0.0.1');
  });
}

async function pickPort(preferred) {
  try {
    return await tryListen(preferred);
  } catch (err) {
    if (err && err.code === 'EADDRINUSE') {
      return tryListen(0);
    }
    throw err;
  }
}

async function waitForPort(port, timeoutMs, chrome) {
  const deadline = Date.now() + timeoutMs;
  let lastErr;
  while (Date.now() < deadline) {
    if (chrome && chrome.exitCode !== null) {
      throw new Error(`Chrome DevTools port ${port} never came up: Chrome process exited early (code ${chrome.exitCode}). stderr tail:\n${(chrome.stderrTail || []).join('')}`);
    }
    try {
      const r = await fetch(`http://127.0.0.1:${port}/json/version`);
      if (r.ok) return;
    } catch (e) {
      lastErr = e;
    }
    await new Promise(r => setTimeout(r, 100));
  }
  throw new Error(`Chrome DevTools port ${port} never came up (waited ${timeoutMs}ms): ${lastErr}. stderr tail:\n${(chrome && chrome.stderrTail || []).join('')}`);
}

class CdpSession {
  constructor(ws) {
    this.ws = ws;
    this.nextId = 1;
    this.pending = new Map();
    this.eventWaiters = [];
    // Without this, a Chrome process that dies mid-sweep (crash, OOM under
    // load, killed externally) closes the socket with every in-flight
    // send() promise still unsettled -- they never resolve or reject, so
    // sweepOnePage()'s await hangs forever and the whole tool never exits,
    // with no diagnostic at all (observed in testing: two node processes
    // sat idle indefinitely after Chrome disappeared from the process
    // list). Reject every pending request, and any future send(), as soon
    // as the socket goes away.
    const failPending = (why) => {
      const err = new Error(`CDP connection closed: ${why}`);
      for (const { reject } of this.pending.values()) reject(err);
      this.pending.clear();
      this.closed = err;
    };
    ws.addEventListener('close', (ev) => failPending(`code=${ev.code} reason=${ev.reason || '(none)'}`));
    ws.addEventListener('error', (ev) => failPending(ev.message || 'unknown error'));
    ws.addEventListener('message', (ev) => {
      const msg = JSON.parse(ev.data);
      if (msg.id !== undefined && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        if (msg.error) reject(new Error(JSON.stringify(msg.error)));
        else resolve(msg.result);
      } else if (msg.method) {
        this.eventWaiters = this.eventWaiters.filter((w) => {
          if (w.method === msg.method) { w.resolve(msg.params); return false; }
          return true;
        });
      }
    });
  }

  send(method, params = {}) {
    if (this.closed) return Promise.reject(this.closed);
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }

  waitForEvent(method, timeoutMs = 10000) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.eventWaiters = this.eventWaiters.filter(w => w.resolve !== wrapped);
        reject(new Error(`timed out waiting for ${method}`));
      }, timeoutMs);
      const wrapped = (params) => { clearTimeout(timer); resolve(params); };
      this.eventWaiters.push({ method, resolve: wrapped });
    });
  }
}

// Per-page data fixtures, keyed by filename, run BEFORE SETUP_SCRIPT below.
// WEB_UI_RESPONSIVE.md sec 3 items 2 and 4 were found to not reproduce
// under this sweep. Item 4 (diagnostics_page.html flex-wrap button overlap)
// no longer applies -- that markup was rewritten into a stacked
// `.linklist` block (see diagnostics_page.html's own 2026-08-22 comment
// above `.linklist`), so there is nothing left to fixture for it; the
// general overlap/undersized-target checks already cover that block at
// every width the same as any other page content.
//
// Item 2 (zones_page.html: input.offsettle overlapping button#saveBtn,
// 1280px) DOES still need a fixture, but not the one first assumed --
// #zones never gets live data under this static server (loadCurrent()'s
// fetch('/api/zones') 404s, so renderZones() never even runs), but
// renderZones() does not actually need a real server response: `current`
// defaults to `{ thermo_count: 0, relay_count: 0, zones: [] }` and every
// missing zone i already falls back to a hardcoded default object in
// renderZones() itself. So the only thing missing is a non-zero
// thermoCount/relayCount and a call to trigger the render that the real
// page only makes after a successful fetch -- no mock server needed.
// zones_page.html's tuning-recommendation panel (`#tuningRecPanel`, added in
// 333dd4e) is a second, independent piece of dynamic content on this page:
// it starts empty ("Enter a peak temperature above and click Recommend."),
// then re-renders via renderTuningRecommendation() into one of FOUR distinct
// states -- see that page's own TUNING_REC_LOOKUP_START comment block:
//   measured / extrapolated / indistinguishable  (three confidence bases)
//   plus the "no artifact" state (unavailable evidence JSON, or nothing
//   entered yet -- the state every real board without the artifact shows).
// Two real defects already shipped and were caught by REVIEW, not by this
// sweep (7ba7bba: off-by-one byte truncated the served artifact so the panel
// never rendered at all; ce595d2: an out-of-range peak rendered green
// MEASURED and a missing confidence field threw a TypeError) -- neither
// would have been visible to a sweep that only ever sees the pre-Recommend
// empty state, because the static server never serves /api/tuning_recommendations
// (see file header). So each state below is reached the same way the real
// page reaches it -- by setting window.tuningRecArtifact (the module-level
// var the page's own fetch() assigns into) to a fixture artifact shaped like
// the real JSON, filling the peak/load inputs, and calling the page's own
// exported renderTuningRecommendation() -- never by hand-writing HTML into
// #tuningRecResult, which would test this sweep's fixture, not the page.
const TUNING_REC_VARIANTS = {
  no_artifact: `
    window.tuningRecArtifact = null;
    document.getElementById('tuningRecPeak').value = '1000';
    document.getElementById('tuningRecLoad').value = 'any';
    window.renderTuningRecommendation();
  `,
  measured: `
    window.tuningRecArtifact = { schema_version: 1, sim_confidence: { extrapolation_boundary_c: 1100 },
      recommendations: [{ load: 'empty', peak_temp_c_max: 1000, confidence: 'measured',
        method: 'relay_autotune', rule: 'ziegler_nichols',
        why: 'Held-out simulation runs at this peak/load confirm this method tracks best.' }] };
    document.getElementById('tuningRecPeak').value = '900';
    document.getElementById('tuningRecLoad').value = 'empty';
    window.renderTuningRecommendation();
  `,
  extrapolated: `
    window.tuningRecArtifact = { schema_version: 1, sim_confidence: { extrapolation_boundary_c: 900 },
      recommendations: [{ load: 'full', peak_temp_c_max: 900, confidence: 'measured',
        method: 'relay_autotune', rule: 'ziegler_nichols', why: 'Checked run at this peak/load.' }] };
    document.getElementById('tuningRecPeak').value = '1350';
    document.getElementById('tuningRecLoad').value = 'full';
    window.renderTuningRecommendation();
  `,
  indistinguishable: `
    window.tuningRecArtifact = { schema_version: 1, sim_confidence: { extrapolation_boundary_c: 1100 },
      recommendations: [{ load: 'light', peak_temp_c_max: 1000, confidence: 'indistinguishable',
        method: 'relay_autotune', runner_up: 'step_test', margin_c: 0.31,
        why: 'Both methods land within simulation noise at this peak/load.' }] };
    document.getElementById('tuningRecPeak').value = '850';
    document.getElementById('tuningRecLoad').value = 'light';
    window.renderTuningRecommendation();
  `,
};

const ZONES_BASE_FIXTURE = `
  var tc = document.getElementById('thermoCount');
  var rc = document.getElementById('relayCount');
  if (!tc || !rc || typeof window.renderZones !== 'function') return 'renderZones not found';
  tc.value = 3;
  rc.value = 4;
  window.renderZones();
`;

// PAGE_FIXTURES values are either a single fixture-script string (run once,
// as before) or an array of {suffix, script} variants -- main() sweeps every
// variant at every width, so a page with N variants gets N times as many
// (page, width) rows, each labelled 'zones_page.html [suffix]' in the
// report. zones_page.html carries one variant per tuningRecPanel state (see
// TUNING_REC_VARIANTS above) so every state is actually rendered and swept,
// not just the pre-Recommend empty one the static server would otherwise
// leave in place.
const PAGE_FIXTURES = {
  'zones_page.html': Object.keys(TUNING_REC_VARIANTS).map((suffix) => ({
    suffix,
    script: `
(function () {
${ZONES_BASE_FIXTURE}
  ${TUNING_REC_VARIANTS[suffix]}
  return 'ok';
})()
`,
  })),
};

// Setup script: mutates page state into the "worst case" the assertion pass
// below needs to see -- the sticky Stop/Pause bar shown (WEB_UI_RESPONSIVE.md
// sec 3's "bar covers the last interactive element" class only manifests while
// it's up) and every <details> disclosure sprung open (its content is only
// reachable to a real user after they click <summary>, so that's the state
// worth testing -- and closed content otherwise confuses the assertion pass's
// isVisible(), see below).
//
// This MUST run, and finish, before sweepOnePage measures document.scrollHeight
// to size the emulated viewport (see that function's comment) -- both
// mutations add real height to the page. Doing them inside the assertion
// pass instead (as a single combined script used to) let the height
// measurement run against the SHORT pre-mutation page, then grew the
// viewport to that stale, too-small height; position:fixed's `bottom: 0`
// bar was then anchored above content the mutations had since pushed further
// down, producing occlusion/overlap failures on safety_commissioning_page.html
// that were an artifact of this ordering bug, not a real layout defect --
// caught by comparing computed body padding-bottom against the actual
// distance from content bottom to the (correctly-computed) Stop bar top.
const SETUP_SCRIPT = `
(function () {
  var bar = document.querySelector('.kc-stop-bar');
  if (bar) {
    bar.removeAttribute('hidden');
    var stopBtn = bar.querySelector('.kc-stop-btn');
    var pauseBtn = bar.querySelector('.kc-pause-btn');
    if (stopBtn) stopBtn.removeAttribute('hidden');
    if (pauseBtn) { pauseBtn.removeAttribute('hidden'); pauseBtn.textContent = 'Pause'; }
    if (window.kcNav && window.kcNav.updateBodyPadding) window.kcNav.updateBodyPadding();
  }

  var allDetails = document.querySelectorAll('details');
  for (var di = 0; di < allDetails.length; di++) {
    // zones_page.html's <details class="advguards"> is deliberately left
    // CLOSED here -- see WEB_UI_RESPONSIVE.md sec 3 item 2 and that
    // page's own 2026-08-22 comment above ".advguards:not([open])": the
    // "offsettle overlaps saveBtn" bug this sweep exists to catch only
    // manifests while the <details> is closed (Chromium still lays out a
    // closed <details>'s non-summary children even though it doesn't
    // paint them). Forcing it open here, like every other <details> on
    // every other page, would make this sweep permanently blind to a
    // regression of the very CSS rule that fixes it.
    if (allDetails[di].classList && allDetails[di].classList.contains('advguards')) continue;
    allDetails[di].open = true;
  }
  if (allDetails.length && window.kcNav && window.kcNav.updateBodyPadding) window.kcNav.updateBodyPadding();
  true;
})()
`;

// The in-page assertion script. Runs inside the target page via
// Runtime.evaluate, after SETUP_SCRIPT above and after the viewport has been
// grown to fit the (now fully mutated) document. Deliberately framework-free
// (no injected library) -- just DOM/CSSOM calls every evergreen browser
// supports.
const IN_PAGE_SCRIPT = `
(function () {
  function isVisible(el) {
    if (!(el instanceof Element)) return false;
    if (el.hasAttribute('hidden')) return false;
    var cs = getComputedStyle(el);
    if (cs.display === 'none' || cs.visibility === 'hidden' || cs.visibility === 'collapse') return false;
    if (parseFloat(cs.opacity) === 0) return false;
    // checkVisibility(), not just display/visibility/opacity above: a closed
    // <details>'s non-summary children are hidden via content-visibility in
    // Chromium, not display:none, so getComputedStyle().display still reads
    // 'inline-block' and getBoundingClientRect() still reports a real-looking
    // box for them. SETUP_SCRIPT (run before this) already forces every
    // <details> open, so this only matters for a page this sweep doesn't
    // know to force open some other way -- keep it as a real check, not a
    // vacuous one, since checkVisibility() is exactly the API built to
    // answer "can the user actually see this".
    if (el.checkVisibility && !el.checkVisibility()) return false;
    var r = el.getBoundingClientRect();
    return r.width > 0 && r.height > 0;
  }

  function isInteractive(el) {
    var tag = el.tagName.toLowerCase();
    if (tag === 'button') return true;
    if (tag === 'a' && el.hasAttribute('href')) return true;
    if (tag === 'select' || tag === 'textarea') return true;
    if (tag === 'input') {
      var t = (el.getAttribute('type') || 'text').toLowerCase();
      return t !== 'hidden';
    }
    var role = el.getAttribute('role');
    if (role === 'button' || role === 'link') return true;
    if (tag === 'summary') return true;
    return false;
  }

  var bar = document.querySelector('.kc-stop-bar');

  var viewportW = document.documentElement.clientWidth;
  var docEl = document.scrollingElement || document.documentElement;

  var overflowFail = docEl.scrollWidth > viewportW + 1; // +1: sub-pixel rounding
  var overflowDetail = overflowFail ? ('scrollWidth=' + docEl.scrollWidth + ' > viewport=' + viewportW) : '';

  var all = Array.from(document.querySelectorAll('*')).filter(isInteractive).filter(isVisible);

  // Occlusion check: for each interactive element's centre point, ask the
  // browser what element actually paints there. Catches BOTH two
  // interactive elements overlapping each other and a non-interactive
  // element (the fixed Stop bar, a card sliding under another) covering an
  // interactive one -- the general form of every overlap bug in sec 3.
  // Requires the caller to have already sized the viewport to the full
  // document height (see sweepOnePage's two-pass Emulation.setDeviceMetricsOverride)
  // so every element's centre point is on-screen with no scrolling -- an
  // un-clamped centre point on a viewport shorter than the document would
  // silently clamp to the viewport's bottom edge and test the WRONG point
  // for anything below the fold, which is exactly the false-positive this
  // sweep hit during development (every autotune control on zones_page.html
  // "covered by" a heading 900px above it, purely from clamping).
  var occluded = [];
  for (var i = 0; i < all.length; i++) {
    var el = all[i];
    var r = el.getBoundingClientRect();
    var cx = r.left + r.width / 2;
    var cy = r.top + r.height / 2;
    if (cx < 0 || cx >= viewportW || cy < 0 || cy >= window.innerHeight) continue; // off-viewport; not an occlusion question here
    var top = document.elementFromPoint(cx, cy);
    if (!top) continue;
    if (top !== el && !el.contains(top) && !top.contains(el)) {
      occluded.push({
        covered: describeEl(el),
        coveredBy: describeEl(top),
      });
    }
  }

  // Belt-and-suspenders pairwise rect intersection between interactive
  // elements themselves (independent of paint order / elementFromPoint),
  // excluding ancestor/descendant pairs.
  var pairsOverlap = [];
  for (var a = 0; a < all.length; a++) {
    for (var b = a + 1; b < all.length; b++) {
      var ea = all[a], eb = all[b];
      if (ea.contains(eb) || eb.contains(ea)) continue;
      var ra = ea.getBoundingClientRect(), rb = eb.getBoundingClientRect();
      var ix = Math.min(ra.right, rb.right) - Math.max(ra.left, rb.left);
      var iy = Math.min(ra.bottom, rb.bottom) - Math.max(ra.top, rb.top);
      if (ix > 2 && iy > 2) { // >2px so shared borders/hairline rounding don't false-positive
        pairsOverlap.push({ a: describeEl(ea), b: describeEl(eb) });
      }
    }
  }

  // Two narrow, deliberate exemptions from the flat 32px floor -- both
  // documented exceptions, not loosening for noise's sake:
  //   - native checkbox/radio inputs: their rendered box is platform chrome
  //     (~13-16px in every browser) that page CSS does not size; WCAG 2.5.8
  //     itself exempts these ("the size is determined by the user agent").
  //     Real interlocks here (e.g. #continueOnZoneTrip) still have a <label>
  //     that extends the actual click target -- a separate, legitimate
  //     question this sweep does not attempt to answer.
  //   - an <a> left at its default inline display, sitting inside running
  //     text (its parent renders inline content) rather than styled as a
  //     button: WCAG 2.5.8's own "inline" exception. This is what nav.js's
  //     menu links and the plain cross-links inside a <p> hint are; the
  //     bug this assertion exists to catch (sec 3 item 3) was a styled
  //     <button>, not a text link.
  function isExemptFromTargetSize(el) {
    var tag = el.tagName.toLowerCase();
    if (tag === 'input') {
      var t = (el.getAttribute('type') || 'text').toLowerCase();
      if (t === 'checkbox' || t === 'radio') return true;
    }
    if (tag === 'a' && getComputedStyle(el).display === 'inline') return true;
    return false;
  }

  var undersized = [];
  for (var j = 0; j < all.length; j++) {
    var el2 = all[j];
    if (isExemptFromTargetSize(el2)) continue;
    var r2 = el2.getBoundingClientRect();
    if (r2.width < ${MIN_TARGET_PX} || r2.height < ${MIN_TARGET_PX}) {
      undersized.push({ el: describeEl(el2), width: Math.round(r2.width), height: Math.round(r2.height) });
    }
  }

  var clipped = [];
  for (var k = 0; k < all.length; k++) {
    var el3 = all[k];
    var r3 = el3.getBoundingClientRect();
    if (r3.right <= 0 || r3.left >= viewportW || r3.bottom <= 0) {
      clipped.push(describeEl(el3));
    }
  }

  function describeEl(el) {
    var id = el.id ? ('#' + el.id) : '';
    var cls = el.className && typeof el.className === 'string' ? ('.' + el.className.trim().split(/\\s+/).join('.')) : '';
    var text = (el.textContent || '').trim().slice(0, 24);
    return el.tagName.toLowerCase() + id + cls + (text ? (' "' + text + '"') : '');
  }

  // Captures the tuningRecPanel confidence badge (the '[MEASURED]' /
  // '[EXTRAPOLATED]' / '[INDISTINGUISHABLE]' span renderTuningRecommendation()
  // writes into #tuningRecResult) so main() can compare it ACROSS the
  // TUNING_REC_VARIANTS runs below and catch a regression that makes two
  // confidence states render identically -- see that check's own comment
  // for why text alone is not enough (an operator mistaking an
  // extrapolated recommendation for a measured one was the review finding
  // that added this panel's confidence rendering in the first place).
  // null on any page without the panel (every page but zones_page.html).
  var tuningBadgeEl = document.querySelector('#tuningRecResult span');
  var tuningBadge = tuningBadgeEl
    ? { color: getComputedStyle(tuningBadgeEl).color, text: tuningBadgeEl.textContent }
    : null;

  return JSON.stringify({
    overflowFail: overflowFail,
    overflowDetail: overflowDetail,
    occluded: occluded,
    pairsOverlap: pairsOverlap,
    undersized: undersized,
    clipped: clipped,
    interactiveCount: all.length,
    tuningBadge: tuningBadge,
  });
})()
`;

async function newTab(port) {
  const r = await fetch(`http://127.0.0.1:${port}/json/new?about:blank`, { method: 'PUT' });
  return r.json();
}

async function closeTab(port, id) {
  try { await fetch(`http://127.0.0.1:${port}/json/close/${id}`); } catch { /* best-effort */ }
}

async function sweepOnePage(port, fileUrl, width, fixtureScript) {
  const tab = await newTab(port);
  const ws = new WebSocket(tab.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve);
    ws.addEventListener('error', reject);
  });
  const cdp = new CdpSession(ws);
  try {
    await cdp.send('Page.enable');
    await cdp.send('Runtime.enable');
    await cdp.send('Emulation.setDeviceMetricsOverride', {
      width, height: VIEWPORT_HEIGHT, deviceScaleFactor: 1, mobile: false,
    });
    const navPromise = cdp.waitForEvent('Page.loadEventFired', 15000);
    await cdp.send('Page.navigate', { url: fileUrl });
    await navPromise;
    // Deferred scripts (nav.js/app.js) run after DOMContentLoaded but the
    // load event already implies parsing finished; give one macrotask tick
    // for their IIFEs (both run synchronously on script execution, no
    // additional async work before DOM is built) plus a safety margin.
    await cdp.send('Runtime.evaluate', { expression: 'new Promise(r => setTimeout(r, 150))', awaitPromise: true });

    // Page-specific data fixture (e.g. populating zones_page.html's #zones
    // the way a real /api/zones response would) runs BEFORE SETUP_SCRIPT so
    // the content it creates is in place for the Stop-bar/<details> mutation
    // and the height measurement below.
    if (fixtureScript) {
      const fixtureResult = await cdp.send('Runtime.evaluate', { expression: fixtureScript, returnByValue: true });
      if (fixtureResult.exceptionDetails) {
        throw new Error('fixture script threw: ' + JSON.stringify(fixtureResult.exceptionDetails));
      }
    }

    // Run the state-mutating setup (Stop bar shown, every <details> sprung
    // open) BEFORE measuring scrollHeight below -- see SETUP_SCRIPT's own
    // comment for why the ordering matters.
    const setupResult = await cdp.send('Runtime.evaluate', { expression: SETUP_SCRIPT });
    if (setupResult.exceptionDetails) {
      throw new Error('setup script threw: ' + JSON.stringify(setupResult.exceptionDetails));
    }

    // Second pass: grow the viewport to the full document height (at the
    // fixed WIDTH under test) so every element's centre point lands inside
    // the viewport for the occlusion check below -- no scrolling, so no
    // clamping-to-the-fold bug. document.title read first just to force
    // layout; scrollHeight already reflects layout by this point regardless.
    const heightResult = await cdp.send('Runtime.evaluate', {
      expression: '(document.scrollingElement || document.documentElement).scrollHeight',
      returnByValue: true,
    });
    const fullHeight = Math.min(Math.max(heightResult.result.value || VIEWPORT_HEIGHT, VIEWPORT_HEIGHT), 30000);
    await cdp.send('Emulation.setDeviceMetricsOverride', {
      width, height: fullHeight, deviceScaleFactor: 1, mobile: false,
    });
    await cdp.send('Runtime.evaluate', { expression: 'new Promise(r => setTimeout(r, 50))', awaitPromise: true });

    const result = await cdp.send('Runtime.evaluate', { expression: IN_PAGE_SCRIPT, returnByValue: true });
    if (result.exceptionDetails) {
      throw new Error('page script threw: ' + JSON.stringify(result.exceptionDetails));
    }
    return JSON.parse(result.result.value);
  } finally {
    ws.close();
    await closeTab(port, tab.id);
  }
}

function formatFailures(page, width, r) {
  const lines = [];
  if (r.overflowFail) lines.push(`  [overflow] ${page} @${width}px: ${r.overflowDetail}`);
  for (const o of r.occluded) lines.push(`  [occluded] ${page} @${width}px: ${o.covered} is covered by ${o.coveredBy}`);
  for (const p of r.pairsOverlap) lines.push(`  [overlap]  ${page} @${width}px: ${p.a} overlaps ${p.b}`);
  for (const u of r.undersized) lines.push(`  [target]   ${page} @${width}px: ${u.el} is ${u.width}x${u.height}px (< ${MIN_TARGET_PX}px)`);
  for (const c of r.clipped) lines.push(`  [clipped]  ${page} @${width}px: ${c} is off-viewport`);
  return lines;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const chromePath = findChrome();
  if (!chromePath) {
    console.error('ui_responsive_sweep: no Chrome/Edge executable found. Set KC_SWEEP_CHROME to a browser path.');
    process.exit(2);
  }
  if (!existsSync(args.dir)) {
    console.error(`ui_responsive_sweep: drivers dir not found: ${args.dir}`);
    process.exit(2);
  }

  let pageFiles = args.pages;
  if (!pageFiles) {
    pageFiles = listDriverFiles(args.dir, (name) => name.endsWith('_page.html'))
      .map((f) => f.name)
      .sort();
  }
  if (pageFiles.length === 0) {
    console.error(`ui_responsive_sweep: no *_page.html files found under ${args.dir} -- glob or directory is broken, not a clean tree.`);
    process.exit(2);
  }

  const cdpPort = await pickPort(args.port);
  if (cdpPort !== args.port) {
    // console.log, not console.error: this is a handled fallback, not a
    // failure -- check_ui_responsive_sweep.ps1 runs this script with
    // `$ErrorActionPreference = "Stop"` and captures via `2>&1`, under which
    // Windows PowerShell treats ANY stderr line from a native command as a
    // terminating error (turning a successful port fallback into a hard
    // abort of the whole check, before the sweep itself ever ran).
    console.log(`ui_responsive_sweep: CDP port ${args.port} was busy, using ${cdpPort} instead`);
  }
  args.port = cdpPort;

  // A fixed --user-data-dir would collide across concurrent sweeps on the
  // same machine (two check_ui_responsive_sweep.ps1 runs, or a manual run
  // alongside CI): Chrome refuses to start a second instance against a
  // profile dir another live Chrome process already holds a SingletonLock
  // on, and exits near-silently (stdio is 'ignore') without ever binding
  // the fallback CDP port pickPort() chose -- waitForPort() then spins for
  // the full timeout with no clue why ("Chrome DevTools port ... never came
  // up"). A fresh mkdtemp'd dir per run removes the collision entirely; it
  // is deleted in main()'s finally so repeated runs don't litter TEMP.
  const userDataDir = await mkdtemp(path.join(os.tmpdir(), 'kc-ui-sweep-profile-'));

  const chrome = spawn(chromePath, [
    `--remote-debugging-port=${args.port}`,
    '--headless=new',
    '--disable-gpu',
    '--no-first-run',
    '--no-default-browser-check',
    '--disable-extensions',
    '--hide-scrollbars',
    `--user-data-dir=${userDataDir}`,
  ], { stdio: ['ignore', 'ignore', 'pipe'] });
  chrome.stderrTail = [];
  chrome.stderr.on('data', (chunk) => {
    chrome.stderrTail.push(chunk.toString());
    // Keep only the last ~4000 chars -- enough for a startup failure
    // message, not an unbounded buffer over a long sweep.
    const joined = chrome.stderrTail.join('');
    if (joined.length > 4000) chrome.stderrTail = [joined.slice(-4000)];
  });

  const staticServer = await startStaticServer(args.dir, args.staticPort);
  // startStaticServer falls back to an ephemeral port if args.staticPort was
  // busy (EADDRINUSE) -- read back whatever it actually bound rather than
  // assuming the requested port, or every page URL below would 404 against
  // a server that isn't there.
  const staticPort = staticServer.address().port;
  if (staticPort !== args.staticPort) {
    // console.log, not console.error -- see the matching comment on the CDP
    // port fallback above: a stderr line here aborts check_ui_responsive_sweep.ps1
    // outright under its `$ErrorActionPreference = "Stop"` + `2>&1` capture.
    console.log(`ui_responsive_sweep: static port ${args.staticPort} was busy, using ${staticPort} instead`);
  }

  const rows = [];
  let anyFail = false;
  let devtoolsSkip = null;

  try {
    try {
      await waitForPort(args.port, 30000, chrome);
    } catch (portErr) {
      // A DevTools port that never comes up is, on this toolchain, mostly an
      // environment fact (headless Chrome failing/slow to start under load,
      // AV scanning a freshly spawned exe, a stuck leftover process from a
      // previous run) rather than a signal about the UI code this sweep
      // exists to check -- see this file's header and 3e5df77/pickPort()
      // for the port-collision case this already handles. Treat it as a
      // loud SKIP (exit 0, clearly labelled) rather than a hard FAIL, so a
      // flaky bench machine does not block run_all_checks.ps1 on a check
      // that never got to look at a single page. A REAL layout regression
      // still fails hard below -- this branch only covers "never got that
      // far at all".
      devtoolsSkip = portErr && portErr.message || String(portErr);
    }

    for (const pf of devtoolsSkip ? [] : pageFiles) {
      const pageUrl = `http://127.0.0.1:${staticPort}/${pf}`;
      const fixture = PAGE_FIXTURES[pf];
      // A fixture is either a single script (run once, no label) or an
      // array of {suffix, script} variants -- see PAGE_FIXTURES' own
      // comment. Normalise to a variants array here so the sweep loop
      // below is the same either way.
      const variants = Array.isArray(fixture)
        ? fixture
        : [{ suffix: null, script: fixture }];

      // suffix -> { color, text } from the FIRST width swept for that
      // variant (the badge does not depend on viewport width -- it's the
      // same DOM content at every width, only its layout position moves).
      // Used by the tuningBadge distinctness check below, once all
      // variants for this page have been swept.
      const badgesBySuffix = {};

      for (const variant of variants) {
        const label = variant.suffix ? `${pf} [${variant.suffix}]` : pf;
        for (const width of args.widths) {
          let result, failures;
          try {
            result = await sweepOnePage(args.port, pageUrl, width, variant.script);
            failures = formatFailures(label, width, result);
            if (variant.suffix && result && result.tuningBadge && !(variant.suffix in badgesBySuffix)) {
              badgesBySuffix[variant.suffix] = result.tuningBadge;
            }
          } catch (e) {
            failures = [`  [error]    ${label} @${width}px: sweep threw: ${e.message}`];
          }
          const pass = failures.length === 0;
          if (!pass) anyFail = true;
          rows.push({ page: label, width, pass, failures });
        }
      }

      // Distinctness check: the tuning-recommendation panel's confidence
      // states must not become visually indistinguishable from each other
      // -- ce595d2 (review finding) is exactly the failure mode this
      // guards: an out-of-range/extrapolated peak rendering as if it were
      // MEASURED. 'measured' vs the other two must differ in BOTH color
      // and text (by design: --ok is the only green). 'extrapolated' vs
      // 'indistinguishable' are DELIBERATELY the same color (both
      // var(--warn) -- see zones_page.html's TUNING_REC_LOOKUP_START
      // comment: "told apart in the rendered text, not the color") so
      // those two are only required to differ in text, not color.
      const suffixes = Object.keys(badgesBySuffix);
      if (suffixes.length > 1) {
        for (let i = 0; i < suffixes.length; i++) {
          for (let j = i + 1; j < suffixes.length; j++) {
            const s1 = suffixes[i], s2 = suffixes[j];
            const b1 = badgesBySuffix[s1], b2 = badgesBySuffix[s2];
            const sameText = b1.text === b2.text;
            const sameColor = b1.color === b2.color;
            const isMeasuredPair = s1 === 'measured' || s2 === 'measured';
            const indistinct = isMeasuredPair ? (sameText && sameColor) : sameText;
            const label = `${pf} [tuning-distinct: ${s1} vs ${s2}]`;
            const failures = indistinct
              ? [`  [distinct]  ${label}: badges render identically (text="${b1.text}" color=${b1.color})`]
              : [];
            if (indistinct) anyFail = true;
            // Always pushed (pass or fail), unlike the per-(page,width)
            // rows above, so this check shows up in the total case count
            // and in the PASS/FAIL table even when green -- a check that
            // only appears in the output on failure is invisible proof
            // that it exists at all.
            rows.push({ page: label, width: '-', pass: !indistinct, failures });
          }
        }
      }
    }
  } finally {
    chrome.kill();
    staticServer.close();
    // Best-effort: wait for the process to actually exit (kill() only
    // requests termination) so Windows has released its open handles on
    // userDataDir before rm -- without this, rm often raced the still-
    // exiting Chrome process and silently left the profile dir behind
    // (observed in testing). Still wrapped in try/catch: this cleanup is
    // not load-bearing for correctness (each run gets its own mkdtemp'd
    // dir), only for not littering TEMP across repeated runs.
    try {
      if (chrome.exitCode === null && chrome.signalCode === null) {
        await Promise.race([
          new Promise((resolve) => chrome.once('exit', resolve)),
          new Promise((resolve) => setTimeout(resolve, 3000)),
        ]);
      }
      // maxRetries/retryDelay: Node's own documented mitigation for exactly
      // this Windows EBUSY/EPERM race (a file briefly still locked by an
      // AV scan or a not-quite-gone handle) -- retries on ENOENT/EBUSY/
      // EPERM/EMFILE/ENFILE/ENOTEMPTY with linear backoff.
      await rm(userDataDir, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
    } catch { /* best-effort */ }
  }

  console.log('');
  if (devtoolsSkip) {
    // Loud, unmissable SKIP -- exit 0 so run_all_checks.ps1 does not fail
    // the whole suite over an environment fact (see the comment above
    // devtoolsSkip's assignment). "SKIPPED" is grepped for the same way
    // check_ui_responsive_sweep.ps1's own no-node path already is, so this
    // reads the same way in that check's console output.
    console.log('=================================================================');
    console.log('ui_responsive_sweep: SKIPPED -- Chrome DevTools port never came up.');
    console.log('This machine/run could not start a working headless Chrome in time');
    console.log('(see reason below); that is an environment condition, not evidence');
    console.log('the pages under test regressed. Re-run when the machine is less');
    console.log('loaded, or set KC_SWEEP_CHROME to a known-good browser path.');
    console.log('Reason: ' + devtoolsSkip);
    console.log('=================================================================');
    console.log('');
    process.exitCode = 0;
    return;
  }
  console.log('UI RESPONSIVE SWEEP -- widths: ' + args.widths.join(', '));
  console.log('');
  for (const row of rows) {
    console.log(`  ${row.pass ? 'PASS' : 'FAIL'}  ${row.page.padEnd(34)} @${String(row.width).padStart(4)}px`);
  }
  console.log('');

  const failedRows = rows.filter(r => !r.pass);
  if (failedRows.length > 0) {
    console.log(`${failedRows.length} of ${rows.length} (page, width) checks FAILED:`);
    for (const r of failedRows) {
      for (const line of r.failures) console.log(line);
    }
    console.log('');
    process.exitCode = 1;
    return;
  }

  console.log(`All ${rows.length} (page, width) checks passed.`);
  process.exitCode = 0;
}

main().catch((e) => {
  console.error('ui_responsive_sweep: fatal: ' + (e && e.stack || e));
  process.exit(2);
});
