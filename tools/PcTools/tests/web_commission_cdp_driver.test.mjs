// web_commission_cdp_driver.test.mjs -- end-to-end test of
// tools/PcTools/scripts/_web_commission_cdp.mjs's 2026-09-21 additions
// (selector-kind "aria-label"/"css", and the --steps runner), driven
// against real headless Chrome the same way
// firmware/KilnFW/App/test/ui_responsive_sweep.mjs is, but pointed at a
// throwaway local HTTP server serving a small static fixture -- never the
// live board (this repo's driver scripts must never touch a real kiln
// outside an explicitly authorized commissioning row).
//
// Motivation: web_commission_row.py's header comment (see ROWS dict,
// "Left unwired, and why") documents that W8 (profile segment builder),
// W9 (per-row delete, no stable id) and W10 (favourite toggle, aria-label
// only) could not be wired because the driver had no way to (a) match an
// element by its aria-label, (b) address a control created by an earlier
// click, or (c) run more than one click in a row. This test proves the
// driver-level primitives those three rows would need actually work,
// without touching web_commission_row.py itself (out of scope for this
// change -- see the fixture's header comment for the exact shapes modelled
// on profiles_page.html's favToggleBtn()/segmentRow()).
//
// Run directly: node tools/PcTools/tests/web_commission_cdp_driver.test.mjs
// Exit 0 + "all N assertions passed" on success, exit 1 + a fixture/driver
// error on failure -- same convention as
// firmware/KilnFW/App/test/ui_responsive_sweep_classify.test.mjs.

import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DRIVER = path.join(HERE, '..', 'scripts', '_web_commission_cdp.mjs');
const FIXTURE = path.join(HERE, '_web_commission_cdp_fixture.html');

let passed = 0;
let failed = 0;
function ok(cond, label) {
  if (cond) { passed++; console.log(`OK:   ${label}`); }
  else { failed++; console.error(`FAIL: ${label}`); }
}

function findChrome() {
  const candidates = [
    process.env.KC_SWEEP_CHROME,
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
  ].filter(Boolean);
  for (const c of candidates) if (existsSync(c)) return c;
  return null;
}

// Minimal static+capture server: serves the fixture at "/", answers any
// /api/* POST with 200 after a short delay (so wait-for-post has a real
// bounded wait to do, not an instant resolve), and records every POST it
// saw so the test can assert on the captured querystring/body rather than
// trusting the driver's own report alone.
function startServer(fixtureHtml) {
  const posts = [];
  const server = createServer((req, res) => {
    if (req.method === 'GET' && req.url === '/') {
      res.writeHead(200, { 'Content-Type': 'text/html' });
      res.end(fixtureHtml);
      return;
    }
    if (req.method === 'POST' && req.url.startsWith('/api/')) {
      posts.push(req.url);
      setTimeout(() => { res.writeHead(200, { 'Content-Type': 'text/plain' }); res.end('ok'); }, 80);
      return;
    }
    res.writeHead(404); res.end('not found');
  });
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => resolve({ server, port: server.address().port, posts }));
  });
}

function runDriver(host, extraArgs) {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [DRIVER, '--host', host, ...extraArgs], {
      env: { ...process.env, KC_SID: 'test-fixture-cookie' },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (d) => { stdout += d; });
    child.stderr.on('data', (d) => { stderr += d; });
    child.on('close', (code) => resolve({ code, stdout, stderr }));
  });
}

function lastJsonLine(stdout) {
  const lines = stdout.trim().split('\n').filter(Boolean);
  return JSON.parse(lines[lines.length - 1]);
}

