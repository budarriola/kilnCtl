"""Mechanical discovery of LVGL callback entry points that run on the `lvgl`
task's own stack, for check_all_task_stack_budgets.py's `lvgl` row.

WHY THIS EXISTS
---------------
Before this file, TASKS["lvgl"]'s `extra_roots` (in
check_all_task_stack_budgets.py) was a hand-typed list of 8 known callback
entry points: the display flush callback, the touch read callback, five
page-refresh timers, and one lock-screen tick timer. That list had no
mechanism forcing it to stay complete -- it already missed
ui_lcd_lock.c's tick_timer_cb once (closed 2026-09-18, see that file's own
CEILING_BYTES comment on "lvgl"), and it had ZERO entries for
lv_obj_add_event_cb() button/widget handlers or for ui_confirm.c's
on_confirm/on_cancel callbacks -- 56 and 11 registration sites respectively
as of 2026-09-24, all of which run on lvgl's own stack via
lv_timer_handler()'s indirect dispatch, and NONE of which the hand list
named. A deep chain through one of them (confirm_yes_cb ->
ui_home_confirm_start_yes_cb -> ui_home_do_start -> profile_executor_run /
ui_confirm_show, measured by hand at 2512 B) was invisible to the check
before this file existed.

WHAT IT SCANS (mechanically, not a hand-kept list -- same "discovery, not a
list" principle test_display_power_wiring.c's own
find_next_registered_callback() already uses for the flush/read-cb/timer
family of registration sites)
---------------------------------------------------------------------------
Five registration-site shapes, over every *.c file under firmware/KilnFW/App
(excluding `build/` output directories and `test/`, which is host-test
source never linked into KilnCtrl.elf and which quotes several of these
call shapes as STRING LITERALS inside its own test fixtures -- scanning it
would find the string "lv_timer_create(" inside a C string and misread it as
a real registration site):

  * lv_obj_add_event_cb(obj, CALLBACK, event, user_data)
  * lv_timer_create(CALLBACK, period, user_data)
  * lv_display_set_flush_cb(disp, CALLBACK)
  * lv_indev_set_read_cb(indev, CALLBACK)
  * ui_confirm_params_t designated initializers: .on_confirm = CALLBACK,
    .on_cancel = CALLBACK

RESOLUTION
----------
The callback argument at a registration site is sometimes a bare function
name (confirm_yes_cb, refresh_cb, ui_home_confirm_start_yes_cb, ...) --
directly resolvable by searching for a matching function DEFINITION anywhere
in the same tree. It is sometimes instead a local PARAMETER being forwarded
one level down (ui_topbar.c's build_icon_named(..., lv_event_cb_t cb, ...)
passing `cb` straight into lv_obj_add_event_cb(); ui_page_config.c's
build_nav_item() and three siblings do the same shape). For those,
resolve_callback() finds the enclosing function, matches the token against
its parameter list, then walks every CALL SITE of that enclosing function
across the tree and recurses on whatever is passed at the same argument
position -- up to MAX_RESOLVE_DEPTH levels, which comfortably covers every
forwarding chain in this codebase today (all one level deep).

A chain that bottoms out in something that is not a bare identifier at all
(ui_topbar.c's `cfg->gear_cb`, `cfg->prev_cb`, etc. -- a struct field read
at a call site, not a named function) cannot be reduced to a concrete symbol
by a source scan; it is the same category of unresolvable dispatch this
whole checker family already accepts for lv_timer_handler()'s own internal
function-pointer calls (see check_all_task_stack_budgets.py's module
docstring, "Indirect calls ... are not followed"). Such a chain is reported
by name and file:line as a DYNAMIC dispatch point -- never silently dropped
-- but does not fail the check on its own, for the same reason indirect
calls don't: the `lvgl` task is already, permanently, reported as
INDETERMINATE rather than a clean pass, and inventing a hard FAIL for a
structural pattern this common (every page that builds a topbar uses it)
would make the check red for a reason unrelated to any actual regression.

By contrast, a BARE IDENTIFIER that cannot be resolved to any function
definition anywhere in the tree (a rename or deletion left a registration
site pointing at nothing) is exactly the failure mode
check_all_task_stack_budgets.py already treats as fatal for TASKS[*]
['extra_roots'] resolution (see main()'s "could not resolve declared
extra_roots callback(s)" FAIL) -- this file reports that case as an error
string for the caller to fail loudly on, not a note.

VACUITY FLOOR
-------------
`discover()` counts every registration site found (regardless of whether it
resolves) and the caller is expected to fail if that count is implausibly
low -- see check_all_task_stack_budgets.py's MIN_PLAUSIBLE_LVGL_CALLBACKS.
This is the same class of bug as a PowerShell `-Filter` with no character
classes silently scanning zero files and reporting a false-clean result
(project_powershell_filter_has_no_character_classes): a scan that finds
nothing must never be indistinguishable from a codebase that has nothing to
find.
"""

