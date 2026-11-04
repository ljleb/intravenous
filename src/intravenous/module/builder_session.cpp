#include <intravenous/module/builder_session.h>

#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/state.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iv::details {
namespace {
using ConfigurationJson = nlohmann::ordered_json;

[[nodiscard]] ConfigurationJson configuration_node_address_json_value(
    NodeAddress const& value)
{
    ConfigurationJson path = ConfigurationJson::array();
    for (auto const& selector : value.path) {
        path.push_back(std::visit([](auto const& concrete) -> ConfigurationJson {
            using Selector = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Selector, ProjectVirtualNodeSelector>) {
                ConfigurationJson result{
                    {"kind", "virtualNode"},
                    {"sourceIdentity", concrete.source_identity},
                };
                if (concrete.type_identity) {
                    result["typeIdentity"] = *concrete.type_identity;
                }
                return result;
            } else if constexpr (std::same_as<
                                     Selector,
                                     ProjectVirtualMemberSelector>) {
                return ConfigurationJson{
                    {"kind", "virtualMember"},
                    {"index", concrete.index},
                };
            } else {
                return ConfigurationJson{
                    {"kind", "tiledChild"},
                    {"name", concrete.name},
                };
            }
        }, selector));
    }
    return ConfigurationJson{
        {"instanceId", value.instance_id},
        {"path", std::move(path)},
    };
}

[[nodiscard]] ConfigurationJson configuration_schema_json_value_impl(
    void const* data,
    ConfigurationExpressionTypeSchema const& schema)
{
    using Kind = ConfigurationExpressionTypeKind;
    if (!data) {
        throw std::invalid_argument(
            "replacement configuration argument has no value");
    }
    if (schema.kind == Kind::aggregate) {
        ConfigurationJson result = ConfigurationJson::object();
        for (auto const& field : std::span(schema.fields, schema.field_count)) {
            if (!field.name || !field.type) {
                throw std::invalid_argument(
                    "replacement aggregate has incomplete generated metadata");
            }
            auto const* field_data = static_cast<std::byte const*>(data)
                + field.byte_offset;
            result[std::string(field.name, field.name_size)] =
                configuration_schema_json_value_impl(field_data, *field.type);
        }
        return result;
    }
    if (schema.kind == Kind::sequence) {
        if (!schema.element_type || !schema.element_count || !schema.element_at) {
            throw std::invalid_argument(
                "replacement sequence has incomplete generated read metadata");
        }
        ConfigurationJson result = ConfigurationJson::array();
        auto const size = schema.element_count(data);
        for (std::size_t index = 0; index < size; ++index) {
            result.push_back(configuration_schema_json_value_impl(
                schema.element_at(data, index), *schema.element_type));
        }
        return result;
    }
    if (schema.kind == Kind::node_address) {
        return configuration_node_address_json_value(
            *static_cast<NodeAddress const*>(data));
    }
    if (schema.kind == Kind::node_reference) {
        throw std::invalid_argument(
            "a replacement argument cannot contain a live NodeRef; use NodeAddress");
    }
    if (!schema.read_scalar) {
        throw std::invalid_argument(
            "replacement argument has no generated scalar read operation");
    }
    ConfigurationExpressionScalar scalar;
    schema.read_scalar(data, scalar);
    if (schema.kind == Kind::enumeration) {
        auto const enumerators = std::span(
            schema.enumerators, schema.enumerator_count);
        auto const found = std::ranges::find_if(
            enumerators,
            [&](ConfigurationExpressionEnumeratorSchema const& enumerator) {
                return scalar.kind == ConfigurationExpressionScalarKind::signed_integer
                    ? enumerator.signed_value == scalar.signed_integer
                    : scalar.kind
                            == ConfigurationExpressionScalarKind::unsigned_integer
                    && enumerator.unsigned_value == scalar.unsigned_integer;
            });
        if (found == enumerators.end()) {
            throw std::invalid_argument(
                "replacement enum value is not a declared enumerator");
        }
        return ConfigurationJson(std::string(found->name, found->name_size));
    }
    switch (scalar.kind) {
    case ConfigurationExpressionScalarKind::boolean:
        return ConfigurationJson(scalar.boolean);
    case ConfigurationExpressionScalarKind::signed_integer:
        return ConfigurationJson(scalar.signed_integer);
    case ConfigurationExpressionScalarKind::unsigned_integer:
        return ConfigurationJson(scalar.unsigned_integer);
    case ConfigurationExpressionScalarKind::floating_point:
        if (!std::isfinite(scalar.floating_point)) {
            throw std::invalid_argument(
                "replacement floating-point argument is not finite");
        }
        return ConfigurationJson(scalar.floating_point);
    case ConfigurationExpressionScalarKind::string:
        if (!scalar.string_data && scalar.string_size != 0) {
            throw std::invalid_argument(
                "replacement string argument has invalid generated data");
        }
        return ConfigurationJson(std::string(
            scalar.string_data ? scalar.string_data : "",
            scalar.string_size));
    }
    throw std::logic_error("unknown replacement scalar representation");
}