async function main() {
  if (!findChrome()) {
    console.log('web_commission_cdp_driver.test.mjs: SKIP -- no Chrome/Edge binary found.');
    process.exit(3);
  }

  const fixtureHtml = await readFile(FIXTURE, 'utf-8');
  const { server, port, posts } = await startServer(fixtureHtml);
  const host = `127.0.0.1:${port}`;

  try {
    // 1. selector-kind aria-label: profiles_page.html's favToggleBtn() has
    //    no id, only a per-profile aria-label -- W10's actual blocker.
    {
      const r = await runDriver(host, [
        '--route', '/', '--selector-kind', 'aria-label',
        '--selector', 'Add "Bisque Fast" to favorites',
        '--expect-post', '/api/fav_toggle',
      ]);
      ok(r.code === 0, 'aria-label click: driver exits 0');
      if (r.code === 0) {
        const j = lastJsonLine(r.stdout);
        ok(j.ok === true, 'aria-label click: JSON ok=true');
        ok(j.post && j.post.status === 200, 'aria-label click: reported POST status 200');
      } else {
        console.error(r.stderr);
      }
      ok(posts.includes('/api/fav_toggle'), 'aria-label click: server actually saw the fetch');
    }

    // 2. selector-kind css: a control with neither an id nor a useful
    //    aria-label -- the other half of what W9's per-row delete button
    //    needs (the row-selecting part; see the --steps case below for the
    //    "created by an earlier click" part).
    {
      posts.length = 0;
      const r = await runDriver(host, [
        '--route', '/', '--selector-kind', 'css', '--selector', '#favBtn',
        '--expect-post', '/api/fav_toggle',
      ]);
      ok(r.code === 0, 'css click: driver exits 0');
      ok(posts.includes('/api/fav_toggle'), 'css click: server actually saw the fetch');
    }

    // 3. --steps: click #addSegBtn (renders a new row asynchronously, like
    //    segmentRow()), wait for that row to exist, fill its <select> (a
    //    class-only control with no id, created by the first click -- W8's
    //    actual blocker), then click its own Save button and wait for the
    //    POST it makes. This is the full W8/W9 shape: an earlier click
    //    creates the target of a later step.
    {
      posts.length = 0;
      const steps = JSON.stringify([
        { action: 'click', kind: 'id', selector: 'addSegBtn' },
        { action: 'wait-for-selector', selector: '#segments .seg', timeoutMs: 3000 },
        { action: 'fill', kind: 'css', selector: '#segments .seg select.seg-kind', value: '1' },
        { action: 'click', kind: 'css', selector: '#segments .seg .save' },
        { action: 'wait-for-post', path: '/api/segment_save', timeoutMs: 3000 },
      ]);
      const r = await runDriver(host, ['--route', '/', '--steps', steps]);
      ok(r.code === 0, 'steps: driver exits 0');
      if (r.code !== 0) console.error(r.stderr);
      ok(posts.some((u) => u.includes('kind=1')), 'steps: fill value reached the POST (kind=1, not the default 0)');
      // A fill that assigns .value without dispatching a bubbling 'change'
      // would satisfy the assertion above and still be inert on
      // profiles_page.html, whose segmentRow() rebuilds the row's fields
      // from that event alone. chg=1 is the fixture's own change-listener
      // count, i.e. what the SERVER observed.
      ok(posts.some((u) => u.includes('chg=1')),
         'steps: fill dispatched a bubbling change event on the <select>');
    }

    // 5. Two POSTs to the SAME path in one run: a mid-sequence
    //    "wait-for-post" followed by a final --expect-post must report the
    //    SECOND request, not re-report the first. Before the postCursor
    //    fix, CdpSession.waitForPost() rescanned `completed` from index 0
    //    on every call, so settle()'s `post` -- what
    //    web_commission_row.py's _cdp_post_status() grades a row on --
    //    described the earlier POST: a create's status passed off as the
    //    delete's.
    {
      posts.length = 0;
      const steps = JSON.stringify([
        { action: 'click', kind: 'id', selector: 'addSegBtn' },
        { action: 'wait-for-selector', selector: '#segments .seg:nth-child(1)', timeoutMs: 3000 },
        { action: 'click', kind: 'css', selector: '#segments .seg:nth-child(1) .save' },
        { action: 'wait-for-post', path: '/api/segment_save', timeoutMs: 3000 },
        { action: 'click', kind: 'id', selector: 'addSegBtn' },
        { action: 'wait-for-selector', selector: '#segments .seg:nth-child(2)', timeoutMs: 3000 },
        { action: 'fill', kind: 'css', selector: '#segments .seg:nth-child(2) select.seg-kind', value: '1' },
      ]);
      const r = await runDriver(host, [
        '--route', '/', '--steps', steps,
        '--selector-kind', 'css', '--selector', '#segments .seg:nth-child(2) .save',
        '--expect-post', '/api/segment_save',
      ]);
      ok(r.code === 0, 'post cursor: steps + trailing click run exits 0');
      if (r.code !== 0) console.error(r.stderr);
      ok(posts.filter((u) => u.startsWith('/api/segment_save')).length === 2,
         'post cursor: server saw both segment_save POSTs');
      const j = r.code === 0 ? lastJsonLine(r.stdout) : null;
      ok(!!(j && j.post && typeof j.post.url === 'string' && j.post.url.includes('kind=1')),
         `post cursor: --expect-post reports the SECOND POST, not the first (got ${j && j.post && j.post.url})`);
    }

    // 4. Negative case: a --steps fill against a selector that does not
    //    exist must be a hard failure (exit 1), never a silent no-op --
    //    same rule --fills already enforces for the single-shot path.
    {
      const steps = JSON.stringify([
        { action: 'fill', kind: 'css', selector: '#does-not-exist', value: 'x' },
      ]);
      const r = await runDriver(host, ['--route', '/', '--steps', steps]);
      ok(r.code === 1, 'steps: fill against a missing selector is a hard failure');
      ok(/not found/.test(r.stderr), 'steps: failure message names "not found"');
    }
  } finally {
    server.close();
  }

  console.log(`\nweb_commission_cdp_driver.test.mjs: ${passed}/${passed + failed} assertions passed.`);
  process.exit(failed === 0 ? 0 : 1);
}

main().catch((err) => {
  console.error(String((err && err.stack) || err));
  process.exit(1);
});
