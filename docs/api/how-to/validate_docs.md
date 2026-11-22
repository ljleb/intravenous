# Validate API documentation

Run the API documentation check in the development shell.

```sh
cmake --build build-release --target intravenous_api_docs_check
```

The check follows local links across the authoring guides, extracts and
compiles the C tutorial and the ABI layout example, rebuilds both C++
presentation examples, and runs the native tutorial through Clang source
introspection and LLVM finalization. Add a
`<!-- check-example: filename -->` comment directly above an executable
code fence to keep the displayed source as the test input.
[Check script](../../../scripts/check_api_docs.py) ·
[Development setup](../../development_run.md)

Use [DocDiff](https://github.com/StevenBock/docdiff) to detect source
changes that leave an associated document untouched. The `@doc` source
annotations associate the SDK, host adapter, and LLVM boundary with their
reference pages. Run [DocDiff v2.0.0](https://github.com/StevenBock/docdiff/releases/tag/v2.0.0)
from the repository root.

```sh
docdiff check
```

To run both checks together, supply its executable path.

```sh
python3 scripts/check_api_docs.py --docdiff "$(command -v docdiff)"
```

Use `docdiff report --stale` to inspect committed changes. When the source
changes, update the relevant reference and re-run the compiled examples.
If the documented contract still holds, review the diff before using
`docdiff ack` to record that review. DocDiff checks change history and the
ownership links. The compiled examples check that the documented code still
works with the current headers and toolchain.
