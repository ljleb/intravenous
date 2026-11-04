#include <intravenous/runtime/project_graph.h>

#include <intravenous/graph/builder.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/configuration_expression.h>
#include <intravenous/runtime/package_pipeline_types.h>
#include <intravenous/runtime/project_graph_events.h>
#include <intravenous/runtime/project_node_path_resolver.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <intravenous/runtime/socket_rpc_response_builders.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <iterator>
#include <ranges>
#include <span>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace iv {
namespace {
std::string identity_string(char const* data, std::size_t size)
{
    if (!data && size != 0) {
        throw std::logic_error("semantic event identity has invalid string storage");
    }
    return std::string(data ? data : "", size);
}

NodeEventValueSchema node_event_value_schema(
    details::ConfigurationExpressionTypeSchema const& source)
{
    using SourceKind = details::ConfigurationExpressionTypeKind;
    NodeEventValueSchema result;
    switch (source.kind) {
    case SourceKind::boolean:
        result.kind = NodeEventValueKind::boolean;
        break;
    case SourceKind::signed_integer:
        result.kind = NodeEventValueKind::signed_integer;
        result.integer_is_signed = true;
        result.integer_bit_width = source.integer_bit_width;
        break;
    case SourceKind::unsigned_integer:
        result.kind = NodeEventValueKind::unsigned_integer;
        result.integer_bit_width = source.integer_bit_width;
        break;
    case SourceKind::floating_point:
        result.kind = NodeEventValueKind::floating_point;
        result.integer_bit_width = source.size * 8;
        break;
    case SourceKind::string:
        result.kind = NodeEventValueKind::string;
        break;
    case SourceKind::converted_scalar:
        result.kind = NodeEventValueKind::converted_scalar;
        break;
    case SourceKind::enumeration:
        result.kind = NodeEventValueKind::enumeration;
        result.integer_is_signed = source.integer_is_signed;
        result.integer_bit_width = source.integer_bit_width;
        for (auto const& enumerator : std::span(
                 source.enumerators, source.enumerator_count)) {
            result.enumerators.push_back(NodeEventEnumeratorSchema{
                .name = identity_string(enumerator.name, enumerator.name_size),
                .value = source.integer_is_signed
                    ? std::to_string(enumerator.signed_value)
                    : std::to_string(enumerator.unsigned_value),
            });
        }
        break;
    case SourceKind::aggregate:
        result.kind = NodeEventValueKind::aggregate;
        for (auto const& field : std::span(source.fields, source.field_count)) {
            if (!field.type) {
                throw std::logic_error(
                    "semantic event aggregate field has no value schema");
            }
            result.fields.push_back(NodeEventFieldSchema{
                .name = identity_string(field.name, field.name_size),
                .value = std::make_shared<NodeEventValueSchema const>(
                    node_event_value_schema(*field.type)),
            });
        }
        break;
    case SourceKind::sequence:
        result.kind = NodeEventValueKind::sequence;
        if (!source.element_type) {
            throw std::logic_error(
                "semantic event sequence has no element schema");
        }
        result.element = std::make_shared<NodeEventValueSchema const>(
            node_event_value_schema(*source.element_type));
        result.fixed_element_count = source.fixed_element_count;
        break;
    case SourceKind::node_address:
        result.kind = NodeEventValueKind::node_address;
        break;
    case SourceKind::node_reference:
        result.kind = NodeEventValueKind::unsupported;
        result.unsupported_reason =
            "project NodeRef values cannot cross nodeMessages.dispatch";
        break;
    case SourceKind::unsupported:
        result.kind = NodeEventValueKind::unsupported;
        result.unsupported_reason =
            "provider type has no semantic event JSON representation";
        break;
    }
    return result;
}

NodeEventTypeSchema node_event_type_schema(
    details::ConfigurationTypeIdentity const& source)
{
    if (!source.expression_schema) {
        throw std::logic_error("semantic event type has no payload schema");
    }
    auto result = NodeEventTypeSchema{
        .id = {
            .nominal_id = identity_string(
                source.nominal_id, source.nominal_id_size),
            .definition_fingerprint = identity_string(
                source.definition_fingerprint,
                source.definition_fingerprint_size),
        },
        .display_name = identity_string(
            source.display_name, source.display_name_size),
        .payload = node_event_value_schema(*source.expression_schema),
    };
    if (result.id.nominal_id.empty()
        || result.id.definition_fingerprint.empty()) {
        throw std::logic_error("semantic event type has incomplete stable identity");
    }
    return result;
}

bool module_handler_address_is_selected(
    NodeAddress const& target,
    BoundNodeMessageHandler const& handler)
{
    if (!handler.module_local_refs
        || target.instance_id != handler.address.instance_id) {
        return false;
    }
    if (target.path == handler.address.path) return true;
    if (target.path.empty()
        || !std::holds_alternative<ProjectVirtualNodeSelector>(
            target.path.back())
        || handler.address.path.size() < target.path.size() + 1
        || handler.address.path.size() > target.path.size() + 2
        || !std::equal(
            target.path.begin(), target.path.end(),
            handler.address.path.begin())
        || !std::holds_alternative<ProjectVirtualMemberSelector>(
            handler.address.path[target.path.size()])) {
        return false;
    }
    return handler.address.path.size() == target.path.size() + 1
        || std::holds_alternative<ProjectTiledChildSelector>(
            handler.address.path.back());
}
} // namespace

