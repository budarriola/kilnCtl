// ui_content_smoke.mjs -- headless-Chrome CONTENT smoke for the web pages whose
// last changes shipped with "browser check still owed":
//   * zones_page.html aux-output editor (SPARE_RELAY_ONOFF_PLAN.md WP-5)
//   * zones_page.html device-type selectors for spare relays (b45d225b)
//   * nav.js reorg: Diagnostics under System, Ready-to-fire under Kiln setup
//     (c44133d5), plus diagnostics_page.html's new flash capacity rows.
//
// ui_responsive_sweep.mjs checks LAYOUT only and its static server answers no
// /api/*. This sibling serves the same drivers/ tree but ALSO answers mocked
// /api/zones, /api/aux_outputs, /api/status, /api/partitions, /api/readiness
// (fixtures below, shaped like the real GET responses), so the pages' real
// fetch -> render paths run. It asserts key DOM exists, nav links resolve to a
// real firmware route (`.uri = "..."` under App/drivers/**.c), and that the page
// raised no console error / uncaught exception / failed resource.
// Never touches a board. SKIPs (exit 3) only when no Chrome is found or its
// DevTools port never comes up.
//
// Usage: node ui_content_smoke.mjs [--dir <drivers dir>]
// Exit: 0 pass, 1 assertion failure, 3 skip/harness error, 4 deadline.
import { spawn, spawnSync } from 'node:child_process';
import { createServer } from 'node:http';
import { existsSync, readFileSync } from 'node:fs';
import { readFile, mkdtemp, rm } from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const { resolveDriversDir, walkFiles } = require('./_drivers_layout.js');
let DIR = resolveDriversDir(__dirname);
for (let i = 2; i < process.argv.length; i++) if (process.argv[i] === '--dir') DIR = path.resolve(process.argv[++i]);

const TIMEOUT = 10000;
const fetchT = (u, o = {}) => fetch(u, { ...o, signal: AbortSignal.timeout(TIMEOUT) });
// Race pass: app.js/nav.js/commissioning_shared.js are held back this long so any inline
// script calling window.kc* helpers at parse time (they come from the deferred app.js) throws.
let scriptDelayMs = 0;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function findChrome() {
  for (const c of [process.env.KC_SWEEP_CHROME,
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    'C:/Program Files/Microsoft/Edge/Application/msedge.exe'].filter(Boolean)) if (existsSync(c)) return c;
  return null;
}
function killTreeSync(pid) {
  if (!pid) return;
  try { spawnSync('taskkill', ['/PID', String(pid), '/T', '/F'], { stdio: 'ignore', timeout: 10000 }); } catch { /* gone */ }
}
function listen(server, port) {
  return new Promise((res, rej) => { server.once('error', rej); server.listen(port, '127.0.0.1', () => res(server.address().port)); });
}
async function freePort() {
  const s = createServer();
  const p = await listen(s, 0);
  await new Promise((r) => s.close(r));
  return p;
}

// ---- mocked API (shapes follow the real GET handlers) ----
const MOCK = {
  '/api/zones': {
    thermo_count: 3, relay_count: 2, max_simultaneous_relays: 0, continue_on_zone_trip: false,
    relay_names: ['Kiln A', 'Kiln B', 'Vent', 'Lamp'],
    relay_types: [0, 0, 4, 5], // relay 3 = fan, relay 4 = light (the spares)
    zones: [
      { name: 'Top', relay_mask: 1, thermo_channel: 0 },
      { name: 'Mid', relay_mask: 2, thermo_channel: 1 },
      { name: 'Bot', relay_mask: 0, thermo_channel: 2 },
    ],
  },
  '/api/aux_outputs': {
    quarantined: false, enabled_mask: 4,
    relays: [{ relay: 3, enabled: true, conflicted: false, tc_zone: 1, hyst_c: 3, min_on_s: 45, min_off_s: 60 }],
  },
  '/api/status': {
    fw_version_known: true, fw_version: '0.0.0-test', fw_build: 'Oct  7 2026', uptime_s: 100, reset_reason: 'poweron',
    flash_size: 16 * 1048576, flash_used: 1048576, flash_partition_size: 4 * 1048576,
    heap_internal: { free: 100000, total: 300000, largest_free_block: 60000, min_free: 50000 },
    heap_spiram: { free: 1000000, total: 8000000, largest_free_block: 900000, min_free: 800000 },
  },
  '/api/partitions': { running: 'app', partitions: [
    { label: 'app', type: 0, subtype: 16, offset: 65536, size: 4 * 1048576 },
    { label: 'cfg', type: 1, subtype: 129, offset: 4259840, size: 1048576 }] },
  '/api/readiness': { items: [
    { key: 'wifi', label: 'Wi-Fi configured', status: 'ok', detail: 'joined' },
    { key: 'estop_verified', label: 'E-stop verified', status: 'not_done', detail: 'not verified', fix_url: '/diagnostics' }] },
};

