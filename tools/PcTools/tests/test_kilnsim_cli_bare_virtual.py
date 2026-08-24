"""Regression test for a defect predating the testmgr work: ``kilnsim
--virtual testmgr --quick`` (bare ``--virtual``, no ``=``, no address)
parsed ``testmgr`` as ``--virtual``'s own value (argparse ``nargs='?'``
greedily consumes the next non-flag token), eating the subcommand entirely
and failing with "the following arguments are required: command". This
affected every subcommand, not just ``testmgr``. Deliberately a NEW test
file rather than an addition to test_kilnsim_cli.py -- that file is owned
by another concurrent session with uncommitted work.

Exercises :func:`kilnsim.cli._fixup_bare_virtual`/`_subcommand_names`
directly (fast, no subprocess) plus one full :func:`kilnsim.cli.main` call
against a real subcommand to prove the whole pipeline, not just the helper
in isolation.
"""

from __future__ import annotations

import sys
from pathlib import Path

_SRC_DIR = Path(__file__).resolve().parents[1] / "src"
if str(_SRC_DIR) not in sys.path:
    sys.path.insert(0, str(_SRC_DIR))

from kilnsim import cli  # noqa: E402


def test_bare_virtual_before_subcommand_is_not_swallowed():
    """The exact repro from the bug report: `--virtual testmgr --quick`."""
    parser = cli.build_parser()
    names = cli._subcommand_names(parser)
    fixed = cli._fixup_bare_virtual(["--virtual", "testmgr", "--quick"], names)
    args = parser.parse_args(fixed)
    assert args.command == "testmgr"
    assert args.quick is True
    assert args.virtual == ""  # "" (not None) means "--virtual was given, no address"


def test_bare_virtual_affects_every_subcommand_not_just_testmgr():
    """Bug report notes this predates testmgr and affects every subcommand
    -- prove it for a couple of unrelated ones too (state, estop)."""
    parser = cli.build_parser()
    names = cli._subcommand_names(parser)

    fixed = cli._fixup_bare_virtual(["--virtual", "state"], names)
    args = parser.parse_args(fixed)
    assert args.command == "state"
    assert args.virtual == ""

    fixed = cli._fixup_bare_virtual(["--virtual", "estop", "open"], names)
    args = parser.parse_args(fixed)
    assert args.command == "estop"
    assert args.state == "open"
    assert args.virtual == ""


def test_explicit_equals_form_still_works():
    """`--virtual=127.0.0.1:8765 testmgr` was never ambiguous -- must stay
    untouched by the fixup."""
    parser = cli.build_parser()
    names = cli._subcommand_names(parser)
    fixed = cli._fixup_bare_virtual(["--virtual=127.0.0.1:8765", "testmgr", "--quick"], names)
    assert fixed == ["--virtual=127.0.0.1:8765", "testmgr", "--quick"]
    args = parser.parse_args(fixed)
    assert args.command == "testmgr"
    assert args.virtual == "127.0.0.1:8765"


def test_space_separated_explicit_address_still_works():
    """`--virtual 127.0.0.1:9000 testmgr` (space-separated, but the address
    is not itself a subcommand name) must be left alone -- only a bare
    `--virtual` directly followed by a *subcommand name* gets rewritten."""
    parser = cli.build_parser()
    names = cli._subcommand_names(parser)
    fixed = cli._fixup_bare_virtual(["--virtual", "127.0.0.1:9000", "testmgr", "--quick"], names)
    assert fixed == ["--virtual", "127.0.0.1:9000", "testmgr", "--quick"]
    args = parser.parse_args(fixed)
    assert args.command == "testmgr"
    assert args.virtual == "127.0.0.1:9000"


def test_main_entry_point_handles_bare_virtual_without_manual_workaround(monkeypatch):
    """Full pipeline: kilnsim.cli.main() itself, not just the parser helpers
    -- catches a regression where main() stops calling the fixup."""
    calls = []

    def fake_cmd_state(args):
        calls.append(args.virtual)
        return 0

    monkeypatch.setattr(cli, "cmd_state", fake_cmd_state)
    # build_parser() binds cmd_state via sp.set_defaults(func=cmd_state) at
    # import time, so patching the module attribute alone won't affect an
    # already-built parser's default. Rebuild via main()'s own call path by
    # patching build_parser to rebind func to our fake after construction.
    orig_build_parser = cli.build_parser

    def patched_build_parser():
        parser = orig_build_parser()
        for action in parser._actions:  # noqa: SLF001
            pass
        # Walk the subparsers to rebind "state"'s func.
        for action in parser._actions:  # noqa: SLF001
            if hasattr(action, "choices") and action.choices and "state" in action.choices:
                action.choices["state"].set_defaults(func=fake_cmd_state)
        return parser

    monkeypatch.setattr(cli, "build_parser", patched_build_parser)
    rc = cli.main(["--virtual", "state"])
    assert rc == 0
    assert calls == [""]
