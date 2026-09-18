// ui_responsive_sweep_classify.test.mjs -- negative test for
// ui_responsive_sweep.mjs's isTransientHarnessError() (the classifier that
// tells a genuine layout-regression FAIL apart from a transient CDP/fetch
// harness error that deserves a retry, then a SKIP if retries exhaust).
//
// 2026-09-17: this classifier was found to be missing the exact signature
// CdpSession.waitForEvent()'s own timeout produces ("timed out waiting for
// Page.loadEventFired") -- see that fix's own comment on
// isTransientHarnessError() in ui_responsive_sweep.mjs. Confirmed live: three
// independent check_ui_responsive_sweep.ps1 runs on 2026-09-17 misfired under
// run_all_checks.ps1's default 8-way parallel phase (twice SKIP, once
// exactly this FAIL shape), all passing on an immediate standalone rerun --
// the signature of resource contention (`tasklist` showed a dozen-plus
// concurrent chrome.exe from sibling agents/checks at the time), not a real
// layout defect. This test pins that exact string, plus every other
// classification this function is documented to make, so a future edit to
// the CDP-timeout wording or the retry-vs-fail boundary cannot silently
// regress this fix the way the original gap regressed silently for however
// long it existed before 2026-09-17's three misfires surfaced it.
//
// Deliberately unit-level, not a real Chrome sweep: isTransientHarnessError()
// is a pure string-classifier function with no browser/network dependency,
// so this test imports it directly (ui_responsive_sweep.mjs now exports it)
// and asserts against synthetic Error objects -- no headless Chrome, no
// static server, seconds not a minute-plus. ui_responsive_sweep.mjs's
// top-level main() call is guarded (`if (path.resolve(process.argv[1] ...)
// === __filename)`) specifically so this import does not also launch a live
// sweep as a side effect -- see that guard's own comment.
//
// Usage: node ui_responsive_sweep_classify.test.mjs
// Exit code 0 = every assertion passed.

import { isTransientHarnessError } from './ui_responsive_sweep.mjs';

const cases = [
  // The bug this test exists to pin: CdpSession.waitForEvent()'s timeout
  // message, used for Page.loadEventFired, MUST be treated as a retriable
  // harness error, not a hard, unretried layout FAIL.
  ['timed out waiting for Page.loadEventFired', true, 'waitForEvent timeout (the 2026-09-17 gap)'],
  ['timed out waiting for Page.frameNavigated', true, 'waitForEvent timeout, different event name'],

  // Pre-existing classifications this fix must not disturb.
  ['fetch failed', true, 'Node fetch() network error'],
  ['request to http://127.0.0.1:9333/json/new?about:blank failed, reason: ECONNREFUSED', true, 'ECONNREFUSED'],
  ['CDP call Runtime.evaluate timed out after 20000ms (Chrome unresponsive)', true, 'CdpSession.send() per-call timeout'],
  ['CDP connection closed: code=1006 reason=(none)', true, 'socket closed under Chrome crash/kill'],
  ['WebSocket connection to ws://127.0.0.1:9333/devtools/page/ABC failed: timeout', true, 'WebSocket connect failure'],
  ['WebSocket was closed before the connection was established', true, 'WebSocket close-before-open'],

  // Must stay hard FAILs -- a real regression in the page's own JS must
  // never be swallowed as "just the harness", even if its message happens to
  // contain a substring that looks transient-ish.
  ['page script threw: {"exceptionDetails":{"text":"Uncaught TypeError"}}', false, 'real page-JS exception'],
  ['fixture script threw: renderZones not found', false, 'real fixture exception'],
  ['setup script threw: boom', false, 'real setup exception'],
  ['some totally unrelated page-JS error message', false, 'unrecognized message defaults to a hard FAIL'],
  // 2026-09-09 regression this suite already guards against in production:
  // a real page-JS message that merely mentions "WebSocket" (a feature-
  // detection string, say) must not be downgraded just because the old
  // broad /WebSocket/i test would have matched it.
  ['page script threw: "WebSocket API is not supported in this browser"', false, 'WebSocket-mentioning real exception must stay a FAIL'],
];

let failures = 0;
for (const [msg, expected, label] of cases) {
  const got = isTransientHarnessError(new Error(msg));
  if (got !== expected) {
    failures++;
    console.error(`FAIL: ${label}\n      message: ${JSON.stringify(msg)}\n      expected isTransientHarnessError() = ${expected}, got ${got}`);
  } else {
    console.log(`OK:   ${label} -> ${got}`);
  }
}

if (failures > 0) {
  console.error(`\nui_responsive_sweep_classify.test.mjs: ${failures} of ${cases.length} assertion(s) failed.`);
  process.exit(1);
}
console.log(`\nui_responsive_sweep_classify.test.mjs: all ${cases.length} assertions passed.`);
process.exit(0);
