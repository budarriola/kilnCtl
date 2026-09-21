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
//
// Optional:
//   --accept-dialogs        answer a native confirm() with OK instead of
//                           Cancel. Off by default: a row that is not
//                           classified as a write gets any unexpected
//                           dialog dismissed, never accepted. Dialogs are
//                           always logged (stderr) and reported (JSON
//                           `dialogs`), accepted or not.
//   --expect-post <path>    after the click, wait (bounded) for a POST whose
//                           URL contains <path> to actually complete before
//                           screenshotting and tearing Chrome down, and
//                           report its status in the JSON `post` field.
//                           Without it the script instead waits (bounded)
//                           for the page to go network-quiet.
//   --fills <json>          a JSON array of {"selector": "<css selector>",
//                           "value": "<string>"} objects, applied via
//                           document.querySelector(selector).value = value
//                           followed by dispatching 'input' and 'change'
//                           events, in order, after navigation but BEFORE
//                           the click. Used by web_commission_row.py's
//                           form-fill rows (a typed value before Save,
//                           e.g. W22/W38/W42) -- selector is a full CSS
//                           selector (e.g. "#pcLink"), not the bare id the
//                           --selector click argument takes. A missing
//                           selector is a hard failure, same as a missing
//                           click target -- and so is an assignment the
//                           element rejects (an unpopulated <select>, an
//                           off-step range value), since clicking on with a
//                           value nobody asked for is how a delete lands on
//                           the wrong row.

import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { existsSync } from 'node:fs';
import { writeFile, mkdtemp, rm } from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';

