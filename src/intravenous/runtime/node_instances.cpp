#include <intravenous/runtime/node_instances.h>

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph/builder/host.hpp>
#include <intravenous/module/builder_session.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/configuration_expression.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/package_pipeline_types.h>
#include <intravenous/runtime/project_node_path_resolver.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/uuid.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <new>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace iv {
namespace {
std::filesystem::path normalize_path(std::filesystem::path const& path)
{
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) return canonical.lexically_normal();
    return std::filesystem::absolute(path).lexically_normal();
}

std::filesystem::path definition_package_root(NodeDefinitionEntry const& definition)
{
    return std::visit(
        [](auto const& concrete) { return concrete.package_root; },
        definition.definition);
}

std::string canonical_replacement_arguments(std::string const& source)
{
    auto value = nlohmann::ordered_json::parse(source);
    if (!value.is_array()) {
        throw std::invalid_argument(
            "node replacement configuration arguments must be a JSON array");
    }
    return value.dump();
}

std::string definition_compatibility_module_id(NodeDefinitionEntry const& definition)
{
    if (auto const* module = std::get_if<ModuleNodeDefinition>(&definition.definition)) {
        return module->module_id;
    }
    return definition.definition_id;
}

NodeDefinitionProvider const& definition_provider(NodeDefinitionEntry const& definition)
{
    return std::visit(
        [](auto const& concrete) -> NodeDefinitionProvider const& {
            return concrete.provider;
        },
        definition.definition);
}

bool configured_definition_closure_matches(
    ConfiguredNodeInstance const& configured,
    NodeDefinitionsSnapshot const& snapshot)
{
    // Snapshot generation is publication order, not semantic identity. A
    // cached configured graph remains exact only while every definition it
    // actually captured, including nested module dependencies, retains its
    // version in the newly published snapshot.
    std::vector<ConfiguredNodeInstance const*> pending{&configured};
    std::unordered_set<ConfiguredNodeInstance const*> visited;
    while (!pending.empty()) {
        auto const* current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second) continue;
        auto const definition = snapshot.by_id.find(current->definition_id);
        if (definition == snapshot.by_id.end()
            || definition->second.version != current->definition_version) {
            return false;
        }
        for (auto const& dependency : current->dependencies) {
            if (!dependency) return false;
            pending.push_back(dependency.get());
        }
    }
    return true;
}

IvModuleInstance make_instance(
    NodeDefinitionEntry const& definition,
    std::string const& instance_id,
    std::string const& display_name,
    std::string const& configuration_source)
{
    return IvModuleInstance{
        .instance_id = instance_id,
        .definition_id = definition.definition_id,
        .display_name = display_name,
        .package_root = definition_package_root(definition),
        .configuration_source = configuration_source,
        .module_id = definition_compatibility_module_id(definition),
    };
}

class OwnedConfigurationArgument {
    void* storage_ = nullptr;
    details::ConfigurationValueOperations const* operations_ = nullptr;
    details::ConfigurationTypeIdentity const* type_ = nullptr;
    bool is_const_ = false;
    bool is_rvalue_ = false;

public:
    OwnedConfigurationArgument() = default;
    OwnedConfigurationArgument(OwnedConfigurationArgument const&) = delete;
    OwnedConfigurationArgument& operator=(OwnedConfigurationArgument const&) = delete;

    OwnedConfigurationArgument(OwnedConfigurationArgument&& other) noexcept
        : storage_(std::exchange(other.storage_, nullptr))
        , operations_(std::exchange(other.operations_, nullptr))
        , type_(std::exchange(other.type_, nullptr))
        , is_const_(other.is_const_)
        , is_rvalue_(other.is_rvalue_)
    {}

    OwnedConfigurationArgument& operator=(OwnedConfigurationArgument&& other) noexcept
    {
        if (this == &other) return *this;
        reset();
        storage_ = std::exchange(other.storage_, nullptr);
        operations_ = std::exchange(other.operations_, nullptr);
        type_ = std::exchange(other.type_, nullptr);
        is_const_ = other.is_const_;
        is_rvalue_ = other.is_rvalue_;
        return *this;
    }

    ~OwnedConfigurationArgument() { reset(); }

    static bool can_own(
        details::ConfigurationArgument const& argument,
        details::ConfigurationValueOperations const* operations) noexcept
    {
        return argument.data && !argument.is_array_decay && operations
            && operations->size != 0 && operations->alignment != 0
            && operations->copy_construct && operations->destroy;
    }

    static bool can_cache(
        details::ConfigurationArgument const& argument,
        details::ConfigurationValueOperations const* operations) noexcept
    {
        return can_own(argument, operations) && operations->equal && operations->hash;
    }

    static OwnedConfigurationArgument clone(
        details::ConfigurationArgument const& argument,
        details::ConfigurationValueOperations const* operations)
    {
        if (!can_own(argument, operations)) {
            throw std::invalid_argument("configuration argument cannot be owned safely");
        }
        OwnedConfigurationArgument result;
        result.operations_ = operations;
        result.type_ = argument.type;
        result.is_const_ = argument.is_const;
        result.is_rvalue_ = argument.is_rvalue;
        result.storage_ = ::operator new(
            operations->size, std::align_val_t{operations->alignment});
        try {
            operations->copy_construct(result.storage_, argument.data);
        } catch (...) {
            ::operator delete(
                result.storage_, std::align_val_t{operations->alignment});
            result.storage_ = nullptr;
            throw;
        }
        return result;
    }

    details::ConfigurationArgument argument() noexcept
    {
        return details::ConfigurationArgument{
            .data = storage_,
            .type = type_,
            .is_const = is_const_,
            .is_rvalue = is_rvalue_,
            .is_array_decay = false,
        };
    }

    bool matches(
        details::ConfigurationArgument const& argument,
        details::ConfigurationValueOperations const* operations) const
    {
        return storage_ && operations_ == operations
            && details::same_configuration_type(type_, argument.type)
            && !argument.is_array_decay
            && is_const_ == argument.is_const
            && is_rvalue_ == argument.is_rvalue
            && operations_->equal
            && operations_->equal(storage_, argument.data);
    }

    std::size_t hash() const
    {
        if (!storage_ || !operations_ || !operations_->hash) {
            throw std::logic_error("configuration argument is not hashable");
        }
        auto value = operations_->hash(storage_);
        value ^= static_cast<std::size_t>(is_const_) + 0x9e3779b9U
            + (value << 6U) + (value >> 2U);
        value ^= static_cast<std::size_t>(is_rvalue_) + 0x9e3779b9U
            + (value << 6U) + (value >> 2U);
        return value;
    }

private:
    void reset() noexcept
    {
        if (!storage_) return;
        operations_->destroy(storage_);
        ::operator delete(storage_, std::align_val_t{operations_->alignment});
        storage_ = nullptr;
    }
};

class ConfigurationStackEntry {
    std::vector<std::string>* stack_ = nullptr;
public:
    ConfigurationStackEntry(std::vector<std::string>& stack, std::string_view id)
        : stack_(&stack)
    {
        if (std::ranges::contains(stack, id)) {
            std::string message = "node configuration cycle: ";
            auto begin = std::ranges::find(stack, id);
            bool first = true;
            for (auto it = begin; it != stack.end(); ++it) {
                if (!first) message += " -> ";
                message += *it;
                first = false;
            }
            if (!first) message += " -> ";
            message += id;
            throw std::runtime_error(std::move(message));
        }
        stack.emplace_back(id);
    }
    ~ConfigurationStackEntry() { stack_->pop_back(); }
};
} // namespace

