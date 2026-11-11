#include <intravenous/runtime/project_graph.h>

#include <intravenous/compat.h>
#include <intravenous/graph/builder.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/configuration_expression.h>
#include <intravenous/runtime/package_pipeline_types.h>
#include <intravenous/runtime/project_graph_events.h>
#include <intravenous/runtime/project_node_path_resolver.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <intravenous/runtime/socket_rpc_response_builders.h>

#include <nlohmann/json.hpp>

#include <iomanip>
#include <chrono>
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

bool manual_control_participates(
    ConfiguredGraph const& graph,
    ManualControlId const& id)
{
    return std::ranges::any_of(
        graph.manual_controls,
        [&](ConfiguredManualControl const& configured) {
            if (configured.id != id) return false;
            if (configured.participation
                == ManualControlParticipation::participate_with_connections) {
                return true;
            }
            auto const channels = graph.node_bundles.sample_input_channels(
                configured.input);
            return std::ranges::none_of(
                channels,
                [&](SampleInputChannelId channel) {
                    return graph.connections.sample_input_is_connected(channel);
                });
        });
}

bool manual_control_is_available(
    ConfiguredGraph const& graph,
    ManualControlId const& id)
{
    return std::ranges::any_of(
        graph.manual_controls,
        [&](ConfiguredManualControl const& configured) {
            return configured.id == id;
        });
}
} // namespace

ProjectGraph::ProjectGraph()
    : definitions_snapshot_(std::make_shared<NodeDefinitionsSnapshot const>())
    , control_compile_worker_(
        [this](std::stop_token stop) { control_compile_worker(stop); })
{}

ProjectGraph::~ProjectGraph()
{
    // Synchronize the stop transition with the worker's wait predicate. If
    // request_stop() and notify_all() occur between that predicate and the
    // worker entering wait(), the notification can otherwise be lost.
    {
        std::scoped_lock lock(mutex_);
        control_compile_worker_.request_stop();
    }
    control_compile_changed_.notify_all();
}

std::vector<ManualControlId> ProjectGraph::desired_dynamic_inputs_locked(
    bool include_predictive_hover) const
{
    std::vector<ManualControlId> result;
    for (auto const& control : manual_controls_) {
        auto const required = std::ranges::any_of(
            manual_interactions_,
            [&](ActiveManualControlInteraction const& interaction) {
                return interaction.control == control.id;
            });
        auto const predicted = include_predictive_hover
            && std::ranges::any_of(
                hovered_controls_,
                [&](auto const& entry) {
                    return entry.second == control.id;
                });
        if ((required || predicted) && current_generation_
            && current_generation_->graph
            && manual_control_participates(
                *current_generation_->graph, control.id)) {
            result.push_back(control.id);
        }
    }
    return result;
}

ProjectManualControl* ProjectGraph::find_manual_control_locked(
    ManualControlId const& id)
{
    auto found = std::ranges::find(
        manual_controls_, id, &ProjectManualControl::id);
    return found == manual_controls_.end() ? nullptr : std::addressof(*found);
}

void ProjectGraph::schedule_control_compile_locked(
    std::vector<ManualControlId> dynamic_inputs,
    bool activate_on_ready)
{
    if (!current_generation_ || !current_generation_->graph
        || !current_generation_->definitions
        || !current_generation_->compiled_graph
        || !current_generation_->graph_executor_accepted) {
        return;
    }
    auto const matches_current_request = [&](auto const& request) {
        return request.request_id == latest_control_compile_request_
            && request.project_generation == current_generation_->generation
            && std::ranges::equal(request.dynamic_inputs, dynamic_inputs);
    };
    if (pending_control_compile_
        && matches_current_request(*pending_control_compile_)) {
        pending_control_compile_->activate_on_ready |= activate_on_ready;
        return;
    }
    if (inflight_control_compile_
        && matches_current_request(*inflight_control_compile_)) {
        inflight_control_compile_->activate_on_ready |= activate_on_ready;
        return;
    }
    auto graph = std::make_shared<ConfiguredGraph>(*current_generation_->graph);
    for (auto& configured : graph->manual_controls) {
        configured.dynamic = std::ranges::contains(
            dynamic_inputs, configured.id);
        if (auto const* durable = find_manual_control_locked(configured.id)) {
            configured.settled_value = durable->stored_value;
            configured.participation = durable->participation;
        }
    }
    auto const request_id = next_control_compile_request_++;
    latest_control_compile_request_ = request_id;
    pending_control_compile_ = ControlCompileRequest{
        .request_id = request_id,
        .project_generation = current_generation_->generation,
        .graph = std::move(graph),
        .definitions = current_generation_->definitions,
        .dynamic_inputs = std::move(dynamic_inputs),
        .activate_on_ready = activate_on_ready,
    };
    control_compile_changed_.notify_all();
}

