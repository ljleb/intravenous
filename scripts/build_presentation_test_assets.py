#!/usr/bin/env python3
"""Build the WASM assets used by the client tests."""

import subprocess
import sys
from pathlib import Path


def main():
    output = Path(sys.argv[1])
    root = Path(__file__).resolve().parents[1]
    builtins = root / "src/intravenous/builtin_packages/builtin/presentations"
    examples = root / "examples/wasm-presentation/packages/marker/presentations"
    for directory, names in ((builtins, ("beats", "click_detector")),
                             (examples, ("marker", "placement"))):
        for name in names:
            subprocess.run([sys.executable, root / "scripts/build_presentation.py",
                            directory / f"{name}.cpp", output / f"{name}.wasm"], check=True)


if __name__ == "__main__":
    main()
