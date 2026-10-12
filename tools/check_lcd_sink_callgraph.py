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
SEC = re.compile(r"^RELOCATION RECORDS FOR \[(\.[a-z_]+)\.(.+)\]:")
REL = re.compile(r"^[0-9a-fA-F]+\s+\S+\s+(\S+?)(?:[+-]0x[0-9a-fA-F]+)?\s*$")
LOCAL_SEC = re.compile(r"^\.(?:text|literal|rodata|data|bss|sdata|srodata)\.(.+?)(?:\.\d+)?$")


def parse(dump_text, objkey, rev):
    cur = None
    for line in dump_text.splitlines():
        m = SEC.match(line)
        if m:
            kind, name = m.group(1), m.group(2)
            if kind == ".xt":  # .xt.lit/.xt.prop: assembler metadata, not code
                cur = None
                continue
            cur = f"{objkey}::{name}"
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
        lm = LOCAL_SEC.match(val)
        tgt = f"{objkey}::{lm.group(1)}" if lm else val
        if tgt != cur:
            rev.setdefault(tgt, set()).add(cur)


def is_ui(node):
    return "/ui/" in node.split("::")[0]


def analyse(rev):
    fails, notes = [], []
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
                bad = [c for c in rev.get(node, ()) if c.partition("::")[2] != GATE_CALLER]
                for c in bad:
                    fails.append(f"{c} references the gated clear action (only {GATE_CALLER} may)")
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
    ap.add_argument("--reloc-dump", action="append", default=[],
                    help="OBJKEY=FILE: pre-dumped objdump -r text (test mode)")
    a = ap.parse_args()
    rev = {}
    if a.reloc_dump:
        for spec in a.reloc_dump:
            k, f = spec.split("=", 1)
            parse(open(f, encoding="utf-8").read(), k, rev)
    else:
        root = a.build_dir or os.environ.get("KILNCTL_KILNFW_BUILD") or os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "firmware", "KilnFW", "build")
        objs = [p for p in glob.glob(os.path.join(root, "**", "*.c.obj"), recursive=True)
                if ".dir/" in p.replace("\\", "/")
                and "/esp-idf/" in p.replace("\\", "/")]
        if not any(os.path.basename(p) == GATE_OBJ for p in objs):
            return skip(f"no {GATE_OBJ} under {root} (build not present)")
        od = find_objdump()
        for p in objs:
            r = subprocess.run([od, "-r", p], capture_output=True, text=True)
            if r.returncode != 0:
                print(f"FAIL: objdump failed on {p}: {r.stderr.strip()[:200]}")
                return 1
            parse(r.stdout, objkey_of(p), rev)
    fails = analyse(rev)
    if fails:
        print("LCD sink call-graph check FAILED:")
        for f in sorted(set(fails)):
            print("  " + f)
        return 1
    print(f"LCD sink call-graph check passed ({len(rev)} referenced symbols).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
