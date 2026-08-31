#!/usr/bin/env python3
"""Regression test for zones_page.html's relay-method Accept-button gate
(final review, blocker 2).

finalize_relay_fit() (autotune_engine.c) never sets s_at.model.valid -- a
relay run measures no plant model at all ("no K, no tau, no L", see that
function's own comment), so /api/autotune's `model_valid` field reads False
for EVERY relay result, forever. The firmware's own autotune_engine_accept()
gate already exempts relay entirely (`if (s_at.method == AUTOTUNE_METHOD_
STEP)`), but zones_page.html's `atLastDoneModelValid` used to be derived
from `s.model_valid` unconditionally, so the JS-side Accept button could
NEVER be enabled for a relay autotune, even a fully successful one.

This test executes the ACTUAL JS from zones_page.html (via Node, the same
runtime the browser uses -- not a hand-reimplementation of the logic in
Python, which could silently drift from what's actually shipped) against a
synthetic relay-DONE status object and a synthetic step-DONE status object,
and asserts the real `updateAcceptButtonEnabled()` function leaves the
button enabled in both cases.

Skipped (not failed) if Node is not available in this environment -- no
other test in this suite depends on a JS runtime, so this stays opt-in
rather than adding a new hard dependency for the whole pytest run.
"""
from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[3]
_ZONES_PAGE_HTML = _REPO_ROOT / "firmware" / "KilnFW" / "App" / "drivers" / "zones_page.html"

_NODE = shutil.which("node")

# Minimal DOM stub sufficient for updateAcceptButtonEnabled() and the
# pollAutotune() slice that sets atLastDoneModelValid/atLastRefused/
# atLastUnsettled -- NOT a full page load (the rest of zones_page.html's
# script references many other elements this test does not populate), so
# this harness calls the two pieces directly rather than executing
# pollAutotune()'s own fetch()-driven body.
_HARNESS_TEMPLATE = r"""
// Deliberately NOT 'use strict' -- strict-mode direct eval() creates its
// own isolated scope even for `function` declarations, so the extracted
// updateAcceptButtonEnabled() would never become visible to runCase() below.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');

// Extract the two things under test, verbatim, from the real file -- not
// retyped by hand, so this test cannot silently drift from what ships.
function extractFunction(src, name) {
  const marker = 'function ' + name + '(';
  const start = src.indexOf(marker);
  if (start < 0) throw new Error('could not find function ' + name);
  let depth = 0, i = src.indexOf('{', start);
  const bodyStart = i;
  for (; i < src.length; i++) {
    if (src[i] === '{') depth++;
    else if (src[i] === '}') { depth--; if (depth === 0) { i++; break; } }
  }
  return src.slice(start, i);
}

function extractAssignment(src, marker) {
  const start = src.indexOf(marker);
  if (start < 0) throw new Error('could not find assignment starting with ' + JSON.stringify(marker));
  const end = src.indexOf(';', start);
  if (end < 0) throw new Error('could not find end of assignment starting with ' + JSON.stringify(marker));
  return src.slice(start, end + 1);
}

const updateFnSrc = extractFunction(html, 'updateAcceptButtonEnabled');
// The SPECIFIC assignment inside pollAutotune (the one that derives
// atLastDoneModelValid from the current status object `s`), not the
// `var atLastDoneModelValid = false;` declaration earlier in the file --
// disambiguated by the ternary's own opening, which is unique to the real
// assignment.
const assignSrc = extractAssignment(html, "atLastDoneModelValid = (s.state === 'done') &&");

// Fake DOM: just enough elements for updateAcceptButtonEnabled() to read
// atAcceptBtn.disabled and atAckUnsettled.checked.
const elements = {
  atAcceptBtn: { disabled: true },
  atAckUnsettled: { checked: false },
};
function getElementById(id) {
  if (!(id in elements)) throw new Error('unexpected getElementById(' + id + ')');
  return elements[id];
}
const document = { getElementById };

let atLastDoneModelValid = false;
let atLastRefused = false;
let atLastUnsettled = false;

// eslint-disable-next-line no-eval
eval(updateFnSrc);

function runCase(s) {
  atLastRefused = false;
  atLastUnsettled = false;
  elements.atAcceptBtn.disabled = true;
  elements.atAckUnsettled.checked = false;
  // eslint-disable-next-line no-eval
  eval(assignSrc);
  updateAcceptButtonEnabled();
  return { atLastDoneModelValid, acceptDisabled: elements.atAcceptBtn.disabled };
}

const relayDone = { state: 'done', method: 'relay', model_valid: false, relay_valid: true };
const stepDone = { state: 'done', method: 'step', model_valid: true, relay_valid: false };

const results = { relay: runCase(relayDone), step: runCase(stepDone) };
process.stdout.write(JSON.stringify(results));
"""


@pytest.mark.skipif(_NODE is None, reason="Node.js not available in this environment")
def test_relay_done_leaves_accept_button_enabled(tmp_path):
    assert _ZONES_PAGE_HTML.is_file(), _ZONES_PAGE_HTML
    harness_path = tmp_path / "harness.js"
    harness_path.write_text(_HARNESS_TEMPLATE, encoding="utf-8")
    proc = subprocess.run(
        [_NODE, str(harness_path), str(_ZONES_PAGE_HTML)],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert proc.returncode == 0, f"node harness failed: {proc.stderr}"
    results = json.loads(proc.stdout)

    assert results["relay"]["atLastDoneModelValid"] is True, (
        "a relay DONE result (model_valid=False, relay_valid=True -- exactly what "
        "finalize_relay_fit() always produces, since a relay run measures no plant "
        "model at all) must set atLastDoneModelValid true. If this fails, the JS "
        "condition went back to reading s.model_valid unconditionally, which makes "
        "Accept permanently unclickable for every relay autotune."
    )
    assert results["relay"]["acceptDisabled"] is False, (
        "with atLastDoneModelValid true and no refusal/unsettled flag, the Accept "
        "button must end up ENABLED (disabled === false) for a relay DONE result"
    )
    assert results["step"]["atLastDoneModelValid"] is True, (
        "sanity/regression guard: a step DONE result (model_valid=True) must still "
        "set atLastDoneModelValid true, same as before this fix"
    )
    assert results["step"]["acceptDisabled"] is False, (
        "sanity/regression guard: Accept must still be enabled for a clean step DONE result"
    )
