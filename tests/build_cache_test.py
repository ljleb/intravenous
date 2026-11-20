import fcntl
from contextlib import ExitStack
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import build_cache as cache


class BuildCacheTest(unittest.TestCase):
    def setUp(self):
        cleanup = ExitStack()
        self.addCleanup(cleanup.close)
        temporary = cleanup.enter_context(tempfile.TemporaryDirectory())
        self.temporary = Path(temporary)
        self.root = self.temporary / "cache"
        self.config = self.temporary / "config" / "intravenous" / "build-cache.json"
        self.config.parent.mkdir(parents=True)
        cleanup.enter_context(patch.dict(os.environ, {
            "IV_BUILD_CACHE_DIR": str(self.root),
            "XDG_CONFIG_HOME": str(self.config.parent.parent),
        }))

    def policy(self, **settings):
        self.config.write_text(json.dumps({"minimumFreeGiB": 0, **settings}))

    def entry(self, relative, *, age=0, size=None):
        entry = self.root / relative
        locks = self.root / "locks" / relative
        entry.mkdir(parents=True)
        locks.mkdir(parents=True)
        used = locks / "last-used"
        used.touch()
        stamp = time.time() - age * cache.DAY
        os.utime(used, (stamp, stamp))
        (locks / "source").write_text(str(entry))
        if size is not None:
            (locks / "size").write_text(str(size))
        return entry, locks

    def cli(self, *args, **kwargs):
        return subprocess.run(
            [sys.executable, cache.__file__, *args], capture_output=True, text=True, **kwargs)

    def test_policy_defaults_and_validation(self):
        self.assertEqual(cache.read_policy(), {
            "thresholdGiB": 40, "targetGiB": 30, "minimumFreeGiB": 15, "maxIdleDays": 30,
        })
        self.policy(thresholdGiB=60, maxIdleDays=5)
        self.assertEqual(cache.read_policy()["thresholdGiB"], 60)
        self.assertEqual(cache.read_policy()["targetGiB"], 30)
        self.assertEqual(cache.read_policy()["maxIdleDays"], 5)
        self.policy(thresholdGiB=20)
        self.assertRaisesRegex(ValueError, "targetGiB", cache.read_policy)
        self.policy(maxIdleDays=-1)
        self.assertRaisesRegex(ValueError, "maxIdleDays", cache.read_policy)

    def test_pressure_evicts_oldest_to_target(self):
        self.policy(thresholdGiB=4, targetGiB=3)
        old, _ = self.entry("server/checkout/old", age=3, size=2 * cache.GIB)
        middle, _ = self.entry("shared-packages/package/middle", age=2, size=2 * cache.GIB)
        new, _ = self.entry("project-packages/project/package/new", age=1, size=cache.GIB)
        result = self.cli("gc", check=True)
        self.assertIn("\nBuild workspaces (removed 1, kept 2)\n", result.stdout)
        self.assertIn("\nPackage artifact sets (removed 0, kept 0)\n", result.stdout)
        self.assertIn("server/checkout/old] (least recently used under storage pressure)", result.stdout)
        self.assertIn("shared-packages/package/middle] (within idle limit)", result.stdout)
        self.assertFalse(old.exists())
        self.assertTrue(middle.exists())
        self.assertTrue(new.exists())

    def test_age_and_pressure_respect_workspace_and_writer_leases(self):
        self.policy(thresholdGiB=0, targetGiB=0, maxIdleDays=3)
        read, read_locks = self.entry("server/checkout/reader", age=5, size=cache.GIB)
        write, write_locks = self.entry("server/checkout/writer", age=5, size=cache.GIB)
        expired, _ = self.entry("server/checkout/expired", age=5)
        with cache.lock(read_locks / "use", fcntl.LOCK_SH):
            with cache.lock(write_locks / "write", fcntl.LOCK_EX):
                result = self.cli("gc")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("server/checkout/expired] (idle limit reached)", result.stdout)
                self.assertIn("server/checkout/reader] (workspace is leased)", result.stdout)
                self.assertIn("server/checkout/writer] (workspace writer lock is held)", result.stdout)
                self.assertFalse(expired.exists())
                self.assertTrue(read.exists())
                self.assertTrue(write.exists())
        cache.collect(self.root)
        self.assertFalse(read.exists())
        self.assertFalse(write.exists())

    def test_failed_artifact_and_superseded_sets_are_collected_separately(self):
        self.policy()
        entry, locks = self.entry("project-packages/project/package/profile")
        old, current, failed = [entry / "out" / name for name in ("old", "current", "failed")]
        for revision in (old, current, failed):
            revision.mkdir(parents=True)
            (revision / "test.bc").write_text("artifact")
        (old / ".complete").touch()
        (entry / ".current-artifact").write_text("current\n")
        with cache.lock(locks / "use", fcntl.LOCK_SH):
            with cache.lock(old / ".use", fcntl.LOCK_SH):
                result = self.cli("gc", check=True)
                self.assertIn("\nBuild workspaces (removed 0, kept 1)\n", result.stdout)
                self.assertIn("\nPackage artifact sets (removed 1, kept 2)\n", result.stdout)
                self.assertIn("out/old (package artifact is leased)", result.stdout)
                self.assertIn("out/current (latest successful package build)", result.stdout)
                self.assertIn("out/failed (incomplete package artifact)", result.stdout)
                self.assertTrue(old.exists())
                self.assertFalse(failed.exists())
            result = self.cli("gc", check=True)
            self.assertIn("out/old (superseded package artifact)", result.stdout)
            self.assertFalse(old.exists())
            self.assertTrue(current.exists())
            self.assertTrue(entry.exists())

    def test_gc_report_omits_retained_artifact_in_an_evicted_workspace(self):
        self.policy(maxIdleDays=3)
        entry, _ = self.entry("project-packages/project/package/profile", age=5)
        (entry / "out/current").mkdir(parents=True)
        (entry / ".current-artifact").write_text("current\n")
        result = self.cli("gc", check=True)
        self.assertIn("project-packages/project/package/profile] (idle limit reached)", result.stdout)
        self.assertNotIn("out/current", result.stdout)
        self.assertIn("\nBuild workspaces (removed 1, kept 0)\n", result.stdout)
        self.assertIn("\nPackage artifact sets (removed 0, kept 0)\n", result.stdout)
        empty = self.cli("gc", check=True)
        self.assertIn(
            "\nBuild workspaces (removed 0, kept 0)\n"
            "Package artifact sets (removed 0, kept 0)\n"
            "Scratch directories (removed 0, kept 0)\n",
            empty.stdout,
        )

    def test_scratch_lives_through_the_child_and_is_removed_on_failure(self):
        self.policy()
        result = self.cli(
            "scratch", "--", sys.executable, "-c",
            'import os, pathlib, subprocess, sys; '
            'path = pathlib.Path(os.environ["IV_BUILD_SCRATCH_DIR"]); '
            'subprocess.run([sys.executable, sys.argv[1], "gc"], check=True, stdout=subprocess.DEVNULL); '
            'print(path); sys.exit(37 if path.is_dir() else 1)',
            cache.__file__)
        self.assertEqual(result.returncode, 37, result.stderr)
        self.assertFalse(Path(result.stdout.strip()).exists())

    def test_abandoned_scratch_is_removed_and_unregistered_content_is_kept(self):
        self.policy()
        with cache.scratch_directory(self.root) as (scratch, fd):
            self.assertIsNotNone(fd)
        unrelated = self.root / "scratch/unrelated"
        unrelated.mkdir()
        result = self.cli("gc", check=True)
        self.assertIn("\nScratch directories (removed 1, kept 0)\n", result.stdout)
        self.assertIn(f"scratch/{scratch.name} (scratch lease ended)", result.stdout)
        self.assertFalse(scratch.exists())
        self.assertTrue(unrelated.exists())

    def test_child_keeps_scratch_after_parent_releases_its_descriptor(self):
        self.policy()
        with ExitStack() as child_scope:
            with cache.scratch_directory(self.root) as (scratch, fd):
                child = child_scope.enter_context(subprocess.Popen(
                    [sys.executable, "-c", "import sys; sys.stdin.read()"],
                    stdin=subprocess.PIPE, pass_fds=(fd,)))
                child_scope.callback(child.stdin.close)
            cache.collect(self.root)
            self.assertTrue(scratch.exists())
        cache.collect(self.root)
        self.assertFalse(scratch.exists())

    def test_preflight_blocks_build_below_free_space_floor(self):
        self.policy(minimumFreeGiB=1000000)
        result = self.cli("scratch", "--", sys.executable, "-c", 'print("ran")')
        self.assertEqual(result.returncode, 1)
        self.assertIn("free", result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertFalse((self.root / "scratch").exists())

    def test_server_configures_once_and_reuses_tree(self):
        self.policy()
        tools = self.temporary / "tools"
        tools.mkdir()
        cmake = tools / "cmake"
        cmake.write_text(
            '#!/bin/sh\nmkdir -p "$4" && echo configured >> "$4/CMakeCache.txt" '
            '&& touch "$4/build.ninja"\n')
        cmake.chmod(0o700)
        env = os.environ | {"PATH": str(tools) + ":" + os.environ["PATH"]}
        command = ("server", "--build-type", "Debug", "--", "/bin/sh", "-c",
                   'cat "$IV_SERVER_BUILD_DIR/CMakeCache.txt"; printf "%s" "$IV_SERVER_BUILD_DIR"')
        first = self.cli(*command, env=env)
        second = self.cli(*command, env=env)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(first.stdout, second.stdout)
        contents, directory = second.stdout.splitlines()
        self.assertEqual(contents, "configured")
        self.assertTrue(Path(directory).is_relative_to(self.root / "server"))

    def test_server_retries_interrupted_configuration(self):
        self.policy()
        tools = self.temporary / "tools"
        tools.mkdir()
        cmake = tools / "cmake"
        cmake.write_text(
            '#!/bin/sh\nmkdir -p "$4" && touch "$4/CMakeCache.txt" || exit 1\n'
            'if [ "${FAIL_CONFIGURE:-}" ]; then exit 19; fi\n'
            'touch "$4/build.ninja"\n')
        cmake.chmod(0o700)
        env = os.environ | {"PATH": str(tools) + ":" + os.environ["PATH"]}
        command = ("server", "--", "/bin/sh", "-c", 'test -f "$IV_SERVER_BUILD_DIR/build.ninja"')
        failed = self.cli(*command, env=env | {"FAIL_CONFIGURE": "1"})
        self.assertEqual(failed.returncode, 19, failed.stderr)
        retried = self.cli(*command, env=env)
        self.assertEqual(retried.returncode, 0, retried.stderr)


if __name__ == "__main__":
    unittest.main()
