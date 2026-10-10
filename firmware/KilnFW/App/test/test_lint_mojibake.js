/* lint_pages.js must flag mojibake (double-encoded UTF-8) in any page and
 * must stay quiet on a clean page. Runs the real lint script on temp dirs.
 * Run: node firmware/KilnFW/App/test/test_lint_mojibake.js */
'use strict';
const fs = require('fs'), os = require('os'), path = require('path');
const { spawnSync } = require('child_process');
let failed = 0;
function ok(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }

function lint(html) {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'lintmoji-'));
  try {
    fs.writeFileSync(path.join(d, 'p.html'), html, 'utf8');
    const r = spawnSync(process.execPath, [path.join(__dirname, 'lint_pages.js'), d], { encoding: 'utf8' });
    return { code: r.status, out: (r.stdout || '') + (r.stderr || '') };
  } finally { fs.rmSync(d, { recursive: true, force: true }); }
}
const MOJI = String.fromCharCode(0xe2, 0x20ac, 0xa6);
const page = (t) => '<html><body><div>' + t + '</div><script>var x = 1;</script></body></html>';

let r = lint(page('Loading' + MOJI));
ok(r.code === 1 && /mojibake/.test(r.out), 'mojibake in markup text is flagged (exit 1)');
r = lint('<html><body><script>var s = "Saving' + MOJI + '";</script></body></html>');
ok(r.code === 1 && /mojibake/.test(r.out), 'mojibake inside a script string is flagged');
r = lint(page('Loading… and ...'));
ok(r.code === 0 && !/mojibake/.test(r.out), 'a real ellipsis and ASCII dots are not flagged');

/* The U+00C2 / U+00C3 lead pairs (8eba89bd2): a double-encoded degree sign is
 * C2 B0 and an accented e is C3 A9. Built from char codes so this file stays ASCII. */
const DEG_MOJI = String.fromCharCode(0xc2, 0xb0), E_MOJI = String.fromCharCode(0xc3, 0xa9);
r = lint(page('Temp 20' + DEG_MOJI + 'C'));
ok(r.code === 1 && /mojibake/.test(r.out), 'double-encoded degree sign (U+00C2 U+00B0) is flagged');
r = lint(page('caf' + E_MOJI));
ok(r.code === 1 && /mojibake/.test(r.out), 'double-encoded e-acute (U+00C3 U+00A9) is flagged');
r = lint(page('Temp 20' + String.fromCharCode(0xb0) + 'C, caf' + String.fromCharCode(0xe9)));
ok(r.code === 0 && !/mojibake/.test(r.out), 'a legitimate degree sign and e-acute pass clean');

console.log(failed ? failed + ' FAILED' : 'all passed');
process.exit(failed ? 1 : 0);