struct BuilderPackage {
    std::string package_root{};
    std::vector<PackageDefinition> definitions{};
    std::vector<NodeConfigPointerFieldData> config_pointer_fields{};
    std::vector<RetainedGlobalData> retained_globals{};
    std::vector<NodeMessageHandlerData> node_message_handlers{};
    std::vector<ModuleMessageHandlerData> module_message_handlers{};
    std::vector<BuilderNodeStateStructures> node_state_structures{};
};

struct BuilderConfiguration {
    std::vector<BuilderPackage> packages{};
    std::vector<std::string> module_stack{};
    std::vector<bool> used_packages{};
    std::vector<ConfiguredNodeMessageHandler> message_handlers{};
};

void validate_definition(PackageDefinition const& definition)
{
    if (!definition.id || definition.id_size == 0) {
        throw std::invalid_argument("IV package definition has an empty stable ID");
    }
    auto const id = std::string_view(definition.id, definition.id_size);
    if (id.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("IV package definition ID contains a null byte");
    }
    if (!definition.source_file || definition.source_file_size == 0) {
        throw std::invalid_argument("IV package definition has no source file");
    }
    if (!definition.package_root || definition.package_root_size == 0) {
        throw std::invalid_argument("IV package definition has no package root");
    }
    if (!definition.signature) {
        throw std::invalid_argument("IV package definition has no construction signature");
    }
    if (definition.kind == PackageDefinitionKind::module) {
        if (!definition.module_build || definition.node_build
            || definition.node_compiler_record) {
            throw std::invalid_argument("IV module definition is invalid");
        }
    } else if (definition.kind == PackageDefinitionKind::node) {
        if (definition.module_build || !definition.node_build
            || !definition.node_compiler_record) {
            throw std::invalid_argument("IV node definition is invalid");
        }
    } else {
        throw std::invalid_argument("IV package definition has invalid kind");
    }
}

std::string module_cycle_message(
    std::span<std::string const> stack, std::string_view requested)
{
    std::ostringstream message;
    message << "iv module configuration cycle: ";
    auto const begin = std::ranges::find(stack, requested);
    bool first = true;
    for (auto it = begin; it != stack.end(); ++it) {
        if (!first) message << " -> ";
        message << *it;
        first = false;
    }
    if (!first) message << " -> ";
    message << requested;
    return std::move(message).str();
}
}

std::string configuration_json_boolean(bool value)
{
    return ConfigurationJson(value).dump();
}

std::string configuration_json_signed_integer(std::int64_t value)
{
    return ConfigurationJson(value).dump();
}

std::string configuration_json_unsigned_integer(std::uint64_t value)
{
    return ConfigurationJson(value).dump();
}

std::string configuration_json_floating_point(double value)
{
    if (!std::isfinite(value)) {
        throw std::invalid_argument(
            "replacement floating-point argument is not finite");
    }
    return ConfigurationJson(value).dump();
}

std::string configuration_json_string(std::string_view value)
{
    return ConfigurationJson(std::string(value)).dump();
}

std::string configuration_node_address_json(NodeAddress const& value)
{
    return configuration_node_address_json_value(value).dump();
}