import os
import re

MAX_RESOLVE_DEPTH = 4

IDENT_RE = re.compile(r'^[A-Za-z_]\w*$')

# One capture group each: the callback token as written at the call site,
# which may be a bare identifier OR an arbitrary expression (e.g. a struct
# field access) -- classification happens after capture, not in the regex.
REGISTRATION_PATTERNS = [
    (re.compile(r'\blv_obj_add_event_cb\s*\(\s*[^,()]+(?:\([^)]*\))?[^,]*,\s*([^,]+?)\s*,'),
     "lv_obj_add_event_cb"),
    (re.compile(r'\blv_timer_create\s*\(\s*([^,]+?)\s*,'), "lv_timer_create"),
    (re.compile(r'\blv_display_set_flush_cb\s*\(\s*[^,]+,\s*([^,)]+?)\s*\)'),
     "lv_display_set_flush_cb"),
    (re.compile(r'\blv_indev_set_read_cb\s*\(\s*[^,]+,\s*([^,)]+?)\s*\)'),
     "lv_indev_set_read_cb"),
    (re.compile(r'\.on_confirm\s*=\s*([^,\n]+?)\s*,'), "ui_confirm.on_confirm"),
    (re.compile(r'\.on_cancel\s*=\s*([^,\n]+?)\s*,'), "ui_confirm.on_cancel"),
]

# A function definition: optional storage-class/return-type tokens, the name,
# a parenthesised parameter list with no unbalanced parens inside (true for
# every function signature in this codebase -- none takes a function-pointer
# parameter written inline), then `{` (a prototype ends in `;` and never
# matches this). DOTALL so a signature that wraps across lines is still
# matched as one span.
def _func_def_pattern(name):
    return re.compile(
        r'(?:^|\n)[ \t]*(?:static[ \t]+)?(?:inline[ \t]+)?(?:const[ \t]+)?'
        r'[A-Za-z_][\w \t\*]*?\b' + re.escape(name) + r'\s*\(([^;{}]*)\)\s*(?:\r?\n[ \t]*)?\{',
        re.DOTALL)


_ANY_FUNC_DEF = re.compile(
    r'(?:^|\n)[ \t]*(?:static[ \t]+)?(?:inline[ \t]+)?(?:const[ \t]+)?'
    r'[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*(?:\r?\n[ \t]*)?\{',
    re.DOTALL)


_COMMENT_OR_STRING_RE = re.compile(
    r'/\*.*?\*/'            # block comment
    r'|//[^\n]*'            # line comment
    r'|"(?:\\.|[^"\\\n])*"'  # string literal
    r"|'(?:\\.|[^'\\\n])*'",  # char literal
    re.DOTALL)


def _strip_comments_and_strings(text):
    """Blank out comments and string/char literals, preserving every newline
    (so byte positions used for enclosing-function lookup stay meaningful)
    and every other character's position (replaced with a space). Without
    this, a comment merely MENTIONING a call shape --
    "build_icon_named()'s comment" in ui_topbar.c, or a string literal
    quoting "lv_timer_create(" in a host test -- is read as a real
    registration/call site. Confirmed necessary: prior to this, plain-text
    mentions in comments produced spurious 0-argument "call sites" for
    build_icon_named/build_nav_item/ui_home_build_button/build_action_button.
    """
    def repl(m):
        return "".join(ch if ch == "\n" else " " for ch in m.group(0))
    return _COMMENT_OR_STRING_RE.sub(repl, text)