bool ProjectGraph::stage_ready_control_code_locked(
    std::span<ManualControlId const> dynamic_inputs)
{
    if (!ready_control_code_ || !current_generation_
        || ready_control_code_->project_generation
            != current_generation_->generation
        || !std::ranges::equal(
            ready_control_code_->dynamic_inputs, dynamic_inputs)) {
        return false;
    }
    auto const result = IV_INVOKE_SINGLETON_EVENT(
        iv_runtime_project_graph_realtime_code_requested_event,
        ready_control_code_->graph);
    if (result != GraphExecutorStageResult::accepted) return false;
    ready_control_code_.reset();
    return true;
}

void ProjectGraph::set_manual_control_target_locked(
    ManualControlId const& id,
    Sample value)
{
    if (!current_generation_) return;
    (void)IV_INVOKE_SINGLETON_EVENT(
        iv_runtime_project_graph_manual_control_target_requested_event,
        current_generation_->generation,
        id,
        value);
}

void ProjectGraph::reconcile_manual_control_activity_locked(
    bool settled_dynamic_input)
{
    dynamic_inputs_ = desired_dynamic_inputs_locked(false);
    if (dynamic_inputs_.empty()) {
        // No finished compile may activate merely because a gesture that
        // required it ended or its client disconnected. A remaining hover can
        // still prepare matching code without activating it.
        pending_control_compile_.reset();
        ready_control_code_.reset();
        latest_control_compile_request_ = next_control_compile_request_++;
        auto const predictive_inputs = desired_dynamic_inputs_locked(true);
        if (!predictive_inputs.empty()) {
            schedule_control_compile_locked(predictive_inputs, false);
        }
        if (settled_dynamic_input) {
            constant_specialization_deadline_ =
                std::chrono::steady_clock::now()
                + manual_control_idle_debounce;
        }
        control_compile_changed_.notify_all();
        return;
    }

    constant_specialization_deadline_.reset();
    auto const requested_inputs = desired_dynamic_inputs_locked(true);
    if (!stage_ready_control_code_locked(requested_inputs)) {
        schedule_control_compile_locked(requested_inputs, true);
    }
}