std::string configuration_schema_json_value(
    void const* data,
    ConfigurationExpressionTypeSchema const& schema)
{
    return configuration_schema_json_value_impl(data, schema).dump();
}

struct BuilderSession {
    std::unique_ptr<GraphBuilderState> state = std::make_unique<GraphBuilderState>();
    bool graph_taken = false;
    std::shared_ptr<BuilderConfiguration> configuration =
        std::make_shared<BuilderConfiguration>();
    std::size_t package_index = static_cast<std::size_t>(-1);
    void* definition_resolver_context = nullptr;
    BuilderDefinitionResolver definition_resolver = nullptr;
    BuilderConcreteNodeReplacementResolver concrete_node_replacement_resolver = nullptr;
    BuilderNodeSourceIdentity pending_node_source_identity{};
    std::vector<ModuleMessageLocalRef> module_message_local_refs{};
    std::vector<ProjectNodePathSelector> semantic_scope_path{};
    struct DirectNodeInvocationCount {
        std::vector<ProjectNodePathSelector> scope_path{};
        std::string source_identity{};
        std::size_t next_member = 0;
    };
    std::vector<DirectNodeInvocationCount> direct_node_invocation_counts{};
    std::vector<std::pair<
        NodeBundleHandle,
        std::vector<ProjectNodePathSelector>>> node_message_paths{};

    struct NodeConfigAllocation {
        void* storage = nullptr;
        std::size_t size = 0;
        std::size_t alignment = 1;

        NodeConfigAllocation() = default;
        NodeConfigAllocation(void* storage_, std::size_t size_, std::size_t alignment_)
            : storage(storage_), size(size_), alignment(alignment_) {}
        NodeConfigAllocation(NodeConfigAllocation const&) = delete;
        NodeConfigAllocation& operator=(NodeConfigAllocation const&) = delete;
        NodeConfigAllocation(NodeConfigAllocation&& other) noexcept
            : storage(std::exchange(other.storage, nullptr))
            , size(other.size)
            , alignment(other.alignment) {}
        NodeConfigAllocation& operator=(NodeConfigAllocation&& other) noexcept
        {
            if (this == &other) return *this;
            reset();
            storage = std::exchange(other.storage, nullptr);
            size = other.size;
            alignment = other.alignment;
            return *this;
        }
        ~NodeConfigAllocation() { reset(); }
        void reset() noexcept
        {
            if (storage) {
                ::operator delete(storage, std::align_val_t{alignment});
                storage = nullptr;
            }
        }
    };
    std::vector<NodeConfigAllocation> pending_node_configs{};
};

extern "C" BuilderSession* iv_builder_session_create()
{
    return new BuilderSession;
}

void set_builder_definition_resolver(
    BuilderSession* session, void* context, BuilderDefinitionResolver resolver) noexcept
{
    if (!session) return;
    session->definition_resolver_context = context;
    session->definition_resolver = resolver;
}

void set_builder_concrete_node_replacement_resolver(
    BuilderSession* session,
    BuilderConcreteNodeReplacementResolver resolver) noexcept
{
    if (!session) return;
    session->concrete_node_replacement_resolver = resolver;
}

BuilderDefinitionResolver builder_definition_resolver(BuilderSession const* session) noexcept
{
    return session ? session->definition_resolver : nullptr;
}

void* builder_definition_resolver_context(BuilderSession const* session) noexcept
{
    return session ? session->definition_resolver_context : nullptr;
}

void prepare_builder_node_source_identity(
    BuilderSession* session,
    std::string_view source_identity,
    std::string_view authored_name)
{
    if (!session || source_identity.empty()) return;
    if (!session->pending_node_source_identity.source_identity.empty()) {
        throw std::logic_error(
            "a builder node source identity was prepared more than once");
    }
    session->pending_node_source_identity = {
        .source_identity = std::string(source_identity),
        .authored_name = std::string(authored_name),
    };
}

BuilderNodeSourceIdentity consume_builder_node_source_identity(
    BuilderSession* session)
{
    if (!session) return {};
    return std::exchange(session->pending_node_source_identity, {});
}

void set_builder_semantic_scope_path(
    BuilderSession* session,
    std::span<ProjectNodePathSelector const> path)
{
    if (!session) return;
    session->semantic_scope_path.assign(path.begin(), path.end());
}

