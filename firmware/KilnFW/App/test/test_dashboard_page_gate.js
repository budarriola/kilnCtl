/* Node-only test for app.js's KG-01 dashboard page gate (owner 2026-10-19):
 *  - kcHandleNextParam(): the dashboard opened with ?next=<page> shows the
 *    sign-in dialog FIRST and navigates only on success; open-redirect values
 *    are refused; a signed-in admin navigates once (loop guard).
 *  - kcInitDashboardLinkGate(): a click on a protected-page link while logged
 *    out is cancelled, the dialog opens, and success navigates.
 * Runs the REAL app.js blocks (extracted by marker line, same approach as
 * test_page_login_gate.js) with openLoginModal()/DOM stubbed.
 *
 * Run: node firmware/KilnFW/App/test/test_dashboard_page_gate.js
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const LINES = fs.readFileSync(resolveDriverFile(resolveDriversDir(__dirname), 'app.js'), 'utf8').split('\n');

function extractRange(startMarker, endMarkerExclusive) {
  const raw = (l) => l.replace(/\r$/, '');
  const s = LINES.findIndex((l) => raw(l) === startMarker);
  if (s === -1) throw new Error('start marker not found: ' + startMarker);
  const e = LINES.findIndex((l, i) => i > s && raw(l) === endMarkerExclusive);
  if (e === -1) throw new Error('end marker not found: ' + endMarkerExclusive);
  return LINES.slice(s, e).join('\n');
}
const LOGIN_SINGLETON = extractRange('  var pendingLogin = null;', '  window.fetch = function (input, init) {');
const GATE = extractRange('  var OPEN_WITHOUT_LOGIN_PATHS = {', '  function pollSession() {');
if (GATE.indexOf('function kcHandleNextParam(') === -1 || GATE.indexOf('function kcInitDashboardLinkGate(') === -1) {
  throw new Error('sanity: KG-01 functions missing from the extracted range');
}

let passed = 0, failed = 0;
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); } else { failed++; console.log('FAIL: ' + label); }
}
const rel = (h) => h.replace('http://kiln.local', '');
const flush = () => new Promise((r) => setImmediate(r)).then(() => new Promise((r) => setImmediate(r)));

function makeContext(opts) {
  const o = Object.assign({ pathname: '/', search: '', hash: '', bootstrap: false, role: null }, opts);
  const opens = [];
  const store = {};
  const loc = { pathname: o.pathname, search: o.search, hash: o.hash, href: 'http://kiln.local' + o.pathname + o.search, origin: 'http://kiln.local' };
  const handlers = [];
  const state = { replaced: null };
  const ctx = {
    window: {
      location: loc,
      history: { replaceState: (a, b, url) => { state.replaced = url; } },
      sessionStorage: {
        getItem: (k) => (k in store ? store[k] : null),
        setItem: (k, v) => { store[k] = String(v); },
        removeItem: (k) => { delete store[k]; },
      },
    },
    document: { addEventListener: (t, fn) => { if (t === 'click') handlers.push(fn); } },
    URL: URL,
    decodeURIComponent: decodeURIComponent,
    openLoginModal: (title) => new Promise((resolve) => { opens.push({ title, settle: resolve }); }),
    kcPageIsDashboard: () => loc.pathname === '/',
    Promise: Promise,
  };
  vm.createContext(ctx);
  vm.runInContext('var lastKnownRole = ' + JSON.stringify(o.role) + ';\n' + LOGIN_SINGLETON + '\n' + GATE +
    '\nlastKnownBootstrapNeeded = ' + (o.bootstrap ? 'true' : 'false') + ';' +
    '\nthis.__next = kcHandleNextParam;' +
    '\nthis.__init = kcInitDashboardLinkGate;' +
    '\nthis.__safe = kcSafeNextPath;' +
    '\nthis.__protected = kcPathIsProtectedPage;' +
    '\nthis.__declined = function () { return authPromptDeclined; };', ctx);
  const click = (href, extra) => {
    let prevented = false;
    const a = { target: '', href: href };
    const ev = Object.assign({
      defaultPrevented: false, button: 0, ctrlKey: false, metaKey: false, shiftKey: false, altKey: false,
      target: { closest: () => a }, preventDefault: () => { prevented = true; },
    }, extra || {});
    handlers.forEach((h) => h(ev));
    return () => prevented;
  };
  return { ctx, opens, loc, state, store, click };
}

(async () => {
  // next-param sanitising (open-redirect guard)
  {
    const { ctx } = makeContext();
    assert(ctx.__safe('/settings') === '/settings', 'safe: /settings accepted');
    assert(ctx.__safe('/safety/commissioning') === '/safety/commissioning', 'safe: nested page accepted');
    for (const bad of ['//evil.com', 'http://evil.com', '/\\evil', '/api/zones', '/', '', 'settings', '/a b', '/x?y=1', null, undefined]) {
      assert(ctx.__safe(bad) === null, 'safe: rejects ' + JSON.stringify(bad));
    }
    assert(ctx.__protected('/settings') === true, 'protected: /settings');
    assert(ctx.__protected('/') === false && ctx.__protected('/login') === false, 'protected: dashboard and login are not');
    assert(ctx.__protected('/wifi') === false, 'protected: /wifi left to the server (open while unprovisioned)');
    assert(ctx.__protected('/api/history.csv') === false && ctx.__protected('/theme.css') === false, 'protected: api/assets are not');
  }

  // ?next= while logged out: dialog first, navigate only on success
  {
    const t = makeContext({ search: '?next=%2Fsettings%2Fsafety', hash: '#x', role: 'none' });
    t.ctx.__next('none');
    await flush();
    assert(t.opens.length === 1 && t.opens[0].title === 'Sign in required', 'next: logged out opens the sign-in dialog');
    assert(rel(t.loc.href) === '/?next=%2Fsettings%2Fsafety', 'next: no navigation before the dialog succeeds');
    assert(t.state.replaced === '/', 'next: ?next= stripped from the dashboard URL');
    t.opens[0].settle(true);
    await flush();
    assert(rel(t.loc.href) === '/settings/safety#x', 'next: success navigates to the page (hash kept)');
  }
  // cancel stays on the dashboard
  {
    const t = makeContext({ search: '?next=%2Fsettings', role: 'none' });
    t.ctx.__next('none');
    await flush();
    t.opens[0].settle(false);
    await flush();
    assert(rel(t.loc.href) === '/?next=%2Fsettings' && t.ctx.__declined(), 'next: Cancel stays on the dashboard, no re-nag');
  }
  // hostile next values never navigate nor open
  for (const q of ['?next=%2F%2Fevil.com', '?next=http%3A%2F%2Fevil.com', '?next=%2Fapi%2Fzones', '?next=%E0%A4%A']) {
    const t = makeContext({ search: q, role: 'admin' });
    t.ctx.__next('admin');
    await flush();
    assert(t.opens.length === 0 && rel(t.loc.href) === '/' + q, 'next: hostile value ' + q + ' is ignored');
  }
  // signed-in admin navigates once, then the loop guard stops a bounce
  {
    const t = makeContext({ search: '?next=%2Fsettings', role: 'admin' });
    t.ctx.__next('admin');
    assert(rel(t.loc.href) === '/settings' && t.opens.length === 0, 'next: signed-in admin navigates without a dialog');
    const t2 = makeContext({ search: '?next=%2Fsettings', role: 'admin' });
    t2.store.kcNextTried = '/settings';
    t2.ctx.__next('admin');
    assert(rel(t2.loc.href) === '/?next=%2Fsettings' && t2.opens.length === 0, 'next: loop guard stops an admin bounce');
  }
  // bootstrap needed: the dialog cannot succeed -> /login?return=...
  {
    const t = makeContext({ search: '?next=%2Fsettings', role: 'admin', bootstrap: true });
    t.ctx.__next('admin');
    await flush();
    assert(t.opens.length === 0 && /^\/login\?return=/.test(rel(t.loc.href)), 'next: bootstrap needed redirects to the bootstrap form');
  }
  // not the dashboard: ignored
  {
    const t = makeContext({ pathname: '/settings', search: '?next=%2Fsafety', role: 'none' });
    t.ctx.__next('none');
    await flush();
    assert(t.opens.length === 0, 'next: ignored off the dashboard');
  }

  // link clicks on the dashboard
  {
    const t = makeContext({ role: 'none' });
    t.ctx.__init();
    const prevented = t.click('http://kiln.local/settings/zones?a=1#h');
    assert(prevented() && t.opens.length === 1, 'click: protected link while logged out is cancelled and opens the dialog');
    assert(rel(t.loc.href) === '/', 'click: no navigation before login');
    t.opens[0].settle(true);
    await flush();
    assert(rel(t.loc.href) === '/settings/zones?a=1#h', 'click: success navigates to the link');
  }
  {
    const t = makeContext({ role: 'none' });
    t.ctx.__init();
    const p1 = t.click('http://kiln.local/live_profile');
    t.opens[0].settle(false);
    await flush();
    assert(p1() && rel(t.loc.href) === '/', 'click: Cancel keeps the operator on the dashboard');
  }
  {
    const cases = [
      ['admin', 'http://kiln.local/settings', {}, 'signed-in admin'],
      [null, 'http://kiln.local/settings', {}, 'role unknown yet (server 302 covers it)'],
      ['none', 'http://kiln.local/login', {}, 'link to /login'],
      ['none', 'http://kiln.local/wifi', {}, 'link to /wifi'],
      ['none', 'http://kiln.local/api/history.csv', {}, 'api download link'],
      ['none', 'http://evil.example/settings', {}, 'cross-origin link'],
      ['none', 'http://kiln.local/settings', { ctrlKey: true }, 'ctrl-click (new tab)'],
      ['none', 'http://kiln.local/settings', { button: 1 }, 'middle click'],
    ];
    for (const [role, href, extra, label] of cases) {
      const t = makeContext({ role });
      t.ctx.__init();
      const p = t.click(href, extra);
      assert(!p() && t.opens.length === 0, 'click: not intercepted for ' + label);
    }
    const t = makeContext({ role: 'none', bootstrap: true });
    t.ctx.__init();
    const p = t.click('http://kiln.local/settings');
    assert(!p() && t.opens.length === 0, 'click: not intercepted while bootstrap is needed (server 302 -> bootstrap form)');
    const t3 = makeContext({ pathname: '/settings', role: 'none' });
    t3.ctx.__init();
    const p3 = t3.click('http://kiln.local/safety');
    assert(!p3() && t3.opens.length === 0, 'click: only the dashboard intercepts');
  }

  console.log('\n' + passed + ' passed, ' + failed + ' failed');
  process.exit(failed === 0 ? 0 : 1);
})();
