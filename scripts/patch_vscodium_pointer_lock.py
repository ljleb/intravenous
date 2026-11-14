#!/usr/bin/env python3
"""Backport detached-window pointer-lock permission to a supported VSCodium bundle.

The extension cannot change Electron's permission handler. Keep this optional
host patch separate from the client and reject unknown bundle shapes.
"""

import argparse
from pathlib import Path
import re


IDENTIFIER = r"[A-Za-z_$][A-Za-z0-9_$]*"
MARKER = "/* intravenous-detached-pointer-lock */"


def handler_pattern(request: bool) -> re.Pattern:
    method = "setPermissionRequestHandler" if request else "setPermissionCheckHandler"
    parameters = rf"(?P<wc>{IDENTIFIER}),(?P<permission>{IDENTIFIER}),"
    parameters += rf"(?P<third>{IDENTIFIER}),(?P<details>{IDENTIFIER})"
    allowed_view = rf"(?P<view_set>{IDENTIFIER})\.has\((?P=permission)\)"
    allowed_core = rf"(?P<core_set>{IDENTIFIER})\.has\((?P=permission)\)"
    denied = "!1"
    if request:
        allowed_view = rf"(?P=third)\({allowed_view}\)"
        allowed_core = rf"(?P=third)\({allowed_core}\)"
        denied = r"(?P=third)\(!1\)"
    return re.compile(
        rf"(?P<receiver>{IDENTIFIER}\.defaultSession)\.{method}"
        rf"\(\((?P<parameters>{parameters})\)=>"
        rf"(?P<expression>(?P<is_view>{IDENTIFIER})\((?P=details)\.requestingUrl\)"
        rf"\?{allowed_view}:(?P<is_core>{IDENTIFIER})\((?P=details)\.requestingUrl\)"
        rf"\?{allowed_core}:{denied})\)"
    )


def patch_bundle(source: str) -> str:
    if MARKER in source:
        raise ValueError("This VSCodium bundle already contains the detached-window patch")
    matches = []
    for request in (True, False):
        candidates = list(handler_pattern(request).finditer(source))
        if len(candidates) != 1:
            raise ValueError("Unsupported VSCodium permission handlers; bundle left unchanged. "
                             "Check whether this editor already includes the upstream fix")
        matches.append(candidates[0])
    for name in ("receiver", "is_view", "is_core", "view_set", "core_set"):
        if matches[0][name] != matches[1][name]:
            raise ValueError("VSCodium request/check policies differ; bundle left unchanged")
    # This property must be initialized on CodeApplication, as in the inspected
    # VSCodium bundle. Do not grant about:blank permission without its registry.
    if not re.search(r"this\.auxiliaryWindowsMainService=\w+\.get\(", source):
        raise ValueError("VSCodium auxiliary-window registry not found; bundle left unchanged")
    replacements = []
    for request, match in zip((True, False), matches):
        wc, permission, details = (match[name] for name in ("wc", "permission", "details"))
        guard = (f'{permission}==="pointerLock"&&{details}.isMainFrame===true'
                 f'&&{details}.requestingUrl==="about:blank"'
                 f'&&!!({wc}&&this.auxiliaryWindowsMainService?.getWindowByWebContents({wc}))')
        expression = match["expression"]
        replacement = (f"({guard})?{match['third']}(true):({expression})" if request
                       else f"({guard})||({expression})")
        replacements.append((match.start("expression"), match.end("expression"), MARKER + replacement))
    # Compute and validate both replacements before changing the caller's file.
    for begin, end, replacement in sorted(replacements, reverse=True):
        source = source[:begin] + replacement + source[end:]
    return source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path, help="VSCodium resources/app/out/main.js in a writable package build")
    args = parser.parse_args()
    try:
        original = args.bundle.read_text(encoding="utf-8")
        patched = patch_bundle(original)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    args.bundle.write_text(patched, encoding="utf-8")
    print(f"Patched detached-window pointer lock: {args.bundle}")


if __name__ == "__main__":
    main()