function routeUris() {
  const uris = new Set();
  for (const f of walkFiles(DIR)) {
    if (!f.endsWith('.c')) continue;
    for (const m of readFileSync(f, 'utf8').matchAll(/\.uri\s*=\s*"([^"]*)"/g)) uris.add(m[1]);
  }
  return uris;
}

function startServer() {
  const files = walkFiles(DIR);
  const server = createServer(async (req, res) => {
    try {
      const p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
      if (p === '/app.js' || p === '/nav.js' || p === '/commissioning_shared.js') await sleep(scriptDelayMs);
      if (p === '/favicon.ico') { res.writeHead(204); res.end(); return; } // browser probe, not a page asset
      if (p.startsWith('/api/')) {
        // Unmocked /api GETs answer a valid empty object so background polls do not
        // register as failed resources; the page under test only needs the mocks above.
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(req.method === 'GET' && MOCK[p] ? MOCK[p] : {}));
        return;
      }
      const hit = files.filter((f) => path.basename(f) === path.basename(p));
      if (hit.length !== 1) { res.writeHead(404); res.end('nf'); return; }
      const ext = path.extname(hit[0]);
      res.writeHead(200, { 'Content-Type': { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' }[ext] || 'application/octet-stream' });
      res.end(await readFile(hit[0]));
    } catch { res.writeHead(404); res.end('nf'); }
  });
  return listen(server, 0).then((port) => ({ server, port }));
}

class Cdp {
  constructor(ws) {
    this.ws = ws; this.id = 1; this.pending = new Map(); this.handlers = [];
    ws.addEventListener('message', (ev) => {
      const m = JSON.parse(ev.data);
      if (m.id !== undefined && this.pending.has(m.id)) {
        const { resolve, reject } = this.pending.get(m.id); this.pending.delete(m.id);
        if (m.error) reject(new Error(JSON.stringify(m.error))); else resolve(m.result);
      } else if (m.method) for (const h of this.handlers) h(m.method, m.params);
    });
    ws.addEventListener('close', () => { for (const { reject } of this.pending.values()) reject(new Error('CDP closed')); this.pending.clear(); });
  }
  send(method, params = {}) {
    const id = this.id++;
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => { this.pending.delete(id); reject(new Error(`CDP ${method} timed out`)); }, 20000);
      this.pending.set(id, { resolve: (v) => { clearTimeout(t); resolve(v); }, reject: (e) => { clearTimeout(t); reject(e); } });
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }
  waitFor(method, ms = 30000) {
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => reject(new Error(`timed out waiting for ${method}`)), ms);
      this.handlers.push((m, p) => { if (m === method) { clearTimeout(t); resolve(p); } });
    });
  }
}