std::span<ProjectNodePathSelector const>
builder_semantic_scope_path(BuilderSession const* session) noexcept
{
    return session
        ? std::span<ProjectNodePathSelector const>{session->semantic_scope_path}
        : std::span<ProjectNodePathSelector const>{};
}

void record_builder_module_message_local_ref(
    BuilderSession* session,
    BuilderNodeSourceIdentity const& identity)
{
    if (!session || identity.source_identity.empty()
        || identity.authored_name.empty()) return;
    ModuleMessageLocalRef local{
        .name = identity.authored_name,
        .relative_path = {
            ProjectVirtualNodeSelector{
                .source_identity = identity.source_identity,
            },
        },
    };
    if (!std::ranges::contains(session->module_message_local_refs, local)) {
        session->module_message_local_refs.push_back(std::move(local));
    }
}

void register_builder_module_class_message_handlers(
    GraphBuilder& builder,
    std::shared_ptr<void const> receiver)
{
    auto* const session = builder._session;
    if (!session || !session->configuration || !receiver) {
        throw std::invalid_argument(
            "module-class message handlers require a builder session and receiver");
    }
    if (session->package_index >= session->configuration->packages.size()
        || session->configuration->module_stack.empty()) {
        throw std::logic_error(
            "module-class message handlers have no active registered module");
    }
    auto local_refs = std::make_shared<ModuleMessageLocalRefs const>(
        ModuleMessageLocalRefs{
            .values = std::exchange(session->module_message_local_refs, {}),
        });
    auto const definition_id = session->configuration->module_stack.back();
    auto const& handlers = session->configuration
        ->packages[session->package_index].module_message_handlers;
    for (auto const& handler : handlers) {
        auto const handler_definition_id = std::string_view(
            static_cast<char const*>(handler.definition_id.data),
            handler.definition_id.size);
        if (handler_definition_id != definition_id) continue;
        if (!handler.event_type || !handler.invoke) {
            throw std::invalid_argument(
                "IV package module message handler is incomplete");
        }
        session->configuration->message_handlers.push_back(
            ConfiguredNodeMessageHandler{
                .relative_path = session->semantic_scope_path,
                .event_type = handler.event_type,
                .receiver = receiver,
                .module_local_refs = local_refs,
                .invoke = handler.invoke,
            });
    }
}

std::vector<std::vector<ProjectNodePathSelector>>
claim_builder_concrete_node_paths(
    BuilderSession* session,
    std::optional<ChannelLayout> tiled_layout)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("concrete node requires a builder session");
    }
    auto const prepared = consume_builder_node_source_identity(session);
    record_builder_module_message_local_ref(session, prepared);
    auto const& source_identity = prepared.source_identity;
    auto relative_path = session->semantic_scope_path;
    if (!source_identity.empty()) {
        auto count = std::ranges::find_if(
            session->direct_node_invocation_counts,
            [&](BuilderSession::DirectNodeInvocationCount const& value) {
                return value.scope_path == session->semantic_scope_path
                    && value.source_identity == source_identity;
            });
        if (count == session->direct_node_invocation_counts.end()) {
            session->direct_node_invocation_counts.push_back({
                .scope_path = session->semantic_scope_path,
                .source_identity = source_identity,
            });
            count = std::prev(session->direct_node_invocation_counts.end());
        }
        relative_path.push_back(ProjectVirtualNodeSelector{
            .source_identity = source_identity,
        });
        relative_path.push_back(ProjectVirtualMemberSelector{
            .index = count->next_member++,
        });
    }
    std::vector<std::vector<ProjectNodePathSelector>> result;
    if (!tiled_layout) {
        result.push_back(std::move(relative_path));
        return result;
    }
    auto const& channel_type = channel_type_info(tiled_layout->channel_type);
    result.reserve(channel_type.channel_names.size());
    for (auto const channel_name : channel_type.channel_names) {
        auto member = relative_path;
        member.push_back(ProjectTiledChildSelector{
            .name = std::string(channel_name),
        });
        result.push_back(std::move(member));
    }
    return result;
}

