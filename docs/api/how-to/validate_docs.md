# Validate API documentation

Run the API documentation check in the development shell.

```sh
cmake --build build-release --target intravenous_api_docs_check
```

The check follows local links across the authoring guides, extracts and
compiles the C tutorial and the ABI layout example, rebuilds the C++ marker,
Click Detector, and Beats presentations, and runs the native tutorial through
Clang source introspection and LLVM finalization. Add a
`<!-- check-example: filename -->` comment directly above an executable
code fence to keep the displayed source as the test input.
[Check script](../../../scripts/check_api_docs.py) ·
[Development setup](../../development_run.md)
