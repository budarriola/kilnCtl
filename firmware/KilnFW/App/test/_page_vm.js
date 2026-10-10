/* _page_vm.js -- shared helper (not a test): runs a page's LAST inline <script>
 * block in a node vm against a minimal fake DOM so a test can click a button
 * and read what the page actually wrote to its status element. Unlike the
 * regex source-contract tests this executes the production handler.
 * Excluded from check_page_js_tests.ps1 like _drivers_layout.js. */
'use strict';
const vm = require('vm');

function lastInlineScript(html) {
  const re = /<script>([\s\S]*?)<\/script>/g;
  let m, last = null;
  while ((m = re.exec(html)) !== null) last = m[1];
  if (last === null) throw new Error('no inline <script> block found');
  return last;
}

function makeEl(id) {
  const handlers = {};
  const attrs = {};
  return {
    id, textContent: '', hidden: false, checked: false, files: null, value: '',
    disabled: false, style: {}, dataset: {}, innerHTML: '', querySelectorAll() { return []; }, classList: { add() {}, remove() {}, toggle() {} },
    addEventListener(ev, fn) { (handlers[ev] = handlers[ev] || []).push(fn); },
    getAttribute(k) { return Object.prototype.hasOwnProperty.call(attrs, k) ? attrs[k] : null; },
    setAttribute(k, v) { attrs[k] = String(v); },
    removeAttribute(k) { delete attrs[k]; },
    fire(ev) { (handlers[ev] || []).forEach((f) => f({ target: this })); },
    _attrs: attrs, _handlers: handlers,
  };
}

/* opts.elements: ids to pre-create; opts.groups: {selector: [el,...]};
 * opts.extra: extra globals (window.* stubs go here too, they are mirrored). */
function runPageScript(html, opts) {
  opts = opts || {};
  const els = {};
  const getEl = (id) => (els[id] = els[id] || makeEl(id));
  (opts.elements || []).forEach(getEl);
  const sandbox = {
    console: { log() {}, warn() {}, error() {} },
    setTimeout, clearTimeout, Promise,
    document: {
      getElementById: getEl,
      querySelectorAll: (sel) => (opts.groups && opts.groups[sel]) || [],
      querySelector: () => null,
      addEventListener() {},
      documentElement: makeEl('html'),
    },
    localStorage: { getItem: () => null, setItem() {} },
    matchMedia: () => ({ matches: false }),
    addEventListener() {},
    FileReader: opts.FileReader,
  };
  sandbox.window = sandbox;
  Object.assign(sandbox, opts.extra || {});
  vm.createContext(sandbox);
  vm.runInContext(lastInlineScript(html), sandbox);
  return { els, getEl, sandbox };
}

/* Let queued promise callbacks drain. */
async function flush(n) {
  for (let i = 0; i < (n || 30); i++) await new Promise((r) => setImmediate(r));
}

function fakeResponse(status, text) {
  return { ok: status >= 200 && status < 300, status, text: () => Promise.resolve(text) };
}

module.exports = { runPageScript, flush, fakeResponse, makeEl, lastInlineScript };
