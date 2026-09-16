#include <intravenous/runtime/node_instances.h>

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/module/builder_session.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/package_pipeline_types.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/uuid.h>

#include <algorithm>
#include <new>
#include <ranges>
#include <stdexcept>
#include <system_error>
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

IvModuleInstance make_instance(
    NodeDefinitionEntry const& definition,
    std::string const& instance_id,
    std::string const& display_name)
{
    return IvModuleInstance{
        .instance_id = instance_id,
        .definition_id = definition.definition_id,
        .display_name = display_name,
        .package_root = definition_package_root(definition),
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
};

struct NodeInstances::ConfigurationContext {
    NodeInstances* owner = nullptr;
    std::shared_ptr<NodeDefinitionsSnapshot const> snapshot{};
    std::vector<std::string> stack{};
    std::vector<std::vector<std::shared_ptr<ConfiguredNodeInstance const>>*>
        dependency_frames{};
};

NodeInstances::NodeInstances()
    : definitions_snapshot_(std::make_shared<NodeDefinitionsSnapshot const>())
{}

NodeInstances::~NodeInstances() = default;

bool NodeInstances::publish_instance_locked(
    std::string const& instance_id,
    NodeDefinitionEntry const& definition,
    IvModuleInstancesChanged& diff)
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
        desired->second.display_name);
    auto existing = published_instances_by_id_.find(instance_id);
    if (existing != published_instances_by_id_.end() && existing->second == instance) {
        return false;
    }
    auto const is_new = existing == published_instances_by_id_.end();
    published_instances_by_id_[instance_id] = instance;
    (is_new ? diff.created : diff.updated).push_back(std::move(instance));
    return true;
}

void NodeInstances::publish_instance_changes(
    IvModuleInstancesChanged diff,
    bool list_changed) const
{
    if (!diff.created.empty() || !diff.updated.empty() || !diff.deleted_instance_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(iv_runtime_iv_module_instances_changed_event, diff);
    }
    if (list_changed) {
        IV_INVOKE_LINKER_EVENT(iv_runtime_iv_module_instances_list_changed_event, list_instances());
    }
}

void NodeInstances::publish_instance_declarations_changed() const
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_iv_module_instance_declarations_changed_event,
        list_instances());
}

std::string NodeInstances::create_instance(
    std::string_view definition_id,
    std::filesystem::path package_root,
    std::optional<std::string> requested_instance_id,
    std::optional<std::string> requested_display_name)
{
    IvModuleRequiredDefinitionsChanged required_diff;
    IvModuleInstancesChanged instance_diff;
    auto const definition_key = std::string(definition_id);
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

        desired_instances_by_id_.emplace(instance_id, DesiredInstance{
            .instance_id = instance_id,
            .definition_id = definition_key,
            .display_name = std::move(display_name),
            .package_root = normalized_root,
        });

        if (auto required = required_definitions_by_id_.find(definition_key);
            required == required_definitions_by_id_.end()) {
            auto value = IvModuleRequiredDefinition{
                .definition_id = definition_key,
                .package_root = normalized_root,
            };
            required_definitions_by_id_.emplace(definition_key, value);
            required_diff.created.push_back(std::move(value));
        } else if (required->second.package_root != normalized_root) {
            required->second.package_root = normalized_root;
            required_diff.updated.push_back(required->second);
        }

        if (definitions_snapshot_) {
            if (auto definition = definitions_snapshot_->by_id.find(definition_key);
                definition != definitions_snapshot_->by_id.end()) {
                publish_instance_locked(instance_id, definition->second, instance_diff);
            }
        }
    }

    if (!required_diff.created.empty() || !required_diff.updated.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_required_definitions_changed_event,
            required_diff);
    }
    publish_instance_changes(std::move(instance_diff), true);
    publish_instance_declarations_changed();
    return instance_id;
}