std::optional<NodeBundleHandle> resolve_builder_concrete_node_replacement(
    BuilderSession* session,
    GraphBuilder& builder,
    std::span<ProjectNodePathSelector const> relative_path)
{
    if (!session || !session->concrete_node_replacement_resolver) {
        return std::nullopt;
    }
    return session->concrete_node_replacement_resolver(
        session->definition_resolver_context,
        builder,
        relative_path);
}

void register_builder_concrete_node_message_handlers(
    BuilderSession* session,
    NodeCodeKey code_key,
    std::shared_ptr<void const> receiver,
    std::span<ProjectNodePathSelector const> relative_path)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("concrete node requires a builder session");
    }
    if (session->package_index >= session->configuration->packages.size()) return;
    for (auto const& handler
         : session->configuration->packages[session->package_index].node_message_handlers) {
        if (handler.code_key != code_key) continue;
        if (!handler.event_type || !handler.invoke || !receiver) {
            throw std::invalid_argument("leaf node message handler is incomplete");
        }
        ConfiguredNodeMessageHandler configured{
            .relative_path = {
                relative_path.begin(), relative_path.end()},
            .event_type = handler.event_type,
            .receiver = receiver,
            .invoke = handler.invoke,
        };
        session->configuration->message_handlers.push_back(
            std::move(configured));
    }
}

void record_builder_node_message_path(
    BuilderSession* session,
    NodeBundleHandle handle,
    std::span<ProjectNodePathSelector const> relative_path)
{
    if (!session) return;
    auto path = std::vector<ProjectNodePathSelector>(
        relative_path.begin(), relative_path.end());
    auto const scope_size = session->semantic_scope_path.size();
    if (path.size() >= scope_size
        && std::equal(
            session->semantic_scope_path.begin(),
            session->semantic_scope_path.end(),
            path.begin())) {
        path.erase(path.begin(), path.begin() + scope_size);
    }
    auto existing = std::ranges::find_if(
        session->node_message_paths,
        [&](auto const& entry) { return entry.first == handle; });
    if (existing == session->node_message_paths.end()) {
        session->node_message_paths.emplace_back(handle, std::move(path));
    } else if (existing->second != path) {
        throw std::logic_error(
            "one builder node bundle has two semantic message paths");
    }
}

void attach_builder_node_message_path(
    BuilderSession* session,
    NodeRef& ref,
    NodeBundleHandle handle)
{
    if (!session) return;
    auto const found = std::ranges::find_if(
        session->node_message_paths,
        [&](auto const& entry) { return entry.first == handle; });
    if (found != session->node_message_paths.end()) {
        ref._set_semantic_message_path(found->second);
    }
}

void register_builder_embedded_message_handlers(
    BuilderSession* session,
    std::span<ProjectNodePathSelector const> prefix,
    std::span<ConfiguredNodeMessageHandler const> handlers)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("embedded message handlers require a builder session");
    }
    for (auto const& handler : handlers) {
        auto copy = handler;
        copy.relative_path.insert(
            copy.relative_path.begin(), prefix.begin(), prefix.end());
        session->configuration->message_handlers.push_back(std::move(copy));
    }
}

void register_builder_embedded_message_handlers(
    GraphBuilder& builder,
    std::span<ProjectNodePathSelector const> prefix,
    std::span<ConfiguredNodeMessageHandler const> handlers)
{
    register_builder_embedded_message_handlers(
        builder._session, prefix, handlers);
}

std::vector<ConfiguredNodeMessageHandler>
take_builder_message_handlers(BuilderSession* session)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("message handlers require a builder session");
    }
    return std::exchange(session->configuration->message_handlers, {});
}

extern "C" void iv_builder_session_destroy(BuilderSession* session) noexcept
{
    delete session;
}

