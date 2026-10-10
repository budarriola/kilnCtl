/* Node-only test for zones_page.html's atCeilingMessage() (review 2026-10-10 L2):
 * every ceiling_adoption value the firmware emits (dashboard_autotune_http.c)
 * must map to an operator message, REFUSED_NOT_WRITTEN must say nothing was
 * written, and an unknown value must NOT read as plain success.
 * Function extracted VERBATIM from the shipped page.
 * Run: node firmware/KilnFW/App/test/test_zones_ceiling_message.js
 */
'use strict';
const fs = require('fs');
const vm = require('vm');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const DIR = resolveDriversDir(__dirname);
const L = fs.readFileSync(resolveDriverFile(DIR, 'zones_page.html'), 'utf8').split('\n').map((l) => l.replace(/\r$/, ''));
const i = L.findIndex((l) => l.startsWith('function atCeilingMessage'));
if (i === -1) throw new Error('atCeilingMessage not found');
const e = L.findIndex((l, k) => k >= i && l === '}');
const sb = {};
vm.createContext(sb);
vm.runInContext(L.slice(i, e + 1).join('\n'), sb);

let failed = 0;
function assert(c, label) { console.log((c ? 'PASS: ' : 'FAIL: ') + label); if (!c) failed++; }
const m = (v) => sb.atCeilingMessage({ ceiling_adoption: v, ceiling_old_c_per_hr: 100, ceiling_new_c_per_hr: 200 });

const refused = m('REFUSED_NOT_WRITTEN');
assert(/NOT adopted/.test(refused) && /nothing was written/.test(refused), 'REFUSED_NOT_WRITTEN says not adopted / nothing written');
assert(refused !== 'Accepted.', 'REFUSED_NOT_WRITTEN is not the bare success text');
const unknown = m('SOME_NEW_VALUE');
assert(unknown !== 'Accepted.' && /result is unknown/.test(unknown) && /SOME_NEW_VALUE/.test(unknown), 'unknown value is flagged, not plain success');
assert(/200/.test(m('ADOPTED')), 'ADOPTED shows the new ceiling');
for (const v of ['SKIPPED_NOT_REQUESTED', 'SKIPPED_RELAY_METHOD', 'SKIPPED_MODEL_NOT_PERSISTED', 'SKIPPED_ZERO',
  'SKIPPED_WOULD_TIGHTEN', 'REJECTED_OUT_OF_RANGE', 'SKIPPED_READ_FAILED', 'FAILED_TO_PERSIST']) {
  assert(!/result is unknown/.test(m(v)), v + ' has a specific message');
}
console.log(failed ? 'FAILED' : 'ALL PASSED');
process.exit(failed ? 1 : 0);