void NodeInstances::remove_instance(std::string const& instance_id)
{
    IvModuleRequiredDefinitionsChanged required_diff;
    IvModuleInstancesChanged instance_diff;
    bool changed = false;

    {
        std::scoped_lock lock(mutex_);
        auto desired = desired_instances_by_id_.find(instance_id);
        if (desired == desired_instances_by_id_.end()) return;
        auto const definition_id = desired->second.definition_id;
        desired_instances_by_id_.erase(desired);
        changed = true;

        if (published_instances_by_id_.erase(instance_id) > 0) {
            instance_diff.deleted_instance_ids.push_back(instance_id);
        }

        auto const still_required = std::ranges::any_of(
            desired_instances_by_id_,
            [&](auto const& entry) { return entry.second.definition_id == definition_id; });
        if (!still_required && required_definitions_by_id_.erase(definition_id) > 0) {
            required_diff.deleted_definition_ids.push_back(definition_id);
        }
    }

    publish_instance_changes(std::move(instance_diff), changed);
    if (changed) publish_instance_declarations_changed();
    if (!required_diff.deleted_definition_ids.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_iv_module_required_definitions_changed_event,
            required_diff);
    }
}

void NodeInstances::update_instances(std::vector<Update> updates)
{
    IvModuleInstancesChanged diff;
    bool list_changed = false;
    {
        std::scoped_lock lock(mutex_);
        for (auto const& update : updates) {
            auto desired = desired_instances_by_id_.find(update.instance_id);
            if (desired == desired_instances_by_id_.end()) {
                throw std::runtime_error("unknown node instance id: " + update.instance_id);
            }
            if (!update.display_name.has_value()) continue;
            auto next = *update.display_name;
            if (next.empty()) next = desired->second.definition_id;
            if (desired->second.display_name == next) continue;
            desired->second.display_name = std::move(next);
            list_changed = true;
            if (auto instance = published_instances_by_id_.find(update.instance_id);
                instance != published_instances_by_id_.end()) {
                instance->second.display_name = desired->second.display_name;
                diff.updated.push_back(instance->second);
            }
        }
    }
    publish_instance_changes(std::move(diff), list_changed);
    if (list_changed) publish_instance_declarations_changed();
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

std::shared_ptr<NodeInstances::CacheEntry> NodeInstances::configure_cached(
    ConfigurationContext& context,
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

    bool ownable = true;
    bool cacheable = true;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        auto const* operations = signature->parameter_operations[index];
        ownable = ownable
            && OwnedConfigurationArgument::can_own(arguments[index], operations);
        cacheable = cacheable
            && OwnedConfigurationArgument::can_cache(arguments[index], operations);
    }
    if (!ownable) {
        throw std::invalid_argument(
            "node definition '" + std::string(definition_id)
            + "' has a configuration argument that cannot be retained safely");
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
            if (configured.definitions_generation != context.snapshot->generation
                || configured.definition_version != definition.version
                || configured.definition_id != definition.definition_id
                || candidate->tiled_layout != tiled_layout
                || candidate->argument_hash != argument_hash
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
    if (auto cached = find_cached()) return cached;

    ConfigurationStackEntry const stack_entry(context.stack, definition_id);

    // Invocation values are a second owned copy. Provider code is allowed to
    // move from or mutate its arguments; the pristine key above must remain
    // value-comparable for future cache lookups.
    auto argument_lifetime = std::make_shared<RetainedConfigurationArguments>();
    argument_lifetime->values.reserve(arguments.size());
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        argument_lifetime->values.push_back(OwnedConfigurationArgument::clone(
            arguments[index], signature->parameter_operations[index]));
    }
    std::vector<details::ConfigurationArgument> invocation_arguments;
    invocation_arguments.reserve(argument_lifetime->values.size());
    for (auto& argument : argument_lifetime->values) {
        invocation_arguments.push_back(argument.argument());
    }

    using Session = std::unique_ptr<details::BuilderSession,
        decltype(&details::iv_builder_session_destroy)>;
    Session session(
        details::iv_builder_session_create(),
        details::iv_builder_session_destroy);
    if (!session) throw std::runtime_error("could not create node configuration session");

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
            .node_state_structures = revision->node_state_structures,
        });
    }
    details::set_builder_packages(session.get(), packages);
    details::set_builder_definition_resolver(
        session.get(), &context, &NodeInstances::resolve_builder_definition);

    std::vector<std::shared_ptr<ConfiguredNodeInstance const>> dependencies;
    context.dependency_frames.push_back(&dependencies);
    struct DependencyFrameGuard {
        ConfigurationContext& context;
        ~DependencyFrameGuard() { context.dependency_frames.pop_back(); }
    } dependency_frame_guard{context};

    GraphBuilder builder(session.get());
    auto root = details::configure_package_definition_provider(
        builder,
        definition_id,
        tiled_layout,
        invocation_arguments);
    auto const local_root = root.node_bundle_handle();
    builder.outputs();
    auto graph = std::make_shared<ConfiguredGraph const>(
        details::take_built_graph(session.get()));

    auto configured = std::make_shared<ConfiguredNodeInstance const>(ConfiguredNodeInstance{
        .definition_id = definition.definition_id,
        .definitions_generation = context.snapshot->generation,
        .definition_version = definition.version,
        .local_root = local_root,
        .definitions = context.snapshot,
        .configuration_lifetime = std::move(argument_lifetime),
        .dependencies = std::move(dependencies),
        .graph = std::move(graph),
    });
    auto entry = std::make_shared<CacheEntry>();
    entry->configured = std::move(configured);
    entry->tiled_layout = tiled_layout;
    entry->argument_hash = argument_hash;
    entry->key_arguments = std::move(key_arguments);

    if (cacheable) {
        // Equality/hash callbacks belong to package code and may execute user
        // operators. Never invoke them while holding the app-module mutex.
        if (auto existing = find_cached()) return existing;
        std::scoped_lock lock(mutex_);
        // A newer definitions publication may have invalidated the cache while
        // this older batch was still evaluating. Let the batch finish against
        // its pinned snapshot, but do not resurrect an invalidated generation.
        if (context.snapshot->generation >= minimum_cache_generation_) {
            configuration_cache_.push_back(entry);
        }
    }
    return entry;
}
NodeRef NodeInstances::resolve_builder_definition(
    void* opaque_context,
    GraphBuilder& builder,
    std::string_view definition_id,
    std::optional<ChannelLayout> tiled_layout,
    std::span<details::ConfigurationArgument> arguments)
{
    if (!opaque_context) {
        throw std::logic_error("node configuration resolver has no batch context");
    }
    auto& context = *static_cast<ConfigurationContext*>(opaque_context);
    auto cached = context.owner->configure_cached(
        context, definition_id, tiled_layout, arguments);
    if (!context.dependency_frames.empty()) {
        context.dependency_frames.back()->push_back(cached->configured);
    }
    auto embedding = builder.embed(*cached->configured->graph, "Cached node instance");
    return NodeRef(builder, embedding.node_bundle(cached->configured->local_root));
}