BuilderSession* iv_builder_child_session_create(
    BuilderSession* parent, std::size_t package_index)
{
    if (!parent || !parent->configuration) {
        throw std::invalid_argument("parent builder session is null");
    }
    if (package_index >= parent->configuration->packages.size()) {
        throw std::out_of_range("builder package index is out of range");
    }
    auto child = std::make_unique<BuilderSession>();
    child->configuration = parent->configuration;
    child->package_index = package_index;
    child->definition_resolver_context = parent->definition_resolver_context;
    child->definition_resolver = parent->definition_resolver;
    child->concrete_node_replacement_resolver =
        parent->concrete_node_replacement_resolver;
    child->semantic_scope_path = parent->semantic_scope_path;
    // The configured graph now contains code/data produced by this package even
    // when the nested iv module contributes no primitive node directly. Pin it.
    child->configuration->used_packages[package_index] = true;
    return child.release();
}

GraphBuilderState& builder_graph_state(GraphBuilder& builder)
{
    if (!builder._session || !builder._session->state
        || builder._session->graph_taken) {
        throw std::logic_error("builder graph is unavailable");
    }
    auto& state = *builder._session->state;
    state.bind(builder);
    return state;
}

bool builder_session_has_packages(BuilderSession const* session) noexcept
{
    return session && session->configuration && !session->configuration->packages.empty();
}

ConfiguredGraph take_built_graph(BuilderSession* session)
{
    if (!session || !session->state || session->graph_taken) {
        throw std::logic_error("builder session has no unfinished graph");
    }
    session->graph_taken = true;
    auto state = std::move(session->state);
    return std::move(*state).finish();
}

void set_builder_packages(
    BuilderSession* session, std::span<BuilderPackageView const> packages)
{
    if (!session) throw std::invalid_argument("builder session is null");
    if (session->graph_taken) {
        throw std::logic_error("cannot configure a finished builder session");
    }

    auto configured = std::make_shared<BuilderConfiguration>();
    configured->packages.reserve(packages.size());
    configured->used_packages.assign(packages.size(), false);
    for (auto const& package : packages) {
        if (package.package_root.empty()) {
            throw std::invalid_argument("builder package has empty package root");
        }
        auto& destination = configured->packages.emplace_back();
        destination.package_root = package.package_root;
        destination.definitions.assign(
            package.definitions.begin(), package.definitions.end());
        destination.config_pointer_fields.assign(
            package.config_pointer_fields.begin(), package.config_pointer_fields.end());
        destination.retained_globals.assign(
            package.retained_globals.begin(), package.retained_globals.end());
        destination.node_message_handlers.assign(
            package.node_message_handlers.begin(), package.node_message_handlers.end());
        destination.module_message_handlers.assign(
            package.module_message_handlers.begin(),
            package.module_message_handlers.end());
        destination.node_state_structures.assign(
            package.node_state_structures.begin(), package.node_state_structures.end());

        for (auto const& definition : destination.definitions) {
            validate_definition(definition);
            auto const root = std::string_view(
                definition.package_root, definition.package_root_size);
            if (root != destination.package_root) {
                throw std::invalid_argument(
                    "IV package definition belongs to a different package root");
            }
        }
        for (auto const& global : destination.retained_globals) {
            if (!global.address || global.size == 0) {
                throw std::invalid_argument("retained LLVM global has invalid native address");
            }
        }
        for (auto const& handler : destination.node_message_handlers) {
            if (!handler.event_type || !handler.invoke) {
                throw std::invalid_argument(
                    "IV package node message handler is incomplete");
            }
        }
        for (auto const& handler : destination.module_message_handlers) {
            if (!handler.definition_id.data || handler.definition_id.size == 0
                || !handler.event_type || !handler.invoke) {
                throw std::invalid_argument(
                    "IV package module message handler is incomplete");
            }
        }
    }
    session->configuration = std::move(configured);
    session->package_index = static_cast<std::size_t>(-1);
}

std::size_t builder_package_index(
    BuilderSession const* session, std::string_view package_root)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    auto const source = std::ranges::find(
        session->configuration->packages, package_root, &BuilderPackage::package_root);
    if (source == session->configuration->packages.end()) {
        throw std::runtime_error(
            "IV package '" + std::string(package_root)
            + "' is unavailable while configuring the graph");
    }
    return static_cast<std::size_t>(source - session->configuration->packages.begin());
}

std::size_t builder_selected_package(BuilderSession const* session) noexcept
{
    return session ? session->package_index : static_cast<std::size_t>(-1);
}