def _split_top_level_commas(s):
    parts = []
    depth = 0
    cur = []
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return parts


def _param_names(params_str):
    names = []
    for p in _split_top_level_commas(params_str):
        p = p.strip()
        if not p or p == "void" or p == "...":
            names.append(None)
            continue
        m = re.search(r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?$', p)
        names.append(m.group(1) if m else None)
    return names


def _extract_call_args(text, open_paren_pos):
    """text[open_paren_pos - 1] must be '('. Returns (arg_list, end_pos) or (None, None)."""
    depth = 1
    i = open_paren_pos
    n = len(text)
    while i < n and depth > 0:
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
        i += 1
    if depth != 0:
        return None, None
    inner = text[open_paren_pos:i - 1]
    return _split_top_level_commas(inner), i


class _Index:
    """Lazily-loaded, cached view of every *.c file under `root`, excluding
    `build/` and `test/` (host-test source, never linked into the ELF this
    checker measures -- see module docstring)."""

    def __init__(self, root):
        self.root = root
        self._files = None
        self._text = {}

    def files(self):
        if self._files is None:
            found = []
            for dirpath, dirs, files in os.walk(self.root):
                dirs[:] = [d for d in dirs if d not in ("build",)]
                rel = os.path.relpath(dirpath, self.root)
                parts = rel.split(os.sep)
                if parts and parts[0] == "test":
                    continue
                for fn in files:
                    if fn.endswith(".c"):
                        found.append(os.path.join(dirpath, fn))
            self._files = sorted(found)
        return self._files

    def text(self, path):
        if path not in self._text:
            with open(path, encoding="utf-8", errors="replace") as f:
                raw = f.read()
            self._text[path] = _strip_comments_and_strings(raw)
        return self._text[path]

    def find_function_defs(self, name):
        """[(file, params_str), ...] for every definition of `name` found."""
        pat = _func_def_pattern(name)
        hits = []
        for path in self.files():
            for m in pat.finditer(self.text(path)):
                hits.append((path, m.group(1)))
        return hits

    def enclosing_function(self, path, pos):
        """The (name, params_str, def_start, def_end) of the last function
        definition in `path` starting before `pos`, or None."""
        text = self.text(path)
        best = None
        for m in _ANY_FUNC_DEF.finditer(text):
            if m.start() <= pos:
                best = m
            else:
                break
        if best is None:
            return None
        return best.group(1), best.group(2), best.start(), best.end()

    def find_call_sites(self, name, exclude_spans_by_file):
        """[(file, pos_after_open_paren, args), ...] for every CALL of `name`
        (not its own definition) across the tree."""
        pat = re.compile(r'\b' + re.escape(name) + r'\s*\(')
        out = []
        for path in self.files():
            text = self.text(path)
            excl = exclude_spans_by_file.get(path, [])
            for m in pat.finditer(text):
                if any(s <= m.start() < e for s, e in excl):
                    continue
                args, end = _extract_call_args(text, m.end())
                if args is None:
                    continue
                out.append((path, m.start(), args))
        return out


def _classify(token):
    token = token.strip()
    if not token or token == "NULL":
        return None
    if IDENT_RE.match(token):
        return ("ident", token)
    return ("dynamic", token)


def resolve_callback(index, token, origin_file, origin_pos, depth, errors, notes, visited):
    """Resolve one callback token to a list of concrete (name, file) roots.

    Never raises; appends to `errors` (fatal) or `notes` (informational,
    non-fatal) and returns [] when nothing concrete can be named.
    """
    kind = _classify(token)
    if kind is None:
        return []
    if kind[0] == "dynamic":
        notes.append(
            f"{os.path.relpath(origin_file)}: callback argument `{token}` is not a bare "
            "function name (a struct field or other expression) -- this is a dynamic "
            "dispatch point this source scan cannot reduce to a concrete symbol, the same "
            "class of gap as lv_timer_handler()'s own internal function-pointer dispatch. "
            "Not added to extra_roots; not a FAIL on its own.")
        return []

    name = kind[1]
    # Keyed on (name, file, position), not just (name, file): the same
    # parameter NAME (e.g. "cb") legitimately denotes different, unrelated
    # parameters at different call sites in the same file (ui_topbar.c's
    # build_icon() and build_icon_named() both have their own "cb" -- see
    # module docstring). Keying on (name, file) alone collided the two and
    # silently stopped the walk one hop short, before cfg->prev_cb/next_cb/
    # add_cb were ever reached -- caught by deliberately checking that all
    # four of ui_topbar.c's forwarded fields were found, not just gear_cb.
    key = (name, origin_file, origin_pos)
    if key in visited:
        return []
    visited.add(key)
    if depth > MAX_RESOLVE_DEPTH:
        errors.append(f"resolving callback `{name}` (from {os.path.relpath(origin_file)}): "
                       f"exceeded MAX_RESOLVE_DEPTH={MAX_RESOLVE_DEPTH} levels of parameter "
                       "forwarding without reaching a function definition -- either a genuine "
                       "cycle or a chain deeper than this codebase has had before; widen "
                       "MAX_RESOLVE_DEPTH only after confirming it terminates.")
        return []

    defs = index.find_function_defs(name)
    if defs:
        chosen = [d for d in defs if d[0] == origin_file] or defs
        return [(name, f) for f, _params in chosen]

    # Not a function definition anywhere -- treat as a forwarded parameter.
    enc = index.enclosing_function(origin_file, origin_pos)
    if enc is None:
        errors.append(
            f"callback `{name}` used at {os.path.relpath(origin_file)} is neither a function "
            f"definition anywhere under {os.path.relpath(index.root)} nor inside a locatable "
            "enclosing function whose parameter it could be -- likely a rename or deletion left "
            "a registration site pointing at nothing.")
        return []
    enc_name, enc_params, enc_start, enc_end = enc
    if name not in _param_names(enc_params):
        errors.append(
            f"callback `{name}` at {os.path.relpath(origin_file)} does not match any function "
            f"definition, and the enclosing function `{enc_name}` there does not have a "
            f"parameter named `{name}` either -- cannot resolve.")
        return []

    idx = _param_names(enc_params).index(name)
    call_sites = index.find_call_sites(enc_name, {origin_file: [(enc_start, enc_end)]})
    if not call_sites:
        errors.append(
            f"callback parameter `{name}` of `{enc_name}` ({os.path.relpath(origin_file)}) has "
            "no locatable call site anywhere under the scanned tree -- cannot resolve what is "
            "actually passed for it.")
        return []

    resolved = []
    for call_file, call_pos, args in call_sites:
        if idx >= len(args):
            errors.append(
                f"call to `{enc_name}` at {os.path.relpath(call_file)} has only {len(args)} "
                f"argument(s), fewer than the {idx + 1} needed to read its `{name}` parameter.")
            continue
        resolved.extend(resolve_callback(index, args[idx], call_file, call_pos,
                                          depth + 1, errors, notes, visited))
    return resolved


def discover(app_dir):
    """Scan `app_dir` for every LVGL callback registration site.

    Returns (roots, errors, notes, raw_count):
      roots      -- deduplicated [(name, file), ...] resolved concrete callback
                     entry points, suitable for TASKS["lvgl"]["extra_roots"].
      errors     -- fatal: caller should FAIL the check if this is non-empty.
      notes      -- informational: dynamic-dispatch points found but not
                     resolvable to a symbol; printed, never silently dropped,
                     never fatal on their own.
      raw_count  -- total registration sites found before any resolution
                     attempt, for the caller's vacuity-floor check.
    """
    index = _Index(app_dir)
    errors = []
    notes = []
    roots = []
    seen_roots = set()
    raw_count = 0

    for path in index.files():
        text = index.text(path)
        for pat, label in REGISTRATION_PATTERNS:
            for m in pat.finditer(text):
                raw_count += 1
                token = m.group(1)
                visited = set()
                for name, rfile in resolve_callback(index, token, path, m.start(), 0,
                                                     errors, notes, visited):
                    rbase = os.path.basename(rfile)
                    rkey = (name, rbase)
                    if rkey not in seen_roots:
                        seen_roots.add(rkey)
                        roots.append((name, rbase))

    roots.sort()
    return roots, errors, notes, raw_count
