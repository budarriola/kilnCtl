/* Syntax-checks every inline <script> block in the KilnFW web pages, plus the
 * standalone .js files.
 *
 * Exists because of a real, shipped bug: thermo_faults_page.html wrote a C-style
 * comment as "MAX31856_MASK_*" followed by "/", which closed the comment early
 * and turned the rest of the line into code. The syntax error killed the whole
 * inline script, so the page sat on "Loading..." forever while its endpoint was
 * perfectly healthy -- invisible to curl, invisible to every host test.
 */
const fs = require('fs'), path = require('path'), vm = require('vm');

const dir = process.argv[2];
let bad = 0, checked = 0;

for (const f of fs.readdirSync(dir).filter(n => n.endsWith('.html') || n.endsWith('.js'))) {
  let src = fs.readFileSync(path.join(dir, f), 'utf8');
  const blocks = [];

  if (f.endsWith('.js')) {
    blocks.push({ code: src, line: 1 });
  } else {
    /* Blank out HTML comments before scanning, preserving newlines so reported
     * line numbers stay correct. Several pages mention the literal word
     * "<script>" inside a comment; matching from there captures nonsense and
     * reports a syntax error in a file that is actually fine. */
    src = src.replace(/<!--[\s\S]*?-->/g, m => m.replace(/[^\n]/g, ' '));

    const re = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
    let m;
    while ((m = re.exec(src))) {
      blocks.push({ code: m[1], line: src.slice(0, m.index).split('\n').length });
    }
  }

  for (const b of blocks) {
    checked++;
    try {
      new vm.Script(b.code);
    } catch (e) {
      bad++;
      console.log(`${f} (inline script at line ${b.line}): ${e.message}`);
    }
  }
}

console.log(`\nchecked ${checked} script blocks, ${bad} with syntax errors`);
process.exit(bad ? 1 : 0);