void ProjectGraph::control_compile_worker(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        std::optional<ControlCompileRequest> request;
        {
            std::unique_lock lock(mutex_);
            while (!stop.stop_requested() && !pending_control_compile_) {
                if (!constant_specialization_deadline_) {
                    control_compile_changed_.wait(lock, [&] {
                        return stop.stop_requested()
                            || pending_control_compile_.has_value()
                            || constant_specialization_deadline_.has_value();
                    });
                    continue;
                }
                auto const deadline = *constant_specialization_deadline_;
                if (control_compile_changed_.wait_until(
                        lock, deadline, [&] {
                            return stop.stop_requested()
                                || pending_control_compile_.has_value()
                                || !constant_specialization_deadline_
                                || *constant_specialization_deadline_
                                    != deadline;
                        })) {
                    continue;
                }
                constant_specialization_deadline_.reset();
                if (dynamic_inputs_.empty()) {
                    schedule_control_compile_locked(
                        desired_dynamic_inputs_locked(false), true);
                }
            }
            if (stop.stop_requested()) return;
            request = std::move(pending_control_compile_);
            pending_control_compile_.reset();
            inflight_control_compile_ = InflightControlCompile{
                .request_id = request->request_id,
                .project_generation = request->project_generation,
                .dynamic_inputs = request->dynamic_inputs,
                .activate_on_ready = request->activate_on_ready,
            };
        }

        auto result = IV_INVOKE_SINGLETON_EVENT(
            iv_runtime_project_graph_graph_jit_requested_event,
            ProjectGraphGraphJitRequest{
                .successor = {
                    .project_generation = request->project_generation,
                    .graph = request->graph,
                    .definitions = request->definitions,
                },
            });

        std::scoped_lock lock(mutex_);
        auto activate_on_ready = request->activate_on_ready;
        if (inflight_control_compile_
            && inflight_control_compile_->request_id == request->request_id) {
            activate_on_ready |=
                inflight_control_compile_->activate_on_ready;
            inflight_control_compile_.reset();
        }
        if (stop.stop_requested()
            || request->request_id != latest_control_compile_request_
            || !current_generation_
            || request->project_generation != current_generation_->generation
            || !result.compiled_graph) {
            continue;
        }
        ready_control_code_ = ReadyControlCode{
            .project_generation = request->project_generation,
            .dynamic_inputs = request->dynamic_inputs,
            .graph = std::move(result.compiled_graph),
        };
        auto staged = false;
        if (activate_on_ready) {
            staged = stage_ready_control_code_locked(request->dynamic_inputs);
        }
        // A settled constant replacement consumes the one ready-code slot.
        // If the client still hovers a participating control, prepare that
        // exact dynamic set again without activating it.
        if (staged && request->dynamic_inputs.empty()
            && dynamic_inputs_.empty()) {
            auto const predictive_inputs = desired_dynamic_inputs_locked(true);
            if (!predictive_inputs.empty()) {
                schedule_control_compile_locked(predictive_inputs, false);
            }
        }
    }
}

namespace {
// Writes one rebuild's timings, from its start to the executor accepting the
// compiled generation, when IV_EDIT_TIMING is set. The edit becomes audible at
// the next realtime cutover after that.
void log_edit_timing(ProjectGraphGeneration const& generation) noexcept
{
    try {
        auto const ms = [](std::uint64_t value) { return static_cast<double>(value) / 1000.0; };
        auto const& rebuild = generation.rebuild_timings;
        auto& out = diagnostic_stream();
        out << std::fixed << std::setprecision(2)
            << "\nedit timing: generation " << generation.generation
            << ", " << (generation.graph ? generation.graph->node_bundles.size() : 0)
            << " bundles, "
            << (generation.graph_executor_accepted ? "accepted" : "not accepted")
            << "\n  total " << ms(rebuild.total_us) << " ms"
            << " = instances " << ms(rebuild.node_instances_us)
            << " + connections " << ms(rebuild.graph_connections_us)
            << " + finish " << ms(rebuild.finish_us)
            << " + graph jit " << ms(rebuild.graph_jit_us)
            << " + stage " << ms(rebuild.executor_stage_us) << " ms\n";
        for (auto const& compile : generation.graph_jit_timings) {
            out << "  " << (compile.transition ? "transition" : "steady")
                << " compile " << ms(compile.total_us) << " ms"
                << " = capture " << ms(compile.capture_us)
                << " + plan " << ms(compile.lowering_plan_us)
                << " + transition plan " << ms(compile.lowering_transition_us)
                << " + emit " << ms(compile.lowering_emit_us)
                << " + verify " << ms(compile.verify_us)
                << " + optimize " << ms(compile.optimization_us)
                << " + codegen " << ms(compile.materialization_us) << " ms"
                << "; " << compile.functions << " functions, "
                << compile.instructions_before_optimization << " -> "
                << compile.instructions_after_optimization << " instructions\n";
            auto const specializations = compile.region_specializations_compiled
                + compile.region_specializations_reused
                + compile.region_specialization_failures
                + compile.region_specializations_skipped;
            if (specializations != 0) {
                out << "  region specializations: " << compile.region_specializations_compiled
                    << " compiled in " << ms(compile.region_specialization_us) << " ms, "
                    << compile.region_specializations_reused << " reused, "
                    << compile.region_specialization_failures << " failed, "
                    << compile.region_specializations_skipped << " skipped, "
                    << compile.region_specialization_substitutions
                    << " workspace loads substituted\n";
                if (!compile.region_specialization_failure.empty()) {
                    out << "  region specialization failure: "
                        << compile.region_specialization_failure << '\n';
                }
                if (!compile.region_specialization_skip.empty()) {
                    out << "  region specialization skip: "
                        << compile.region_specialization_skip << '\n';
                }
            }
        }
        out.flush();
    } catch (...) {
    }
}

// Writes a failed configuration pass to the server diagnostic log, including
// the stack where a construction failure was first recorded.
void log_configuration_failure(ProjectGraphConfigurationFailure const& failure) noexcept
{
    try {
        auto& out = diagnostic_stream();
        out << "\nproject configuration failed; the previous generation stays current\n";
        for (auto const& diagnostic : failure.node_instance_diagnostics) {
            out << "node instance '" << diagnostic.instance_id << "' ("
                << diagnostic.definition_id << "): " << diagnostic.message << '\n';
            if (!diagnostic.stacktrace.empty()) out << diagnostic.stacktrace << '\n';
        }
        if (failure.connection_failure) {
            out << "project connection '" << failure.connection_failure->connection_id
                << "': " << failure.connection_failure->message << '\n';
        }
        out.flush();
    } catch (...) {
    }
}
} // namespace

