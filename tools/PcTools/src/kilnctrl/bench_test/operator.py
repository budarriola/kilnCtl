"""operator.py -- the `--attended` operator-prompt mechanism
(docs/BENCH_TEST_SYSTEM_PLAN.md section 6, SP-08/SP-09, WEB-WIFI-06).

A handful of bench-test cases need a human at the bench to physically do
something (press the E-stop, pull the link cable, watch the AP fallback come
up) and then answer a yes/no question about what happened. Every other case
in this suite is fully automatic; these are deliberately not, and must never
silently pretend otherwise.

Contract:
  - `ctx["attended"]` (bool, default False) says whether a human is present
    for this run at all -- set from bench_test.ps1's `-Attended` switch /
    `bench_test_run(attended=True)`.
  - When `ctx["attended"]` is falsy, `require_attended()` returns a SKIP
    CaseResult with reason "requires --attended" -- never FAIL, never a
    hang, never a silent NOT_RUN. The case function should call this FIRST
    and return its result immediately if it is not None.
  - When attended, `ask_operator(ctx, question, timeout_s)` prints the
    question and blocks (bounded by `timeout_s`) for a y/n answer. A caller
    can inject `ctx["operator_prompt_fn"]` (a `(question, timeout_s) ->
    Optional[bool]` callable) to test this without a real terminal --
    `runner.py`/production code never needs to set this key.
  - A timeout or unparseable answer is treated as "no" (None), which the
    calling case must turn into FAIL or SKIP as its own judge sees fit --
    this module never decides case verdicts itself.
  - Prompts must NEVER echo a credential. This module only ever asks
    yes/no questions about what the operator observed; it takes no
    credential as an argument and does not read any environment variable.
"""
from __future__ import annotations

import sys
from typing import Callable, Optional

#: Type alias for the injectable prompt function.
PromptFn = Callable[[str, float], Optional[bool]]


def _default_prompt(question: str, timeout_s: float) -> Optional[bool]:
    """Ask on stdin/stdout with a plain (unbounded-by-Python, operator is
    expected to answer promptly) y/n prompt. There is no cross-platform
    stdlib way to bound `input()` itself by a wall-clock timeout without a
    background thread; bench-test runs are already interactive/attended
    when this path is reached, so a slow operator only delays this one
    case, and `timeout_s` is surfaced in the prompt text as a hint rather
    than enforced by a hard kill -- enforcing it would risk killing the
    process mid-input on some platforms, which is worse than a slow
    answer."""
    sys.stdout.write(f"\n[bench-test operator prompt, answer within ~{timeout_s:.0f}s] {question} [y/n]: ")
    sys.stdout.flush()
    try:
        answer = sys.stdin.readline().strip().lower()
    except Exception:  # noqa: BLE001
        return None
    if answer in ("y", "yes"):
        return True
    if answer in ("n", "no"):
        return False
    return None


def ask_operator(ctx: dict, question: str, timeout_s: float = 60.0) -> Optional[bool]:
    """Ask the operator a yes/no question. Returns True/False, or None if
    unanswered/unparseable/timed out. Never raises for a bad answer.

    Uses `ctx["operator_prompt_fn"]` when present (test seam) instead of the
    real stdin/stdout prompt -- production callers (bench_test.ps1 via
    mcp_server_bench_test.py) never set this key."""
    prompt_fn: PromptFn = ctx.get("operator_prompt_fn") or _default_prompt
    return prompt_fn(question, timeout_s)


def require_attended(ctx: dict, reason: str = "requires --attended"):
    """Return a SKIP CaseResult if `ctx["attended"]` is not truthy, else
    None. Case functions call this first and return immediately on a
    non-None result -- this is what keeps an unattended run from ever
    blocking on, or failing, an operator-only case."""
    from .registry import CaseResult, Verdict  # local import: avoid a cycle

    if not ctx.get("attended"):
        return CaseResult(verdict=Verdict.SKIP, reason=reason)
    return None