ProjectGraph::ProjectGraph()
    : definitions_snapshot_(std::make_shared<NodeDefinitionsSnapshot const>())
{}

ProjectGraph::RebuildResult ProjectGraph::rebuild_locked(
    NodeInstancesMutation instance_mutation,
    GraphConnectionsMutation connection_mutation,
    bool publish_execution)
{
    GraphBuilder root_builder;
    NodeInstancesProjectGraphRequest request{
        .snapshot = definitions_snapshot_,
        .root_builder = &root_builder,
        .mutation = std::move(instance_mutation),
    };
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_graph_node_instances_requested_event,
        request);
    if (!request.handled) {
        throw std::runtime_error("ProjectGraph cannot rebuild without NodeInstances");
    }

    GraphConnectionsProjectGraphRequest connections_request{
        .root_builder = &root_builder,
        .placements = &request.result.placements,
        .mutation = std::move(connection_mutation),
    };
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_graph_connections_requested_event,
        connections_request);
    if (!connections_request.handled) {
        throw std::runtime_error("ProjectGraph cannot rebuild without GraphConnections");
    }

    root_builder.outputs();
    auto graph = std::make_shared<ConfiguredGraph const>(std::move(root_builder).finish());
    auto generation = std::make_shared<ProjectGraphGeneration>();
    generation->generation = next_generation_;
    generation->definitions_generation = definitions_snapshot_->generation;
    generation->definitions = definitions_snapshot_;
    generation->graph = std::move(graph);
    generation->placements = std::move(request.result.placements);
    generation->diagnostics = std::move(request.result.diagnostics);
    generation->applied_connection_ids =
        std::move(connections_request.result.applied_connection_ids);
    generation->connection_diagnostics =
        std::move(connections_request.result.diagnostics);
    generation->node_replacements =
        std::move(request.realized_node_replacements);
    for (auto const& [instance_id, placement] : generation->placements) {
        if (!placement.configured) continue;
        for (auto const& handler : placement.configured->message_handlers) {
            auto address = NodeAddress{.instance_id = instance_id};
            address.path = handler.relative_path;
            generation->message_handlers.push_back(BoundNodeMessageHandler{
                .address = std::move(address),
                .event_type = handler.event_type,
                .receiver = handler.receiver,
                .module_local_refs = handler.module_local_refs,
                .invoke = handler.invoke,
            });
        }
    }

    if (publish_execution) {
        auto graph_jit_result = IV_INVOKE_SINGLETON_EVENT(
            iv_runtime_project_graph_graph_jit_requested_event,
            ProjectGraphGraphJitRequest{
                .successor = {
                    .project_generation = generation->generation,
                    .graph = generation->graph,
                    .definitions = generation->definitions,
                },
                .predecessor = accepted_execution_graph_,
            });
        generation->graph_jit_attempted = graph_jit_result.attempted;
        generation->compiled_graph = std::move(graph_jit_result.compiled_graph);
        generation->graph_jit_diagnostics = std::move(graph_jit_result.diagnostics);
        if (generation->compiled_graph) {
            auto const execution_result = IV_INVOKE_SINGLETON_EVENT(
                iv_runtime_project_graph_graph_executor_requested_event,
                generation->compiled_graph);
            generation->graph_executor_attempted = execution_result.attempted;
            generation->graph_executor_accepted = execution_result.accepted;
            if (generation->graph_executor_accepted) {
                accepted_execution_graph_ = generation->compiled_graph;
            }
        }
        current_generation_ = generation;
        ++next_generation_;
    }
    return RebuildResult{
        .generation = std::move(generation),
        .created_instance_ids = std::move(request.created_instance_ids),
    };
}

