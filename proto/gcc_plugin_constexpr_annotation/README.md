# GCC plugin constexpr annotation probe

This probe tests whether `PLUGIN_FINISH_PARSE_FUNCTION` can rewrite a source
expression before a later constant evaluation consumes that function.

The plugin replaces the argument to `iv_plugin_probe_mark(0)` with the call's
source line. `probe.cpp` then checks that value in a `static_assert`. A build
without the plugin must fail; the same build with the plugin must succeed.

`wrapper_probe.cpp` is the closer architectural test. Its user-authored header
contains an ordinary builder-style node call with no annotation placeholder.
The plugin replaces that call with `iv_plugin_probe_annotate(original_call,
source_location)` in both GCC function-body representations. Its
`static_assert`s prove that the included header path and expression range were
transformed before constant evaluation.

`real_graph_probe.cpp` uses the repository's actual `GraphBuilder::node`,
`TypedNodeRef`, source annotation, virtual-node metadata, and consteval graph
metadata path. It is the integration threshold for considering the prototype
applicable to Intravenous rather than only to isolated C++ trees.

This is only the timing prerequisite for replacing the Clang source rewriter.
It does not yet prove that the plugin can reproduce expression spans, binding
identity, public-output argument spans, or `Node::State` layout metadata.
