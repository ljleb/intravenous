#include <intravenous/module/package_definitions.h>

#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/state.h>
#include <intravenous/module/builder_session.h>

#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace iv::details {
namespace {
class ModuleStackEntry {
    BuilderSession* session_ = nullptr;
public:
    ModuleStackEntry(BuilderSession* session, std::string_view id)
        : session_(session)
    {
        begin_builder_module(session_, id);
    }
    ModuleStackEntry(ModuleStackEntry const&) = delete;
    ModuleStackEntry& operator=(ModuleStackEntry const&) = delete;
    ~ModuleStackEntry() { end_builder_module(session_); }
};

class PackageSelection {
    BuilderSession* session_ = nullptr;
    std::size_t previous_ = static_cast<std::size_t>(-1);
public:
    PackageSelection(BuilderSession* session, std::size_t package_index)
        : session_(session), previous_(builder_selected_package(session))
    {
        select_builder_package(session_, package_index);
    }
    PackageSelection(PackageSelection const&) = delete;
    PackageSelection& operator=(PackageSelection const&) = delete;
    ~PackageSelection()
    {
        restore_builder_package(session_, previous_);
    }
};
} // namespace

namespace {
NodeRef construct_package_definition(
    GraphBuilder& builder,
    BuilderSession* session,
    std::string_view id,
    std::optional<ChannelLayout> tiled_layout,
    std::span<ConfigurationArgument> arguments)
{
    auto const found = find_builder_definition(session, id);
    auto const& definition = found.definition;
    auto const registered_id = std::string_view(definition.id, definition.id_size);
    if (!definition.signature) {
        throw std::logic_error(
            "registered definition '" + std::string(id)
            + "' has no construction signature");
    }
    auto const* signature = definition.signature();
    if (!signature) {
        throw std::logic_error(
            "registered definition '" + std::string(id)
            + "' returned no construction signature");
    }
    validate_registered_signature(registered_id, *signature, arguments);
    if (definition.kind == PackageDefinitionKind::node) {
        PackageSelection const package(session, found.package_index);
        auto node = definition.node_build(
            builder, arguments,
            tiled_layout ? std::addressof(*tiled_layout) : nullptr);
        throw_if_builder_failed(session);
        auto const provider_package_root = std::string_view(
            definition.package_root, definition.package_root_size);
        builder_graph_state(builder).set_registered_node_type_identity(
            node.node_bundle_handle(),
            RegisteredNodeTypeIdentity{
                .node_type_id = std::string(registered_id),
                .provider_package_root = std::string(provider_package_root),
            });
        return node;
    }

    ModuleStackEntry const stack_entry(session, registered_id);
    PackageSelection const package(session, found.package_index);
    // One module body: a definition scope of the caller's session, constructed
    // in place and finalized without copying. `member_suffix` names a tiled
    // member inside the invocation.
    auto construct_body = [&](std::span<ProjectNodePathSelector const> member_suffix,
                              bool tiled_member) {
        auto const scope = begin_builder_scope(session, BuilderScopeKind::definition);
        begin_builder_module_body(session, member_suffix);
        GraphBuilder body(session, scope);
        definition.module_build(body, arguments);
        // A failure that the module body caught still fails the pass.
        throw_if_builder_failed(session);
        if (tiled_member && !builder_scope_sample_outputs_defined(session, scope)) {
            error("tiled IV module must call g.outputs(...) before it can be configured");
        }
        end_builder_module_body(session);
        auto const qualifier = member_suffix.empty()
            ? std::string{}
            : builder_invocation_qualifier(member_suffix);
        return finish_builder_scope(
            session, scope, "module", qualifier);
    };
    if (!tiled_layout) {
        return NodeRef(session, construct_body({}, false));
    }

    // Every tiled member is constructed and the member interfaces validated
    // before the tiled bundle exists. A failure fails the pass, so no partial
    // tile is ever published.
    auto const member_count = channel_count(tiled_layout->channel_type);
    std::vector<NodeBundleHandle> members;
    members.reserve(member_count);
    for (std::size_t channel = 0; channel < member_count; ++channel) {
        auto const& channel_name =
            channel_type_info(tiled_layout->channel_type).channel_names[channel];
        ProjectNodePathSelector const selector = ProjectTiledChildSelector{
            .name = std::string(channel_name),
        };
        members.push_back(construct_body(std::span{&selector, 1}, true));
    }
    validate_builder_tiled_module_interfaces(session, members);
    return NodeRef(session, iv_builder_append_tiled_node_bundles(
        builder,
        {members.data(), members.size()},
        *tiled_layout));
}
} // namespace

// The library boundary around one leaf or module factory. Whatever escapes
// it, from the library or from authored code, is recorded as the session's
// construction failure before it reaches the caller.
NodeRef configure_package_definition_provider(
    GraphBuilder& builder,
    std::string_view id,
    std::optional<ChannelLayout> tiled_layout,
    std::span<ConfigurationArgument> arguments)
{
    if (!builder._session) {
        throw std::logic_error("IV package definition requires a BuilderSession");
    }
    try {
        return construct_package_definition(
            builder, builder._session, id, tiled_layout, arguments);
    } catch (...) {
        record_builder_failure(
            builder._session, std::current_exception(),
            "while configuring '" + std::string(id) + "'");
        throw;
    }
}

NodeRef configure_package_definition_impl(
    GraphBuilder& builder,
    std::string_view id,
    std::optional<ChannelLayout> tiled_layout,
    std::span<ConfigurationArgument> arguments)
{
    if (!builder._session) {
        throw std::logic_error("IV package definition requires a BuilderSession");
    }
    auto* const session = builder._session;
    auto const prepared = consume_builder_node_source_identity(session);
    try {
        record_builder_module_message_local_ref(session, prepared);
        auto const relative_path = claim_builder_registered_call_path(
            session, prepared.source_identity);
        if (auto resolver = builder_definition_resolver(session)) {
            return resolver(
                builder_definition_resolver_context(session),
                builder,
                id,
                relative_path,
                tiled_layout,
                arguments);
        }

        // Without a configuration cache the call is still one invocation of
        // the caller's session: its records are qualified by its semantic path
        // and its message handlers are re-rooted at the caller.
        auto const invocation = begin_builder_invocation(session);
        auto constructed = configure_package_definition_provider(
            builder, id, tiled_layout, arguments);
        auto finished = finish_builder_invocation(
            session, invocation, constructed.node_bundle_handle(),
            builder_invocation_qualifier(relative_path), false);
        register_builder_embedded_message_handlers(
            session, relative_path, finished.message_handlers);
        return constructed;
    } catch (...) {
        // A registered call is where authored module code sees the failure;
        // record it before the caller can catch it.
        record_builder_failure(session, std::current_exception(), {});
        throw;
    }
}

NodeRef configure_package_definition(
    GraphBuilder& builder,
    std::string_view id,
    std::span<ConfigurationArgument> arguments)
{
    return configure_package_definition_impl(builder, id, std::nullopt, arguments);
}

NodeRef configure_tiled_package_definition(
    GraphBuilder& builder,
    std::string_view id,
    ChannelLayout layout,
    std::span<ConfigurationArgument> arguments)
{
    return configure_package_definition_impl(builder, id, layout, arguments);
}
} // namespace iv::details
