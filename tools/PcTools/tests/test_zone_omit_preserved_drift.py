"""Drift test: zones_http_client's omit-preserved key lists must match what
firmware/KilnFW/App/drivers/http/zones_http_post_parse.c really treats as
omit-preserves-current (review BENCHFX2 LOW-1).

The later-override scan is a cheap second line, not a C parser. It covers plain and compound
assignment, ++/--, and memset/memmove/strncpy (with or without `&`, a cast, or a space before
the paren), on these left-hand sides: `z->f`, spaced `z -> f`, `(z)->f`, `((z))->f`,
`(z + 0)->f`, `(*z).f` with any inner spacing, `z[k].f`, and an index after the field with
spaces or one level of nesting (`z->f [s[0]]`).

It does NOT see, and these remain UNCOVERED here (BENCHFX9 LOW-2, contrived forms, left as is):
`(&*z)->f`, `(*(z)).f`, `(&z[0])->f`, `(z + 0u)->f` / `(0 + z)->f` (only the literal `z + 0`),
`z[idx[0]].f` (nested index inside the brackets before the field), `memset(&(z->f), ..)`,
`memset((uint8_t *)(z->f), ..)`, `memset(z, 0, sizeof *z)`, pointer aliases (`zp = z; zp->f = ..`,
`float *hp = &z->f; *hp = ..`), whole-struct assignment (`*z = tmp;` or via a helper), compound literals
(`*z = (zone_cfg_t){...}`), ternary lvalues (`*(c ? &z->f : &x) = ..`), writes through a macro,
memcpy or any other helper that is given `&z->f`, a two-level index, and a conditional default
nested inside the omit `else` itself. The backstop for all of those is the per-field
omit-preserve assertions in firmware/KilnFW/App/test/test_zones_http.c (BENCHFX7 review LOW-1,
BENCHFX8 review LOW-2)."""
import re
import unittest
from pathlib import Path

from kilnctrl import zones_http_client as zhc

_SRC = (Path(__file__).resolve().parents[3]
        / "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c")
_KEY_RE = re.compile(r'snprintf\(\s*g?key,\s*sizeof\(g?key\),\s*"z%u_([a-z_0-9]+?)(?:%u)?"')
_PRESERVE_RE = re.compile(r"zones_config_json_field_present\(body, key\)|zones_http_field_nonblank\(body, key\)")


# The omit branch must be EXACTLY `else { z-><f> = current_z-><f>; }` (same field and same
# optional [index] on both sides; a trailing `continue;` is allowed). Comments are stripped
# first, so a constant with the field named in a comment no longer counts (review BENCHFX4).
_EXACT_ELSE_RE = re.compile(
    r"\belse\s*\{\s*z->(\w+)((?:\[\w+\])?)\s*=\s*current_z->(\w+)((?:\[\w+\])?)\s*;\s*(?:continue;\s*)?\}")
# Fields legitimately assigned again elsewhere, mapped to the ONLY right-hand side allowed
# (review BENCHFX5 LOW-1): tc_type is restored from preserved_tc_type after the
# `*z = *current_z` bulk copy of the i >= thermo_count early return; settings_source[g] takes
# src_raw in the gkey loop (its else-if on have_legacy is legitimate, so the else-if rule skips it);
# tc_type_raw is the parsed value on the present branch. Any other assignment to these fields is still a violation.
_ALLOWED_RHS = {"settings_source": {"src_raw"}, "tc_type": {"preserved_tc_type", "tc_type_raw"}}


# Left-hand-side shapes the scan understands: `z->`, `(z)->`, `((z))->`, `(z + 0)->`,
# `( * z ).`, `z[k].`; optional spaced and one-level-nested `[index]` after the field.
_Z_DEREF = (r"(?:(?:\bz|\(+\s*z\s*\)+|\(\s*z\s*\+\s*0\s*\))\s*->"
            r"|\(\s*\*\s*z\s*\)\s*\."
            r"|\bz\s*\[[^\]]*\]\s*\.)")
