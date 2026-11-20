#!/usr/bin/env python3
"""Manage the writable Intravenous build cache on Linux."""

import argparse
from contextlib import contextmanager, ExitStack
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time


GIB = 1024 ** 3
DAY = 24 * 60 * 60
DEFAULTS = {
    "thresholdGiB": 40,
    "targetGiB": 30,
    "minimumFreeGiB": 15,
    "maxIdleDays": 30,
}
ENTRY_DEPTH = {"server": 2, "project-packages": 3, "shared-packages": 2}
REPO = Path(__file__).resolve().parent.parent


def user_directory(variable, default):
    value = os.environ.get(variable)
    if value and Path(value).is_absolute():
        return Path(value)
    return Path.home() / default


def cache_root():
    override = os.environ.get("IV_BUILD_CACHE_DIR")
    if override:
        if not Path(override).is_absolute():
            raise ValueError("IV_BUILD_CACHE_DIR must be an absolute path")
        return Path(override).resolve()
    return (user_directory("XDG_CACHE_HOME", ".cache") / "intravenous/build-cache").resolve()


def read_policy():
    path = user_directory("XDG_CONFIG_HOME", ".config") / "intravenous/build-cache.json"
    try:
        values = json.loads(path.read_text())
    except FileNotFoundError:
        return DEFAULTS
    if not isinstance(values, dict) or values.keys() - DEFAULTS.keys():
        raise ValueError(f"Unknown build cache configuration in {path}")
    for name, value in values.items():
        if type(value) is not int or value < 0:
            raise ValueError(f"{name} in {path} must be a nonnegative integer")
    policy = DEFAULTS | values
    if policy["targetGiB"] > policy["thresholdGiB"]:
        raise ValueError(f"targetGiB must be at most thresholdGiB in {path}")
    return policy


@contextmanager
def lock(path, flags):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, flags)
        except BlockingIOError:
            yield None
        else:
            yield fd
    finally:
        os.close(fd)


def subdirectories(path):
    if path.is_symlink():
        return []
    try:
        with os.scandir(path) as entries:
            return [Path(entry.path) for entry in entries if entry.is_dir(follow_symlinks=False)]
    except FileNotFoundError:
        return []


def allocated_bytes(path, *, changing=False):
    # st_blocks counts allocated 512-byte blocks, including directories.
    # Never follow symlinks into the source tree or Nix store.
    size = 0
    try:
        with os.scandir(path) as entries:
            size = path.lstat().st_blocks * 512
            for entry in entries:
                try:
                    if entry.is_dir(follow_symlinks=False):
                        size += allocated_bytes(Path(entry.path), changing=changing)
                    else:
                        size += entry.stat(follow_symlinks=False).st_blocks * 512
                except FileNotFoundError:
                    if not changing:
                        raise
    except (FileNotFoundError, NotADirectoryError):
        if not changing:
            raise
    return size


def last_used(locks):
    try:
        return (locks / "last-used").stat().st_mtime_ns
    except FileNotFoundError:
        return 0


def gc_decision(decisions, path, action, reason, locks=None):
    if decisions is None:
        return
    source = ""
    if locks is not None:
        try:
            source = (locks / "source").read_text(errors="replace").strip()
        except OSError:
            pass
    decisions[path] = (action, reason, source)


def print_gc_report(root, workspaces, artifacts, scratch_dirs, total, free):
    removed_workspaces = {path for path, (action, _, _) in workspaces.items() if action == "Removed"}
    artifacts_in_kept_workspaces = {
        path: decision for path, decision in artifacts.items()
        if not removed_workspaces.intersection(path.parents)
    }
    print(f"Build cache at {root}")
    for category, decisions in (
        ("Build workspaces", workspaces),
        ("Package artifact sets", artifacts_in_kept_workspaces),
        ("Scratch directories", scratch_dirs),
    ):
        rows = {"Removed": [], "Kept": []}
        for path, (action, reason, source) in sorted(decisions.items()):
            relative = path.relative_to(root)
            label = f"{source} [{relative}]" if source else relative
            rows[action].append(f"  {action} {label} ({reason})")
        print(f"{category} (removed {len(rows['Removed'])}, kept {len(rows['Kept'])})")
        for details in rows.values():
            for detail in details:
                print(detail)
    print(f"Managed cache now uses {total / GIB:.2f} GiB. Filesystem has {free / GIB:.2f} GiB free.")


