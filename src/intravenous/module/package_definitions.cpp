#include <intravenous/module/package_definitions.h>

#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/state.h>
#include <intravenous/module/builder_session.h>

#include <memory>
#include <optional>
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

NodeRef configure_package_definition_provider(
    GraphBuilder& builder,
    std::string_view id,
    std::optional<ChannelLayout> tiled_layout,
    std::span<ConfigurationArgument> arguments)
{
    if (!builder._session) {
        throw std::logic_error("IV package definition requires a BuilderSession");
    }
    auto const found = find_builder_definition(builder._session, id);
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
        PackageSelection const package(builder._session, found.package_index);
        auto node = definition.node_build(
            builder, arguments,
            tiled_layout ? std::addressof(*tiled_layout) : nullptr);
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

    ModuleStackEntry const stack_entry(builder._session, registered_id);
    using ChildSession = std::unique_ptr<BuilderSession,
        decltype(&iv_builder_session_destroy)>;
    auto make_child_session = [&] {
        return ChildSession(
                iv_builder_child_session_create(builder._session, found.package_index),
                iv_builder_session_destroy);
    };
    if (tiled_layout) {
        auto const member_count = channel_count(tiled_layout->channel_type);
        std::vector<ChildSession> child_sessions;
        std::vector<std::unique_ptr<GraphBuilder>> children;
        child_sessions.reserve(member_count);
        children.reserve(member_count);
        for (std::size_t channel = 0; channel < member_count; ++channel) {
            child_sessions.push_back(make_child_session());
            auto child = std::make_unique<GraphBuilder>(child_sessions.back().get());
            definition.module_build(*child, arguments);
            children.push_back(std::move(child));
        }
        std::vector<GraphBuilder*> child_views;
        child_views.reserve(children.size());
        for (auto const& child : children) child_views.push_back(child.get());
        iv_builder_validate_tiled_module_interfaces(child_views);

        std::vector<NodeBundleHandle> members;
        members.reserve(children.size());
        for (auto const& child : children) {
            members.push_back(
                builder.embed_child(*child, "IV module definition").node_bundle_handle());
        }
        return NodeRef(builder, iv_builder_append_tiled_node_bundles(
            builder,
            {members.data(), members.size()},
            *tiled_layout));
    }
    auto child_session = make_child_session();
    GraphBuilder child(child_session.get());
    definition.module_build(child, arguments);
    return builder.embed_child(child, "IV module definition");
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
    if (auto resolver = builder_definition_resolver(builder._session)) {
        return resolver(
            builder_definition_resolver_context(builder._session),
            builder,
            id,
            tiled_layout,
            arguments);
    }
    return configure_package_definition_provider(
        builder, id, tiled_layout, arguments);
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