// Load one page, collect console errors, run `probe` (an in-page expression returning
// an array of failure strings), return failures.
async function checkPage(cdpPort, base, page, probe, settleMs = 2500, ignore404 = false) {
  const tab = await (await fetchT(`http://127.0.0.1:${cdpPort}/json/new?about:blank`, { method: 'PUT' })).json();
  const ws = await new Promise((res, rej) => {
    const w = new WebSocket(tab.webSocketDebuggerUrl);
    w.addEventListener('open', () => res(w), { once: true });
    w.addEventListener('error', () => rej(new Error('ws open failed')), { once: true });
  });
  const cdp = new Cdp(ws);
  const errors = [];
  cdp.handlers.push((m, p) => {
    if (m === 'Runtime.exceptionThrown') errors.push('uncaught: ' + (p.exceptionDetails.exception?.description || p.exceptionDetails.text));
    else if (m === 'Runtime.consoleAPICalled' && p.type === 'error') errors.push('console.error: ' + p.args.map((a) => a.value ?? a.description).join(' '));
    else if (m === 'Log.entryAdded' && p.entry.level === 'error' && !(ignore404 && /\b404\b/.test(p.entry.text))) errors.push('log: ' + p.entry.text + ' ' + (p.entry.url || ''));
  });
  try {
    await cdp.send('Page.enable'); await cdp.send('Runtime.enable'); await cdp.send('Log.enable');
    await cdp.send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 1400, deviceScaleFactor: 1, mobile: false });
    const loaded = cdp.waitFor('Page.loadEventFired');
    await cdp.send('Page.navigate', { url: `${base}/${page}` });
    await loaded;
    await sleep(settleMs);
    const r = await cdp.send('Runtime.evaluate', { expression: probe, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) return [`${page}: probe threw: ${JSON.stringify(r.exceptionDetails.exception?.description || r.exceptionDetails.text)}`];
    const fails = (r.result.value || []).map((s) => `${page}: ${s}`);
    for (const e of errors) fails.push(`${page}: ${e}`);
    return fails;
  } finally {
    ws.close();
    try { await fetchT(`http://127.0.0.1:${cdpPort}/json/close/${tab.id}`); } catch { /* best effort */ }
  }
}

const HELPERS = `
  var F = [];
  function need(sel, n, root) { var c = (root || document).querySelectorAll(sel).length;
    if (n === undefined ? c < 1 : c !== n) F.push('selector ' + sel + ': found ' + c + (n === undefined ? ', want >=1' : ', want ' + n)); return c; }
  function eq(a, b, what) { if (a !== b) F.push(what + ': got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b)); }
`;

const ZONES_PROBE = `(function(){ ${HELPERS}
  // Relay name/device-type rows: one per hardware relay (4), 2 of them spares.
  need('#relayNames .relayname', 4);
  need('#relayNames .relayname.relayspare', 2);
  var spare = document.querySelectorAll('#relayNames .relayname.relayspare');
  spare.forEach(function (row, i) {
    var sel = row.querySelector('select.relaytypeinput');
    if (!sel) { F.push('spare relay row ' + i + ' has no select.relaytypeinput'); return; }
    eq(Array.prototype.map.call(sel.options, function (o) { return o.value; }).join(','), '0,1,2,3,4,5,6', 'relay type option values (spare ' + i + ')');
    eq(Array.prototype.map.call(sel.options, function (o) { return o.textContent; }).join(','),
       'not set,damper,outlet,valve,fan,light,other', 'relay type option labels (spare ' + i + ')');
    eq(sel.value, String([4, 5][i]), 'stored relay type selected (spare ' + i + ')');
    if (!row.querySelector('input.relaynameinput[type=text]')) F.push('spare relay row ' + i + ' has no name input');
  });
  // Zone-owned relays carry hidden echoes, not a selectable type.
  need('#relayNames .relayname:not(.relayspare) input.relaytypeinput[type=hidden]', 2);
  // Aux editor: one row per hardware relay (AUX_COUNT 4); relays 1-2 are zone-owned.
  need('#auxOutputs .auxrow', 4);
  need('#auxOutputs .auxrow .aux-enabled', 2);
  var rows = document.querySelectorAll('#auxOutputs .auxrow');
  var r3 = rows[2];
  if (r3 && r3.querySelector('.aux-enabled')) {
    eq(r3.querySelector('.aux-enabled').checked, true, 'aux relay 3 enabled checkbox reflects GET /api/aux_outputs');
    eq(r3.querySelector('.aux-tc').value, '1', 'aux relay 3 thermocouple zone');
    eq(r3.querySelector('.aux-hyst').value, '3', 'aux relay 3 hysteresis');
    eq(r3.querySelector('.aux-minon').value, '45', 'aux relay 3 min on');
    eq(r3.querySelector('.aux-minoff').value, '60', 'aux relay 3 min off');
    eq(r3.querySelectorAll('.aux-tc option').length, 4, 'aux thermocouple options (none + 3 zones)');
    need('.aux-apply', 1, r3); need('.aux-cycles', 1, r3);
  } else F.push('aux relay 3 row has no controls');
  var r4 = rows[3];
  if (r4 && r4.querySelector('.aux-enabled')) eq(r4.querySelector('.aux-enabled').checked, false, 'aux relay 4 enabled (no stored entry)');
  else F.push('aux relay 4 row has no controls');
  need('#auxHeading', 1); need('#auxMsg', 1);
  return F; })()`;

