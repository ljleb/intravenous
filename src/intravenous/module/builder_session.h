#pragma once

#include <intravenous/node/code_key.h>
#include <intravenous/node/config_relocations.h>

#include <cstddef>
#include <memory>
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
    std::span<std::size_t const> pointer_offsets{};
};

// Addresses materialized by the temporary authoring JIT. `symbol` is an
// opaque finalizer handle for the corresponding master-module global.
struct AuthoringGlobalAddress {
    void const* address = nullptr;
    std::size_t size = 0;
    void const* symbol = nullptr;
};

// The host creates this opaque session before running a module's temporary ORC
// build generation. The generation may borrow its GraphBuilder only while
// `iv_source_build_registered_module` is executing; the completed
// AuthoredGraph remains host owned and is taken before that generation is
// released.
struct BuilderSession;

extern "C" BuilderSession* iv_builder_session_create();
extern "C" void iv_builder_session_destroy(BuilderSession*) noexcept;

AuthoredGraph take_built_graph(BuilderSession*);

// The compiler supplies pointer field offsets for every node type emitted by
// this build. The finalizer also supplies the JIT addresses of retained
// immutable globals. Capturing a config converts each pointer slot into a
// symbolic relocation while the authoring generation is still live.
void set_builder_node_config_layouts(
    BuilderSession*, std::span<NodeConfigLayout const>);
void set_builder_authoring_globals(
    BuilderSession*, std::span<AuthoringGlobalAddress const>);
NodeConfigRelocations capture_node_config(
    BuilderSession*, NodeCodeKey, void const*, std::size_t);

// Module-side node constructors request storage from the shared builder and
// placement-construct directly into it. Ownership transfers synchronously to
// `take_builder_node_config` when GraphBuilder appends the node.
void* iv_builder_allocate_node_config(
    BuilderSession*, std::size_t size, std::size_t alignment);
void iv_builder_discard_node_config(
    BuilderSession*, void* storage) noexcept;
std::shared_ptr<void const> take_builder_node_config(
    BuilderSession*, void const* storage, std::size_t size, std::size_t alignment);

// Private bridge used by GraphBuilder's out-of-line facade implementation.
// It is intentionally not a module builder API.
GraphBuilderState& builder_graph_state(GraphBuilder&);

}
} // namespace iv
