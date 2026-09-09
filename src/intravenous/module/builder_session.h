#pragma once

#include <intravenous/node/code_key.h>
#include <intravenous/node/config_relocations.h>

#include <cstddef>
#include <span>

namespace iv {
class GraphBuilder;
class GraphBuilderState;
struct AuthoredGraph;

namespace details {
// Compiler-derived layout facts for one node configuration type. This is an
// implementation boundary between the finalizer and the precompiled builder;
// node definitions neither provide nor see it.
struct NodeConfigLayout {
    NodeCodeKey node_code_key{};
    std::span<std::size_t const> c_string_offsets{};
};

// The host creates this opaque session before running a module's temporary ORC
// build generation. The generation may borrow its GraphBuilder only while
// `iv_module_build` is executing; the completed AuthoredGraph remains host
// owned and is taken before that generation is released.
struct BuilderSession;

extern "C" BuilderSession* iv_builder_session_create();
extern "C" void iv_builder_session_destroy(BuilderSession*) noexcept;

AuthoredGraph take_built_graph(BuilderSession*);

// The compiler supplies C-string field offsets for every node type emitted by
// this build. The session owns a copy before module code executes. Capturing a
// node configuration then copies every non-null `char const*` field while its
// source generation is still live.
void set_builder_node_config_layouts(
    BuilderSession*, std::span<NodeConfigLayout const>);
NodeConfigStringRelocations capture_node_config(
    BuilderSession*, NodeCodeKey, void const*, std::size_t);

// Private bridge used by GraphBuilder's out-of-line facade implementation.
// It is intentionally not a module builder API.
GraphBuilderState& builder_graph_state(GraphBuilder&);

}
} // namespace iv