const DIAG_PROBE = `(function(){ ${HELPERS}
  ['flashChip', 'flashAlloc', 'flashUsed'].forEach(function (id) {
    var e = document.getElementById(id);
    if (!e) { F.push('#' + id + ' missing'); return; }
    if (e.textContent.trim() === '--' || e.textContent.trim() === '') F.push('#' + id + ' never populated from mocked /api/status');
  });
  eq((document.getElementById('flashChip') || {}).textContent, '16.00 MB', 'flash chip size text');
  eq((document.getElementById('flashUsed') || {}).textContent, '1.00 MB of 4.00 MB (25%)', 'flash used text');
  need('#partitionsSection .row');
  return F; })()`;

const READY_PROBE = `(function(){ ${HELPERS}
  need('#items .item', 2);
  need('#estop_verified a.fix', 1);
  return F; })()`;

// Nav: group membership + every link resolves to a real route.
const NAV_PROBE = (uris) => `(function(){ ${HELPERS}
  var uris = ${JSON.stringify(uris)};
  var groups = {};
  document.querySelectorAll('.kc-menu-panel details.kc-menu-group').forEach(function (d) {
    groups[d.querySelector('summary').textContent] = Array.prototype.map.call(d.querySelectorAll('a'), function (a) { return a.getAttribute('href'); });
  });
  var want = { 'Kiln setup': '/readiness', 'System': '/diagnostics' };
  Object.keys(want).forEach(function (g) {
    if (!groups[g]) F.push('nav group missing: ' + g);
    else if (groups[g].indexOf(want[g]) < 0) F.push('nav group ' + g + ' does not contain ' + want[g] + ' (has ' + groups[g].join(' ') + ')');
  });
  ['Firing', 'Kiln setup'].forEach(function (g) { if (groups[g] && groups[g].indexOf('/diagnostics') >= 0) F.push('/diagnostics wrongly under ' + g); });
  ['Firing', 'System'].forEach(function (g) { if (groups[g] && groups[g].indexOf('/readiness') >= 0) F.push('/readiness wrongly under ' + g); });
  var links = document.querySelectorAll('.kc-menu-panel a');
  if (links.length < 10) F.push('only ' + links.length + ' nav links rendered');
  links.forEach(function (a) {
    var p = a.getAttribute('href').split('#')[0].split('?')[0];
    if (uris.indexOf(p) < 0) F.push('nav link ' + a.getAttribute('href') + ' has no .uri route in firmware');
  });
  return F; })()`;