namespace {
struct RetainedConfigurationArguments {
    std::vector<OwnedConfigurationArgument> values{};
};
}

struct NodeInstances::CacheEntry {
    std::shared_ptr<ConfiguredNodeInstance const> configured{};
    std::optional<ChannelLayout> tiled_layout{};
    std::size_t argument_hash = 0;
    std::vector<OwnedConfigurationArgument> key_arguments{};
    std::vector<ProjectNodeReplacement> subtree_replacements{};
};

struct NodeInstances::ExpressionCacheEntry {
    std::string definition_id{};
    std::uint64_t definition_version = 0;
    ParsedConfigurationExpressionList parsed{};
};

struct NodeInstances::ConfigurationContext {
    // The durable project address of one registered invocation being
    // constructed. Declaration counting belongs to the builder session; this
    // only anchors replacement lookup and nested addresses.
    struct SemanticScope {
        NodeAddress address{};
    };

    NodeInstances* owner = nullptr;
    std::shared_ptr<NodeDefinitionsSnapshot const> snapshot{};
    std::vector<ProjectNodeReplacement> replacements{};
    std::vector<std::string> stack{};
    std::vector<std::vector<std::shared_ptr<ConfiguredNodeInstance const>>*>
        dependency_frames{};
    std::vector<SemanticScope> semantic_scopes{};
};

NodeInstances::NodeInstances()
    : definitions_snapshot_(std::make_shared<NodeDefinitionsSnapshot const>())
{}

NodeInstances::~NodeInstances() = default;

std::shared_ptr<NodeInstances::ExpressionCacheEntry const>
NodeInstances::parse_expression_cached(
    std::string_view definition_id,
    std::uint64_t definition_version,
    std::string source)
{
    {
        std::scoped_lock lock(mutex_);
        auto const found = std::ranges::find_if(
            expression_cache_, [&](auto const& entry) {
                return entry && entry->definition_id == definition_id
                    && entry->definition_version == definition_version
                    && entry->parsed.source == source;
            });
        if (found != expression_cache_.end()) return *found;
    }
    auto entry = std::make_shared<ExpressionCacheEntry const>(ExpressionCacheEntry{
        .definition_id = std::string(definition_id),
        .definition_version = definition_version,
        .parsed = parse_configuration_expression_list(std::move(source)),
    });
    std::scoped_lock lock(mutex_);
    auto const found = std::ranges::find_if(
        expression_cache_, [&](auto const& candidate) {
            return candidate
                && candidate->definition_id == entry->definition_id
                && candidate->definition_version == entry->definition_version
                && candidate->parsed.source == entry->parsed.source;
        });
    if (found != expression_cache_.end()) return *found;
    expression_cache_.push_back(entry);
    return entry;
}

bool NodeInstances::publish_instance_locked(
    std::string const& instance_id,
    NodeDefinitionEntry const& definition)
{
    auto desired = desired_instances_by_id_.find(instance_id);
    if (desired == desired_instances_by_id_.end()
        || desired->second.definition_id != definition.definition_id) {
        return false;
    }

    auto const package_root = definition_package_root(definition);
    if (desired->second.package_root != package_root) {
        desired->second.package_root = package_root;
        if (auto required = required_definitions_by_id_.find(definition.definition_id);
            required != required_definitions_by_id_.end()) {
            required->second.package_root = package_root;
        }
    }

    auto instance = make_instance(
        definition,
        desired->second.instance_id,
        desired->second.display_name,
        desired->second.configuration_source);
    auto existing = published_instances_by_id_.find(instance_id);
    if (existing != published_instances_by_id_.end() && existing->second == instance) {
        return false;
    }
    published_instances_by_id_[instance_id] = std::move(instance);
    return true;
}

IvModuleRequiredDefinitionsChanged
NodeInstances::rebuild_required_definitions_locked()
{
    std::unordered_map<std::string, IvModuleRequiredDefinition> next;
    auto add = [&](std::string const& definition_id,
                   std::filesystem::path package_root) {
        if (definitions_snapshot_) {
            if (auto definition = definitions_snapshot_->by_id.find(definition_id);
                definition != definitions_snapshot_->by_id.end()) {
                package_root = definition_package_root(definition->second);
            }
        }
        if (package_root.empty()) return;
        auto [found, inserted] = next.emplace(
            definition_id,
            IvModuleRequiredDefinition{
                .definition_id = definition_id,
                .package_root = package_root,
            });
        if (!inserted && found->second.package_root != package_root) {
            throw std::invalid_argument(
                "node definition has conflicting package roots: "
                + definition_id);
        }
    };

    for (auto& [_, desired] : desired_instances_by_id_) {
        if (definitions_snapshot_) {
            if (auto definition = definitions_snapshot_->by_id.find(
                    desired.definition_id);
                definition != definitions_snapshot_->by_id.end()) {
                desired.package_root = definition_package_root(definition->second);
            }
        }
        add(desired.definition_id, desired.package_root);
    }
    for (auto& replacement : desired_node_replacements_) {
        auto package_root = replacement.package_root.value_or(
            std::filesystem::path{});
        if (definitions_snapshot_) {
            if (auto definition = definitions_snapshot_->by_id.find(
                    replacement.definition_id);
                definition != definitions_snapshot_->by_id.end()) {
                package_root = definition_package_root(definition->second);
                replacement.package_root = package_root;
            }
        }
        add(replacement.definition_id, std::move(package_root));
    }

    IvModuleRequiredDefinitionsChanged diff;
    for (auto const& [definition_id, value] : next) {
        auto const current = required_definitions_by_id_.find(definition_id);
        if (current == required_definitions_by_id_.end()) {
            diff.created.push_back(value);
        } else if (current->second.package_root != value.package_root) {
            diff.updated.push_back(value);
        }
    }
    for (auto const& [definition_id, _] : required_definitions_by_id_) {
        if (!next.contains(definition_id)) {
            diff.deleted_definition_ids.push_back(definition_id);
        }
    }
    required_definitions_by_id_ = std::move(next);
    return diff;
}

std::string NodeInstances::create_instance(
    std::string_view definition_id,
    std::filesystem::path package_root,
    std::optional<std::string> requested_instance_id,
    std::optional<std::string> requested_display_name,
    std::string configuration_source)
{
    Changes changes;
    return create_instance(
        changes,
        definition_id,
        std::move(package_root),
        std::move(requested_instance_id),
        std::move(requested_display_name),
        std::move(configuration_source));
}