_IDX = r"(?:\s*\[(?:[^\[\]]|\[[^\]]*\])*\])?"


def _js_function_body(text: str, name: str):
    """Brace-balanced body of `function <name>(...) {...}` (skips strings and regex literals
    only crudely: none of the pinned function's body holds an unbalanced brace)."""
    m = re.search(r"function\s+" + re.escape(name) + r"\b[^{]*\{", text)
    if not m:
        return None
    depth = 0
    for i in range(m.end() - 1, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[m.end():i]
    return None


def strip_c_comments(text: str) -> str:
    """Remove /* */ and // comments, keeping newlines so line numbers survive."""
    return re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)


def _classify_full(text: str) -> "tuple[set[str], set[str], list[str]]":
    """(preserved, defaulted, violations). A key is a candidate when a field_present /
    field_nonblank call follows its snprintf within 4 lines. It is omit-PRESERVED only if
    its block (up to the next key snprintf) has an exact same-field `else` copy from
    current_z; otherwise omit-DEFAULTED. violations lists any later plain assignment to the
    same field after that else. coupling_c%u is normalised to coupling_c; gkey sub-keys are
    ignored."""
    whole = strip_c_comments(text)
    lines = whole.splitlines()
    fields_to_check = []
    starts = [n for n, line in enumerate(lines) if _KEY_RE.search(line)]
    preserved, defaulted, violations = set(), set(), []
    for idx, n in enumerate(starts):
        end = starts[idx + 1] if idx + 1 < len(starts) else len(lines)
        if not _PRESERVE_RE.search(chr(10).join(lines[n + 1:n + 5])):
            continue
        name = _KEY_RE.search(lines[n]).group(1)
        name = "coupling_c" if name.startswith("coupling_c") else name
        block = chr(10).join(lines[n + 1:end])
        exact = [m for m in _EXACT_ELSE_RE.finditer(block)
                 if m.group(1) == m.group(3) and m.group(2) == m.group(4)]
        if not exact:
            defaulted.add(name)
            continue
        preserved.add(name)
        field = exact[-1].group(1)
        block_start = len(chr(10).join(lines[:n + 1])) + 1
        em = exact[-1]
        span = (block_start + em.start(), block_start + em.end())
        # `else if` chained onto this key's presence check (conditional default, BENCHFX5 X1)
        # exempt fields may chain only the one literal `else if (have_legacy)` arm
        elifs = re.findall(r"\belse\s+if\s*\(([^)]*)\)", block[:em.start()])
        if any(c.strip() != "have_legacy" for c in elifs) or (elifs and field not in _ALLOWED_RHS):
            violations.append(f"{name}: `else if` in the omit branch chain")
        fields_to_check.append((name, field, span, block_start))
    for name, field, span, bstart in fields_to_check:
        lhs = _Z_DEREF + r"\s*" + re.escape(field) + _IDX
        # Real compound operators only: `>=` / `<=` comparisons are not assignments (LOW-2).
        pat = re.compile(lhs + r"\s*((?:<<|>>|[-+*/%&|^])?)=(?!=)\s*([^;]*);")
        incdec = re.compile(r"(?:\+\+|--)\s*" + lhs + "|" + lhs + r"\s*(?:\+\+|--)")
        for m in pat.finditer(whole):
            if span[0] <= m.start() < span[1]:
                continue
            where = "after" if m.start() >= span[0] else "before"
            # The key's own present branch (block start .. omit else) assigns the parsed value;
            # that is legitimate unless it is conditioned on current_z (BENCHFX6 Y6).
            if (where == "before" and m.start() >= bstart
                    and "current_z" not in whole[bstart:m.start()]
                    and not m.group(1) and field not in _ALLOWED_RHS):
                continue
            rhs = m.group(2).strip()
            if (not m.group(1) and re.fullmatch(r"[A-Za-z_]\w*", rhs)
                    and rhs in _ALLOWED_RHS.get(field, ())):
                continue
            kind = "compound assignment" if m.group(1) else "assignment"
            violations.append(f"{name}: {kind} to z->{field} {where} the omit branch (rhs {rhs!r})")
        for m in incdec.finditer(whole):
            if not (span[0] <= m.start() < span[1]):
                violations.append(f"{name}: ++/-- on z->{field} outside the omit branch")
        for m in re.finditer(r"\b(?:memset|memmove|strncpy)\s*\(\s*(?:\([^)]*\)\s*)?&?\s*" + lhs, whole):
            if not (span[0] <= m.start() < span[1]):
                violations.append(f"{name}: memset/memmove/strncpy of z->{field} outside the omit branch")
    return preserved, defaulted, violations


