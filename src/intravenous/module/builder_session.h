#pragma once

#include <intravenous/module/abi.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/node/config_relocations.h>
#include <intravenous/node/node_state_structure.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace iv {
class GraphBuilder;
class GraphBuilderState;
struct ConfiguredGraph;

namespace details {
// All package-specific data needed while configuring a graph. The loader builds
// these views from the IV packages that are loaded for the configuration; the
// BuilderSession copies the records so nested iv-module calls use one stable
// lookup set even if other packages are reloaded concurrently.
struct BuilderNodeStateStructure {
    NodeCodeKey code_key{};
    NodeStateStructure structure{};
};

struct BuilderPackageView {
    std::string_view package_root{};
    std::span<PackageDefinition const> definitions{};
    std::span<NodeConfigPointerFieldData const> config_pointer_fields{};
    std::span<RetainedGlobalData const> retained_globals{};
    std::span<BuilderNodeStateStructure const> node_state_structures{};
};

struct BuilderDefinition {
    PackageDefinition definition{};
    std::size_t package_index = 0;
};

struct BuilderSession;

extern "C" BuilderSession* iv_builder_session_create();
extern "C" void iv_builder_session_destroy(BuilderSession*) noexcept;

// A child session owns an independent GraphBuilderState and node-configuration
// allocations but shares the loaded-package tables and iv-module call stack.
BuilderSession* iv_builder_child_session_create(
    BuilderSession* parent, std::size_t package_index);

ConfiguredGraph take_built_graph(BuilderSession*);

void set_builder_packages(
    BuilderSession*, std::span<BuilderPackageView const> packages);
std::size_t builder_package_index(
    BuilderSession const*, std::string_view package_root);
std::size_t builder_selected_package(BuilderSession const*) noexcept;
void restore_builder_package(BuilderSession*, std::size_t package_index) noexcept;
void select_builder_package(BuilderSession*, std::size_t package_index);
BuilderDefinition find_builder_definition(
    BuilderSession const*, std::string_view id);
std::vector<std::size_t> builder_used_packages(BuilderSession const*);
void begin_builder_module(BuilderSession*, std::string_view id);
void end_builder_module(BuilderSession*) noexcept;

NodeConfigRelocations capture_node_config(
    BuilderSession*, NodeCodeKey, void const*, std::size_t);
std::shared_ptr<NodeStateStructure const> copy_builder_node_state_structure(
    BuilderSession*, NodeCodeKey);

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
GraphBuilderState& builder_graph_state(GraphBuilder&);

}
} // namespace iv