std::string NodeInstances::create_instance(
    Changes& changes,
    std::string_view definition_id,
    std::filesystem::path package_root,
    std::optional<std::string> requested_instance_id,
    std::optional<std::string> requested_display_name,
    std::string configuration_source)
{
    IvModuleRequiredDefinitionsChanged required_diff;
    auto const definition_key = std::string(definition_id);
    if (definition_key.empty()) {
        throw std::invalid_argument("node definition id must not be empty");
    }
    if (requested_instance_id && requested_instance_id->empty()) {
        throw std::invalid_argument("node instance id must not be empty");
    }
    auto normalized_root = normalize_path(package_root);
    auto display_name = requested_display_name.value_or(definition_key);
    if (display_name.empty()) display_name = definition_key;
    std::string instance_id;

    {
        std::scoped_lock lock(mutex_);
        if (definitions_snapshot_) {
            if (auto definition = definitions_snapshot_->by_id.find(definition_key);
                definition != definitions_snapshot_->by_id.end()) {
                normalized_root = definition_package_root(definition->second);
            }
        }

        instance_id = requested_instance_id.value_or(generate_uuid_v4().str());
        if (desired_instances_by_id_.contains(instance_id)) {
            throw std::runtime_error("duplicate node instance id: " + instance_id);
        }

        for (auto& [_, desired] : desired_instances_by_id_) {
            if (desired.definition_id == definition_key) {
                desired.package_root = normalized_root;
            }
        }
        for (auto& replacement : desired_node_replacements_) {
            if (replacement.definition_id == definition_key) {
                replacement.package_root = normalized_root;
            }
        }

        desired_instances_by_id_.emplace(instance_id, DesiredInstance{
            .instance_id = instance_id,
            .definition_id = definition_key,
            .display_name = std::move(display_name),
            .package_root = normalized_root,
            .configuration_source = std::move(configuration_source),
        });

        required_diff = rebuild_required_definitions_locked();

        if (definitions_snapshot_) {
            if (auto definition = definitions_snapshot_->by_id.find(definition_key);
                definition != definitions_snapshot_->by_id.end()) {
                publish_instance_locked(instance_id, definition->second);
            }
        }
    }

    if (!required_diff.created.empty() || !required_diff.updated.empty()
        || !required_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_required_definitions_changed_event,
            required_diff);
    }
    changes.instances_list_changed = true;
    changes.instance_declarations_changed = true;
    return instance_id;
}

void NodeInstances::remove_instance(std::string const& instance_id)
{
    Changes changes;
    remove_instance(changes, instance_id);
}

void NodeInstances::remove_instance(Changes& changes, std::string const& instance_id)
{
    IvModuleRequiredDefinitionsChanged required_diff;
    bool changed = false;

    {
        std::scoped_lock lock(mutex_);
        auto desired = desired_instances_by_id_.find(instance_id);
        if (desired == desired_instances_by_id_.end()) return;
        desired_instances_by_id_.erase(desired);
        std::erase_if(
            desired_node_replacements_,
            [&](ProjectNodeReplacement const& replacement) {
                return replacement.address.instance_id == instance_id;
            });
        changed = true;
        published_instances_by_id_.erase(instance_id);
        required_diff = rebuild_required_definitions_locked();
    }

    if (changed) {
        changes.instances_list_changed = true;
        changes.instance_declarations_changed = true;
    }
    if (!required_diff.created.empty() || !required_diff.updated.empty()
        || !required_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_required_definitions_changed_event,
            required_diff);
    }
}

void NodeInstances::update_instances(std::vector<Update> updates)
{
    Changes changes;
    update_instances(changes, std::move(updates));
}

void NodeInstances::update_instances(Changes& changes, std::vector<Update> updates)
{
    bool list_changed = false;
    {
        std::scoped_lock lock(mutex_);
        // Validate the complete mutation batch before changing any desired
        // state. ProjectGraph treats this call as one logical transaction.
        for (auto const& update : updates) {
            if (!desired_instances_by_id_.contains(update.instance_id)) {
                throw std::runtime_error("unknown node instance id: " + update.instance_id);
            }
        }
        for (auto const& update : updates) {
            auto desired = desired_instances_by_id_.find(update.instance_id);
            bool changed = false;
            if (update.display_name.has_value()) {
                auto next = *update.display_name;
                if (next.empty()) next = desired->second.definition_id;
                if (desired->second.display_name != next) {
                    desired->second.display_name = std::move(next);
                    changed = true;
                }
            }
            if (update.configuration_source
                && desired->second.configuration_source != *update.configuration_source) {
                desired->second.configuration_source = *update.configuration_source;
                changed = true;
            }
            if (!changed) continue;
            list_changed = true;
            if (auto instance = published_instances_by_id_.find(update.instance_id);
                instance != published_instances_by_id_.end()) {
                instance->second.display_name = desired->second.display_name;
                instance->second.configuration_source =
                    desired->second.configuration_source;
            }
        }
    }
    if (list_changed) {
        changes.instances_list_changed = true;
        changes.instance_declarations_changed = true;
    }
}

std::vector<IvModuleInstanceInfo> NodeInstances::list_instances() const
{
    std::vector<IvModuleInstanceInfo> result;
    std::scoped_lock lock(mutex_);
    result.reserve(desired_instances_by_id_.size());
    for (auto const& [id, desired] : desired_instances_by_id_) {
        auto info = IvModuleInstanceInfo{
            .instance_id = id,
            .definition_id = desired.definition_id,
            .display_name = desired.display_name,
            .package_root = desired.package_root,
            .configuration_source = desired.configuration_source,
            .module_id = desired.definition_id,
        };
        if (auto published = published_instances_by_id_.find(id);
            published != published_instances_by_id_.end()) {
            info.realized = true;
            info.module_id = published->second.module_id;
        }
        result.push_back(std::move(info));
    }
    std::ranges::sort(result, {}, &IvModuleInstanceInfo::instance_id);
    return result;
}

std::vector<ProjectNodeInstanceDeclaration> NodeInstances::desired_instances() const
{
    std::vector<ProjectNodeInstanceDeclaration> result;
    std::scoped_lock lock(mutex_);
    result.reserve(desired_instances_by_id_.size());
    for (auto const& [instance_id, desired] : desired_instances_by_id_) {
        result.push_back(ProjectNodeInstanceDeclaration{
            .instance_id = instance_id,
            .definition_id = desired.definition_id,
            .package_root = desired.package_root.empty()
                ? std::nullopt
                : std::optional<std::filesystem::path>{desired.package_root},
            .display_name = desired.display_name != desired.definition_id
                ? std::optional<std::string>{desired.display_name}
                : std::nullopt,
            .configuration_source = desired.configuration_source,
        });
    }
    std::ranges::sort(result, {}, &ProjectNodeInstanceDeclaration::instance_id);
    return result;
}

std::vector<ProjectNodeReplacement> NodeInstances::desired_node_replacements() const
{
    std::scoped_lock lock(mutex_);
    return desired_node_replacements_;
}