const CDP_CALL_TIMEOUT_MS = 20000;
// Bounded post-click waits (see CdpSession.waitForPost/waitForQuiet).
const POST_WAIT_TIMEOUT_MS = 10000;
const QUIET_TIMEOUT_MS = 5000;

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
    else if (a === '--expect-post') out.expectPost = argv[++i];
    else if (a === '--accept-dialogs') out.acceptDialogs = true;
    else if (a === '--fills') out.fillsJson = argv[++i];
  }
  for (const req of ['host', 'route', 'selectorKind']) {
    if (!out[req]) throw new Error(`--${req} is required`);
  }
  out.fills = out.fillsJson ? JSON.parse(out.fillsJson) : [];
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
  // `acceptDialogs` decides what a native window.confirm()/alert() gets
  // answered with. It is NOT a blanket default: web_commission_row.py passes
  // --accept-dialogs only for a row it already classifies as a write/
  // owner-gated action, so a read-only row that unexpectedly raises a
  // confirm() is DISMISSED (no state change) instead of silently
  // authorizing whatever it guards -- several of these dialogs gate
  // genuinely destructive controls (diagnostics_page.html's dangerEnterBtn,
  // the per-relay lifetime-cycle reset). Either way the dialog's type and
  // message are recorded and reported, so a run never accepts or dismisses
  // something without saying which prompt it answered.
  constructor(ws, acceptDialogs = false) {
    this.ws = ws;
    this.nextId = 1;
    this.pending = new Map();
    this.acceptDialogs = !!acceptDialogs;
    this.dialogs = [];
    // In-flight request bookkeeping for waitForQuiet()/waitForPost() below.
    this.inFlight = new Map();   // requestId -> {method, url}
    this.completed = [];         // {method, url, status|null, failed}
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
      } else if (msg.method === 'Network.requestWillBeSent') {
        const p = msg.params || {};
        this.inFlight.set(p.requestId, {
          method: (p.request && p.request.method) || '?',
          url: (p.request && p.request.url) || '',
        });
      } else if (msg.method === 'Network.responseReceived') {
        const rec = this.inFlight.get(msg.params.requestId);
        if (rec) rec.status = msg.params.response && msg.params.response.status;
      } else if (msg.method === 'Network.loadingFinished' || msg.method === 'Network.loadingFailed') {
        const rec = this.inFlight.get(msg.params.requestId);
        if (rec) {
          this.inFlight.delete(msg.params.requestId);
          this.completed.push({
            method: rec.method,
            url: rec.url,
            status: rec.status === undefined ? null : rec.status,
            failed: msg.method === 'Network.loadingFailed',
          });
        }
      } else if (msg.method === 'Page.javascriptDialogOpening') {
        // Defect found running the first live class sweep: several controls
        // (e.g. diagnostics_page.html's watchdog-panic toggle, main_page.html's
        // clear-trip confirm) route through app.js's window.kcConfirm(), which
        // today is literally window.confirm() -- a native, renderer-blocking
        // dialog. Without this handler, the click's Runtime.evaluate never
        // returns (the renderer thread is frozen waiting on the dialog) and
        // every such row hangs for the full CDP_CALL_TIMEOUT_MS before
        // failing, never actually completing or rejecting the action.
        // Answering it is opt-in per invocation (--accept-dialogs, set by
        // web_commission_row.py only for a write/owner-gated row): a
        // read-only row gets the dialog DISMISSED, which unhangs the
        // renderer without authorizing the guarded action. Every dialog is
        // recorded with its message and reported on stderr + in the result
        // JSON, so the run log always names the prompt that was answered.
        const p = msg.params || {};
        const accept = this.acceptDialogs;
        this.dialogs.push({ type: p.type || 'confirm', message: p.message || '', accepted: accept });
        console.error(`_web_commission_cdp: ${accept ? 'ACCEPTED' : 'DISMISSED'} ` +
                      `${p.type || 'confirm'} dialog: ${JSON.stringify(p.message || '')}`);
        this.send('Page.handleJavaScriptDialog', { accept }).catch(() => {
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

  // W30 defect (2026-09-21 live run): the watchdog toggle's click handler
  // does its state-changing fetch('/api/watchdog_cfg', {method:'POST'})
  // asynchronously AFTER the confirm() dialog resolves. The old fixed 500 ms
  // post-click sleep followed by chrome.kill() in main()'s finally block
  // could tear the browser down before that request was written, so the
  // write never landed and the row read back unchanged -- an invisible
  // false negative. These two waits replace "sleep and hope": waitForPost()
  // when the row declares the request it expects, waitForQuiet() otherwise.
  // Both are bounded, and both return what they saw so the caller can
  // report it rather than assume it.
  async waitForPost(pathFragment, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    const matches = (r) => r.method === 'POST' && r.url.includes(pathFragment);
    for (;;) {
      const hit = this.completed.find(matches);
      if (hit) return hit;
      if (Date.now() >= deadline) {
        const inflight = [...this.inFlight.values()].filter(matches).length;
        throw new Error(
          `expected POST containing "${pathFragment}" did not complete within ${timeoutMs}ms ` +
          `(${inflight} still in flight; completed: ` +
          `${JSON.stringify(this.completed.map((r) => `${r.method} ${r.url}`))})`);
      }
      await new Promise((r) => setTimeout(r, 50));
    }
  }

  async waitForQuiet(timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (this.inFlight.size > 0 && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 50));
    }
    return this.inFlight.size === 0;
  }
}