ProjectGraph::RebuildResult ProjectGraph::rebuild_locked(
    NodeInstancesMutation instance_mutation,
    GraphConnectionsMutation connection_mutation,
    bool publish_execution)
{
    using Clock = std::chrono::steady_clock;
    auto const rebuild_begin = Clock::now();
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

    // A failed configuration pass discards the root builder and publishes no
    // generation; the previous generation stays current and keeps running.
    auto fail = [&](ProjectGraphConfigurationFailure failure) {
        auto shared = std::make_shared<ProjectGraphConfigurationFailure const>(
            std::move(failure));
        if (publish_execution) {
            log_configuration_failure(*shared);
            configuration_failure_ = shared;
        }
        return RebuildResult{
            .created_instance_ids = std::move(request.created_instance_ids),
            .failure = std::move(shared),
        };
    };
    auto const definitions_generation =
        definitions_snapshot_ ? definitions_snapshot_->generation : 0;
    // A failed instance batch leaves the root builder unusable, so connections
    // are not realized; their mutation still joins the desired revision.
    bool const instances_failed = !request.result.diagnostics.empty();
    GraphConnectionsProjectGraphRequest connections_request{
        .root_builder = instances_failed ? nullptr : &root_builder,
        .placements = &request.result.placements,
        .mutation = std::move(connection_mutation),
    };
    auto const instances_end = Clock::now();
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_graph_connections_requested_event,
        connections_request);
    auto const connections_end = Clock::now();
    if (!connections_request.handled) {
        throw std::runtime_error("ProjectGraph cannot rebuild without GraphConnections");
    }
    if (instances_failed) {
        return fail(ProjectGraphConfigurationFailure{
            .definitions_generation = definitions_generation,
            .node_instance_diagnostics = std::move(request.result.diagnostics),
        });
    }
    if (connections_request.result.failure) {
        return fail(ProjectGraphConfigurationFailure{
            .definitions_generation = definitions_generation,
            .connection_failure = std::move(connections_request.result.failure),
        });
    }

    root_builder.outputs();
    auto configured_graph = std::move(root_builder).finish();
    auto generation = std::make_shared<ProjectGraphGeneration>();
    generation->generation = next_generation_;
    generation->definitions_generation = definitions_snapshot_->generation;
    generation->definitions = definitions_snapshot_;
    generation->placements = std::move(request.result.placements);
    generation->applied_connection_ids =
        std::move(connections_request.result.applied_connection_ids);
    generation->connection_diagnostics =
        std::move(connections_request.result.diagnostics);
    generation->node_replacements =
        std::move(request.realized_node_replacements);
    generation->manual_controls = manual_controls_;

    for (auto const& control : manual_controls_) {
        if (!control.enabled) continue;
        auto const matches = resolve_node_address(
            configured_graph, generation->placements, control.id.node);
        auto const bundles = concrete_node_address_matches(
            configured_graph, matches);
        if (bundles.empty()) {
            generation->manual_control_diagnostics.push_back({
                .id = control.id,
                .message = "manual control address does not select a concrete node",
            });
            continue;
        }
        for (std::size_t selection_index = 0;
             selection_index < bundles.size(); ++selection_index) {
            auto const bundle = bundles[selection_index];
            auto const sample_inputs =
                configured_graph.node_bundles.typed_ports(bundle).sample_inputs();
            auto const found = std::ranges::find(
                sample_inputs, control.id.input_name, &ContinuousInputConfig::name);
            if (found == sample_inputs.end()) {
                generation->manual_control_diagnostics.push_back({
                    .id = control.id,
                    .message = "manual control input name does not select a continuous input",
                });
                continue;
            }
            if (!is_sequential(*found)) {
                generation->manual_control_diagnostics.push_back({
                    .id = control.id,
                    .message = "manual controls require a Sequential continuous input",
                });
                continue;
            }
            auto const port = static_cast<std::size_t>(
                std::distance(sample_inputs.begin(), found));
            configured_graph.manual_controls.push_back(ConfiguredManualControl{
                .id = control.id,
                .input = NodeBundlePortId{bundle, PortKind::sample, port},
                .concrete_selection_index = selection_index,
                .settled_value = control.stored_value,
                .participation = control.participation,
            });
        }
    }
    auto next_manual_interactions = manual_interactions_;
    auto next_hovered_controls = hovered_controls_;
    auto next_dynamic_inputs = dynamic_inputs_;
    if (publish_execution) {
        auto const is_available = [&](ManualControlId const& id) {
            return std::ranges::any_of(
                configured_graph.manual_controls,
                [&](ConfiguredManualControl const& configured) {
                    return configured.id == id;
                });
        };
        std::erase_if(
            next_manual_interactions,
            [&](ActiveManualControlInteraction const& interaction) {
                return !is_available(interaction.control);
            });
        std::erase_if(next_hovered_controls, [&](auto const& entry) {
            return !is_available(entry.second);
        });
        next_dynamic_inputs.clear();
        for (auto const& control : manual_controls_) {
            auto const required = std::ranges::any_of(
                next_manual_interactions,
                [&](ActiveManualControlInteraction const& interaction) {
                    return interaction.control == control.id;
                });
            if (required
                && manual_control_participates(configured_graph, control.id)) {
                next_dynamic_inputs.push_back(control.id);
            }
        }
        for (auto& configured : configured_graph.manual_controls) {
            configured.dynamic = std::ranges::contains(
                next_dynamic_inputs, configured.id);
        }
    }
    generation->graph = std::make_shared<ConfiguredGraph const>(
        std::move(configured_graph));
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
        auto const graph_jit_begin = Clock::now();
        auto graph_jit_result = IV_INVOKE_SINGLETON_EVENT(
            iv_runtime_project_graph_graph_jit_requested_event,
            ProjectGraphGraphJitRequest{
                .successor = {
                    .project_generation = generation->generation,
                    .graph = generation->graph,
                    .definitions = generation->definitions,
                },
                .predecessor = accepted_execution_graph_,
                .initial_generation = !accepted_execution_graph_,
            });
        auto const graph_jit_end = Clock::now();
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
        auto const microseconds = [](auto begin, auto end) {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
        };
        auto const stage_end = Clock::now();
        generation->rebuild_timings = ProjectGraphRebuildTimings{
            .node_instances_us = microseconds(rebuild_begin, instances_end),
            .graph_connections_us = microseconds(instances_end, connections_end),
            .finish_us = microseconds(connections_end, graph_jit_begin),
            .graph_jit_us = microseconds(graph_jit_begin, graph_jit_end),
            .executor_stage_us = microseconds(graph_jit_end, stage_end),
            .total_us = microseconds(rebuild_begin, stage_end),
        };
        generation->graph_jit_timings = std::move(graph_jit_result.timings);
        if (diagnostic_flag_enabled("IV_EDIT_TIMING")) log_edit_timing(*generation);
        manual_interactions_ = std::move(next_manual_interactions);
        hovered_controls_ = std::move(next_hovered_controls);
        dynamic_inputs_ = std::move(next_dynamic_inputs);
        constant_specialization_deadline_.reset();
        current_generation_ = generation;
        configuration_failure_.reset();
        ready_control_code_.reset();
        pending_control_compile_.reset();
        latest_control_compile_request_ = next_control_compile_request_++;
        control_compile_changed_.notify_all();
        ++next_generation_;
        auto const requested_inputs = desired_dynamic_inputs_locked(true);
        if (!std::ranges::equal(requested_inputs, dynamic_inputs_)) {
            schedule_control_compile_locked(
                requested_inputs, !dynamic_inputs_.empty());
        }
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