NodeInstances::RealizedInvocation NodeInstances::configure_cached(
    ConfigurationContext& context,
    GraphBuilder& builder,
    NodeAddress const& address,
    std::string_view qualifier,
    std::string_view definition_id,
    std::optional<ChannelLayout> tiled_layout,
    std::span<details::ConfigurationArgument> arguments)
{
    if (!context.snapshot) {
        throw std::invalid_argument("node configuration requires a definitions snapshot");
    }
    auto definition_it = context.snapshot->by_id.find(std::string(definition_id));
    if (definition_it == context.snapshot->by_id.end()) {
        throw std::runtime_error(
            "node definition '" + std::string(definition_id)
            + "' is unavailable in this definitions snapshot");
    }
    auto const& definition = definition_it->second;
    auto const& provider = definition_provider(definition);
    if (!provider.signature) {
        throw std::logic_error(
            "node definition '" + std::string(definition_id)
            + "' has no construction signature");
    }
    auto const* signature = provider.signature();
    if (!signature) {
        throw std::logic_error(
            "node definition '" + std::string(definition_id)
            + "' returned no construction signature");
    }
    details::validate_registered_signature(definition_id, *signature, arguments);

    auto const is_path_prefix = [](auto const& prefix, auto const& value) {
        return prefix.size() <= value.size()
            && std::equal(prefix.begin(), prefix.end(), value.begin());
    };
    std::vector<ProjectNodeReplacement> subtree_replacements;
    for (auto const& replacement : context.replacements) {
        if (replacement.address.instance_id == address.instance_id
            && is_path_prefix(address.path, replacement.address.path)) {
            subtree_replacements.push_back(replacement);
        }
    }

    // Constructibility is the validated signature. Retainability decides
    // whether this invocation owns a copy of an argument; cache-keyability
    // decides whether the invocation can be reused. A value that is
    // constructible but not retainable is passed through for this synchronous
    // construction and makes the invocation unreusable.
    // A live NodeRef is construction environment: it refers into the session
    // that is constructing this invocation, so it is never retained and the
    // invocation it reaches is never reused.
    std::vector<bool> retained(arguments.size(), false);
    bool cacheable = true;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        auto const* operations = signature->parameter_operations[index];
        auto const* type = arguments[index].type;
        bool const node_reference = type && type->expression_schema
            && type->expression_schema->kind
                == details::ConfigurationExpressionTypeKind::node_reference;
        retained[index] = !node_reference
            && OwnedConfigurationArgument::can_own(arguments[index], operations);
        cacheable = cacheable && !node_reference
            && OwnedConfigurationArgument::can_cache(arguments[index], operations);
    }

    std::vector<OwnedConfigurationArgument> key_arguments;
    std::size_t argument_hash = 0;
    if (cacheable) {
        key_arguments.reserve(arguments.size());
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            auto key = OwnedConfigurationArgument::clone(
                arguments[index], signature->parameter_operations[index]);
            auto const value_hash = key.hash();
            argument_hash ^= value_hash + 0x9e3779b9U
                + (argument_hash << 6U) + (argument_hash >> 2U);
            key_arguments.push_back(std::move(key));
        }
    }

    auto find_cached = [&]() -> std::shared_ptr<CacheEntry> {
        if (!cacheable) return {};
        std::vector<std::shared_ptr<CacheEntry>> candidates;
        {
            std::scoped_lock lock(mutex_);
            candidates = configuration_cache_;
        }
        for (auto const& candidate : candidates) {
            auto const& configured = *candidate->configured;
            if (!configured_definition_closure_matches(
                    configured, *context.snapshot)
                || configured.definition_version != definition.version
                || configured.definition_id != definition.definition_id
                || candidate->tiled_layout != tiled_layout
                || candidate->argument_hash != argument_hash
                || candidate->subtree_replacements != subtree_replacements
                || candidate->key_arguments.size() != arguments.size()) {
                continue;
            }
            bool equal = true;
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                if (!candidate->key_arguments[index].matches(
                        arguments[index], signature->parameter_operations[index])) {
                    equal = false;
                    break;
                }
            }
            if (equal) return candidate;
        }
        return {};
    };
    auto* const session = builder._builder_session();
    if (auto cached = find_cached()) {
        // A hit substitutes the frozen contents of an equivalent earlier
        // construction directly into the current scope.
        auto const root = details::import_builder_invocation(
            session, *cached->configured->graph, cached->configured->local_root,
            qualifier);
        return {.root = root, .configured = cached->configured};
    }

    ConfigurationStackEntry const stack_entry(context.stack, definition_id);

    // Retained invocation values are a second owned copy. Provider code is
    // allowed to move from or mutate its arguments; the pristine key above
    // must remain value-comparable for future cache lookups.
    auto argument_lifetime = std::make_shared<RetainedConfigurationArguments>();
    argument_lifetime->values.reserve(arguments.size());
    std::vector<details::ConfigurationArgument> invocation_arguments;
    invocation_arguments.reserve(arguments.size());
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!retained[index]) {
            invocation_arguments.push_back(arguments[index]);
            continue;
        }
        argument_lifetime->values.push_back(OwnedConfigurationArgument::clone(
            arguments[index], signature->parameter_operations[index]));
        invocation_arguments.push_back(argument_lifetime->values.back().argument());
    }

    std::vector<std::shared_ptr<ConfiguredNodeInstance const>> dependencies;
    context.dependency_frames.push_back(&dependencies);
    struct DependencyFrameGuard {
        ConfigurationContext& context;
        ~DependencyFrameGuard() { context.dependency_frames.pop_back(); }
    } dependency_frame_guard{context};

    context.semantic_scopes.push_back(ConfigurationContext::SemanticScope{
        .address = address,
    });
    struct SemanticScopeGuard {
        ConfigurationContext& context;
        ~SemanticScopeGuard() { context.semantic_scopes.pop_back(); }
    } semantic_scope_guard{context};

    // A miss constructs directly in the caller's session. Only a successful,
    // self-contained invocation is copied for reuse; the live construction is
    // never extracted and reimported. A failure escapes with the session
    // failed, and no cache entry is populated.
    auto const invocation = details::begin_builder_invocation(session);
    auto const root = details::configure_package_definition_provider(
        builder,
        definition_id,
        tiled_layout,
        invocation_arguments).node_bundle_handle();
    auto finished = details::finish_builder_invocation(
        session, invocation, root, qualifier, cacheable);

    auto configured = std::make_shared<ConfiguredNodeInstance const>(ConfiguredNodeInstance{
        .definition_id = definition.definition_id,
        .definitions_generation = context.snapshot->generation,
        .definition_version = definition.version,
        .local_root = finished.snapshot_root,
        .definitions = context.snapshot,
        .configuration_lifetime = std::move(argument_lifetime),
        .dependencies = std::move(dependencies),
        .message_handlers = std::move(finished.message_handlers),
        .graph = std::move(finished.snapshot),
    });

    if (cacheable && configured->graph) {
        auto entry = std::make_shared<CacheEntry>();
        entry->configured = configured;
        entry->tiled_layout = tiled_layout;
        entry->argument_hash = argument_hash;
        entry->key_arguments = std::move(key_arguments);
        entry->subtree_replacements = std::move(subtree_replacements);
        // Equality/hash callbacks belong to package code and may execute user
        // operators. Never invoke them while holding the app-module mutex.
        if (!find_cached()) {
            std::scoped_lock lock(mutex_);
            // A newer definitions publication may have invalidated the cache
            // while this older batch was still evaluating. Let the batch finish
            // against its pinned snapshot, but do not resurrect an invalidated
            // generation.
            if (context.snapshot->generation >= minimum_cache_generation_) {
                configuration_cache_.push_back(std::move(entry));
            }
        }
    }
    return {.root = root, .configured = std::move(configured)};
}

std::optional<NodeBundleHandle>
NodeInstances::resolve_builder_concrete_node_replacement(
    void* opaque_context,
    GraphBuilder& builder,
    std::span<ProjectNodePathSelector const> relative_path)
{
    if (!opaque_context) {
        throw std::logic_error(
            "concrete node replacement resolver has no batch context");
    }
    auto& context = *static_cast<ConfigurationContext*>(opaque_context);
    if (context.semantic_scopes.empty()) {
        throw std::logic_error(
            "concrete node replacement has no semantic parent scope");
    }
    // The empty relative path is the root concrete node of the definition
    // currently being configured. Its replacement, if any, was already
    // selected by the top-level request or resolve_builder_definition().
    if (relative_path.empty()) return std::nullopt;

    NodeAddress address = context.semantic_scopes.back().address;
    address.path.insert(
        address.path.end(), relative_path.begin(), relative_path.end());
    auto const replacement = std::ranges::find(
        context.replacements,
        address,
        &ProjectNodeReplacement::address);
    if (replacement == context.replacements.end()) return std::nullopt;

    auto const definition = context.snapshot->by_id.find(
        replacement->definition_id);
    if (definition == context.snapshot->by_id.end()) {
        throw std::runtime_error(
            "node replacement definition is unavailable: "
            + replacement->definition_id);
    }
    auto const& provider = definition_provider(definition->second);
    auto const* signature = provider.signature ? provider.signature() : nullptr;
    if (!signature) {
        throw std::logic_error(
            "node replacement definition has no construction signature: "
            + replacement->definition_id);
    }
    auto arguments = evaluate_configuration_json_arguments(
        nlohmann::ordered_json::parse(
            replacement->configuration_arguments_json),
        *signature);
    auto realized = context.owner->configure_cached(
        context,
        builder,
        address,
        details::builder_invocation_qualifier(relative_path),
        replacement->definition_id,
        std::nullopt,
        arguments.arguments());
    details::register_builder_embedded_message_handlers(
        builder,
        relative_path,
        realized.configured->message_handlers);
    if (!context.dependency_frames.empty()) {
        context.dependency_frames.back()->push_back(realized.configured);
    }
    return realized.root;
}

