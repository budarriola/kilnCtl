"""Resolve the Pico SDK path for SaftyFW (RP2040) builds.

The Pico SDK is not vendored in this repo -- it lives unvendored on this
bench machine at ``C:\\pico-tools\\pico-sdk``. ``firmware/SaftyFW/test/
check_00_saftyfw_target_build.ps1`` already defaults ``PICO_SDK_PATH`` to
that path when it has to configure a build directory from scratch. This
module is the single source of truth that default is meant to mirror -- see
that script's own comment naming this file -- so a fresh worktree's
``build_saftyfw()`` call (the ``kiln_fw_root``-style "build from a clean
worktree" flow) does not need a human to set the environment variable by
hand first.

Resolution order:
  1. ``PICO_SDK_PATH`` from the environment, if set (never overridden).
  2. ``C:\\pico-tools\\pico-sdk``, if it looks like a real SDK checkout
     (contains ``pico_sdk_init.cmake``).
  3. Raise ``PicoSdkNotFoundError`` naming both locations checked.
"""

from __future__ import annotations

import os

#: Bench-machine fallback location for an unvendored Pico SDK checkout.
#: Mirrored in firmware/SaftyFW/test/check_00_saftyfw_target_build.ps1 --
#: keep both in sync if this ever moves.
DEFAULT_PICO_SDK_PATH = r"C:\pico-tools\pico-sdk"

#: File that must exist directly under a real SDK checkout root.
_SDK_MARKER = "pico_sdk_init.cmake"


class PicoSdkNotFoundError(RuntimeError):
    """Raised when no usable Pico SDK path can be resolved."""


def resolve_pico_sdk_path(env: "os._Environ[str] | dict | None" = None) -> str:
    """Return a usable ``PICO_SDK_PATH`` value.

    ``env`` defaults to ``os.environ`` and is only ever read, never mutated
    -- callers that need the value injected into a subprocess should pass it
    via that subprocess's own ``env=`` argument, not by setting it in this
    process's environment.
    """
    if env is None:
        env = os.environ

    existing = env.get("PICO_SDK_PATH")
    if existing:
        return existing

    if os.path.isfile(os.path.join(DEFAULT_PICO_SDK_PATH, _SDK_MARKER)):
        return DEFAULT_PICO_SDK_PATH

    raise PicoSdkNotFoundError(
        "PICO_SDK_PATH is not set and the bench default "
        f"{DEFAULT_PICO_SDK_PATH!r} does not contain {_SDK_MARKER!r} "
        "(no usable Pico SDK checkout found there either). Set "
        "PICO_SDK_PATH to a real Pico SDK checkout, or install one at "
        f"{DEFAULT_PICO_SDK_PATH!r}."
    )