def prune_package_artifacts(entry, dirty, decisions):
    try:
        current = (entry / ".current-artifact").read_text().strip()
    except FileNotFoundError:
        current = ""
    for revision in subdirectories(entry / "out"):
        if revision.name == current:
            gc_decision(decisions, revision, "Kept", "latest successful package build")
            continue
        # The caller holds the workspace writer lock. Only that lock's
        # holder can open new revision leases, so this lock can disappear
        # together with its revision.
        with lock(revision / ".use", fcntl.LOCK_EX | fcntl.LOCK_NB) as lease:
            if lease is not None:
                if decisions is not None:
                    reason = (
                        "superseded package artifact" if (revision / ".complete").is_file()
                        else "incomplete package artifact"
                    )
                    gc_decision(decisions, revision, "Removed", reason)
                dirty.touch()
                shutil.rmtree(revision)
            else:
                gc_decision(decisions, revision, "Kept", "package artifact is leased")


def cache_entries(root):
    for kind, depth in ENTRY_DEPTH.items():
        level = [root / kind] if (root / kind).is_dir() and not (root / kind).is_symlink() else []
        for _ in range(depth):
            level = [child for parent in level for child in subdirectories(parent)]
        for entry in level:
            locks = root / "locks" / entry.relative_to(root)
            if (locks / "last-used").is_file():
                yield entry, locks, kind


def collect(root, *, before=False, report=False):
    policy = read_policy()
    threshold = policy["thresholdGiB"] * GIB
    target = policy["targetGiB"] * GIB
    minimum_free = policy["minimumFreeGiB"] * GIB
    expired = time.time_ns() - policy["maxIdleDays"] * DAY * 10 ** 9
    exclusive = fcntl.LOCK_EX | fcntl.LOCK_NB
    workspace_decisions = {} if report else None
    artifact_decisions = {} if report else None
    scratch_decisions = {} if report else None
    with lock(root / "locks/collect", fcntl.LOCK_EX):
        scratch_size = 0
        for scratch in subdirectories(root / "scratch"):
            if not scratch.name.startswith("run-") or not (scratch / ".use").is_file():
                continue
            with lock(scratch / ".use", exclusive) as lease:
                if lease is not None:
                    shutil.rmtree(scratch)
                    gc_decision(scratch_decisions, scratch, "Removed", "scratch lease ended")
                else:
                    scratch_size += allocated_bytes(scratch, changing=True)
                    gc_decision(scratch_decisions, scratch, "Kept", "scratch is in use")
        entries = []
        for entry, locks, kind in cache_entries(root):
            size_file = locks / "size"
            dirty = locks / "dirty"
            reason = "workspace writer lock is held"
            with lock(locks / "write", exclusive) as writer:
                if writer is not None:
                    if kind != "server":
                        prune_package_artifacts(entry, dirty, artifact_decisions)
                    with lock(locks / "use", exclusive) as lease:
                        if lease is not None and last_used(locks) <= expired:
                            shutil.rmtree(entry)
                            size_file.unlink(missing_ok=True)
                            dirty.unlink(missing_ok=True)
                            gc_decision(workspace_decisions, entry, "Removed", "idle limit reached", locks)
                            continue
                        reason = "workspace is leased" if lease is None else "within idle limit"
                    if dirty.exists() or not size_file.exists():
                        pending = size_file.with_suffix(".next")
                        pending.write_text(str(allocated_bytes(entry)))
                        pending.replace(size_file)
                        dirty.unlink(missing_ok=True)
                if dirty.exists() or not size_file.exists():
                    size = allocated_bytes(entry, changing=True)
                else:
                    size = int(size_file.read_text())
            gc_decision(workspace_decisions, entry, "Kept", reason, locks)
            entries.append((last_used(locks), entry, locks, size))

        total = scratch_size + sum(size for _, _, _, size in entries)
        free = shutil.disk_usage(root).free
        if total > threshold or free < minimum_free:
            for used, entry, locks, size in sorted(entries):
                if total <= target and free >= minimum_free:
                    break
                with lock(locks / "write", exclusive) as writer:
                    if writer is None:
                        gc_decision(workspace_decisions, entry, "Kept", "workspace writer lock is held", locks)
                        continue
                    with lock(locks / "use", exclusive) as lease:
                        if lease is None:
                            gc_decision(workspace_decisions, entry, "Kept", "workspace is leased", locks)
                            continue
                        if last_used(locks) != used:
                            gc_decision(workspace_decisions, entry, "Kept", "used again during this sweep", locks)
                            continue
                        shutil.rmtree(entry)
                        (locks / "size").unlink(missing_ok=True)
                        (locks / "dirty").unlink(missing_ok=True)
                total -= size
                free = shutil.disk_usage(root).free
                if not report:
                    identity = (locks / "source").read_text().strip()
                    print(f"Build cache evicted {identity} ({entry})", file=sys.stderr)
                else:
                    gc_decision(
                        workspace_decisions, entry, "Removed", "least recently used under storage pressure", locks,
                    )
        if report:
            print_gc_report(root, workspace_decisions, artifact_decisions, scratch_decisions, total, free)
        if free < minimum_free:
            message = (f"Build cache filesystem has {free / GIB:.2f} GiB free, "
                       f"below the configured {policy['minimumFreeGiB']} GiB floor")
            if before:
                raise ValueError(message)
            print(message, file=sys.stderr)
        if total > threshold:
            print(f"Build cache is above its threshold ({total / GIB:.2f} GiB). "
                  "Entries in use were kept.", file=sys.stderr)


