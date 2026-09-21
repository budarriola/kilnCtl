// _web_commission_cdp.mjs -- CDP click-and-screenshot helper for
// web_commission_row.py's live mode (tools/PcTools/src/kilnctrl/web_commission_row.py).
//
// Reuses the same Chrome-launch and CdpSession approach already reviewed
// for this repo in firmware/KilnFW/App/test/ui_responsive_sweep.mjs, but
// against the LIVE board instead of a throwaway static server -- that file's
// own header comment explains why it must never point at a real board (a
// firing may be in progress and the httpd has wedged under load before);
// this script is invoked only for one named, pre-authorized commissioning
// row at a time, never as a sweep.
//
// Usage (the session cookie is passed via the KC_SID environment variable,
// never as a command-line argument -- see parseArgs() below):
//   KC_SID=<kiln_sid value> node _web_commission_cdp.mjs --host 192.168.1.156 \
//     --route /diagnostics --selector-kind id --selector crashAckBtn \
//     --screenshot out.png
//
// selector-kind "page" performs no click, just navigates and screenshots
// (page-load-only rows). Exit 0 + one JSON line to stdout on success; exit 1
// + an error message on stderr on failure.

import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { existsSync } from 'node:fs';
import { writeFile, mkdtemp, rm } from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';

const CDP_CALL_TIMEOUT_MS = 20000;

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
  const out = { port: 9433 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--host') out.host = argv[++i];
    else if (a === '--route') out.route = argv[++i];
    else if (a === '--selector-kind') out.selectorKind = argv[++i];
    else if (a === '--selector') out.selector = argv[++i];
    else if (a === '--screenshot') out.screenshot = argv[++i];
    else if (a === '--port') out.port = parseInt(argv[++i], 10);
  }
  for (const req of ['host', 'route', 'selectorKind']) {
    if (!out[req]) throw new Error(`--${req} is required`);
  }
  // The session cookie is deliberately NOT a CLI argument -- argv is visible
  // in process listings and gets logged by callers more often than an
  // environment variable does. web_commission_row.py's run_row_live() sets
  // KC_SID in this child process's own environment instead.
  out.cookie = process.env.KC_SID;
  if (!out.cookie) throw new Error('KC_SID environment variable is required (session cookie)');
  return out;
}

// Same EADDRINUSE fallback as ui_responsive_sweep.mjs's pickPort(): a fixed
// CDP port can already be held by a leftover Chrome or a concurrent run on
// this shared machine -- fall back to an OS-assigned ephemeral port rather
// than fail outright.
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
      throw new Error(`Chrome exited early (code ${chrome.exitCode})`);
    }
    try {
      const r = await fetch(`http://127.0.0.1:${port}/json/version`);
      if (r.ok) return;
    } catch (e) {
      lastErr = e;
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error(`Chrome DevTools port ${port} never came up: ${lastErr}`);
}

class CdpSession {
  constructor(ws) {
    this.ws = ws;
    this.nextId = 1;
    this.pending = new Map();
    const failPending = (why) => {
      const err = new Error(`CDP connection closed: ${why}`);
      for (const { reject } of this.pending.values()) reject(err);
      this.pending.clear();
      this.closed = err;
    };
    ws.addEventListener('close', (ev) => failPending(`code=${ev.code}`));
    ws.addEventListener('error', (ev) => failPending(ev.message || 'unknown error'));
    ws.addEventListener('message', (ev) => {
      const msg = JSON.parse(ev.data);
      if (msg.id !== undefined && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        if (msg.error) reject(new Error(JSON.stringify(msg.error)));
        else resolve(msg.result);
      } else if (msg.method === 'Page.javascriptDialogOpening') {
        // Defect found running the first live class sweep: several controls
        // (e.g. diagnostics_page.html's watchdog-panic toggle, main_page.html's
        // clear-trip confirm) route through app.js's window.kcConfirm(), which
        // today is literally window.confirm() -- a native, renderer-blocking
        // dialog. Without this handler, the click's Runtime.evaluate never
        // returns (the renderer thread is frozen waiting on the dialog) and
        // every such row hangs for the full CDP_CALL_TIMEOUT_MS before
        // failing, never actually completing or rejecting the action.
        // Auto-accepting (never auto-dismissing) matches a human operator who
        // intends the click's action to proceed -- this script is only ever
        // invoked for one already-authorized row at a time, never as a blind
        // sweep, so the underlying action itself is not this fix's concern.
        this.send('Page.handleJavaScriptDialog', { accept: true }).catch(() => {
          /* best effort -- if this races the page/context going away, the
             row's own error handling (selector-not-found / timeout) still
             surfaces the underlying problem */
        });
      }
    });
  }