void restore_builder_package(BuilderSession* session, std::size_t package_index) noexcept
{
    if (session) session->package_index = package_index;
}

void select_builder_package(BuilderSession* session, std::size_t package_index)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    if (package_index >= session->configuration->packages.size()) {
        throw std::out_of_range("builder package index is out of range");
    }
    session->package_index = package_index;
    session->configuration->used_packages[package_index] = true;
}

BuilderDefinition find_builder_definition(
    BuilderSession const* session, std::string_view id)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    auto find_exact = [&](std::string_view requested) -> std::optional<BuilderDefinition> {
        std::optional<BuilderDefinition> found;
        for (std::size_t package_index = 0;
             package_index < session->configuration->packages.size(); ++package_index) {
            for (auto const& definition
                 : session->configuration->packages[package_index].definitions) {
                if (std::string_view(definition.id, definition.id_size) != requested) continue;
                if (found) {
                    throw std::runtime_error(
                        "IV package definition '" + std::string(requested)
                        + "' has multiple providers in the loaded package definitions");
                }
                found = BuilderDefinition{
                    .definition = definition,
                    .package_index = package_index,
                };
            }
        }
        return found;
    };

    if (auto found = find_exact(id)) return *found;

    // Builtins have canonical stable IDs, but their short names are useful in
    // ordinary package source.  Resolve a bare name only after exact lookup,
    // so a package's deliberately registered `saw_oscillator` still wins over
    // the convenience alias for `iv.builtin.saw_oscillator`.
    if (id.find('.') == std::string_view::npos) {
        auto const builtin_id = "iv.builtin." + std::string(id);
        if (auto found = find_exact(builtin_id)) return *found;
    }
    throw std::runtime_error(
        "IV package definition '" + std::string(id)
        + "' is unavailable in the loaded package definitions");
}

std::vector<std::size_t> builder_used_packages(BuilderSession const* session)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < session->configuration->used_packages.size(); ++index) {
        if (session->configuration->used_packages[index]) result.push_back(index);
    }
    return result;
}

void begin_builder_module(BuilderSession* session, std::string_view id)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    auto& stack = session->configuration->module_stack;
    if (std::ranges::contains(stack, id)) {
        throw std::runtime_error(module_cycle_message(stack, id));
    }
    stack.emplace_back(id);
}

void end_builder_module(BuilderSession* session) noexcept
{
    if (!session || !session->configuration) return;
    auto& stack = session->configuration->module_stack;
    if (!stack.empty()) stack.pop_back();
}

void* iv_builder_allocate_node_config(
    BuilderSession* session, std::size_t size, std::size_t alignment)
{
    if (!session || session->graph_taken || size == 0 || alignment == 0) {
        throw std::invalid_argument("invalid builder node configuration allocation");
    }
    auto* storage = ::operator new(size, std::align_val_t{alignment});
    try {
        session->pending_node_configs.emplace_back(storage, size, alignment);
    } catch (...) {
        ::operator delete(storage, std::align_val_t{alignment});
        throw;
    }
    return storage;
}

void iv_builder_discard_node_config(BuilderSession* session, void* storage) noexcept
{
    if (!session || !storage) return;
    auto const allocation = std::find_if(
        session->pending_node_configs.begin(), session->pending_node_configs.end(),
        [&](auto const& candidate) { return candidate.storage == storage; });
    if (allocation != session->pending_node_configs.end()) {
        session->pending_node_configs.erase(allocation);
    }
}

std::shared_ptr<void const> take_builder_node_config(
    BuilderSession* session,
    void const* storage,
    std::size_t size,
    std::size_t alignment)
{
    if (!session || !storage || size == 0 || alignment == 0) {
        throw std::invalid_argument("invalid builder-owned node configuration");
    }
    auto const allocation = std::find_if(
        session->pending_node_configs.begin(), session->pending_node_configs.end(),
        [&](auto const& candidate) {
            return candidate.storage == storage && candidate.size == size
                && candidate.alignment == alignment;
        });
    if (allocation == session->pending_node_configs.end()) {
        throw std::logic_error("node configuration was not allocated by this builder session");
    }
    auto* owned = allocation->storage;
    allocation->storage = nullptr;
    session->pending_node_configs.erase(allocation);
    return std::shared_ptr<void const>(
        owned,
        [alignment](void const* pointer) {
            ::operator delete(const_cast<void*>(pointer), std::align_val_t{alignment});
        });
}

