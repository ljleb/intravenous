#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import time
import zipfile


def git(*arguments: str) -> bytes:
    return subprocess.check_output(["git", *arguments])


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Archive the current contents of every Git-tracked file."
    )
    parser.add_argument(
        "output",
        nargs="?",
        help="output ZIP (default: <repository>/intravenous-source.zip)",
    )
    arguments = parser.parse_args()

    repository = Path(git("rev-parse", "--show-toplevel").decode().strip()).resolve()
    output = (
        Path(arguments.output).expanduser().resolve()
        if arguments.output
        else repository / "intravenous-source.zip"
    )
    output.parent.mkdir(parents=True, exist_ok=True)

    tracked = git("-C", str(repository), "ls-files", "-z", "--cached").split(b"\0")
    paths = [Path(os.fsdecode(path)) for path in tracked if path]

    for path in paths:
        source = repository / path
        if source.absolute() == output.absolute():
            continue
        if not source.exists() and not source.is_symlink():
            raise FileNotFoundError(f"tracked file is missing from the working tree: {path}")

    temporary_file = tempfile.NamedTemporaryFile(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent, delete=False
    )
    temporary = Path(temporary_file.name)
    temporary_file.close()

    try:
        with zipfile.ZipFile(
            temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as archive:
            for path in paths:
                source = repository / path
                if source.absolute() == output.absolute():
                    continue
                if source.is_symlink():
                    metadata = os.lstat(source)
                    entry = zipfile.ZipInfo(path.as_posix())
                    entry.create_system = 3
                    entry.date_time = time.localtime(metadata.st_mtime)[:6]
                    entry.external_attr = (stat.S_IFLNK | metadata.st_mode) << 16
                    archive.writestr(entry, os.readlink(source))
                elif source.is_file():
                    archive.write(source, path.as_posix())

        os.replace(temporary, output)
        output.chmod(0o644)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise

    print(output)


if __name__ == "__main__":
    main()
