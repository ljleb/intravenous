#!/usr/bin/env python3
"""Compile the authoring documentation's examples and check local links."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs-dev/api"
MARKDOWN_LINK = re.compile(r"(?<!!)\[[^\]]+\]\(([^\s)]+)\)")
EXAMPLE = re.compile(r"<!-- check-example: ([\w.]+) -->\s*```\w+\n(.*?)\n```", re.S)


def check_links():
    for page in DOCS.rglob("*.md"):
        source = re.sub(r"```.*?```", "", page.read_text(), flags=re.S)
        for reference in MARKDOWN_LINK.findall(source):
            url = urlsplit(reference)
            if url.scheme or url.netloc or not url.path:
                continue
            target = page.parent / unquote(url.path)
            if not target.exists():
                raise ValueError(f"{page.relative_to(ROOT)} points to missing {reference}")


def extract_examples(page, directory):
    examples = {}
    for name, code in EXAMPLE.findall(page.read_text()):
        if name in examples:
            raise ValueError(f"{page.relative_to(ROOT)} repeats example file {name}")
        path = directory / name
        path.write_text(code + "\n")
        examples[name] = path
    return examples


def run(*command):
    subprocess.run(command, cwd=ROOT, check=True)


def compile_examples(build):
    with tempfile.TemporaryDirectory(prefix="iv-api-docs-") as work:
        scratch = Path(work)
        c = extract_examples(DOCS / "tutorials/presentation_c.md", scratch)["diagonal.c"]
        builtins = ROOT / "src/intravenous/builtin_packages/builtin/presentations"
        examples = ROOT / "examples/wasm-presentation/packages/marker/presentations"
        schemas = ROOT / "vscode/client/bindings"
        for binding, header, namespace in (
            ("beats", builtins / "beats_events.hpp", "presentation_beats_events"),
            ("automation", builtins / "automation_events.hpp", "automation_events"),
            ("automation", examples / "automation_events.hpp", "automation_events"),
        ):
            generated = scratch / header.name
            arguments = schemas / f"{binding}.construction.json"
            run(sys.executable, ROOT / "scripts/generate_presentation_events.py",
                schemas / f"{binding}.node-events.json", schemas / f"{binding}.names.json",
                generated, "--namespace", namespace,
                *(["--arguments", arguments] if arguments.is_file() else []))
            if generated.read_bytes() != header.read_bytes():
                raise ValueError(f"Stale presentation event binding {header.relative_to(ROOT)}")
        sources = [c, examples / "marker.cpp", examples / "placement.cpp", builtins / "automation_curve.cpp",
                   builtins / "click_detector.cpp", builtins / "beats.cpp"]
        for source in sources:
            run(sys.executable, ROOT / "scripts/build_presentation.py", source, scratch / (source.stem + ".wasm"))

        layout = extract_examples(DOCS / "reference/presentation_c_abi.md", scratch)["abi_layout.c"]
        run(os.environ.get("IV_PRESENTATION_WASM_COMPILER", "clang"), "--target=wasm32-unknown-unknown",
            "-std=c17", "-ffreestanding", "-nostdlibinc", "-fsyntax-only", "-I", ROOT / "src", layout)

        package = scratch / "half_gain"
        package.mkdir()
        files = extract_examples(DOCS / "tutorials/llvm_node.md", package)
        for name in ("module.cpp", "iv_package.json"):
            if name not in files:
                raise ValueError(f"The LLVM tutorial is missing {name}")
        run(build / "package_tools/iv_package_prebuild", package, scratch / "artifacts", ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-release")
    args = parser.parse_args()
    check_links()
    compile_examples(args.build_dir.resolve())
    print("API documentation links and examples passed.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"API documentation check failed: {error}", file=sys.stderr)
        sys.exit(1)