def _classify(text: str) -> "tuple[set[str], set[str]]":
    return _classify_full(text)[:2]


def firmware_omit_preserved_keys(text: str) -> "set[str]":
    return _classify(text)[0]


def firmware_omit_defaulted_keys(text: str) -> "set[str]":
    return _classify(text)[1]


def python_omit_preserved_keys() -> "set[str]":
    return (set(zhc.ZONE_OMIT_PRESERVED_FLOAT_KEYS) | set(zhc.ZONE_OMIT_PRESERVED_NONFLOAT_KEYS)
            | {"coupling_c"})


class ZoneOmitPreservedDriftTest(unittest.TestCase):
    def test_firmware_source_present_and_parsed(self):
        self.assertTrue(_SRC.is_file(), _SRC)  # never skip: a moved file must fail loudly
        self.assertGreater(len(firmware_omit_preserved_keys(_SRC.read_text(encoding="utf-8"))), 25)

    def test_python_lists_match_firmware(self):
        fw = firmware_omit_preserved_keys(_SRC.read_text(encoding="utf-8"))
        py = python_omit_preserved_keys()
        self.assertEqual(fw - py, set(), "omit-preserved in firmware but unlisted in Python")
        self.assertEqual(py - fw, set(), "listed in Python but not omit-preserved in firmware")

    def test_defaulted_keys_match_firmware_and_are_never_stripped(self):
        text = _SRC.read_text(encoding="utf-8")
        self.assertEqual(firmware_omit_defaulted_keys(text), set(zhc.ZONE_OMIT_DEFAULTED_KEYS))
        self.assertEqual(set(zhc.ZONE_OMIT_DEFAULTED_KEYS) & set(zhc.ZONE_OMIT_PRESERVED_FLOAT_KEYS), set())
        self.assertEqual(set(zhc.ZONE_OMIT_DEFAULTED_KEYS) & set(zhc.ZONE_OMIT_PRESERVED_NONFLOAT_KEYS), set())
        for k in tuple(zhc.ZONE_OMIT_DEFAULTED_KEYS) + tuple(zhc.ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS):
            self.assertFalse(zhc.ZONE_OMIT_PRESERVED_KEY_RE.match(f"z1_{k}"), k)
            body = f"z1_{k}=3&z1_kp=1"
            self.assertIn(f"z1_{k}", zhc.strip_omit_preserved(body), k)

    def test_omit_branch_must_assign_from_current(self):
        def blk(name, assign):
            return [f'snprintf(key, sizeof(key), "z%u_{name}", i);', "{",
                    " if (zones_config_json_field_present(body, key)) { x = 1; }",
                    " else {", f" {assign}", " }", "}"]
        src = chr(10).join(blk("foo", "z->foo = 0.0f;") + blk("bar", "z->bar = current_z->bar;"))
        self.assertEqual(firmware_omit_preserved_keys(src), {"bar"})
        self.assertEqual(firmware_omit_defaulted_keys(src), {"foo"})

    def test_omit_branch_exact_same_field_only(self):
        def blk(name, body):
            return chr(10).join([f'snprintf(key, sizeof(key), "z%u_{name}", i);',
                                 "if (zones_config_json_field_present(body, key)) { x = 1; }", body])
        def cls(body):
            return _classify_full(blk("foo", body))
        ok = "else { z->foo = current_z->foo; }"
        self.assertEqual(cls(ok), ({"foo"}, set(), []))
        # wrong field copied (review BENCHFX4 MED-1)
        self.assertEqual(cls("else { z->foo = current_z->other; }")[:2], (set(), {"foo"}))
        # name only in a comment
        self.assertEqual(cls("else { z->foo = 0.0f; /* current_z->foo */ }")[:2], (set(), {"foo"}))
        self.assertEqual(cls("else { z->foo = 0.0f; // z->foo = current_z->foo;" + chr(10) + "}")[:2], (set(), {"foo"}))
        # conditional preserve and current_z-dependent default
        self.assertEqual(cls("else { if (current_z->t == 1u) { z->foo = current_z->foo; } else { z->foo = 0.0f; } }")[:2],
                         (set(), {"foo"}))
        self.assertEqual(cls("else { z->foo = (current_z->t == 1u) ? 2.0f : 0.0f; }")[:2], (set(), {"foo"}))
        # post-branch override of the same field
        self.assertEqual(len(cls(ok + chr(10) + "z->foo = 0.0f;")[2]), 1)
        self.assertEqual(cls(ok + chr(10) + "if (z->foo == 1.0f) { y = 1; }")[2], [])
        # BENCHFX5: else-if default, compound assignment, override in a later block
        self.assertTrue(any("else if" in v for v in cls("else if (current_z->t == 0u) { z->foo = 0.0f; } " + ok)[2]))
        self.assertEqual(len(cls(ok + chr(10) + "z->foo *= 0.0f;")[2]), 1)
        later = (blk("foo", ok) + chr(10) + 'snprintf(key, sizeof(key), "z%u_bar", i);' + chr(10)
                 + "if (zones_config_json_field_present(body, key)) { x = 1; }" + chr(10)
                 + "else { z->bar = current_z->bar; }" + chr(10) + "z->foo = 0.0f;")
        self.assertEqual(len(_classify_full(later)[2]), 1)
        # BENCHFX5 LOW-1: exempt fields only allow their known right-hand side
        tc = blk("tc_type", "else { z->tc_type = current_z->tc_type; }")
        self.assertEqual(_classify_full(tc + chr(10) + "z->tc_type = preserved_tc_type;")[2], [])
        self.assertEqual(len(_classify_full(tc + chr(10) + "z->tc_type = 5u;")[2]), 1)
        # BENCHFX6 LOW-1/LOW-2: escape shapes and per-rule self-tests
        self.assertEqual(len(_classify_full(tc + chr(10) + "z->tc_type = preserved_tc_type & 0u;")[2]), 1)  # Y1
        self.assertEqual(len(_classify_full(chr(10).join(
            [f'snprintf(key, sizeof(key), "z%u_tc_type", i);',
             "if (zones_config_json_field_present(body, key)) { x = 1; }",
             "else if (g == 3) { src = 1; }", "else { z->tc_type = current_z->tc_type; }"]))[2]), 1)  # Y2
        self.assertEqual(_classify_full(chr(10).join(
            ['snprintf(key, sizeof(key), "z%u_tc_type", i);',
             "if (zones_config_json_field_present(body, key)) { x = 1; }",
             "else if (have_legacy) { src = 1; }", "else { z->tc_type = current_z->tc_type; }"]))[2], [])
        self.assertEqual(len(cls(ok + chr(10) + "memset(&z->foo, 0, sizeof(z->foo));")[2]), 1)  # Y3
        self.assertEqual(len(cls(ok + chr(10) + "(*z).foo = 0.0f;")[2]), 1)  # Y4
        self.assertEqual(len(cls("else { z->foo[j] = current_z->foo[j]; }" + chr(10)
                                 + "z->foo[(i + 1u) % N] = 0.0f;")[2]), 1)  # Y5
        self.assertEqual(len(cls("if (current_z->t == 0u) { z->foo = 0.0f; } else {" + chr(10) + "z->foo = 2.0f; }" + chr(10) + ok)[2]), 2)  # Y6 conditioned before span
        self.assertEqual(cls("z->foo = parsed;" + chr(10) + ok)[2], [])  # the present branch is fine
        self.assertEqual(len(cls("z->foo *= 0.0f;" + chr(10) + ok)[2]), 1)  # P1 compound before span
        self.assertEqual(len(_classify_full("z->tc_type = other;" + chr(10) + tc)[2]), 1)  # P4
        self.assertEqual(len(_classify_full("z->tc_type *= preserved_tc_type;" + chr(10) + tc)[2]), 1)  # P1
        self.assertEqual(len(_classify_full(tc + chr(10) + "z->tc_type *= preserved_tc_type;")[2]), 1)  # P1
        self.assertEqual(len(_classify_full("z->foo = 0.0f;" + chr(10) + blk("foo", ok))[2]), 1)  # P4 outside own block
        self.assertEqual(len(_classify_full(tc + chr(10) + "z->tc_type = other_ident;")[2]), 1)  # P6
        # BENCHFX7 LOW-2: comparisons are not compound assignments
        self.assertEqual(cls(ok + chr(10) + "if (!(z->foo == 0.0f || z->foo >= 1.0f)) { y = 1; }")[2], [])
        self.assertEqual(cls(ok + chr(10) + "if (z->foo <= 1.0f) { y = 1; }")[2], [])
        # BENCHFX7 LOW-3 S1: every real compound operator (the old class missed ^= %= <<= >>=)
        for op in ("^=", "%=", "<<=", ">>=", "|=", "&="):
            self.assertEqual(len(cls(ok + chr(10) + f"z->foo {op} 1u;")[2]), 1, op)
        # BENCHFX7 LOW-3 S6: a non-conditioned present-branch write to an exempt field still needs its allowed RHS
        self.assertEqual(len(_classify_full(chr(10).join(
            ['snprintf(key, sizeof(key), "z%u_tc_type", i);', "z->tc_type = other;",
             "if (zones_config_json_field_present(body, key)) { x = 1; }",
             "else { z->tc_type = current_z->tc_type; }"]))[2]), 1)
        # BENCHFX7 LOW-3 S11: an else-if arm on a non-exempt field is a violation even for have_legacy
        self.assertEqual(len(_classify_full(chr(10).join(
            ['snprintf(key, sizeof(key), "z%u_foo", i);',
             "if (zones_config_json_field_present(body, key)) { x = 1; }",
             "else if (have_legacy) { y = 1; }", "else { z->foo = current_z->foo; }"]))[2]), 1)
        # BENCHFX7 LOW-1: ++/--, spaced and parenthesised deref, memset without &
        for stmt in ("z->foo++;", "--z->foo;", "z -> foo = 0.0f;", "(z)->foo = 0.0f;",
                     "memset(z->foo, 0, sizeof(z->foo));", "memset(&z->foo, 0, 4);"):
            self.assertEqual(len(cls(ok + chr(10) + stmt)[2]), 1, stmt)
        # BENCHFX8 LOW-1: one self-test per spacing/parenthesis form
        for stmt in ("z->foo [1] = 0.0f;", "( *z ).foo = 0.0f;", "(* z).foo = 0.0f;",
                     "((z))->foo = 0.0f;", "(z + 0)->foo = 0.0f;", "z->foo[s[0]] = 0.0f;",
                     "memset (z->foo, 0, 4);", "memset((void *)z->foo, 0, 4);",
                     "memset ((void *) &z->foo, 0, 4);",
                     # BENCHFX8 LOW-2 (cheap extras): indexed z and memmove/strncpy
                     "z[0].foo = 0.0f;", "memmove(z->foo, src, 4);", "strncpy(z->foo, src, 4);"):
            self.assertEqual(len(cls(ok + chr(10) + stmt)[2]), 1, stmt)
        # and none of them is a false positive when it is only read or compared
        self.assertEqual(cls(ok + chr(10) + "if (z->foo [1] == 0.0f && ( *z ).foo >= 1.0f) { y = 1; }")[2], [])
        # indexed form must match on both sides
        self.assertEqual(cls("else { z->foo[j] = current_z->foo[j]; }")[0], {"foo"})
        self.assertEqual(cls("else { z->foo[j] = current_z->foo[k]; }")[1], {"foo"})

    def test_firmware_has_no_later_overrides(self):
        self.assertEqual(_classify_full(_SRC.read_text(encoding="utf-8"))[2], [])

    def test_legacy_settings_source_key_classified_by_its_own_gkey_else(self):
        text = strip_c_comments(_SRC.read_text(encoding="utf-8"))
        self.assertIn("settings_source", firmware_omit_preserved_keys(_SRC.read_text(encoding="utf-8")))
        i = text.index('"z%u_settings_source", i);')
        j = text.index('"z%u_settings_source_%s"', i)
        k = text.index("}", text.index("else {", text.index("have_legacy)", j)))
        m = _EXACT_ELSE_RE.search(text[j:k + 1 + 80])
        self.assertIsNotNone(m, "the gkey loop's final else must copy current_z->settings_source[g]")
        self.assertEqual(m.group(1), "settings_source")
        self.assertEqual((m.group(1), m.group(2)), (m.group(3), m.group(4)))
        # refactor of that else changes the classification
        mutated = text.replace("z->settings_source[g] = current_z->settings_source[g];",
                               "z->settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;")
        self.assertNotIn("settings_source", _classify(mutated)[0])

    def test_regex_matches_exactly_the_float_keys(self):
        for k in zhc.ZONE_OMIT_PRESERVED_FLOAT_KEYS:
            self.assertTrue(zhc.ZONE_OMIT_PRESERVED_KEY_RE.match(f"z1_{k}"), k)
        self.assertTrue(zhc.ZONE_OMIT_PRESERVED_KEY_RE.match("z2_coupling_c0"))
        for k in zhc.ZONE_OMIT_PRESERVED_NONFLOAT_KEYS:
            self.assertFalse(zhc.ZONE_OMIT_PRESERVED_KEY_RE.match(f"z1_{k}"), k)
        for k in ("kp", "ki", "kd", "maxtemp", "mintemp", "relay_mask", "cal", "ramp", "sanity",
                  "mode", "window", "minon", "minoff", "timingprofile", "name"):
            self.assertFalse(zhc.ZONE_OMIT_PRESERVED_KEY_RE.match(f"z1_{k}"), k)

    def test_page_optional_key_regex_pinned_to_preserved_keys(self):
        """zones_page.html's ZONE_OPTIONAL_KEY_RE may only drop a blank for keys the firmware
        omit-PRESERVES (BENCHFX7 LOW-4). Adding an omit-defaulted (ct_mask, thermo_mask) or a
        required key must fail here."""
        page = (_SRC.parent / "zones_page.html").read_text(encoding="utf-8")
        m = re.search(r"var ZONE_OPTIONAL_KEY_RE = /\^z\\d\+_\((.*?)\)=\\s\*\$/;", page)
        self.assertIsNotNone(m, "ZONE_OPTIONAL_KEY_RE not found in zones_page.html")
        keys = {("coupling_c" if k.startswith("coupling_c") else k) for k in m.group(1).split("|")}
        self.assertGreater(len(keys), 15)
        fw = firmware_omit_preserved_keys(_SRC.read_text(encoding="utf-8"))
        self.assertEqual(keys - fw, set(), "page drops a blank for a key the firmware does not omit-preserve")
        # (The old `keys & ZONE_OMIT_DEFAULTED_KEYS` assertion was removed: the subset check
        # above already implies it, since a defaulted key can never be in `fw`.)
        # BENCHFX8 INFO-2: a second filter or a reassignment of the regex would bypass the pin.
        # `=` not followed by `=`, so `===`/`==` comparisons do not count (BENCHFX9 INFO-3).
        self.assertEqual(len(re.findall(r"\bZONE_OPTIONAL_KEY_RE\s*=(?!=)", page)), 1,
                         "ZONE_OPTIONAL_KEY_RE must be assigned exactly once")
        body = _js_function_body(page, "omitBlankOptionalParams")
        self.assertIsNotNone(body, "omitBlankOptionalParams not found in zones_page.html")
        # BENCHFX9 LOW-1: exactly one regex call, and it is ZONE_OPTIONAL_KEY_RE.test(...).
        self.assertEqual(len(re.findall(r"\.\s*test\s*\(", body)), 1,
                         "omitBlankOptionalParams must filter with exactly one regex test")
        self.assertEqual(len(re.findall(r"\bZONE_OPTIONAL_KEY_RE\s*\.\s*test\s*\(", body)), 1,
                         "the single test must be ZONE_OPTIONAL_KEY_RE.test(")
        self.assertIsNone(re.search(r"\.\s*(?:match|compile|exec|replace|search)\s*\(|/\^", body),
                          "omitBlankOptionalParams must not use another regex or call")
        self.assertEqual(len(re.findall(r"\.\s*filter\s*\(", body)), 1, "exactly one .filter in the function")
        self.assertEqual(len(re.findall(r"\bZONE_OPTIONAL_KEY_RE\s*\.\s*(?:compile|lastIndex)", page)), 0,
                         "ZONE_OPTIONAL_KEY_RE must not be rewritten in place")
        # Call site (BENCHFX10 LOW-1): every use of `params` in the Save handler, from its declaration to
        # the POST, must be an allowed form; anything else (a filter, an alias, pop/splice/length, a second
        # assignment, a filtered body:) could drop a blank mask entry from the real POST body. The node test
        # (test_host_refusal_pages.js) also runs the real fetch tail against a mocked fetch.
        self.assertEqual(len(re.findall(r"=\s*omitBlankOptionalParams\s*\(\s*params\s*\)", page)), 1,
                         "exactly one `params = omitBlankOptionalParams(params)` call")
        self.assertEqual(len(re.findall(r"fetch\(\s*['\"]/api/zones['\"]\s*,\s*\{\s*method:\s*'POST'", page)), 1,
                         "exactly one POST to /api/zones in the page")
        h0 = page.index("document.getElementById('saveBtn').addEventListener('click'")
        h1 = page.index("}).then(", page.index("fetch('/api/zones'", h0))
        rest = page[h0:h1]
        for allowed, expect in ((r"var params = \[", 1), (r"\bparams\.push\(", None),
                                (r"blankGuardLabels\(params, guardFieldHidden\)", 1),
                                (r"\bparams = omitBlankOptionalParams\(params\);", 1),
                                (r"body: params\.join\('&'\),", 1)):
            if expect is not None:
                self.assertEqual(len(re.findall(allowed, rest)), expect, f"Save handler: expected {expect} x {allowed}")
            rest = re.sub(allowed, "", rest)
        self.assertIsNone(re.search(r"\bparams\b", rest),
                          "Save handler uses `params` other than push / declaration / blankGuardLabels / the one "
                          "omitBlankOptionalParams call / body: params.join('&')")
        self.assertIsNone(re.search(r"\b(?:blankGuardLabels|omitBlankOptionalParams)\s*=(?!=)", page),
                          "helpers must not be reassigned")
        # BENCHFX10 INFO-1: no other mention of the regex (window[...] swaps) outside its declaration and
        # omitBlankOptionalParams; comments excluded.
        code = re.sub(r"/\*.*?\*/", "", page, flags=re.S)
        code = code.replace(_js_function_body(page, "omitBlankOptionalParams") or "\0", "")
        self.assertEqual(len(re.findall(r"ZONE_OPTIONAL_KEY_RE", code)), 1,
                         "ZONE_OPTIONAL_KEY_RE must appear only in its declaration and omitBlankOptionalParams")

    def test_guard_keys_stripped_from_body(self):
        body = "z0_runawaymargin=12.2&z0_xzone=5.0&z0_debounce=3&z0_kp=1&z0_zonetype=1"
        out = zhc.strip_omit_preserved(body)
        self.assertEqual(out, "z0_kp=1&z0_zonetype=1")
        kept = zhc.strip_omit_preserved(body, {"z0_runawaymargin"})
        self.assertIn("z0_runawaymargin", kept)


if __name__ == "__main__":
    unittest.main()