struct ProjectGraph::NodeMessageInteraction {
    struct ReplacementIdentity {
        NodeAddress address{};
        std::string definition_id{};
        std::string configuration_arguments_json{};

        bool operator==(ReplacementIdentity const&) const = default;
    };

    struct DeliveredHandler {
        NodeAddress address{};
        std::vector<ReplacementIdentity> replacement_lineage{};

        bool operator==(DeliveredHandler const&) const = default;
    };

    std::shared_ptr<ProjectGraphGeneration const> generation{};
    std::vector<DeliveredHandler> delivered{};
    std::vector<ProjectNodeReplacement> original_replacements{};
    std::vector<ProjectNodeReplacement> replacements{};
};

std::shared_ptr<ProjectGraphGeneration const> ProjectGraph::current_generation() const
{
    std::scoped_lock lock(mutex_);
    return current_generation_;
}

NodeEventProtocolSnapshot ProjectGraph::node_event_protocol() const
{
    std::lock_guard lock(mutex_);
    NodeEventProtocolSnapshot result;
    auto const generation = current_generation_;
    if (!generation) return result;

    result.project_graph_generation = generation->generation;
    result.definitions_generation = generation->definitions_generation;
    auto add_event = [&](details::ConfigurationTypeIdentity const* identity) {
        if (!identity) {
            throw std::logic_error("semantic event handler has no event type");
        }
        auto event = node_event_type_schema(*identity);
        auto id = event.id;
        auto const existing = std::ranges::find_if(
            result.event_types,
            [&](NodeEventTypeSchema const& value) {
                return value.id == event.id;
            });
        if (existing == result.event_types.end()) {
            result.event_types.push_back(std::move(event));
        }
        return id;
    };
    auto add_definition_event = [&](std::string definition_id,
                                    NodeEventTypeId id) {
        if (definition_id.empty()) return;
        auto definition = std::ranges::find_if(
            result.definitions,
            [&](NodeEventDefinitionProtocol const& value) {
                return value.definition_id == definition_id;
            });
        if (definition == result.definitions.end()) {
            result.definitions.push_back(NodeEventDefinitionProtocol{
                .definition_id = std::move(definition_id),
            });
            definition = std::prev(result.definitions.end());
        }
        if (!std::ranges::contains(definition->supported_event_types, id)) {
            definition->supported_event_types.push_back(std::move(id));
        }
    };

    if (generation->definitions) {
        for (auto const& revision : generation->definitions->package_revisions) {
            if (!revision) continue;
            for (auto const& handler : revision->node_message_handlers) {
                auto id = add_event(handler.event_type);
                auto const definition = std::ranges::find_if(
                    revision->leaf_definitions,
                    [&](PackageLeafDefinition const& value) {
                        return value.compiler_record.code_key == handler.code_key;
                    });
                if (definition != revision->leaf_definitions.end()) {
                    add_definition_event(definition->definition_id, std::move(id));
                }
            }
            for (auto const& handler : revision->module_message_handlers) {
                auto id = add_event(handler.event_type);
                add_definition_event(
                    identity_string(
                        static_cast<char const*>(handler.definition_id.data),
                        handler.definition_id.size),
                    std::move(id));
            }
        }
    }
    for (auto const& handler : generation->message_handlers) {
        auto id = add_event(handler.event_type);
        auto receiver = std::ranges::find_if(
            result.receivers,
            [&](NodeEventReceiverProtocol const& value) {
                return value.address == handler.address;
            });
        if (receiver == result.receivers.end()) {
            result.receivers.push_back(NodeEventReceiverProtocol{
                .address = handler.address,
            });
            receiver = std::prev(result.receivers.end());
        }
        if (!std::ranges::contains(receiver->supported_event_types, id)) {
            receiver->supported_event_types.push_back(std::move(id));
        }
    }
    std::ranges::sort(result.event_types, [](auto const& lhs, auto const& rhs) {
        return std::tie(lhs.id.nominal_id, lhs.id.definition_fingerprint)
            < std::tie(rhs.id.nominal_id, rhs.id.definition_fingerprint);
    });
    for (auto& receiver : result.receivers) {
        std::ranges::sort(receiver.supported_event_types, [](auto const& lhs,
                                                             auto const& rhs) {
            return std::tie(lhs.nominal_id, lhs.definition_fingerprint)
                < std::tie(rhs.nominal_id, rhs.definition_fingerprint);
        });
    }
    std::ranges::sort(
        result.definitions,
        std::ranges::less{},
        &NodeEventDefinitionProtocol::definition_id);
    for (auto& definition : result.definitions) {
        std::ranges::sort(definition.supported_event_types, [](auto const& lhs,
                                                               auto const& rhs) {
            return std::tie(lhs.nominal_id, lhs.definition_fingerprint)
                < std::tie(rhs.nominal_id, rhs.definition_fingerprint);
        });
    }
    return result;
}