NodeRef NodeInstances::resolve_builder_definition(
    void* opaque_context,
    GraphBuilder& builder,
    std::string_view definition_id,
    std::span<ProjectNodePathSelector const> relative_path,
    std::optional<ChannelLayout> tiled_layout,
    std::span<details::ConfigurationArgument> arguments)
{
    if (!opaque_context) {
        throw std::logic_error("node configuration resolver has no batch context");
    }
    auto& context = *static_cast<ConfigurationContext*>(opaque_context);
    if (context.semantic_scopes.empty()) {
        throw std::logic_error(
            "nested node configuration has no semantic parent scope");
    }
    if (relative_path.size() < 2) {
        throw std::logic_error(
            "a registered call path must name its declaration and member");
    }
    NodeAddress child_address = context.semantic_scopes.back().address;
    child_address.path.insert(
        child_address.path.end(), relative_path.begin(), relative_path.end());

    std::optional<EvaluatedConfigurationArguments> replacement_arguments;
    auto configured_definition_id = std::string(definition_id);
    auto configured_arguments = arguments;
    auto const replacement = std::ranges::find(
        context.replacements,
        child_address,
        &ProjectNodeReplacement::address);
    if (replacement != context.replacements.end()) {
        configured_definition_id = replacement->definition_id;
        auto const definition = context.snapshot->by_id.find(
            configured_definition_id);
        if (definition == context.snapshot->by_id.end()) {
            throw std::runtime_error(
                "node replacement definition is unavailable: "
                + configured_definition_id);
        }
        auto const& provider = definition_provider(definition->second);
        auto const* signature = provider.signature ? provider.signature() : nullptr;
        if (!signature) {
            throw std::logic_error(
                "node replacement definition has no construction signature: "
                + configured_definition_id);
        }
        replacement_arguments.emplace(evaluate_configuration_json_arguments(
            nlohmann::ordered_json::parse(
                replacement->configuration_arguments_json),
            *signature));
        configured_arguments = replacement_arguments->arguments();
    }

    auto realized = context.owner->configure_cached(
        context,
        builder,
        child_address,
        details::builder_invocation_qualifier(relative_path),
        configured_definition_id,
        tiled_layout,
        configured_arguments);
    details::register_builder_embedded_message_handlers(
        builder,
        relative_path,
        realized.configured->message_handlers);
    if (!context.dependency_frames.empty()) {
        context.dependency_frames.back()->push_back(realized.configured);
    }
    NodeRef result(builder, realized.root);
    // A captured NodeRef addresses its declaration and member relative to the
    // module that made the call.
    result._set_semantic_message_path(relative_path.last(2));
    return result;
}