// Sets .value on each fill's target element and dispatches 'input'/'change'
// so the page's own listeners (e.g. settings_display_page.html's brightness
// live-preview handler) see the new value the same way a real keystroke or
// drag would. Runs strictly before the click -- a Save button reads these
// fields' current DOM value at click time, not at page-load time.
// The assignment is READ BACK and compared before the events are dispatched:
// setting .value does not always take. A <select> whose <option> list has not
// been populated yet (these pages fill their pickers from an async fetch, and
// this runs on a fixed post-navigate delay) silently leaves .value as '', and
// an <input type="range"> snaps an off-step value to a neighbouring step.
// Either way the page would then be clicked while holding a value nobody
// asked for -- for a delete button that means acting on whatever row the
// page's own fallback selection lands on, i.e. a wrong-target destructive
// click rather than a failed test. So a rejected assignment is a hard
// failure here, never a silent continue.
async function applyFills(cdp, fills) {
  const REJECTED = 'VALUE_REJECTED:';
  for (const f of fills) {
    const expr = `(() => {
      const el = document.querySelector(${JSON.stringify(f.selector)});
      if (!el) return 'NOT_FOUND';
      el.value = ${JSON.stringify(f.value)};
      if (String(el.value) !== ${JSON.stringify(String(f.value))}) {
        return ${JSON.stringify(REJECTED)} + String(el.value);
      }
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
      return 'OK';
    })()`;
    const res = await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true });
    const verdict = res.result.value;
    if (verdict === 'OK') continue;
    if (typeof verdict === 'string' && verdict.startsWith(REJECTED)) {
      throw new Error(
        `fill selector ${JSON.stringify(f.selector)} did not accept value ` +
        `${JSON.stringify(String(f.value))} -- element reads ` +
        `${JSON.stringify(verdict.slice(REJECTED.length))} instead ` +
        `(option list not populated yet, or value out of range/step). ` +
        `Refusing to click with an unintended value.`);
    }
    throw new Error(`fill selector ${JSON.stringify(f.selector)} not found on page`);
  }
}

// Post-click settle. The dialog (if any) has already been answered by the
// CdpSession message handler by the time the click's Runtime.evaluate
// returns; what is still outstanding is the handler's own fetch(). Give the
// handler a moment to issue it, then either wait for the row's declared
// POST or for the page to go network-quiet -- never just sleep, since the
// caller kills Chrome immediately after this returns.
async function settle(cdp, args) {
  await cdp.send('Runtime.evaluate', { expression: 'new Promise(r => setTimeout(r, 500))', awaitPromise: true });
  if (args.expectPost) {
    const hit = await cdp.waitForPost(args.expectPost, POST_WAIT_TIMEOUT_MS);
    return { method: hit.method, url: hit.url, status: hit.status, failed: hit.failed };
  }
  await cdp.waitForQuiet(QUIET_TIMEOUT_MS);
  return null;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  let postResult = null;
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
    const cdp = new CdpSession(ws, args.acceptDialogs);
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

    if (args.fills.length) {
      await applyFills(cdp, args.fills);
    }

    if (args.selectorKind === 'id') {
      const expr = `(() => { const el = document.getElementById(${JSON.stringify(args.selector)}); if (!el) return 'NOT_FOUND'; el.click(); return 'CLICKED'; })()`;
      const res = await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true });
      if (res.result.value !== 'CLICKED') {
        throw new Error(`selector #${args.selector} not found on ${args.route} at runtime`);
      }
      postResult = await settle(cdp, args);
    } else if (args.selectorKind === 'text') {
      const expr = `(() => { const btn = [...document.querySelectorAll('button')].find(b => b.textContent.includes(${JSON.stringify(args.selector)})); if (!btn) return 'NOT_FOUND'; btn.click(); return 'CLICKED'; })()`;
      const res = await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true });
      if (res.result.value !== 'CLICKED') {
        throw new Error(`button text "${args.selector}" not found on ${args.route} at runtime`);
      }
      postResult = await settle(cdp, args);
    } // "page": no click, load only

    if (args.screenshot) {
      const shot = await cdp.send('Page.captureScreenshot', { format: 'png' });
      await writeFile(args.screenshot, Buffer.from(shot.data, 'base64'));
    }

    console.log(JSON.stringify({
      ok: true, route: args.route, selector: args.selector || null,
      fills: args.fills, dialogs: cdp.dialogs, post: postResult,
    }));
  } finally {
    try { chrome.kill(); } catch { /* already gone */ }
    try { await rm(userDataDir, { recursive: true, force: true }); } catch { /* best effort */ }
  }
}

main().catch((err) => {
  console.error(String(err && err.stack || err));
  process.exit(1);
});
