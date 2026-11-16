# Test workspaces

Mutable fixtures use stable directories in the configured CMake build tree:

```text
test_runtime_modules/
  <test executable>/
    <test suite>/
      <test name>/
        <fixture name>/
```

The shared GoogleTest main resets only the current case's directory before the
case runs. Parallel CTest cases therefore use different directories. Rerunning a
case, including with `--gtest_repeat`, replaces its previous files. The latest
files remain after success or failure for inspection. Fixture names have no PID
or source-line suffix. Names containing path separators or punctuation are
escaped without merging distinct names.

Standalone test executables use one stable directory per executable and reset it
on the first workspace request. Separate concurrent runs of the same test should
use separate CMake build directories.

Shared package fixtures live in `test_shared_fixtures/`. Their existing locks and
source/build fingerprints control updates and reuse. They must survive the CTest
setup process because dependent test processes load them afterward.

Editor file watching excludes generated build directories in the repository's
VS Code settings. This also avoids spending file watches on current build
artifacts while tests run.