std::shared_ptr<NodeDefinitionsSnapshot const> ProjectGraph::definitions_snapshot() const
{
    std::scoped_lock lock(mutex_);
    return definitions_snapshot_;
}

void ProjectGraph::dispatch_node_message_locked(
    NodeAddress const& target,
    OwnedNodeMessage message,
    NodeMessageInteraction& interaction,
    NodeMessageDispatchResult& result)
{
    if (!message) {
        throw std::invalid_argument("cannot dispatch an empty node message");
    }
    auto generation = interaction.generation;
    if (!generation || !generation->graph) {
        throw std::runtime_error("cannot dispatch a node message without a project graph");
    }
    auto const target_matches = resolve_node_address(
        *generation->graph, generation->placements, target);
    auto const target_bundles = concrete_node_address_matches(
        *generation->graph, target_matches);
    // A BoundNodeMessageHandler with module_local_refs can realize the target
    // even when resolve_node_address() has no concrete ProjectNodePathMatch.
    // Validate the address after checking both mechanisms.

    struct InteractionContext {
        ProjectGraph* owner = nullptr;
        NodeMessageInteraction* interaction = nullptr;
        NodeMessageDispatchResult* result = nullptr;
    } callback_context{
        .owner = this,
        .interaction = &interaction,
        .result = &result,
    };
    NodeMessageContextOperations const operations{
        .context = &callback_context,
        .send = [](void* opaque, NodeAddress const& next, OwnedNodeMessage next_message) {
            auto& state = *static_cast<InteractionContext*>(opaque);
            state.owner->dispatch_node_message_locked(
                next,
                std::move(next_message),
                *state.interaction,
                *state.result);
        },
        .replace = [](
            void* opaque,
            NodeAddress const& address,
            std::string definition_id,
            std::string configuration_arguments_json) {
            auto& state = *static_cast<InteractionContext*>(opaque);
            auto replacement = ProjectNodeReplacement{
                .address = address,
                .definition_id = std::move(definition_id),
                .configuration_arguments_json =
                    std::move(configuration_arguments_json),
            };
            auto const definition = state.interaction->generation->definitions
                ->by_id.find(replacement.definition_id);
            if (definition == state.interaction->generation->definitions
                    ->by_id.end()) {
                throw std::runtime_error(
                    "node replacement definition is not loaded: "
                    + replacement.definition_id);
            }
            auto replacements = state.interaction->replacements;
            auto existing = std::ranges::find(
                replacements,
                address,
                &ProjectNodeReplacement::address);
            if (existing == replacements.end()) {
                replacements.push_back(std::move(replacement));
            } else {
                *existing = std::move(replacement);
            }
            auto preview = state.owner->rebuild_locked(
                NodeReplacementPreviewMutation{
                    .replacements = replacements,
                },
                std::monostate{},
                false);
            if (auto diagnostic = std::ranges::find(
                    preview.generation->diagnostics,
                    address.instance_id,
                    &NodeInstanceDiagnostic::instance_id);
                diagnostic != preview.generation->diagnostics.end()) {
                throw std::runtime_error(
                    "node replacement failed to configure: "
                    + diagnostic->message);
            }
            state.interaction->replacements = std::move(replacements);
            state.interaction->generation = std::move(preview.generation);
        },
    };

    std::vector<BoundNodeMessageHandler const*> selected_handlers;
    std::unordered_map<
        NodeBundleHandle,
        BoundNodeMessageHandler const*> bundle_handlers;
    for (auto const& handler : generation->message_handlers) {
        if (!(handler.event_type == message.type()
              || details::same_configuration_type(
                  handler.event_type, message.type()))) {
            continue;
        }
        auto const handler_matches = resolve_node_address(
            *generation->graph, generation->placements, handler.address);
        auto const handler_bundles = concrete_node_address_matches(
            *generation->graph, handler_matches);
        std::vector<NodeBundleHandle> overlaps;
        for (auto const bundle : target_bundles) {
            if (std::ranges::contains(handler_bundles, bundle)) {
                overlaps.push_back(bundle);
            }
        }
        auto const semantic_module_match =
            module_handler_address_is_selected(target, handler);
        if (overlaps.empty() && !semantic_module_match) continue;
        for (auto const bundle : overlaps) {
            auto const [existing, inserted] = bundle_handlers.emplace(
                bundle, &handler);
            if (!inserted && existing->second != &handler) {
                throw std::runtime_error(
                    "a concrete node has more than one handler for the dispatched event type");
            }
        }
        if (semantic_module_match
            && std::ranges::any_of(
                selected_handlers,
                [&](BoundNodeMessageHandler const* selected) {
                    return selected->address == handler.address;
                })) {
            throw std::runtime_error(
                "one NodeAddress has more than one module handler for the dispatched event type");
        }
        selected_handlers.push_back(&handler);
    }
    // One NodeAddress can select several node bundles. Bundles without a
    // handler for this event type are skipped. Reject the dispatch only when
    // the address selects no matching handler at all.
    if (selected_handlers.empty()) {
        result.diagnostics.push_back({
            .target = target,
            .message = target_matches.empty()
                ? "node-message target does not resolve in the current project graph"
                : "selected node does not implement the dispatched event type",
        });
        return;
    }

    for (auto const* selected : selected_handlers) {
        auto delivered = NodeMessageInteraction::DeliveredHandler{
            .address = selected->address,
        };
        auto is_ancestor_replacement = [&](ProjectNodeReplacement const& replacement) {
            return replacement.address.instance_id
                    == selected->address.instance_id
                && replacement.address.path.size()
                    <= selected->address.path.size()
                && std::equal(
                    replacement.address.path.begin(),
                    replacement.address.path.end(),
                    selected->address.path.begin());
        };
        for (auto const& replacement : interaction.replacements) {
            if (!is_ancestor_replacement(replacement)) continue;
            delivered.replacement_lineage.push_back({
                .address = replacement.address,
                .definition_id = replacement.definition_id,
                .configuration_arguments_json =
                    replacement.configuration_arguments_json,
            });
        }
        if (std::ranges::contains(interaction.delivered, delivered)) {
            throw std::runtime_error(
                "one root node interaction delivered to the same concrete node twice");
        }
        interaction.delivered.push_back(delivered);
        selected->invoke(
            selected->receiver.get(),
            operations,
            selected->address,
            selected->module_local_refs.get(),
            message.data());
    }
}