NodeInstancesBatchResult NodeInstances::configure_and_embed(
    std::shared_ptr<NodeDefinitionsSnapshot const> snapshot,
    GraphBuilder& root_builder,
    std::span<NodeInstanceConfigurationRequest const> requests,
    std::vector<ProjectNodeReplacement> const* replacement_override)
{
    if (!snapshot) {
        throw std::invalid_argument("NodeInstances batch requires a definitions snapshot");
    }
    ConfigurationContext context{
        .owner = this,
        .snapshot = std::move(snapshot),
    };
    if (replacement_override) {
        context.replacements = *replacement_override;
    } else {
        std::scoped_lock lock(mutex_);
        context.replacements = desired_node_replacements_;
    }

    // Every request is constructed directly in the root builder's session,
    // against exactly this snapshot's package tables.
    auto* const session = root_builder._builder_session();
    if (!session) {
        throw std::invalid_argument("NodeInstances batch requires a root builder session");
    }
    {
        std::vector<std::string> package_roots;
        std::vector<details::BuilderPackageView> packages;
        package_roots.reserve(context.snapshot->package_revisions.size());
        packages.reserve(context.snapshot->package_revisions.size());
        for (auto const& revision : context.snapshot->package_revisions) {
            if (!revision) continue;
            package_roots.push_back(revision->package_root.generic_string());
            packages.push_back(details::BuilderPackageView{
                .package_root = package_roots.back(),
                .definitions = revision->provider_definitions,
                .config_pointer_fields = revision->config_pointer_fields,
                .retained_globals = revision->retained_globals,
                .node_message_handlers = revision->node_message_handlers,
                .module_message_handlers = revision->module_message_handlers,
                .node_state_structures = revision->node_state_structures,
            });
        }
        details::set_builder_packages(session, packages);
    }
    struct ResolverInstallation {
        details::BuilderSession* session;
        ~ResolverInstallation()
        {
            details::set_builder_definition_resolver(session, nullptr, nullptr);
            details::set_builder_concrete_node_replacement_resolver(session, nullptr);
        }
    } const resolver_installation{session};
    details::set_builder_definition_resolver(
        session, &context, &NodeInstances::resolve_builder_definition);
    details::set_builder_concrete_node_replacement_resolver(
        session, &NodeInstances::resolve_builder_concrete_node_replacement);

    NodeInstancesBatchResult result;
    auto diagnose = [&](NodeInstanceConfigurationRequest const& request,
                        std::string message) {
        result.diagnostics.push_back({
            .instance_id = request.instance_id,
            .definition_id = request.definition_id,
            .message = std::move(message),
        });
    };
    auto signature_of = [&](std::string const& definition_id)
        -> std::pair<NodeDefinitionEntry const*, details::RegisteredSignature const*> {
        auto definition = context.snapshot->by_id.find(definition_id);
        if (definition == context.snapshot->by_id.end()) {
            throw std::runtime_error(
                "node definition '" + definition_id
                + "' is unavailable in this definitions snapshot");
        }
        auto const& provider = definition_provider(definition->second);
        auto const* signature = provider.signature ? provider.signature() : nullptr;
        if (!signature) {
            throw std::logic_error(
                "node definition '" + definition_id + "' has no construction signature");
        }
        return {&definition->second, signature};
    };

    // A project-node reference in authored source materializes as a live
    // NodeRef of the root session, so a referencing request is realized after
    // the request it references.
    struct Pending {
        NodeInstanceConfigurationRequest const* request = nullptr;
        std::shared_ptr<ExpressionCacheEntry const> parsed{};
        details::RegisteredSignature const* signature = nullptr;
        std::vector<std::string> references{};
        // Arguments evaluated before construction. A source with project-node
        // references is evaluated when the instances it references are placed.
        std::optional<EvaluatedConfigurationArguments> evaluated{};
    };

    // Phase 1: every problem that needs no construction, for every request.
    // Any of them fails the whole batch before anything is constructed.
    std::vector<Pending> pending;
    std::unordered_set<std::string> seen_ids;
    for (auto const& request : requests) {
        if (request.instance_id.empty()) {
            diagnose(request, "node instance request has an empty instance id");
            continue;
        }
        if (!seen_ids.insert(request.instance_id).second) {
            diagnose(request, "duplicate node instance id in one configuration batch");
            continue;
        }
        Pending item{.request = &request};
        try {
            if (request.configuration_source
                && request.configuration_arguments_json) {
                throw std::invalid_argument(
                    "node instance request has both authored source and "
                    "replacement arguments");
            }
            auto const [definition, signature] = signature_of(request.definition_id);
            item.signature = signature;
            if (request.configuration_source) {
                item.parsed = parse_expression_cached(
                    request.definition_id,
                    definition->version,
                    *request.configuration_source);
                item.references = configuration_referenced_instance_ids(
                    item.parsed->parsed);
                if (item.references.empty()) {
                    item.evaluated.emplace(evaluate_configuration_expression_list(
                        item.parsed->parsed, *signature));
                }
            } else if (request.configuration_arguments_json) {
                item.evaluated.emplace(evaluate_configuration_json_arguments(
                    nlohmann::ordered_json::parse(*request.configuration_arguments_json),
                    *signature));
            }
        } catch (std::exception const& error) {
            diagnose(request, error.what());
            continue;
        }
        pending.push_back(std::move(item));
    }
    for (auto const& item : pending) {
        for (auto const& reference : item.references) {
            if (!seen_ids.contains(reference)) {
                diagnose(*item.request,
                    "configuration reference names node instance '" + reference
                    + "', which this configuration batch does not declare");
            }
        }
    }
    // Realization order follows references; whatever cannot be ordered is a
    // reference cycle.
    std::vector<std::size_t> order;
    {
        std::vector<bool> ordered(pending.size(), false);
        auto unordered_request = [&](std::string const& instance_id) {
            for (std::size_t index = 0; index < pending.size(); ++index) {
                if (!ordered[index] && pending[index].request->instance_id == instance_id) {
                    return true;
                }
            }
            return false;
        };
        for (bool progressed = true; progressed;) {
            progressed = false;
            for (std::size_t index = 0; index < pending.size(); ++index) {
                if (ordered[index]
                    || std::ranges::any_of(pending[index].references, unordered_request)) {
                    continue;
                }
                ordered[index] = true;
                order.push_back(index);
                progressed = true;
            }
        }
        for (std::size_t index = 0; index < pending.size(); ++index) {
            if (!ordered[index]) {
                diagnose(*pending[index].request,
                    "configuration node references form a cycle");
            }
        }
    }
    if (!result.diagnostics.empty()) return result;

    auto materialize_reference = [&](ConfigurationNodeReferenceExpression const& reference)
        -> NodeRef {
        auto const placement = result.placements.find(reference.instance_id);
        if (placement == result.placements.end()) {
            throw std::logic_error(
                "configuration reference names node instance '"
                + reference.instance_id + "', which has no placement yet");
        }
        auto const& state = details::builder_session_state(session);
        auto const matches = resolve_project_node_path(
            ProjectNodePathGraph{state.node_bundles(), state.virtual_nodes()},
            placement->second.root,
            reference.path);
        auto const match = require_single_project_node_path_match(
            matches, "configuration node reference");
        if (match.kind == ProjectNodePathMatch::Kind::bundle) {
            return NodeRef(session, match.bundle);
        }
        if (match.virtual_member_indices.size() != 1) {
            throw std::invalid_argument(
                "configuration node reference selects a virtual node with "
                + std::to_string(match.virtual_member_indices.size())
                + " direct members; select one member");
        }
        auto const& members = state.virtual_nodes()
            .record(match.virtual_node).node_bundle_handles;
        return NodeRef(session, members.at(match.virtual_member_indices.front()));
    };

    // Phase 2: construction in reference order. The first failure stops the
    // batch; the session is failed and none of its placements may be used.
    for (auto const index : order) {
        auto& item = pending[index];
        auto const& request = *item.request;
        try {
            if (!item.evaluated && item.parsed) {
                ConfigurationNodeReferenceMaterializer const materializer =
                    materialize_reference;
                item.evaluated.emplace(evaluate_configuration_expression_list(
                    item.parsed->parsed, *item.signature, &materializer));
            }
            auto const arguments = item.evaluated
                ? item.evaluated->arguments()
                : request.arguments;
            auto realized = configure_cached(
                context,
                root_builder,
                NodeAddress{.instance_id = request.instance_id},
                "@&" + request.instance_id,
                request.definition_id,
                request.tiled_layout,
                arguments);
            details::throw_if_builder_failed(session);
            result.placements.emplace(request.instance_id, NodeInstancePlacement{
                .configured = std::move(realized.configured),
                .root = realized.root,
            });
        } catch (...) {
            details::record_builder_failure(
                session, std::current_exception(),
                "in project node instance '" + request.instance_id + "'");
            result.diagnostics.push_back({
                .instance_id = request.instance_id,
                .definition_id = request.definition_id,
                .message = details::builder_failure_message(session),
                .stacktrace = details::builder_failure_stacktrace(session),
            });
            result.placements.clear();
            return result;
        }
    }
    return result;
}

std::size_t NodeInstances::configuration_cache_size() const
{
    std::scoped_lock lock(mutex_);
    return configuration_cache_.size();
}

void NodeInstances::handle_node_definitions_snapshot_changed(
    NodeDefinitionsSnapshotChanged const& change)
{
    Changes changes;
    install_definitions_snapshot(changes, change.snapshot);
}

void NodeInstances::install_definitions_snapshot(
    Changes& changes,
    std::shared_ptr<NodeDefinitionsSnapshot const> const& snapshot)
{
    if (!snapshot) {
        throw std::invalid_argument("NodeDefinitions snapshot change is empty");
    }

    std::vector<std::shared_ptr<CacheEntry>> retired_cache_entries;
    bool list_changed = false;
    {
        std::scoped_lock lock(mutex_);
        definitions_snapshot_ = snapshot;
        (void)rebuild_required_definitions_locked();
        minimum_cache_generation_ = std::max(
            minimum_cache_generation_, snapshot->generation);
        std::vector<std::shared_ptr<CacheEntry>> retained_cache_entries;
        retained_cache_entries.reserve(configuration_cache_.size());
        for (auto& entry : configuration_cache_) {
            if (entry && entry->configured
                && configured_definition_closure_matches(
                    *entry->configured, *snapshot)) {
                retained_cache_entries.push_back(std::move(entry));
            } else {
                retired_cache_entries.push_back(std::move(entry));
            }
        }
        configuration_cache_ = std::move(retained_cache_entries);
        std::erase_if(expression_cache_, [&](auto const& entry) {
            if (!entry) return true;
            auto const definition = snapshot->by_id.find(entry->definition_id);
            return definition == snapshot->by_id.end()
                || definition->second.version != entry->definition_version;
        });

        std::unordered_set<std::string> realized_ids;
        for (auto& [instance_id, desired] : desired_instances_by_id_) {
            auto definition = definitions_snapshot_->by_id.find(desired.definition_id);
            if (definition == definitions_snapshot_->by_id.end()) continue;
            if (publish_instance_locked(instance_id, definition->second)) {
                list_changed = true;
            }
            realized_ids.insert(instance_id);
        }
        for (auto it = published_instances_by_id_.begin();
             it != published_instances_by_id_.end();) {
            if (realized_ids.contains(it->first)) {
                ++it;
                continue;
            }
            it = published_instances_by_id_.erase(it);
            list_changed = true;
        }
    }
    if (list_changed) changes.instances_list_changed = true;
}

