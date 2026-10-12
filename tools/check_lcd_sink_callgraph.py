#!/usr/bin/env python3
"""check_lcd_sink_callgraph.py -- REVIEW_LCDFX9 HIGH-2/MED-3 structural gate.

Text rules in check_lcd_admin_gates.ps1 cannot see wrappers, macros, ## paste or
function pointers. This check works on the built objects instead: it reads the
relocations (xtensa objdump -r) of every .c.obj, builds a function-level reverse
reference graph, and walks it backwards from the LCD trip-clear sinks.

  * Sinks: dashboard_safety_clear_trip, safety_link_send_clear_trip.
  * Any function, static table or callback in a drivers/ui object that can reach a
    sink FAILS, except ui_page_safety.c's clear_action, which may be referenced
    only by btn_clicked_cb (the ADMIN gate edge; the text check pins that body).
  * Direct callers of a sink outside drivers/ui must be in NON_UI_ALLOW (web/HTTP/
    link plumbing); a new one forces a review.

Function pointers, wrappers and macros all appear as relocations, so they are seen.
Limit: names are merged across objects for global symbols; a static of the same
name elsewhere can only over-report (never hide a path).
Exit 0 pass, 1 fail, 3 SKIP (no build objects) -- SKIP-FAST when KILNCTL_CHECKS_FAST.
"""
import argparse, os, re, subprocess, sys, glob

SINKS = {"dashboard_safety_clear_trip", "safety_link_send_clear_trip"}
GATE_OBJ = "ui_page_safety.c.obj"
GATE_FN = "clear_action"
GATE_CALLER = "btn_clicked_cb"
NON_UI_ALLOW = [
    "dashboard_exec_http.c", "uart_bridge_safety.c", "sw_reset_http.c",
    "safety_link_",
]
OBJDUMP_GLOB = r"C:\Espressif\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin\xtensa-esp32s3-elf-objdump.exe"
SEC = re.compile(r"^RELOCATION RECORDS FOR \[(\.[^\]]+)\]:")
KIND_PFX = re.compile(r"^\.(?:text|literal|rodata|data|bss|sdata|srodata|iram\d*|dram\d*|flash\.text|flash\.rodata|rtc\.\w+)\.(.+)$")
SYM = re.compile(r"^[0-9a-fA-F]+\s+.*?\s(\.\S+)\s+[0-9a-fA-F]+\s+(\S+)$")
REL = re.compile(r"^[0-9a-fA-F]+\s+\S+\s+(\S+?)(?:[+-]0x[0-9a-fA-F]+)?\s*$")
LOCAL_SEC = re.compile(r"^\.(?:text|literal|rodata|data|bss|sdata|srodata)\.(.+?)(?:\.\d+)?$")


def parse_syms(sym_text):
    """objdump -t text -> {section name: function/object symbol defined in it}."""
    m = {}
    for line in sym_text.splitlines():
        sm = SYM.match(line.strip())
        if sm and not sm.group(2).startswith("."):
            lst = m.setdefault(sm.group(1), [])  # REVIEW_LCDFX11 LOW-2: keep every symbol of the section
            if sm.group(2) not in lst:
                lst.append(sm.group(2))
    return m


def node_name(sec, secmap, unattrib, strict=False):
    """Section name -> node name. A symbol-table hit wins (e.g. .iram1.5 -> the function)."""
    if secmap and sec in secmap:
        names = secmap[sec]
        if strict and len(names) > 1:
            # Only for UI objects (non-UI objects legitimately share sections: aliases, $part$0 clones, ICF).
            # Two symbols share one section (e.g. -fno-toplevel-reorder, hand-written section attributes): relocations
            # cannot be split between them, so a hidden second function would inherit the first one's edges. Fail closed.
            unattrib.append(sec + " (shared by " + ", ".join(names) + ")")
        return names[0]
    lm = LOCAL_SEC.match(sec)
    if lm:
        return lm.group(1)
    km = KIND_PFX.match(sec)
    if km:
        n = km.group(1)
        n = re.sub(r"\.\d+$", "", n)
        if n.isdigit():
            unattrib.append(sec)
        return n
    return sec  # plain .rodata/.data/...: its own node, never the previous section


def parse(dump_text, objkey, rev, defined=None, secmap=None, unattrib=None):
    unattrib = unattrib if unattrib is not None else []
    cur = None
    ui = is_ui(objkey + "::")
    for line in dump_text.splitlines():
        m = SEC.match(line)
        if m:
            sec = m.group(1)
            if sec.startswith((".xt.", ".debug", ".comment", ".note", ".xtensa")):  # metadata/debug info, not code
                cur = None
                continue
            cur = f"{objkey}::{node_name(sec, secmap, unattrib, ui)}"
            if defined is not None:
                defined.add(cur)
            continue
        if line.startswith("RELOCATION RECORDS"):
            cur = None
            continue
        if cur is None:
            continue
        m = REL.match(line.strip())
        if not m:
            continue
        val = m.group(1)
        if val.startswith("."):
            tgt = f"{objkey}::{node_name(val, secmap, unattrib, ui)}"
        else:
            tgt = val
        if tgt != cur:
            rev.setdefault(tgt, set()).add(cur)


def is_ui(node):
    return "/ui/" in node.split("::")[0]