def digest(value):
    return hashlib.sha256(os.fsencode(value)).hexdigest()[:24]


def server_path(root, build_type):
    configuration = "\n".join(os.environ.get(name, "") for name in (
        "CC", "CXX", "CMAKE_PREFIX_PATH", "CMAKE_TOOLCHAIN_FILE", "JUCE_DIR", "IV_DEV_NIXPKGS"))
    tools = "\n".join(str(Path(shutil.which(name) or name).resolve()) for name in ("cmake", "ninja"))
    profile = digest(f"Ninja\n{build_type}\n{platform.machine()}\n{tools}\n{configuration}")
    return root / "server" / digest(str(REPO)) / profile


def finish(root):
    try:
        collect(root)
    except (OSError, ValueError) as error:
        print(f"Build cache: {error}", file=sys.stderr)


@contextmanager
def scratch_directory(root):
    with ExitStack() as lease:
        with lock(root / "locks/collect", fcntl.LOCK_EX):
            parent = root / "scratch"
            parent.mkdir(exist_ok=True)
            scratch = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
            fd = lease.enter_context(lock(scratch / ".use", fcntl.LOCK_EX))
        yield scratch, fd


def run_command(root, arguments):
    external = getattr(arguments, "external_build_directory", None)
    if external is not None and external.resolve().is_relative_to(root):
        raise ValueError("Use the managed server command for a build tree inside the cache")
    collect(root, before=True)
    with ExitStack() as active:
        # Run the final sweep after the command's locks close, even on failure.
        active.callback(finish, root)
        fds = []
        env = os.environ.copy()
        if arguments.command == "scratch" or arguments.scratch:
            scratch, fd = active.enter_context(scratch_directory(root))
            env["IV_BUILD_SCRATCH_DIR"] = str(scratch)
            fds.append(fd)
        if arguments.command == "server":
            entry = server_path(root, arguments.build_type)
            locks = root / "locks" / entry.relative_to(root)
            fds.append(active.enter_context(lock(locks / "write", fcntl.LOCK_EX)))
            fds.append(active.enter_context(lock(locks / "use", fcntl.LOCK_SH)))
            (locks / "source").write_text(f"{REPO} [{arguments.build_type}]\n")
            (locks / "last-used").touch()
            active.callback((locks / "last-used").touch)
            (locks / "dirty").touch()
            env["IV_SERVER_BUILD_DIR"] = str(entry)
            configured = entry / ".iv-configured"
            if not configured.is_file() or not (entry / "build.ninja").is_file():
                subprocess.run([
                    "cmake", "-S", str(REPO), "-B", str(entry), "-G", "Ninja",
                    f"-DCMAKE_BUILD_TYPE={arguments.build_type}", f"-DJUCE_DIR={env.get('JUCE_DIR', '')}",
                ], env=env, pass_fds=fds, check=True)
                configured.touch()
            if not arguments.argv:
                arguments.argv = ["cmake", "--build", str(entry), "--target", "intravenous"]
        argv = arguments.argv[1:] if arguments.argv[:1] == ["--"] else arguments.argv
        if not argv:
            raise ValueError("A command is required after --")
        env["IV_BUILD_CACHE_LEASE_FDS"] = " ".join(map(str, fds))
        code = subprocess.run(argv, env=env, pass_fds=fds).returncode
        return code if code >= 0 else 128 - code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("gc", help="sweep the cache")
    commands.add_parser("before-build", help=argparse.SUPPRESS)
    commands.add_parser("after-build", help=argparse.SUPPRESS)
    server = commands.add_parser("server", help="configure and use a leased CMake build tree")
    server.add_argument("--build-type", default="Release")
    server.add_argument("--scratch", action="store_true")
    server.add_argument("argv", nargs=argparse.REMAINDER)
    scratch = commands.add_parser("scratch", help="run with a temporary IV_BUILD_SCRATCH_DIR")
    scratch.add_argument("--external-build-directory", type=Path, help=argparse.SUPPRESS)
    scratch.add_argument("argv", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        root = cache_root()
        if args.command in {"gc", "before-build", "after-build"}:
            collect(root, before=args.command == "before-build", report=args.command == "gc")
            return 0
        return run_command(root, args)
    except subprocess.CalledProcessError as error:
        return error.returncode
    except (OSError, ValueError) as error:
        print(f"Build cache: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
