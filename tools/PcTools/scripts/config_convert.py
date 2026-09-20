#!/usr/bin/env python3
"""config_convert.py -- thin CLI wrapper, so this tool can be invoked as
`python tools/PcTools/scripts/config_convert.py ...` without an editable
install, the same way other tools/PcTools/scripts/*.py entry points work.
The real implementation (store detection, forward/backward conversion,
lossy reporting) lives in kilnctrl.config_convert -- see that module's
docstring for scope and the store-by-store support matrix. Also registered
as the `kilnctrl-config-convert` console script (pyproject.toml).

Usage:
    python tools/PcTools/scripts/config_convert.py <input.json> --to-version N [--report] [-o out.json]
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from kilnctrl.config_convert import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
