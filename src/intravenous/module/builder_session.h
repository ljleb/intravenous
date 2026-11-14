#pragma once

#include <intravenous/module/abi.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/node/config_relocations.h>
#include <intravenous/node/address.h>
#include <intravenous/node/message.h>
#include <intravenous/node/node_state_structure.h>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph/builder/mark.h>
#include <intravenous/graph/builder/subgraphs.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iv {
class GraphBuilder;
class GraphBuilderState;
class NodeRef;
struct ConfiguredGraph;

namespace details {
// All package-specific data needed while configuring a graph. The loader builds
// these views from the IV packages that are loaded for the configuration; the
// BuilderSession copies the records so nested iv-module calls use one stable
// lookup set even if other packages are reloaded concurrently.
struct BuilderNodeStateStructures {
    NodeCodeKey code_key{};
    NodeStateStructures structures{};
};

struct BuilderPackageView {
    std::string_view package_root{};
    std::span<PackageDefinition const> definitions{};
    std::span<NodeConfigPointerFieldData const> config_pointer_fields{};
    std::span<RetainedGlobalData const> retained_globals{};
    std::span<NodeMessageHandlerData const> node_message_handlers{};
    std::span<ModuleMessageHandlerData const> module_message_handlers{};
    std::span<BuilderNodeStateStructures const> node_state_structures{};
};

struct BuilderDefinition {
    PackageDefinition definition{};
    std::size_t package_index = 0;
};

struct BuilderSession;

// `relative_path` is the semantic path of this registered call relative to
// the innermost enclosing invocation: the current tiled-member scope path,
// then the call's declaration and its member index among calls of that
// declaration.
using BuilderDefinitionResolver = NodeRef (*)(
    void* context,
    GraphBuilder&,
    std::string_view id,
    std::span<ProjectNodePathSelector const> relative_path,
    std::optional<ChannelLayout> tiled_layout,
    std::span<ConfigurationArgument> arguments);

using BuilderConcreteNodeReplacementResolver = std::optional<NodeBundleHandle> (*)(
    void* context,
    GraphBuilder&,
    std::span<ProjectNodePathSelector const> relative_path);

void set_builder_definition_resolver(
    BuilderSession*, void* context, BuilderDefinitionResolver resolver) noexcept;
void set_builder_concrete_node_replacement_resolver(
    BuilderSession*,
    BuilderConcreteNodeReplacementResolver resolver) noexcept;
BuilderDefinitionResolver builder_definition_resolver(BuilderSession const*) noexcept;
void* builder_definition_resolver_context(BuilderSession const*) noexcept;
struct BuilderNodeSourceIdentity {
    std::string source_identity{};
    std::string authored_name{};
};
void prepare_builder_node_source_identity(
    BuilderSession*,
    std::string_view source_identity,
    std::string_view authored_name);
[[nodiscard]] BuilderNodeSourceIdentity
consume_builder_node_source_identity(BuilderSession*);
[[nodiscard]] std::span<ProjectNodePathSelector const>
builder_semantic_scope_path(BuilderSession const*) noexcept;
// Claims the semantic path of one registered call made in the current
// semantic frame: scope path, declaration, and the next member index of that
// declaration.
[[nodiscard]] std::vector<ProjectNodePathSelector>
claim_builder_registered_call_path(
    BuilderSession*, std::string_view source_identity);
// The virtual-record qualifier of a placement at `relative_path`.
[[nodiscard]] std::string builder_invocation_qualifier(
    std::span<ProjectNodePathSelector const> relative_path);
void record_builder_module_message_local_ref(
    BuilderSession*, BuilderNodeSourceIdentity const&);
void register_builder_module_class_message_handlers(
    GraphBuilder&, std::shared_ptr<void const> receiver);
[[nodiscard]] std::vector<std::vector<ProjectNodePathSelector>>
claim_builder_concrete_node_paths(
    BuilderSession*, std::optional<ChannelLayout> tiled_layout = std::nullopt);
[[nodiscard]] std::optional<NodeBundleHandle>
resolve_builder_concrete_node_replacement(
    BuilderSession*,
    GraphBuilder&,
    std::span<ProjectNodePathSelector const> relative_path);
void register_builder_concrete_node_message_handlers(
    BuilderSession*,
    NodeCodeKey,
    std::shared_ptr<void const> receiver,
    std::span<ProjectNodePathSelector const> relative_path);
void record_builder_node_message_path(
    BuilderSession*,
    NodeBundleHandle,
    std::span<ProjectNodePathSelector const> relative_path);
void attach_builder_node_message_path(
    BuilderSession*, NodeRef&, NodeBundleHandle);
void register_builder_embedded_message_handlers(
    BuilderSession*,
    std::span<ProjectNodePathSelector const> prefix,
    std::span<ConfiguredNodeMessageHandler const> handlers);
void register_builder_embedded_message_handlers(
    GraphBuilder&,
    std::span<ProjectNodePathSelector const> prefix,
    std::span<ConfiguredNodeMessageHandler const> handlers);
[[nodiscard]] std::vector<ConfiguredNodeMessageHandler>
take_builder_message_handlers(BuilderSession*);

extern "C" BuilderSession* iv_builder_session_create();
extern "C" void iv_builder_session_destroy(BuilderSession*) noexcept;

ConfiguredGraph take_built_graph(BuilderSession*);

// ---- construction failure ------------------------------------------------
// A construction failure is never recovered inside its session. The first one
// is recorded on the session; from then on the session refuses every further
// construction operation, and the configuration pass that owns it fails.
// Library boundaries that run authored code record an escaping exception and
// add one context line each as it propagates outwards, innermost first.
void record_builder_failure(
    BuilderSession*, std::exception_ptr failure, std::string_view context) noexcept;
[[nodiscard]] bool builder_failed(BuilderSession const*) noexcept;
// The recorded failure's message followed by its context lines.
[[nodiscard]] std::string builder_failure_message(BuilderSession const*);
// The stack captured where the failure was first recorded.
[[nodiscard]] std::string builder_failure_stacktrace(BuilderSession const*);
// Throws the recorded failure as a std::logic_error when there is one. Used
// by boundaries after authored code returned normally, so a failure it caught
// still fails the pass.
void throw_if_builder_failed(BuilderSession*);

[[nodiscard]] BuilderScopeId begin_builder_scope(BuilderSession*, BuilderScopeKind);
// `qualifier` applies to a definition scope's records; std::nullopt selects
// the handle-relative qualifier used for nested live GraphBuilder scopes.
NodeBundleHandle finish_builder_scope(
    BuilderSession*, BuilderScopeId, std::string_view kind,
    std::optional<std::string_view> qualifier);
// A scope that is destroyed or abandoned while still open fails its session.
void abandon_builder_scope(BuilderSession*, BuilderScopeId) noexcept;
[[nodiscard]] bool builder_scope_sample_outputs_defined(
    BuilderSession*, BuilderScopeId);
void validate_builder_tiled_module_interfaces(
    BuilderSession*, std::span<NodeBundleHandle const> member_subgraphs);

// One module body: its semantic frame inherits the enclosing scope path plus
// `scope_suffix` and starts fresh declaration counters and local refs.
void begin_builder_module_body(
    BuilderSession*, std::span<ProjectNodePathSelector const> scope_suffix);
void end_builder_module_body(BuilderSession*) noexcept;

// One registered-definition invocation. Its semantic frame starts at the
// invocation root (an empty scope path). Finishing it returns the message
// handlers registered inside it with paths relative to the invocation root
// and removes them from the session, optionally copies its contents into a
// closed graph for reuse, and then qualifies its virtual records.
struct BuilderInvocation {
    GraphBuilderMark mark{};
    std::size_t message_handler_begin = 0;
    std::size_t semantic_depth = 0;
};
struct BuilderInvocationResult {
    std::vector<ConfiguredNodeMessageHandler> message_handlers{};
    // Null when no snapshot was requested or the contents are not
    // independently representable (a free graph capture).
    std::shared_ptr<ConfiguredGraph const> snapshot{};
    NodeBundleHandle snapshot_root = 0;
};
[[nodiscard]] BuilderInvocation begin_builder_invocation(BuilderSession*);
BuilderInvocationResult finish_builder_invocation(
    BuilderSession*, BuilderInvocation, NodeBundleHandle result,
    std::string_view qualifier, bool snapshot);
// Places a closed invocation graph into the current scope as a hit for the
// same invocation. Returns the live handle of `local_root`.
NodeBundleHandle import_builder_invocation(
    BuilderSession*, ConfiguredGraph const&, NodeBundleHandle local_root,
    std::string_view qualifier);

void set_builder_packages(
    BuilderSession*, std::span<BuilderPackageView const> packages);
std::size_t builder_package_index(
    BuilderSession const*, std::string_view package_root);
std::size_t builder_selected_package(BuilderSession const*) noexcept;
void restore_builder_package(BuilderSession*, std::size_t package_index) noexcept;
void select_builder_package(BuilderSession*, std::size_t package_index);
// The built-in definition a bare ID (one without a '.') also names:
// `iv.builtin.<id>`. Every definition lookup tries the exact ID first and this
// alias second, so built-in names are imported into authored code.
[[nodiscard]] std::optional<std::string> builtin_definition_alias(std::string_view id);
BuilderDefinition find_builder_definition(
    BuilderSession const*, std::string_view id);
std::vector<std::size_t> builder_used_packages(BuilderSession const*);
void begin_builder_module(BuilderSession*, std::string_view id);
void end_builder_module(BuilderSession*) noexcept;

NodeConfigRelocations capture_node_config(
    BuilderSession*, NodeCodeKey, void const*, std::size_t);
std::shared_ptr<NodeStateStructures const> copy_builder_node_state_structures(
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

// The graph tables of a session, for libiv_builder and host inspection.
GraphBuilderState& builder_session_state(BuilderSession*);
GraphBuilderState& builder_graph_state(GraphBuilder&);
// A GraphBuilder used directly by host/lowering code has no package table.
// Source-configured package builders always have one, including the shipped
// built-in package, so source sugar can resolve its registered IDs normally.
bool builder_session_has_packages(BuilderSession const*) noexcept;

}
} // namespace iv