std::shared_ptr<ProjectGraphConfigurationFailure const>
ProjectGraph::configuration_failure() const
{
    std::scoped_lock lock(mutex_);
    return configuration_failure_;
}

std::vector<ProjectManualControl> ProjectGraph::manual_controls() const
{
    std::scoped_lock lock(mutex_);
    return manual_controls_;
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
            if (preview.failure) {
                auto const& diagnostics = preview.failure->node_instance_diagnostics;
                throw std::runtime_error(
                    "node replacement failed to configure: "
                    + (!diagnostics.empty() ? diagnostics.front().message
                        : preview.failure->connection_failure
                        ? preview.failure->connection_failure->message
                        : std::string("unknown configuration failure")));
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
    std::vector<ManualControlId> manual_control_ids;
    manual_control_ids.reserve(request.manual_controls.size());
    for (auto const& control : request.manual_controls) {
        if (control.id.node.instance_id.empty()) {
            throw std::invalid_argument(
                "project manual control address has an empty instance id");
        }
        if (control.id.input_name.empty()) {
            throw std::invalid_argument(
                "project manual control input name must not be empty");
        }
        if (!instance_ids.contains(control.id.node.instance_id)) {
            throw std::invalid_argument(
                "project manual control addresses an unknown instance: "
                + control.id.node.instance_id);
        }
        if (std::ranges::contains(manual_control_ids, control.id)) {
            throw std::invalid_argument(
                "duplicate project manual control identity");
        }
        manual_control_ids.push_back(control.id);
    }
    {
        std::scoped_lock lock(mutex_);
        auto previous_manual_controls = manual_controls_;
        manual_controls_ = request.manual_controls;
        try {
            (void)rebuild_locked(
                NodeInstanceReplaceAllMutation{
                    .instances = request.instances,
                    .node_replacements = request.node_replacements,
                },
                GraphConnectionReplaceAllMutation{
                    .connections = request.connections,
                });
        } catch (...) {
            manual_controls_ = std::move(previous_manual_controls);
            throw;
        }
    }
    builder.succeed();
}

void ProjectGraph::handle_project_upsert_manual_control(
    ProjectUpsertManualControlRequest const& request,
    ProjectAckBuilder& builder)
{
    if (request.control.id.node.instance_id.empty()) {
        throw std::invalid_argument(
            "project manual control address has an empty instance id");
    }
    if (request.control.id.input_name.empty()) {
        throw std::invalid_argument(
            "project manual control input name must not be empty");
    }
    {
        std::scoped_lock lock(mutex_);
        auto previous_manual_controls = manual_controls_;
        auto found = std::ranges::find(
            manual_controls_, request.control.id, &ProjectManualControl::id);
        if (found == manual_controls_.end()) {
            manual_controls_.push_back(request.control);
        } else {
            *found = request.control;
        }
        try {
            (void)rebuild_locked(std::monostate{}, std::monostate{});
        } catch (...) {
            manual_controls_ = std::move(previous_manual_controls);
            throw;
        }
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_delete_manual_control(
    ProjectDeleteManualControlRequest const& request,
    ProjectAckBuilder& builder)
{
    {
        std::scoped_lock lock(mutex_);
        auto previous_manual_controls = manual_controls_;
        std::erase_if(manual_controls_, [&](ProjectManualControl const& control) {
            return control.id == request.id;
        });
        try {
            (void)rebuild_locked(std::monostate{}, std::monostate{});
        } catch (...) {
            manual_controls_ = std::move(previous_manual_controls);
            throw;
        }
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_set_manual_control_hover(
    ProjectSetManualControlHoverRequest const& request,
    ProjectAckBuilder& builder)
{
    if (request.client_id.empty()) {
        throw std::invalid_argument(
            "manual-control hover requires a client id");
    }
    {
        std::scoped_lock lock(mutex_);
        if (request.id) {
            auto const* control = find_manual_control_locked(*request.id);
            if (!control || !control->enabled
                || !current_generation_ || !current_generation_->graph
                || !manual_control_is_available(
                    *current_generation_->graph, *request.id)) {
                throw std::invalid_argument(
                    "manual-control hover names an unavailable control");
            }
            hovered_controls_.insert_or_assign(request.client_id, *request.id);
            auto const requested_inputs = desired_dynamic_inputs_locked(true);
            if (!requested_inputs.empty()) {
                schedule_control_compile_locked(
                    requested_inputs, !dynamic_inputs_.empty());
            } else {
                pending_control_compile_.reset();
                ready_control_code_.reset();
                latest_control_compile_request_ =
                    next_control_compile_request_++;
                control_compile_changed_.notify_all();
            }
        } else {
            hovered_controls_.erase(request.client_id);
            auto const requested_inputs = desired_dynamic_inputs_locked(true);
            if (!requested_inputs.empty()) {
                schedule_control_compile_locked(
                    requested_inputs, !dynamic_inputs_.empty());
            } else {
                // A hover-only artifact must never become current after its
                // hover ended. The active executable is unchanged.
                pending_control_compile_.reset();
                ready_control_code_.reset();
                latest_control_compile_request_ =
                    next_control_compile_request_++;
                control_compile_changed_.notify_all();
            }
        }
    }
    builder.succeed();
}

void ProjectGraph::handle_project_begin_manual_control_interaction(
    ProjectBeginManualControlInteractionRequest const& request,
    ProjectAckBuilder& builder)
{
    if (request.interaction.client_id.empty()
        || request.interaction.interaction_id.empty()) {
        throw std::invalid_argument(
            "manual-control interaction requires client and interaction ids");
    }
    {
        std::scoped_lock lock(mutex_);
        auto* control = find_manual_control_locked(request.id);
        if (!control || !control->enabled
            || !current_generation_ || !current_generation_->graph
            || !manual_control_is_available(
                *current_generation_->graph, request.id)) {
            throw std::invalid_argument(
                "manual-control interaction names an unavailable control");
        }
        if (std::ranges::any_of(
                manual_interactions_,
                [&](ActiveManualControlInteraction const& interaction) {
                    return interaction.interaction == request.interaction;
                })) {
            throw std::invalid_argument(
                "manual-control interaction id is already active");
        }
        control->stored_value = request.value;
        manual_interactions_.push_back({
            .interaction = request.interaction,
            .control = request.id,
        });
        constant_specialization_deadline_.reset();
        dynamic_inputs_ = desired_dynamic_inputs_locked(false);
        auto const requested_inputs = desired_dynamic_inputs_locked(true);
        set_manual_control_target_locked(request.id, request.value);
        if (!requested_inputs.empty()
            && !stage_ready_control_code_locked(requested_inputs)) {
            schedule_control_compile_locked(requested_inputs, true);
        }
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_update_manual_control_interaction(
    ProjectUpdateManualControlInteractionRequest const& request,
    ProjectAckBuilder& builder)
{
    {
        std::scoped_lock lock(mutex_);
        auto interaction = std::ranges::find(
            manual_interactions_,
            request.interaction,
            &ActiveManualControlInteraction::interaction);
        if (interaction == manual_interactions_.end()) {
            throw std::invalid_argument(
                "manual-control interaction id is not active");
        }
        auto* control = find_manual_control_locked(interaction->control);
        if (!control || !control->enabled) {
            throw std::logic_error(
                "active manual-control interaction lost its durable control");
        }
        control->stored_value = request.value;
        set_manual_control_target_locked(interaction->control, request.value);
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_end_manual_control_interaction(
    ProjectEndManualControlInteractionRequest const& request,
    ProjectAckBuilder& builder)
{
    {
        std::scoped_lock lock(mutex_);
        auto interaction = std::ranges::find(
            manual_interactions_,
            request.interaction,
            &ActiveManualControlInteraction::interaction);
        if (interaction == manual_interactions_.end()) {
            throw std::invalid_argument(
                "manual-control interaction id is not active");
        }
        auto const control_id = interaction->control;
        auto* control = find_manual_control_locked(control_id);
        if (!control || !control->enabled) {
            throw std::logic_error(
                "active manual-control interaction lost its durable control");
        }
        control->stored_value = request.value;
        set_manual_control_target_locked(control_id, request.value);
        auto const settled_dynamic_input = std::ranges::contains(
            dynamic_inputs_, control_id);
        manual_interactions_.erase(interaction);
        reconcile_manual_control_activity_locked(settled_dynamic_input);
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder& builder) const
{
    std::scoped_lock lock(mutex_);
    builder.add_manual_controls(manual_controls_);
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

void ProjectGraph::handle_socket_rpc_upsert_manual_control(
    UpsertManualControlRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_upsert_manual_control(
            ProjectUpsertManualControlRequest{.control = request.control},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_delete_manual_control(
    DeleteManualControlRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_delete_manual_control(
            ProjectDeleteManualControlRequest{.id = request.id},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_set_manual_control_hover(
    SetManualControlHoverRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_set_manual_control_hover(
            ProjectSetManualControlHoverRequest{
                .client_id = request.client_id,
                .id = request.id,
            },
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_begin_manual_control_interaction(
    BeginManualControlInteractionRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_begin_manual_control_interaction(
            ProjectBeginManualControlInteractionRequest{
                .interaction = request.interaction,
                .id = request.id,
                .value = request.value,
            },
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_update_manual_control_interaction(
    UpdateManualControlInteractionRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_update_manual_control_interaction(
            ProjectUpdateManualControlInteractionRequest{
                .interaction = request.interaction,
                .value = request.value,
            },
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_end_manual_control_interaction(
    EndManualControlInteractionRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_end_manual_control_interaction(
            ProjectEndManualControlInteractionRequest{
                .interaction = request.interaction,
                .value = request.value,
            },
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_client_disconnected(
    std::string const& client_id)
{
    if (client_id.empty()) return;
    std::scoped_lock lock(mutex_);
    auto const interaction_count = manual_interactions_.size();
    auto const hover_count = hovered_controls_.size();
    auto settled_dynamic_input = false;
    std::erase_if(
        manual_interactions_,
        [&](ActiveManualControlInteraction const& interaction) {
            auto const remove =
                interaction.interaction.client_id == client_id;
            if (remove && std::ranges::contains(
                    dynamic_inputs_, interaction.control)) {
                settled_dynamic_input = true;
            }
            return remove;
        });
    hovered_controls_.erase(client_id);
    if (manual_interactions_.size() != interaction_count
        || hovered_controls_.size() != hover_count) {
        reconcile_manual_control_activity_locked(settled_dynamic_input);
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