NodeInstancesBatchResult NodeInstances::configure_and_embed(
    std::shared_ptr<NodeDefinitionsSnapshot const> snapshot,
    GraphBuilder& root_builder,
    std::span<NodeInstanceConfigurationRequest const> requests)
{
    if (!snapshot) {
        throw std::invalid_argument("NodeInstances batch requires a definitions snapshot");
    }
    ConfigurationContext context{
        .owner = this,
        .snapshot = std::move(snapshot),
    };
    NodeInstancesBatchResult result;
    std::unordered_set<std::string> seen_ids;
    for (auto const& request : requests) {
        if (request.instance_id.empty()) {
            result.diagnostics.push_back({
                .instance_id = request.instance_id,
                .definition_id = request.definition_id,
                .message = "node instance request has an empty instance id",
            });
            continue;
        }
        if (!seen_ids.insert(request.instance_id).second) {
            result.diagnostics.push_back({
                .instance_id = request.instance_id,
                .definition_id = request.definition_id,
                .message = "duplicate node instance id in one configuration batch",
            });
            continue;
        }
        try {
            auto cached = configure_cached(
                context,
                request.definition_id,
                request.tiled_layout,
                request.arguments);
            auto embedding = root_builder.embed(
                *cached->configured->graph, "Project node instance");
            auto const root = embedding.node_bundle(cached->configured->local_root);
            result.placements.emplace(request.instance_id, NodeInstancePlacement{
                .configured = cached->configured,
                .embedding = std::move(embedding),
                .root = root,
            });
        } catch (std::exception const& error) {
            result.diagnostics.push_back({
                .instance_id = request.instance_id,
                .definition_id = request.definition_id,
                .message = error.what(),
            });
        } catch (...) {
            result.diagnostics.push_back({
                .instance_id = request.instance_id,
                .definition_id = request.definition_id,
                .message = "unknown node configuration failure",
            });
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
    if (!change.snapshot) {
        throw std::invalid_argument("NodeDefinitions snapshot change is empty");
    }

    IvModuleInstancesChanged diff;
    bool list_changed = false;
    {
        std::scoped_lock lock(mutex_);
        definitions_snapshot_ = change.snapshot;
        // Correctness-first invalidation. Preserving entries whose transitive
        // provider versions are unchanged is the later caching optimization pass.
        minimum_cache_generation_ = std::max(
            minimum_cache_generation_, change.snapshot->generation);
        configuration_cache_.clear();

        std::unordered_set<std::string> realized_ids;
        for (auto& [instance_id, desired] : desired_instances_by_id_) {
            auto definition = definitions_snapshot_->by_id.find(desired.definition_id);
            if (definition == definitions_snapshot_->by_id.end()) continue;
            if (publish_instance_locked(instance_id, definition->second, diff)) {
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
            diff.deleted_instance_ids.push_back(it->first);
            it = published_instances_by_id_.erase(it);
            list_changed = true;
        }
    }
    publish_instance_changes(std::move(diff), list_changed);
}

void NodeInstances::handle_project_create_iv_module_instance(
    ProjectCreateIvModuleInstanceRequest const& request,
    ProjectStringBuilder& builder)
{
    std::optional<std::filesystem::path> package_root;
    {
        std::scoped_lock lock(mutex_);
        if (definitions_snapshot_) {
            if (auto definition = definitions_snapshot_->by_id.find(request.module_id);
                definition != definitions_snapshot_->by_id.end()) {
                package_root = definition_package_root(definition->second);
            }
        }
    }
    if (!package_root.has_value()) package_root = request.package_root;
    if (!package_root.has_value()) {
        throw std::runtime_error("unknown loaded node definition: " + request.module_id);
    }

    builder.succeed(create_instance(
        request.module_id,
        *package_root,
        request.instance_id,
        request.display_name));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void NodeInstances::handle_project_delete_iv_module_instance(
    ProjectDeleteIvModuleInstanceRequest const& request,
    ProjectAckBuilder& builder)
{
    remove_instance(request.instance_id);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void NodeInstances::handle_project_update_iv_module_instances(
    ProjectUpdateIvModuleInstancesRequest const& request,
    ProjectAckBuilder& builder)
{
    std::vector<Update> updates;
    updates.reserve(request.updates.size());
    for (auto const& update : request.updates) {
        updates.push_back(Update{
            .instance_id = update.instance_id,
            .display_name = update.display_name,
        });
    }
    update_instances(std::move(updates));
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void NodeInstances::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder& builder) const
{
    builder.add_iv_module_instances(list_instances());
}

void NodeInstances::handle_socket_rpc_create_iv_module_instance(
    CreateIvModuleInstanceRequest const& request,
    SocketRpcCreateIvModuleInstanceResultBuilder& builder)
{
    try {
        ProjectStringBuilder project_builder;
        handle_project_create_iv_module_instance(
            ProjectCreateIvModuleInstanceRequest{
                .module_id = request.module_id,
                .display_name = request.display_name,
            },
            project_builder);
        builder.succeed(project_builder.build());
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void NodeInstances::handle_socket_rpc_delete_iv_module_instance(
    DeleteIvModuleInstanceRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_delete_iv_module_instance(
            ProjectDeleteIvModuleInstanceRequest{.instance_id = request.instance_id},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void NodeInstances::handle_socket_rpc_update_iv_module_instances(
    UpdateIvModuleInstancesRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        std::vector<ProjectUpdateIvModuleInstance> updates;
        updates.reserve(request.updates.size());
        for (auto const& update : request.updates) {
            updates.push_back(ProjectUpdateIvModuleInstance{
                .instance_id = update.instance_id,
                .display_name = update.display_name,
            });
        }
        ProjectAckBuilder project_builder;
        handle_project_update_iv_module_instances(
            ProjectUpdateIvModuleInstancesRequest{.updates = std::move(updates)},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void NodeInstances::handle_socket_rpc_get_iv_module_instances(
    GetIvModuleInstancesRequest const& request,
    SocketRpcIvModuleInstancesResultBuilder& builder) const
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