void NodeInstances::handle_project_graph_transaction(
    NodeInstancesProjectGraphRequest& request)
{
    if (request.handled) {
        throw std::logic_error("NodeInstances ProjectGraph request was handled more than once");
    }
    request.handled = true;
    if (!request.snapshot) {
        throw std::invalid_argument("ProjectGraph transaction requires a definitions snapshot");
    }
    if (!request.root_builder) {
        throw std::invalid_argument("ProjectGraph transaction requires a root GraphBuilder");
    }

    Changes changes;
    bool install_snapshot = false;
    {
        std::scoped_lock lock(mutex_);
        install_snapshot = definitions_snapshot_.get() != request.snapshot.get();
    }
    if (install_snapshot) {
        install_definitions_snapshot(changes, request.snapshot);
    }

    std::visit(
        [&](auto const& mutation) {
            using Mutation = std::remove_cvref_t<decltype(mutation)>;
            if constexpr (std::same_as<Mutation, std::monostate>) {
                return;
            } else if constexpr (std::same_as<Mutation, NodeInstanceCreateMutation>) {
                std::optional<std::filesystem::path> package_root;
                {
                    std::scoped_lock lock(mutex_);
                    if (auto definition = definitions_snapshot_->by_id.find(mutation.definition_id);
                        definition != definitions_snapshot_->by_id.end()) {
                        package_root = definition_package_root(definition->second);
                    }
                }
                if (!package_root.has_value()) package_root = mutation.package_root;
                if (!package_root.has_value()) {
                    throw std::runtime_error(
                        "unknown loaded node definition: " + mutation.definition_id);
                }
                request.created_instance_ids.push_back(create_instance(
                    changes,
                    mutation.definition_id,
                    *package_root,
                    mutation.instance_id,
                    mutation.display_name,
                    mutation.configuration_source));
            } else if constexpr (std::same_as<Mutation, NodeInstanceDeleteMutation>) {
                remove_instance(changes, mutation.instance_id);
            } else if constexpr (std::same_as<Mutation, NodeInstanceUpdateMutation>) {
                std::vector<Update> updates;
                updates.reserve(mutation.updates.size());
                for (auto const& update : mutation.updates) {
                    updates.push_back(Update{
                        .instance_id = update.instance_id,
                        .display_name = update.display_name,
                        .configuration_source = update.configuration_source,
                    });
                }
                update_instances(changes, std::move(updates));
            } else if constexpr (std::same_as<Mutation, NodeInstanceReplaceAllMutation>) {
                std::unordered_set<std::string> instance_ids;
                std::unordered_map<std::string, DesiredInstance> desired;
                std::unordered_map<std::string, IvModuleRequiredDefinition> required;
                desired.reserve(mutation.instances.size());
                for (auto const& declaration : mutation.instances) {
                    if (declaration.instance_id.empty()) {
                        throw std::invalid_argument(
                            "project node instance id must not be empty");
                    }
                    if (declaration.definition_id.empty()) {
                        throw std::invalid_argument(
                            "project node definition id must not be empty");
                    }
                    if (!instance_ids.insert(declaration.instance_id).second) {
                        throw std::invalid_argument(
                            "duplicate project node instance id in replacement batch: "
                            + declaration.instance_id);
                    }

                    std::filesystem::path package_root;
                    if (auto definition = request.snapshot->by_id.find(
                            declaration.definition_id);
                        definition != request.snapshot->by_id.end()) {
                        package_root = definition_package_root(definition->second);
                    } else if (declaration.package_root
                               && !declaration.package_root->empty()) {
                        package_root = normalize_path(*declaration.package_root);
                    }
                    auto display_name = declaration.display_name.value_or(
                        declaration.definition_id);
                    if (display_name.empty()) display_name = declaration.definition_id;
                    desired.emplace(declaration.instance_id, DesiredInstance{
                        .instance_id = declaration.instance_id,
                        .definition_id = declaration.definition_id,
                        .display_name = std::move(display_name),
                        .package_root = package_root,
                        .configuration_source = declaration.configuration_source,
                    });

                    if (!package_root.empty()) {
                        auto [required_it, inserted] = required.emplace(
                            declaration.definition_id,
                            IvModuleRequiredDefinition{
                                .definition_id = declaration.definition_id,
                                .package_root = package_root,
                            });
                        if (!inserted
                            && required_it->second.package_root != package_root) {
                            throw std::invalid_argument(
                                "project node definition has conflicting package roots: "
                                + declaration.definition_id);
                        }
                    }
                }

                std::vector<ProjectNodeReplacement> replacements;
                replacements.reserve(mutation.node_replacements.size());
                for (auto replacement : mutation.node_replacements) {
                    if (replacement.address.instance_id.empty()
                        || !instance_ids.contains(replacement.address.instance_id)) {
                        throw std::invalid_argument(
                            "node replacement addresses an unknown project instance: "
                            + replacement.address.instance_id);
                    }
                    if (replacement.definition_id.empty()) {
                        throw std::invalid_argument(
                            "node replacement definition id must not be empty");
                    }
                    replacement.configuration_arguments_json =
                        canonical_replacement_arguments(
                            replacement.configuration_arguments_json);
                    if (std::ranges::any_of(
                            replacements,
                            [&](ProjectNodeReplacement const& existing) {
                                return existing.address == replacement.address;
                            })) {
                        throw std::invalid_argument(
                            "duplicate node replacement address in replacement batch");
                    }
                    if (auto definition = request.snapshot->by_id.find(
                            replacement.definition_id);
                        definition != request.snapshot->by_id.end()) {
                        replacement.package_root =
                            definition_package_root(definition->second);
                    } else if (replacement.package_root
                               && !replacement.package_root->empty()) {
                        replacement.package_root = normalize_path(
                            *replacement.package_root);
                    }
                    if (replacement.package_root
                        && !replacement.package_root->empty()) {
                        auto [required_it, inserted] = required.emplace(
                            replacement.definition_id,
                            IvModuleRequiredDefinition{
                                .definition_id = replacement.definition_id,
                                .package_root = *replacement.package_root,
                            });
                        if (!inserted
                            && required_it->second.package_root
                                != *replacement.package_root) {
                            throw std::invalid_argument(
                                "node replacement definition has conflicting package roots: "
                                + replacement.definition_id);
                        }
                    }
                    replacements.push_back(std::move(replacement));
                }

                IvModuleRequiredDefinitionsChanged required_diff;
                bool list_changed = false;
                bool published_changed = false;
                {
                    std::scoped_lock lock(mutex_);
                    list_changed = desired_instances_by_id_.size() != desired.size();
                    if (!list_changed) {
                        for (auto const& [instance_id, next] : desired) {
                            auto const current = desired_instances_by_id_.find(instance_id);
                            if (current == desired_instances_by_id_.end()
                                || current->second.instance_id != next.instance_id
                                || current->second.definition_id != next.definition_id
                                || current->second.display_name != next.display_name
                                || current->second.package_root != next.package_root
                                || current->second.configuration_source
                                    != next.configuration_source) {
                                list_changed = true;
                                break;
                            }
                        }
                    }

                    for (auto const& [definition_id, value] : required) {
                        auto const current = required_definitions_by_id_.find(definition_id);
                        if (current == required_definitions_by_id_.end()) {
                            required_diff.created.push_back(value);
                        } else if (current->second.package_root != value.package_root) {
                            required_diff.updated.push_back(value);
                        }
                    }
                    for (auto const& [definition_id, _] : required_definitions_by_id_) {
                        if (!required.contains(definition_id)) {
                            required_diff.deleted_definition_ids.push_back(definition_id);
                        }
                    }

                    std::unordered_map<std::string, IvModuleInstance> published;
                    for (auto const& [instance_id, next] : desired) {
                        auto const definition = request.snapshot->by_id.find(
                            next.definition_id);
                        if (definition == request.snapshot->by_id.end()) continue;
                        auto instance = make_instance(
                            definition->second,
                            instance_id,
                            next.display_name,
                            next.configuration_source);
                        auto const current = published_instances_by_id_.find(instance_id);
                        if (current == published_instances_by_id_.end()
                            || current->second != instance) {
                            published_changed = true;
                        }
                        published.emplace(instance_id, std::move(instance));
                    }
                    for (auto const& [instance_id, _] : published_instances_by_id_) {
                        if (!published.contains(instance_id)) {
                            published_changed = true;
                        }
                    }

                    desired_instances_by_id_ = std::move(desired);
                    desired_node_replacements_ = std::move(replacements);
                    required_definitions_by_id_ = std::move(required);
                    published_instances_by_id_ = std::move(published);
                }

                if (!required_diff.created.empty()
                    || !required_diff.updated.empty()
                    || !required_diff.deleted_definition_ids.empty()) {
                    IV_INVOKE_LINKER_EVENT(
                        iv_runtime_iv_module_required_definitions_changed_event,
                        required_diff);
                }
                if (list_changed || published_changed) {
                    changes.instances_list_changed = true;
                }
                if (list_changed) changes.instance_declarations_changed = true;
            } else if constexpr (std::same_as<
                                     Mutation,
                                     NodeReplacementPreviewMutation>) {
                std::vector<NodeAddress> addresses;
                addresses.reserve(mutation.replacements.size());
                for (auto const& replacement : mutation.replacements) {
                    if (replacement.address.instance_id.empty()
                        || replacement.definition_id.empty()) {
                        throw std::invalid_argument(
                            "node replacement preview has an incomplete replacement");
                    }
                    if (!request.snapshot->by_id.contains(
                            replacement.definition_id)) {
                        throw std::runtime_error(
                            "node replacement definition is not loaded: "
                            + replacement.definition_id);
                    }
                    (void)canonical_replacement_arguments(
                        replacement.configuration_arguments_json);
                    if (std::ranges::contains(
                            addresses, replacement.address)) {
                        throw std::invalid_argument(
                            "node replacement preview contains a duplicate address");
                    }
                    addresses.push_back(replacement.address);
                }
                std::scoped_lock lock(mutex_);
                for (auto const& replacement : mutation.replacements) {
                    if (!desired_instances_by_id_.contains(
                            replacement.address.instance_id)) {
                        throw std::runtime_error(
                            "node replacement addresses an unknown project instance: "
                            + replacement.address.instance_id);
                    }
                }
            } else if constexpr (std::same_as<
                                     Mutation,
                                     NodeReplacementsReplaceMutation>) {
                auto replacements = mutation.replacements;
                std::vector<NodeAddress> addresses;
                addresses.reserve(replacements.size());
                for (auto& replacement : replacements) {
                    if (replacement.address.instance_id.empty()
                        || replacement.definition_id.empty()) {
                        throw std::invalid_argument(
                            "node replacement set has an incomplete replacement");
                    }
                    replacement.configuration_arguments_json =
                        canonical_replacement_arguments(
                            replacement.configuration_arguments_json);
                    auto const definition = request.snapshot->by_id.find(
                        replacement.definition_id);
                    if (definition == request.snapshot->by_id.end()) {
                        throw std::runtime_error(
                            "node replacement definition is not loaded: "
                            + replacement.definition_id);
                    }
                    replacement.package_root =
                        definition_package_root(definition->second);
                    if (std::ranges::contains(
                            addresses, replacement.address)) {
                        throw std::invalid_argument(
                            "node replacement set contains a duplicate address");
                    }
                    addresses.push_back(replacement.address);
                }

                IvModuleRequiredDefinitionsChanged required_diff;
                {
                    std::scoped_lock lock(mutex_);
                    for (auto const& replacement : replacements) {
                        if (!desired_instances_by_id_.contains(
                                replacement.address.instance_id)) {
                            throw std::runtime_error(
                                "node replacement addresses an unknown project instance: "
                                + replacement.address.instance_id);
                        }
                    }
                    if (desired_node_replacements_ != replacements) {
                        desired_node_replacements_ = std::move(replacements);
                    }
                    required_diff = rebuild_required_definitions_locked();
                }
                if (!required_diff.created.empty()
                    || !required_diff.updated.empty()
                    || !required_diff.deleted_definition_ids.empty()) {
                    IV_INVOKE_LINKER_EVENT(
                        iv_runtime_iv_module_required_definitions_changed_event,
                        required_diff);
                }
            }
        },
        request.mutation);

    std::vector<ProjectNodeReplacement> replacements_for_realization;
    if (auto const* preview = std::get_if<NodeReplacementPreviewMutation>(
            &request.mutation)) {
        replacements_for_realization = preview->replacements;
    } else {
        std::scoped_lock lock(mutex_);
        replacements_for_realization = desired_node_replacements_;
    }
    request.realized_node_replacements = replacements_for_realization;

    std::vector<NodeInstanceConfigurationRequest> configuration_requests;
    {
        std::scoped_lock lock(mutex_);
        configuration_requests.reserve(desired_instances_by_id_.size());
        for (auto const& [instance_id, desired] : desired_instances_by_id_) {
            auto const replacement = std::ranges::find_if(
                replacements_for_realization,
                [&](ProjectNodeReplacement const& candidate) {
                    return candidate.address.instance_id == instance_id
                        && candidate.address.path.empty();
                });
            configuration_requests.push_back(NodeInstanceConfigurationRequest{
                .instance_id = instance_id,
                .definition_id = replacement == replacements_for_realization.end()
                    ? desired.definition_id
                    : replacement->definition_id,
                .configuration_source = replacement == replacements_for_realization.end()
                    ? std::optional<std::string>(desired.configuration_source)
                    : std::nullopt,
                .configuration_arguments_json =
                    replacement == replacements_for_realization.end()
                    ? std::nullopt
                    : std::optional<std::string>(
                        replacement->configuration_arguments_json),
            });
        }
    }
    std::ranges::sort(
        configuration_requests, {}, &NodeInstanceConfigurationRequest::instance_id);
    request.result = configure_and_embed(
        request.snapshot,
        *request.root_builder,
        configuration_requests,
        &replacements_for_realization);

    request.instances_list_changed = changes.instances_list_changed;
    request.instance_declarations_changed = changes.instance_declarations_changed;
    if (changes.instances_list_changed || changes.instance_declarations_changed) {
        request.instances = list_instances();
    }
}

void NodeInstances::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder& builder) const
{
    builder.add_node_instances(desired_instances());
    builder.add_node_replacements(desired_node_replacements());
}

void NodeInstances::handle_socket_rpc_get_node_instances(
    GetNodeInstancesRequest const& request,
    SocketRpcNodeInstancesResultBuilder& builder) const
{
    auto instances = list_instances();
    if (request.source_file_path.has_value()) {
        IvModuleInstancesSourceFileFilterBuilder filter_builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_instances_source_file_filter_event,
            *request.source_file_path,
            instances,
            filter_builder);
        if (filter_builder.has_response()) instances = filter_builder.build();
    }
    builder.succeed(std::move(instances));
}
} // namespace iv