  send(method, params = {}) {
    if (this.closed) return Promise.reject(this.closed);
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id);
          reject(new Error(`CDP call ${method} timed out after ${CDP_CALL_TIMEOUT_MS}ms`));
        }
      }, CDP_CALL_TIMEOUT_MS);
      this.pending.set(id, {
        resolve: (v) => { clearTimeout(timer); resolve(v); },
        reject: (e) => { clearTimeout(timer); reject(e); },
      });
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const chromePath = findChrome();
  if (!chromePath) throw new Error('no Chrome/Edge binary found (set KC_SWEEP_CHROME)');

  const cdpPort = await pickPort(args.port);
  if (cdpPort !== args.port) {
    console.log(`_web_commission_cdp: CDP port ${args.port} was busy, using ${cdpPort} instead`);
  }
  args.port = cdpPort;

  const userDataDir = await mkdtemp(path.join(os.tmpdir(), 'kc-web-commission-'));
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

  try {
    await waitForPort(args.port, 30000, chrome);
    const tabResp = await fetch(`http://127.0.0.1:${args.port}/json/new?about:blank`, { method: 'PUT' });
    const tab = await tabResp.json();
    const ws = new WebSocket(tab.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => {
      ws.addEventListener('open', resolve, { once: true });
      ws.addEventListener('error', reject, { once: true });
    });
    const cdp = new CdpSession(ws);
    await cdp.send('Page.enable');
    await cdp.send('Network.enable');
    // The cookie must be set before navigation so the request the page load
    // itself makes is already authenticated -- this is the ONE login this
    // process performs; it never re-POSTs /api/auth/login. `url`, not a
    // bare `domain`, is what CDP documents as reliable for a plain IPv4
    // host (a bare domain is intended for real DNS names and cookie-jar
    // domain matching, which behaves inconsistently for a literal IP).
    await cdp.send('Network.setCookie', {
      name: 'kiln_sid', value: args.cookie, url: `http://${args.host}/`, path: '/',
    });
    await cdp.send('Page.navigate', { url: `http://${args.host}${args.route}` });
    await cdp.send('Runtime.evaluate', { expression: 'new Promise(r => setTimeout(r, 500))', awaitPromise: true });

    if (args.selectorKind === 'id') {
      const expr = `(() => { const el = document.getElementById(${JSON.stringify(args.selector)}); if (!el) return 'NOT_FOUND'; el.click(); return 'CLICKED'; })()`;
      const res = await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true });
      if (res.result.value !== 'CLICKED') {
        throw new Error(`selector #${args.selector} not found on ${args.route} at runtime`);
      }
      await cdp.send('Runtime.evaluate', { expression: 'new Promise(r => setTimeout(r, 500))', awaitPromise: true });
    } else if (args.selectorKind === 'text') {
      const expr = `(() => { const btn = [...document.querySelectorAll('button')].find(b => b.textContent.includes(${JSON.stringify(args.selector)})); if (!btn) return 'NOT_FOUND'; btn.click(); return 'CLICKED'; })()`;
      const res = await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true });
      if (res.result.value !== 'CLICKED') {
        throw new Error(`button text "${args.selector}" not found on ${args.route} at runtime`);
      }
    } // "page": no click, load only

    if (args.screenshot) {
      const shot = await cdp.send('Page.captureScreenshot', { format: 'png' });
      await writeFile(args.screenshot, Buffer.from(shot.data, 'base64'));
    }

    console.log(JSON.stringify({ ok: true, route: args.route, selector: args.selector || null }));
  } finally {
    try { chrome.kill(); } catch { /* already gone */ }
    try { await rm(userDataDir, { recursive: true, force: true }); } catch { /* best effort */ }
  }
}

main().catch((err) => {
  console.error(String(err && err.stack || err));
  process.exit(1);
});