void ProjectGraph::dispatch_root_node_message_locked(
    NodeAddress const& target,
    OwnedNodeMessage message,
    NodeMessageDispatchResult& result)
{
    if (!current_generation_) {
        throw std::runtime_error(
            "cannot dispatch a node message without a project graph");
    }
    NodeMessageInteraction interaction{
        .generation = current_generation_,
        .original_replacements = current_generation_->node_replacements,
        .replacements = current_generation_->node_replacements,
    };
    dispatch_node_message_locked(
        target, std::move(message), interaction, result);
    if (interaction.replacements == interaction.original_replacements) return;

    (void)rebuild_locked(
        NodeReplacementsReplaceMutation{
            .replacements = std::move(interaction.replacements),
        },
        std::monostate{});
    result.project_state_changed = true;
}

NodeMessageDispatchResult ProjectGraph::dispatch_node_message(
    NodeAddress const& target,
    OwnedNodeMessage message)
{
    NodeMessageDispatchResult result;
    {
        std::scoped_lock lock(mutex_);
        dispatch_root_node_message_locked(
            target, std::move(message), result);
    }
    if (result.project_state_changed) {
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
    }
    return result;
}

NodeMessageDispatchResult ProjectGraph::dispatch_node_message(
    NodeAddress const& target,
    SerializedNodeMessage const& message)
{
    if (message.event_type_nominal_id.empty()
        || message.event_type_definition_fingerprint.empty()
        || !message.payload) {
        throw std::invalid_argument(
            "serialized node message requires a type identity and payload");
    }

    NodeMessageDispatchResult result;
    {
        std::scoped_lock lock(mutex_);
        auto generation = current_generation_;
        if (!generation || !generation->graph) {
            throw std::runtime_error(
                "cannot dispatch a node message without a project graph");
        }
        auto const target_matches = resolve_node_address(
            *generation->graph, generation->placements, target);
        auto const target_bundles = concrete_node_address_matches(
            *generation->graph, target_matches);
        // Module handlers are also address realizations. Do not reject the
        // serialized request before checking their BoundNodeMessageHandler.

        details::ConfigurationTypeIdentity const* selected_type = nullptr;
        for (auto const& handler : generation->message_handlers) {
            if (details::configuration_type_nominal_id(handler.event_type)
                    != message.event_type_nominal_id
                || details::configuration_type_definition_fingerprint(
                       handler.event_type)
                    != message.event_type_definition_fingerprint) {
                continue;
            }
            auto const handler_matches = resolve_node_address(
                *generation->graph,
                generation->placements,
                handler.address);
            auto const handler_bundles = concrete_node_address_matches(
                *generation->graph, handler_matches);
            auto const concrete_match = std::ranges::any_of(
                handler_bundles,
                [&](NodeBundleHandle handle) {
                    return std::ranges::contains(
                        target_bundles, handle);
                });
            if (!concrete_match
                && !module_handler_address_is_selected(target, handler)) {
                continue;
            }
            selected_type = handler.event_type;
            break;
        }
        if (!selected_type) {
            result.diagnostics.push_back({
                .target = target,
                .message = target_matches.empty()
                    ? "node-message target does not resolve in the current project graph"
                    : "selected nodes do not implement the serialized event type",
            });
            return result;
        }

        auto value = std::make_shared<EvaluatedConfigurationValue>(
            evaluate_configuration_json_value(
                *message.payload, *selected_type));
        auto owned = OwnedNodeMessage::from_owned(
            value, value->data(), selected_type);
        dispatch_root_node_message_locked(
            target, std::move(owned), result);
    }
    if (result.project_state_changed) {
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
    }
    return result;
}