NodeConfigRelocations capture_node_config(
    BuilderSession* session,
    NodeCodeKey code_key,
    void const* config,
    std::size_t config_size)
{
    if (!session || !config || config_size == 0) {
        throw std::logic_error("node configuration has no builder-owned storage");
    }
    if (!session->configuration
        || session->package_index >= session->configuration->packages.size()) {
        // Nodes created directly by host code do not have package compiler
        // metadata, and therefore have no source-relative pointer relocations.
        return {};
    }
    auto const& package = session->configuration->packages[session->package_index];

    NodeConfigRelocations relocations;
    auto const* bytes = static_cast<std::byte const*>(config);
    for (auto const& field : package.config_pointer_fields) {
        if (field.code_key != code_key) continue;
        auto const offset = field.byte_offset;
        if (offset > config_size || config_size - offset < sizeof(void const*)) {
            throw std::logic_error(
                "compiler pointer field metadata is outside node configuration");
        }
        void const* value = nullptr;
        std::memcpy(&value, bytes + offset, sizeof(value));
        if (!value) {
            relocations.push_back({.byte_offset = offset});
            continue;
        }
        auto const value_address = reinterpret_cast<std::uintptr_t>(value);
        // The field layout belongs to the concrete node's provider package,
        // but the value stored in the field may have originated in the
        // package that called g.node<Id>(...).  A string literal is the
        // ordinary example: the provider node stores the caller's literal
        // pointer.  Find the retained global across the complete configured
        // package set rather than incorrectly assuming both sides share one
        // package.
        struct FoundGlobal {
            std::size_t package_index = 0;
            RetainedGlobalData const* global = nullptr;
        };
        std::optional<FoundGlobal> found_global;
        for (std::size_t candidate_package_index = 0;
             candidate_package_index < session->configuration->packages.size();
             ++candidate_package_index) {
            auto const& candidate_package =
                session->configuration->packages[candidate_package_index];
            auto const global = std::find_if(
                candidate_package.retained_globals.begin(),
                candidate_package.retained_globals.end(),
                [&](RetainedGlobalData const& candidate) {
                    auto const begin = reinterpret_cast<std::uintptr_t>(candidate.address);
                    return value_address >= begin
                        && value_address - begin < candidate.size;
                });
            if (global == candidate_package.retained_globals.end()) continue;
            found_global = FoundGlobal{
                .package_index = candidate_package_index,
                .global = std::addressof(*global),
            };
            break;
        }
        if (!found_global) {
            throw std::logic_error(
                "node configuration pointer does not refer to a retained LLVM global");
        }
        auto const& global_package =
            session->configuration->packages[found_global->package_index];
        // It is possible for a configuration pointer to be the only reason
        // this package participates in the graph. Retain its code/data just
        // as we retain a package selected for a node or nested module.
        session->configuration->used_packages[found_global->package_index] = true;
        relocations.push_back({
            .byte_offset = offset,
            .package_root = global_package.package_root,
            .retained_global_index = found_global->global->index,
            .addend = static_cast<std::size_t>(
                value_address
                - reinterpret_cast<std::uintptr_t>(found_global->global->address)),
        });
    }
    std::ranges::sort(relocations, {}, &NodeConfigRelocation::byte_offset);
    return relocations;
}

std::shared_ptr<NodeStateStructures const> copy_builder_node_state_structures(
    BuilderSession* session, NodeCodeKey code_key)
{
    if (!session || !session->configuration
        || session->package_index >= session->configuration->packages.size()) {
        return {};
    }
    auto const& package = session->configuration->packages[session->package_index];
    auto const found = std::ranges::find(
        package.node_state_structures, code_key, &BuilderNodeStateStructures::code_key);
    if (found == package.node_state_structures.end()) return {};
    return std::make_shared<NodeStateStructures const>(found->structures);
}

} // namespace iv::details