async function main() {
  const chromePath = findChrome();
  if (!chromePath) { console.log('ui_content_smoke: SKIPPED -- no Chrome/Edge found (set KC_SWEEP_CHROME).'); process.exit(3); }
  const uris = [...routeUris()];
  if (uris.length < 50) { console.log(`ui_content_smoke: FAILED -- only ${uris.length} firmware routes found; route scan is broken.`); process.exit(1); }
  const cdpPort = await freePort();
  // kc-ui-sweep-profile- prefix: check_ui_responsive_sweep.ps1's orphan reaper keys on it.
  const profile = await mkdtemp(path.join(os.tmpdir(), 'kc-ui-sweep-profile-smoke-'));
  const chrome = spawn(chromePath, [`--remote-debugging-port=${cdpPort}`, '--headless=new', '--disable-gpu', '--no-first-run',
    '--no-default-browser-check', '--disable-extensions', `--user-data-dir=${profile}`], { stdio: 'ignore' });
  const wd = setTimeout(() => { console.log('ui_content_smoke: OVERALL TIMEOUT'); killTreeSync(chrome.pid); process.exit(4); }, 200000);
  wd.unref();
  const { server, port } = await startServer();
  const base = `http://127.0.0.1:${port}`;
  let failures = [];
  try {
    const t0 = Date.now();
    for (;;) {
      try { if ((await fetchT(`http://127.0.0.1:${cdpPort}/json/version`)).ok) break; } catch { /* retry */ }
      if (chrome.exitCode !== null || Date.now() - t0 > 30000) { console.log('ui_content_smoke: SKIPPED -- Chrome DevTools port never came up.'); process.exitCode = 3; return; }
      await sleep(100);
    }
    const cases = [
      ['zones_page.html', ZONES_PROBE],
      ['diagnostics_page.html', DIAG_PROBE],
      ['readiness_page.html', READY_PROBE],
      ['main_page.html', NAV_PROBE(uris)],
    ];
    for (const [page, probe] of cases) {
      const f = await checkPage(cdpPort, base, page, probe);
      console.log(`ui_content_smoke: ${page}: ${f.length ? f.length + ' failure(s)' : 'ok'}`);
      failures = failures.concat(f);
    }
    // Race pass: EVERY *_page.html with the shared scripts delayed. Inline scripts that call
    // window.kc* helpers (defined by the deferred app.js) at parse time throw; any uncaught page
    // error / console error fails. (A call wrapped in try/catch is not seen here; fix by review.)
    const pages = walkFiles(DIR).filter((f) => /_page\.html$/.test(f)).map((f) => path.basename(f));
    if (pages.length < 10) throw new Error(`only ${pages.length} *_page.html found; scan is broken`);
    scriptDelayMs = 1500;
    for (let i = 0; i < pages.length; i += 6) {
      const res = await Promise.all(pages.slice(i, i + 6).map((p) => checkPage(cdpPort, base, p, p === 'readiness_page.html' ? READY_PROBE : '[]', scriptDelayMs + 1500, true)));
      res.forEach((f, j) => {
        console.log(`ui_content_smoke: ${pages[i + j]} (scripts delayed ${scriptDelayMs} ms): ${f.length ? f.length + ' failure(s)' : 'ok'}`);
        failures = failures.concat(f);
      });
    }
    scriptDelayMs = 0;
  } catch (e) {
    console.log(`ui_content_smoke: HARNESS_ERROR ${e && e.message}`);
    process.exitCode = 3;
    return;
  } finally {
    server.close(); killTreeSync(chrome.pid);
    await rm(profile, { recursive: true, force: true }).catch(() => {});
  }
  if (failures.length) {
    for (const f of failures) console.log('  FAIL ' + f);
    console.log(`ui_content_smoke: ${failures.length} content checks FAILED`);
    process.exit(1);
  }
  console.log('ui_content_smoke: All page content checks passed.');
  process.exit(0);
}
// Exit explicitly once cleanup is done: a lingering handle (keep-alive CDP/HTTP sockets,
// the spawned Chrome handle) otherwise keeps the loop alive until the 120 s watchdog fires (exit 4).
main().then(() => process.exit(process.exitCode || 0));