void ProjectGraph::handle_node_definitions_snapshot_changed(
    NodeDefinitionsSnapshotChanged const& change)
{
    if (!change.snapshot) {
        throw std::invalid_argument("ProjectGraph received an empty definitions snapshot");
    }

    std::scoped_lock lock(mutex_);
    if (definitions_snapshot_
        && change.snapshot->generation <= definitions_snapshot_->generation) {
        return;
    }
    auto previous_snapshot = definitions_snapshot_;
    definitions_snapshot_ = change.snapshot;
    try {
        (void)rebuild_locked(std::monostate{}, std::monostate{});
    } catch (...) {
        // A snapshot is committed only together with the root generation built
        // from it. This also keeps an identical publication retryable after a
        // fatal wiring/structural failure.
        definitions_snapshot_ = std::move(previous_snapshot);
        throw;
    }
}

void ProjectGraph::handle_project_create_node_instance(
    ProjectCreateNodeInstanceRequest const& request,
    ProjectStringBuilder& builder)
{
    std::string created_instance_id;
    {
        std::scoped_lock lock(mutex_);
        auto rebuilt = rebuild_locked(NodeInstanceCreateMutation{
            .instance_id = request.instance_id,
            .definition_id = request.definition_id,
            .package_root = request.package_root,
            .display_name = request.display_name,
            .configuration_source = request.configuration_source,
        }, std::monostate{});
        if (rebuilt.created_instance_ids.size() != 1) {
            throw std::logic_error("NodeInstances did not return one created instance id");
        }
        created_instance_id = std::move(rebuilt.created_instance_ids.front());
    }
    builder.succeed(std::move(created_instance_id));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_delete_node_instance(
    ProjectDeleteNodeInstanceRequest const& request,
    ProjectAckBuilder& builder)
{
    {
        std::scoped_lock lock(mutex_);
        (void)rebuild_locked(NodeInstanceDeleteMutation{
            .instance_id = request.instance_id,
        }, std::monostate{});
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_update_node_instances(
    ProjectUpdateNodeInstancesRequest const& request,
    ProjectAckBuilder& builder)
{
    NodeInstanceUpdateMutation mutation;
    mutation.updates.reserve(request.updates.size());
    for (auto const& update : request.updates) {
        mutation.updates.push_back(NodeInstanceDisplayNameUpdate{
            .instance_id = update.instance_id,
            .display_name = update.display_name,
            .configuration_source = update.configuration_source,
        });
    }
    {
        std::scoped_lock lock(mutex_);
        (void)rebuild_locked(std::move(mutation), std::monostate{});
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_upsert_graph_connection(
    ProjectUpsertGraphConnectionRequest const& request,
    ProjectAckBuilder& builder)
{
    {
        std::scoped_lock lock(mutex_);
        (void)rebuild_locked(
            std::monostate{},
            GraphConnectionUpsertMutation{.connection = request.connection});
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_delete_graph_connection(
    ProjectDeleteGraphConnectionRequest const& request,
    ProjectAckBuilder& builder)
{
    {
        std::scoped_lock lock(mutex_);
        (void)rebuild_locked(
            std::monostate{},
            GraphConnectionDeleteMutation{.connection_id = request.connection_id});
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_replace_graph(
    ProjectReplaceGraphRequest const& request,
    ProjectAckBuilder& builder)
{
    std::unordered_set<std::string> instance_ids;
    for (auto const& instance : request.instances) {
        if (instance.instance_id.empty()) {
            throw std::invalid_argument("project node instance id must not be empty");
        }
        if (instance.definition_id.empty()) {
            throw std::invalid_argument("project node definition id must not be empty");
        }
        if (!instance_ids.insert(instance.instance_id).second) {
            throw std::invalid_argument(
                "duplicate project node instance id in replacement batch: "
                + instance.instance_id);
        }
    }
    std::unordered_set<std::string> connection_ids;
    for (auto const& connection : request.connections) {
        validate_project_connection(connection);
        auto const& connection_id = project_connection_id(connection);
        if (!connection_ids.insert(connection_id).second) {
            throw std::invalid_argument(
                "duplicate project connection id in replacement batch: "
                + connection_id);
        }
    }
    std::vector<NodeAddress> replacement_addresses;
    replacement_addresses.reserve(request.node_replacements.size());
    for (auto const& replacement : request.node_replacements) {
        if (replacement.address.instance_id.empty()) {
            throw std::invalid_argument(
                "project node replacement address has an empty instance id");
        }
        if (!instance_ids.contains(replacement.address.instance_id)) {
            throw std::invalid_argument(
                "project node replacement addresses an unknown instance: "
                + replacement.address.instance_id);
        }
        if (replacement.definition_id.empty()) {
            throw std::invalid_argument(
                "project node replacement definition id must not be empty");
        }
        if (std::ranges::contains(
                replacement_addresses, replacement.address)) {
            throw std::invalid_argument(
                "duplicate project node replacement address");
        }
        replacement_addresses.push_back(replacement.address);
    }
    {
        std::scoped_lock lock(mutex_);
        (void)rebuild_locked(
            NodeInstanceReplaceAllMutation{
                .instances = request.instances,
                .node_replacements = request.node_replacements,
            },
            GraphConnectionReplaceAllMutation{.connections = request.connections});
    }
    builder.succeed();
}

void ProjectGraph::handle_socket_rpc_create_node_instance(
    CreateNodeInstanceRequest const& request,
    SocketRpcCreateNodeInstanceResultBuilder& builder)
{
    try {
        ProjectStringBuilder project_builder;
        handle_project_create_node_instance(
            ProjectCreateNodeInstanceRequest{
                .instance_id = request.instance_id,
                .definition_id = request.definition_id,
                .display_name = request.display_name,
                .configuration_source = request.configuration_source,
            },
            project_builder);
        builder.succeed(project_builder.build());
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_delete_node_instance(
    DeleteNodeInstanceRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_delete_node_instance(
            ProjectDeleteNodeInstanceRequest{.instance_id = request.instance_id},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_update_node_instances(
    UpdateNodeInstancesRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        std::vector<ProjectUpdateNodeInstance> updates;
        updates.reserve(request.updates.size());
        for (auto const& update : request.updates) {
            updates.push_back(ProjectUpdateNodeInstance{
                .instance_id = update.instance_id,
                .display_name = update.display_name,
                .configuration_source = update.configuration_source,
            });
        }
        ProjectAckBuilder project_builder;
        handle_project_update_node_instances(
            ProjectUpdateNodeInstancesRequest{.updates = std::move(updates)},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_upsert_graph_connection(
    UpsertGraphConnectionRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_upsert_graph_connection(
            ProjectUpsertGraphConnectionRequest{.connection = request.connection},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_delete_graph_connection(
    DeleteGraphConnectionRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_delete_graph_connection(
            ProjectDeleteGraphConnectionRequest{
                .connection_id = request.connection_id,
            },
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_dispatch_node_message(
    DispatchNodeMessageRequest const& request,
    SocketRpcNodeMessageResultBuilder& builder)
{
    try {
        auto result = dispatch_node_message(
            request.target,
            SerializedNodeMessage{
                .event_type_nominal_id = request.event_type_nominal_id,
                .event_type_definition_fingerprint =
                    request.event_type_definition_fingerprint,
                .payload = request.payload,
            });
        std::vector<SocketRpcNodeMessageDiagnostic> diagnostics;
        diagnostics.reserve(result.diagnostics.size());
        for (auto& diagnostic : result.diagnostics) {
            diagnostics.push_back(SocketRpcNodeMessageDiagnostic{
                .target = std::move(diagnostic.target),
                .message = std::move(diagnostic.message),
            });
        }
        builder.succeed(std::move(diagnostics));
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_get_node_event_protocol(
    GetNodeEventProtocolRequest const&,
    SocketRpcNodeEventProtocolResultBuilder& builder)
{
    try {
        builder.succeed(node_event_protocol());
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}
} // namespace iv
