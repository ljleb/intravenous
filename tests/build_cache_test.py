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

IVCTL = Path(sys.argv.pop(1)).resolve()
IV_DEV_CACHE = Path(sys.argv.pop(1)).resolve()
BUILD_SERVER = Path(__file__).resolve().parents[1] / "scripts/build_server.sh"
GIB = 1024 ** 3
DAY = 24 * 60 * 60


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
        self.config.write_text(json.dumps({"minimumFreeBytes": 0, **settings}))

    def entry(self, relative, *, age=0, size=None):
        entry = self.root / relative
        locks = self.root / "locks" / relative
        entry.mkdir(parents=True)
        locks.mkdir(parents=True)
        used = locks / "last-used"
        used.touch()
        stamp = time.time() - age * DAY
        os.utime(used, (stamp, stamp))
        (locks / "source").write_text(str(entry))
        if size is not None:
            (locks / "size").write_text(str(size))
        return entry, locks

    def cli(self, *args, **kwargs):
        return subprocess.run(
            [IVCTL, "cache", *args], capture_output=True, text=True, **kwargs)

    def config_values(self, *args):
        output = self.cli("config", *args, check=True).stdout
        heading, *rows = output.splitlines()
        self.assertEqual(heading.split(), ["SETTING", "VALUE"])
        return dict(row.split() for row in rows)

    def development(self, *args, **kwargs):
        return subprocess.run([IV_DEV_CACHE, *args], capture_output=True, text=True, **kwargs)

    def build_server(self, *args, env):
        env = env | {"IV_DEV_CACHE": str(IV_DEV_CACHE)}
        return subprocess.run([BUILD_SERVER, *args], capture_output=True, text=True, env=env)

    def lock(self, scope, path, mode):
        handle = scope.enter_context(path.open("a"))
        fcntl.flock(handle, mode)
        return handle

    def make_scratch(self, name):
        scratch = self.root / "scratch" / name
        scratch.mkdir(parents=True)
        (scratch / ".use").touch()
        return scratch

    def test_policy_defaults_and_validation(self):
        self.policy()
        recent, _ = self.entry("server/checkout/recent", age=2, size=30 * GIB)
        older, _ = self.entry("server/checkout/older", age=10, size=11 * GIB)
        expired, _ = self.entry("server/checkout/expired", age=31)
        preview = self.cli("gc", "--dry-run", check=True)
        self.assertIn("server/checkout/expired] (idle limit reached)", preview.stdout)
        self.assertIn("server/checkout/older] (least recently used under storage pressure)", preview.stdout)
        self.assertTrue(recent.exists() and older.exists() and expired.exists())
        self.policy(thresholdBytes=20 * GIB)
        invalid_target = self.cli("gc")
        self.assertNotEqual(invalid_target.returncode, 0)
        self.assertIn("targetBytes", invalid_target.stderr)
        self.policy(maxIdleDays=-1)
        invalid_age = self.cli("gc")
        self.assertNotEqual(invalid_age.returncode, 0)
        self.assertIn("maxIdleDays", invalid_age.stderr)

    def test_config_accepts_decimal_and_binary_sizes_and_stores_bytes(self):
        defaults = self.config_values()
        self.assertEqual(defaults, {
            "thresholdBytes": str(40 * GIB), "targetBytes": str(30 * GIB),
            "minimumFreeBytes": str(15 * GIB), "maxIdleDays": "30",
        })
        self.assertEqual(self.config_values("-h"), {
            "thresholdBytes": "40G", "targetBytes": "30G",
            "minimumFreeBytes": "15G", "maxIdleDays": "30",
        })
        self.assertFalse(self.config.exists())
        values = self.config_values(
            "set", "thresholdBytes=10.5g", "targetBytes=10.5GB",
            "minimumFreeBytes=0.0000000005GB", "maxIdleDays=7",
        )
        expected = {
            "thresholdBytes": 11274289152, "targetBytes": 10500000000,
            "minimumFreeBytes": 1, "maxIdleDays": 7,
        }
        self.assertEqual(values, {key: str(value) for key, value in expected.items()})
        self.assertEqual(self.config_values("-h"), {
            "thresholdBytes": "10.5G", "targetBytes": "9.78G",
            "minimumFreeBytes": "1B", "maxIdleDays": "7",
        })
        self.assertEqual(self.config_values("-H"), {
            "thresholdBytes": "11.27GB", "targetBytes": "10.5GB",
            "minimumFreeBytes": "1B", "maxIdleDays": "7",
        })
        self.assertEqual(json.loads(self.config.read_text()), expected)
        self.cli("config", "set", "targetBytes=10.5G", check=True)
        self.assertEqual(json.loads(self.config.read_text())["targetBytes"], expected["thresholdBytes"])
        self.assertFalse(self.root.exists())

    def test_invalid_config_edit_preserves_previous_policy(self):
        self.policy(maxIdleDays=7)
        before = self.config.read_bytes()
        for bad in ("targetBytes=50GiB", "thresholdBytes=18446744073709551616",
                    "minimumFreeBytes=1XB", "maxIdleDays=1.5"):
            with self.subTest(bad=bad):
                result = self.cli("config", "set", "maxIdleDays=4", bad)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.config.read_bytes(), before)

    def test_config_reset_clears_selected_overrides_or_all(self):
        self.cli("config", "set", "targetBytes=10.5GiB", "maxIdleDays=7", check=True)
        self.assertEqual(self.config_values("reset", "maxIdleDays")["maxIdleDays"], "30")
        self.assertEqual(json.loads(self.config.read_text()), {"targetBytes": 11274289152})
        self.config.write_text("invalid json")
        self.assertEqual(self.config_values("reset")["targetBytes"], str(30 * GIB))
        self.assertEqual(json.loads(self.config.read_text()), {})

    def test_config_display_flags_cannot_edit_policy(self):
        self.policy(maxIdleDays=7)
        before = self.config.read_bytes()
        for flag in ("-h", "-H"):
            for operation in (("set", "maxIdleDays=5"), ("reset",)):
                with self.subTest(flag=flag, operation=operation):
                    for arguments in ((flag, *operation), (*operation, flag)):
                        result = self.cli("config", *arguments)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertEqual(self.config.read_bytes(), before)

    def test_pressure_evicts_oldest_to_target(self):
        self.policy(thresholdBytes=4 * GIB, targetBytes=3 * GIB)
        old, _ = self.entry("server/checkout/old", age=3, size=2 * GIB)
        middle, _ = self.entry("shared-packages/package/middle", age=2, size=2 * GIB)
        new, _ = self.entry("project-packages/project/package/new", age=1, size=GIB)
        result = self.cli("gc", check=True)
        self.assertIn("\nBuild workspaces (removed 1, kept 2)\n", result.stdout)
        self.assertIn("\nPackage artifact sets (removed 0, kept 0)\n", result.stdout)
        self.assertIn("server/checkout/old] (least recently used under storage pressure)", result.stdout)
        self.assertIn("shared-packages/package/middle] (within idle limit)", result.stdout)
        self.assertFalse(old.exists())
        self.assertTrue(middle.exists())
        self.assertTrue(new.exists())

    def test_age_and_pressure_respect_workspace_and_writer_leases(self):
        self.policy(thresholdBytes=0, targetBytes=0, maxIdleDays=3)
        read, read_locks = self.entry("server/checkout/reader", age=5, size=GIB)
        write, write_locks = self.entry("server/checkout/writer", age=5, size=GIB)
        expired, _ = self.entry("server/checkout/expired", age=5)
        with ExitStack() as scope:
            self.lock(scope, read_locks / "use", fcntl.LOCK_SH)
            self.lock(scope, write_locks / "write", fcntl.LOCK_EX)
            result = self.cli("gc")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("server/checkout/expired] (idle limit reached)", result.stdout)
            self.assertIn("server/checkout/reader] (workspace is leased)", result.stdout)
            self.assertIn("server/checkout/writer] (workspace writer lock is held)", result.stdout)
            self.assertFalse(expired.exists())
            self.assertTrue(read.exists())
            self.assertTrue(write.exists())
        self.cli("gc", check=True)
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
        with ExitStack() as workspace:
            self.lock(workspace, locks / "use", fcntl.LOCK_SH)
            with ExitStack() as artifact:
                self.lock(artifact, old / ".use", fcntl.LOCK_SH)
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
        result = self.development(
            "scratch", "--", sys.executable, "-c",
            'import os, pathlib, subprocess, sys; '
            'path = pathlib.Path(os.environ["IV_BUILD_SCRATCH_DIR"]); '
            'subprocess.run([sys.argv[1], "cache", "gc"], check=True, stdout=subprocess.DEVNULL); '
            'print(path); sys.exit(37 if path.is_dir() else 1)',
            str(IVCTL))
        self.assertEqual(result.returncode, 37, result.stderr)
        self.assertFalse(Path(result.stdout.strip()).exists())

    def test_abandoned_scratch_is_removed_and_unregistered_content_is_kept(self):
        self.policy()
        scratch = self.make_scratch("run-abandoned")
        unrelated = self.root / "scratch/unrelated"
        unrelated.mkdir()
        result = self.cli("gc", check=True)
        self.assertIn("\nScratch directories (removed 1, kept 0)\n", result.stdout)
        self.assertIn(f"scratch/{scratch.name} (scratch lease ended)", result.stdout)
        self.assertFalse(scratch.exists())
        self.assertTrue(unrelated.exists())

    def test_child_keeps_scratch_after_parent_releases_its_descriptor(self):
        self.policy()
        with subprocess.Popen(
            [IV_DEV_CACHE, "scratch", "--", sys.executable, "-c",
             'import os, sys; print(os.environ["IV_BUILD_SCRATCH_DIR"], flush=True); sys.stdin.read()'],
            text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        ) as parent:
            try:
                scratch = Path(parent.stdout.readline().strip())
                self.assertTrue(scratch.is_relative_to(self.root / "scratch"))
                parent.kill()
                parent.wait()
                self.cli("gc", check=True)
                self.assertTrue(scratch.exists())
            finally:
                parent.stdin.close()
            with ExitStack() as released:
                self.lock(released, scratch / ".use", fcntl.LOCK_EX)
        self.cli("gc", check=True)
        self.assertFalse(scratch.exists())

    def test_preflight_blocks_build_below_free_space_floor(self):
        self.policy(minimumFreeBytes=1000000 * GIB)
        result = self.development("scratch", "--", sys.executable, "-c", 'print("ran")')
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
        (tools / "ninja").symlink_to("/bin/sh")
        env = os.environ | {"PATH": str(tools) + ":" + os.environ["PATH"]}
        command = ("--build-type", "Debug", "--", "/bin/sh", "-c",
                   'cat "$IV_SERVER_BUILD_DIR/CMakeCache.txt"; printf "%s" "$IV_SERVER_BUILD_DIR"')
        first = self.build_server(*command, env=env)
        second = self.build_server(*command, env=env)
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
        (tools / "ninja").symlink_to("/bin/sh")
        env = os.environ | {"PATH": str(tools) + ":" + os.environ["PATH"]}
        command = ("--", "/bin/sh", "-c", 'test -f "$IV_SERVER_BUILD_DIR/build.ninja"')
        failed = self.build_server(*command, env=env | {"FAIL_CONFIGURE": "1"})
        self.assertEqual(failed.returncode, 19, failed.stderr)
        retried = self.build_server(*command, env=env)
        self.assertEqual(retried.returncode, 0, retried.stderr)

    def test_public_cli_does_not_expose_development_commands(self):
        help_text = self.cli("--help", check=True).stdout
        for command in ("scratch", "server"):
            with self.subTest(command=command):
                self.assertNotIn(f"ivctl cache {command}", help_text)
                self.assertNotEqual(self.cli(command).returncode, 0)
        self.assertFalse(self.root.exists())

    def test_path_and_status_do_not_create_cache(self):
        self.assertEqual(self.cli("path", check=True).stdout.strip(), str(self.root))
        status = self.cli("status", check=True)
        self.assertIn("Build workspaces (present 0)", status.stdout)
        self.assertFalse(self.root.exists())

    def test_status_reports_a_lease_without_creating_writer_metadata(self):
        entry, locks = self.entry("server/checkout/live")
        with ExitStack() as lease:
            self.lock(lease, locks / "use", fcntl.LOCK_SH)
            status = self.cli("status", check=True)
        self.assertIn("Build workspaces (present 1)", status.stdout)
        self.assertIn("server/checkout/live] (workspace is leased)", status.stdout)
        self.assertFalse((locks / "write").exists())
        self.assertTrue(entry.exists())

    def test_dry_run_preserves_artifacts_and_lock_metadata(self):
        self.policy()
        workspace, locks = self.entry("project-packages/project/package/profile")
        (workspace / "out/failed").mkdir(parents=True)
        scratch = self.make_scratch("run-abandoned")
        preview = self.cli("gc", "--dry-run", check=True)
        self.assertIn("Package artifact sets (would remove 1, would keep 0)", preview.stdout)
        self.assertIn("Scratch directories (would remove 1, would keep 0)", preview.stdout)
        self.assertTrue(scratch.exists())
        self.assertTrue((workspace / "out/failed").exists())
        self.assertEqual(sorted(path.name for path in locks.iterdir()), ["last-used", "source"])
        self.assertEqual(list((workspace / "out/failed").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
