# Build and diagnose a native node package

## Compile outside the editor

Build Intravenous with the matching SDK and toolchain. Pass your package
directory, a writable scratch artifact directory, and the Intravenous
source root to the prebuild tool. Use absolute paths for inputs resolved by
another process.

```sh
cmake --build build-release --target iv_package_prebuild
build-release/package_tools/iv_package_prebuild "$package" "$artifacts" "$PWD"
```

A successful build prints the path to the finalized LLVM artifact. The
prebuild tool uses the same compiler and finalizer as package loading.
[Prebuild tool](../../../package_tools/iv_package_prebuild.cpp)

## Diagnose a registration or scheduling error

1. Check `iv_package.json` for the entry filename, package namespace, and
   dependencies. Use the provider's namespace for registered IDs.
2. Read the first Clang or source-introspection error. For leaves, compare
   the callback, access kinds, and production kinds with the
   [registration constraints](../reference/llvm_nodes.md).
3. If the package compiles but the configured graph fails, inspect the
   connection's type, channel layout, access, and resolved timing. These
   depend on the full graph.
4. For a Tock output, verify that forward propagation publishes the intended
   coverage. Verify that reverse propagation requests what the callback
   reads. Iterate the prepared runs in the callback.
5. For a Tick replacement, inspect how the `State` type moves or reconstructs
   its data. Test playback while the graph switches.

The [automation implementation](../../../src/intravenous/basic_nodes/automation.h)
shows both forward and reverse propagation and a named Tock callback.
[Coverage and background evaluation](../../coverage_and_background_evaluation.md)
