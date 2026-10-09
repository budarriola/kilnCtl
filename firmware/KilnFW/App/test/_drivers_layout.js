/* _drivers_layout.js -- shared helper for App/test scripts (JS/mjs) that
 * read specific firmware/KilnFW/App/drivers files by name.
 *
 * firmware/KilnFW/App/drivers/ is being split into layer subdirectories
 * (drivers/<layer>/<name> -- common,hw,owners,control,safety,persist,net,
 * http,ui,bridge,sim; see tools/drivers_reorg/mapping.csv). Several test
 * scripts in this directory used to build a flat
 * `path.join(__dirname, '..', 'drivers', '<name>')` path, or scan drivers/
 * with a non-recursive readdirSync -- both silently stop finding the real
 * file (or find zero pages and report a vacuous pass) the moment a file
 * moves one layer deeper. This mirrors _drivers_layout.py's contract
 * (recursive basename search, unique match required, loud failure) and
 * lint_pages.js's own find_file()/walk_files() for the same reason: same
 * shape, same directory, kept in sync deliberately rather than reinvented
 * per file.
 *
 * Usage:
 *   const { resolveDriversDir, resolveDriverFile, listDriverFiles } = require('./_drivers_layout.js');
 *   const dir = resolveDriversDir(__dirname);
 *   const pagePath = resolveDriverFile(dir, 'main_page.html');
 *   const pageFiles = listDriverFiles(dir, (name) => name.endsWith('_page.html'));
 */
'use strict';
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

/** Default drivers root: <testDir>/../drivers (App/test -> App/drivers). */
function resolveDriversDir(testDir) {
  return path.resolve(testDir, '..', 'drivers');
}

/** Recursively collect every file under `root` (skips node_modules/.git),
 * as absolute paths. Mirrors lint_pages.js's walk_files(). */
function walkFiles(root) {
  let out = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    if (entry.name === 'node_modules' || entry.name === '.git') continue;
    const full = path.join(root, entry.name);
    if (entry.isDirectory()) out = out.concat(walkFiles(full));
    else out.push(full);
  }
  return out;
}

/** Find `basename` anywhere under `driversDir`. Throws loudly (never
 * returns a nonexistent/wrong/ambiguous path silently) if it is missing or
 * matched more than once. */
/** Git-tracked files under `dir`, as absolute paths. Returns null if `git`
 * is unavailable or the directory isn't inside a repo (caller then skips
 * the tracked-file preference and falls back to the original behavior). */
function trackedFiles(dir) {
  try {
    const out = execFileSync('git', ['-C', dir, 'ls-files', '.'], { encoding: 'utf8' });
    return out.split('\n').filter(Boolean).map((p) => path.resolve(dir, p));
  } catch (err) {
    return null;
  }
}

function resolveDriverFile(driversDir, basename) {
  const matches = walkFiles(driversDir).filter((p) => path.basename(p) === basename);
  if (matches.length === 0) {
    throw new Error(
      `_drivers_layout: '${basename}' not found anywhere under ${driversDir} -- has it moved or been renamed?`
    );
  }
  if (matches.length > 1) {
    // An untracked in-flight draft sharing a basename with the real,
    // git-tracked file must not make every test ambiguous. Prefer the
    // tracked file(s); only throw if more than one TRACKED file shares
    // this basename -- that's the real defect this check exists to catch.
    const tracked = trackedFiles(driversDir);
    if (tracked) {
      const trackedMatches = matches.filter((p) => tracked.includes(p));
      if (trackedMatches.length === 1) {
        return trackedMatches[0];
      }
      if (trackedMatches.length > 1) {
        throw new Error(
          `_drivers_layout: '${basename}' matched more than one git-tracked file under ${driversDir}: ` +
          `${trackedMatches.join(', ')} -- cannot tell which one is the real file.`
        );
      }
    }
    throw new Error(
      `_drivers_layout: '${basename}' matched more than one file under ${driversDir}: ${matches.join(', ')} -- ` +
      'cannot tell which one is the real file.'
    );
  }
  return matches[0];
}

/** List every file (basename) anywhere under `driversDir` whose basename
 * passes `filterFn` (e.g. `(n) => n.endsWith('_page.html')`). Returns
 * `{ name, fullPath }` objects, recursive -- the direct replacement for a
 * flat `readdirSync(driversDir).filter(...)` that would otherwise find
 * nothing (or find files only in the top layer) once drivers/ splits. */
function listDriverFiles(driversDir, filterFn) {
  return walkFiles(driversDir)
    .filter((p) => filterFn(path.basename(p)))
    .map((p) => ({ name: path.basename(p), fullPath: p }));
}

module.exports = { resolveDriversDir, resolveDriverFile, listDriverFiles, walkFiles };