def analyse(rev, defined=None):
    fails, notes = [], []
    # REVIEW_LCDFX10 MED-3: renaming/removing a sink or the gate edge must fail, not turn the check into a no-op.
    if defined is not None:
        defs = {n.partition("::")[2] for n in defined}
        for s in sorted(SINKS):
            if s not in defs:
                fails.append(f"sink {s} has no defining section in the scanned objects (renamed or removed? update SINKS)")
            if not rev.get(s):
                fails.append(f"sink {s} has no referrers (renamed or removed? update SINKS)")
    gate_nodes = [n for n in rev if n.partition("::")[2] == GATE_FN and os.path.basename(n.partition("::")[0]) == GATE_OBJ]
    edge = any(os.path.basename(c.partition("::")[0]) == GATE_OBJ and c.partition("::")[2] == GATE_CALLER
               for g in gate_nodes for c in rev.get(g, ()))
    if not edge:
        fails.append(f"gate edge {GATE_OBJ}::{GATE_CALLER} -> {GATE_FN} not found")
    if not any(any(r.partition("::")[2] == GATE_FN and os.path.basename(r.partition("::")[0]) == GATE_OBJ
                   for r in rev.get(s, ())) for s in SINKS):
        fails.append(f"{GATE_OBJ}::{GATE_FN} does not reference a trip-clear sink (gate renamed?)")
    seen, work = set(), []
    for s in SINKS:
        for c in rev.get(s, ()):
            work.append((c, s))
    # direct-caller allowlist outside ui
    for s in SINKS:
        for c in rev.get(s, ()):
            if not is_ui(c):
                f = os.path.basename(c.split("::")[0])
                if not any(a in f for a in NON_UI_ALLOW):
                    fails.append(f"unreviewed non-UI caller of {s}: {c}")
    while work:
        node, via = work.pop()
        if node in seen:
            continue
        seen.add(node)
        obj, _, fn = node.partition("::")
        if is_ui(node):
            if os.path.basename(obj) == GATE_OBJ and fn == GATE_FN:
                # MED-2: exact qualified match (object AND function); a bare-name (global) reference is never allowed.
                bad = [c for c in rev.get(node, ())
                       if not (os.path.basename(c.partition("::")[0]) == GATE_OBJ and c.partition("::")[2] == GATE_CALLER)]
                bad += [f"(global reference by bare name) {c}" for c in rev.get(fn, ())]
                for c in bad:
                    fails.append(f"{c} references the gated clear action (only {GATE_OBJ}::{GATE_CALLER} may)")
                continue  # gate edge: stop here
            fails.append(f"LCD UI code reaches a trip-clear sink: {node} (via {via})")
        for c in rev.get(node, ()):
            work.append((c, node))
        for c in rev.get(fn, ()):  # global symbol referenced by bare name
            work.append((c, node))
    return fails


def objkey_of(p):
    q = p.replace("\\", "/")
    comp = q.split("/esp-idf/", 1)[1].split("/", 1)[0]
    return comp + "/" + q.rsplit(".dir/", 1)[1]


def find_objdump():
    c = sorted(glob.glob(OBJDUMP_GLOB))
    return c[-1] if c else "xtensa-esp32s3-elf-objdump"


def skip(msg):
    if os.environ.get("KILNCTL_CHECKS_FAST"):
        print("SKIP-FAST: " + msg)
    else:
        print("SKIP: " + msg)
    return 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir")
    ap.add_argument("--sym-dump", action="append", default=[],
                    help="OBJKEY=FILE: pre-dumped objdump -t text (test mode; section -> symbol map)")
    ap.add_argument("--reloc-dump", action="append", default=[],
                    help="OBJKEY=FILE: pre-dumped objdump -r text (test mode)")
    a = ap.parse_args()
    rev, defined, unattrib = {}, set(), []
    if a.reloc_dump:
        syms = {}
        for spec in a.sym_dump:
            k, f = spec.split("=", 1)
            syms[k] = parse_syms(open(f, encoding="utf-8").read())
        for spec in a.reloc_dump:
            k, f = spec.split("=", 1)
            parse(open(f, encoding="utf-8").read(), k, rev, defined, syms.get(k), unattrib)
    else:
        root = a.build_dir or os.environ.get("KILNCTL_KILNFW_BUILD") or os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "firmware", "KilnFW", "build")
        objs = [p for p in glob.glob(os.path.join(root, "**", "*.c.obj"), recursive=True)
                if ".dir/" in p.replace("\\", "/")
                and "/esp-idf/" in p.replace("\\", "/")]
        if not any(os.path.basename(p) == GATE_OBJ for p in objs):
            return skip(f"no {GATE_OBJ} under {root} (build not present)")
        od = find_objdump()
        def dump(p):
            r = subprocess.run([od, "-r", p], capture_output=True, text=True)
            t = subprocess.run([od, "-t", p], capture_output=True, text=True) if r.returncode == 0 else None
            return p, r, t
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=min(16, (os.cpu_count() or 4))) as ex:
            results = list(ex.map(dump, objs))
        for p, r, t in results:
            if r.returncode != 0:
                print(f"FAIL: objdump failed on {p}: {r.stderr.strip()[:200]}")
                return 1
            parse(r.stdout, objkey_of(p), rev, defined, parse_syms(t.stdout), unattrib)
    fails = analyse(rev, defined)
    for u in sorted(set(unattrib)):
        fails.append(f"relocation section {u} cannot be attributed to a single function (no symbol-table entry, or several symbols share it)")
    if fails:
        print("LCD sink call-graph check FAILED:")
        for f in sorted(set(fails)):
            print("  " + f)
        return 1
    print(f"LCD sink call-graph check passed ({len(rev)} referenced symbols).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
