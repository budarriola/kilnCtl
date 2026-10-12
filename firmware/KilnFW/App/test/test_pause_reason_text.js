/* Node test for main_page.html's pauseReasonText() (pause_reason -> operator text) and that the firing card
 * renders it through kcEscapeHtml. Run: node firmware/KilnFW/App/test/test_pause_reason_text.js */
'use strict';
const fs = require('fs');
const vm = require('vm');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const SRC = fs.readFileSync(resolveDriverFile(resolveDriversDir(__dirname), 'main_page.html'), 'utf8');
let failed = 0;
function check(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }

const m = SRC.match(/function pauseReasonText\(reason\) \{[\s\S]*?\r?\n\}\r?\n/);
check(!!m, 'pauseReasonText found in main_page.html');
const ctx = {};
vm.createContext(ctx);
vm.runInContext(m[0], ctx);
const f = ctx.pauseReasonText;
check(f('') === '' && f(undefined) === '', 'empty reason -> empty');
check(/Heat withheld/.test(f('pico_reboot_undecided')), 'pico_reboot_undecided text');
check(/fatal fault/.test(f('pico_fatal_reboot')), 'pico_fatal_reboot text');
check(/heat grant/.test(f('heat_grant_unconfirmed')), 'heat_grant_unconfirmed text');
check(f('new_reason') === 'Paused: new_reason', 'unknown reason verbatim');
check(/kcEscapeHtml\(pauseReasonText\(st\.pause_reason\)\)/.test(SRC), 'firing card escapes the pause reason text');
process.exit(failed ? 1 : 0);
