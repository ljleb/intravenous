#!/usr/bin/env python3
"""Build a freestanding Intravenous presentation and publish its WASM artifact."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

EXPORTS = ("iv_alloc", "iv_free", "iv_create_session", "iv_destroy_session",
           "iv_present", "iv_save_state", "iv_restore_state")


def build(args):
    output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".presentation-", dir=output.parent) as directory:
        work = Path(directory)
        guest = work / "guest.o"
        presentation = work / "presentation.o"
        artifact = work / "presentation.wasm"
        flags = ["--target=wasm32-unknown-unknown", "-O2", "-ffreestanding",
                 "-nostdlibinc", "-I", str(args.sdk)]
        runtime = args.sdk / "intravenous/presentation/guest_runtime.c"
        subprocess.run([args.clang, *flags, "-std=c17", "-c", runtime, "-o", guest], check=True)
        cxx = args.source.suffix in {".cc", ".cpp", ".cxx"}
        language = ["-std=c++20", "-fno-exceptions", "-fno-rtti"] if cxx else ["-std=c17"]
        subprocess.run([args.clang, *flags, *language, "-c", args.source, "-o", presentation], check=True)
        subprocess.run([args.linker, "--no-entry", "--export-memory", "--strip-debug",
                        "--max-memory=8388608", "--fatal-warnings",
                        *("--export=" + name for name in EXPORTS),
                        presentation, guest, "-o", artifact], check=True)
        if not output.is_file() or output.read_bytes() != artifact.read_bytes():
            os.replace(artifact, output)
            print(f"Published {output}", flush=True)


def source_snapshot(paths):
    snapshot = {}
    for path in paths:
        files = path.rglob("*") if path.is_dir() else [path]
        for file in files:
            try:
                stat = file.stat()
                if file.is_file():
                    snapshot[file] = (stat.st_mtime_ns, stat.st_size)
            except FileNotFoundError:
                continue
    return snapshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="C or C++ entry source")
    parser.add_argument("output", type=Path, help="published .wasm path")
    parser.add_argument("--sdk", type=Path, default=Path(__file__).resolve().parents[1] / "src")
    parser.add_argument("--clang", default=os.environ.get("IV_PRESENTATION_WASM_COMPILER", "clang"))
    parser.add_argument("--linker", default=os.environ.get("IV_PRESENTATION_WASM_LINKER", "wasm-ld"))
    parser.add_argument("--watch", action="store_true")
    parser.add_argument("--watch-path", action="append", type=Path, default=[],
                        help="additional file or directory to monitor (repeatable)")
    args = parser.parse_args()

    if not args.watch:
        build(args)
        return
    paths = [args.source, args.sdk / "intravenous/presentation", *args.watch_path]
    previous = None
    while True:
        current = source_snapshot(paths)
        if current != previous:
            previous = current
            try:
                build(args)
            except (OSError, subprocess.CalledProcessError) as error:
                print(f"Presentation build failed: {error}", file=sys.stderr)
        time.sleep(0.25)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Presentation build failed: {error}", file=sys.stderr)
        sys.exit(1)
