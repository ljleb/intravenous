#include <intravenous/project/project_graph.h>
#include <intravenous/module/package_manifest.h>
#include <intravenous/observations/observation_resolution.h>
#include <intravenous/execution/graph_executor.h>

#include <intravenous/compat.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/state.h>
#include <intravenous/graph_jit/output_gain_execution.h>
#include <intravenous/module/builder_session.h>
#include <intravenous/packages/node_definitions_events.h>
#include <intravenous/project/configuration_expression.h>
#include <intravenous/packages/package_pipeline_types.h>
#include <intravenous/project/project_graph_events.h>
#include <intravenous/project/project_node_path_resolver.h>
#include <intravenous/project/persistence/project_persistence_builder.h>
#include <intravenous/project/runtime_project_events.h>
#include <intravenous/rpc/socket_rpc_requests.h>
#include <intravenous/rpc/socket_rpc_response_builders.h>
#include <intravenous/project/uuid.h>

#include <nlohmann/json.hpp>

#include <iomanip>
#include <chrono>
#include <algorithm>
#include <bit>
#include <functional>
#include <iterator>
#include <map>
#include <ranges>
#include <span>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace iv {
namespace {

void log_edit_timing(ProjectGraphGeneration const& generation) noexcept;

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
        || !std::holds_alternative<ProjectSymbolicNodeSelector>(
            target.path.back())
        || handler.address.path.size() < target.path.size() + 1
        || handler.address.path.size() > target.path.size() + 2
        || !std::equal(
            target.path.begin(), target.path.end(),
            handler.address.path.begin())
        || !std::holds_alternative<ProjectSymbolicMemberSelector>(
            handler.address.path[target.path.size()])) {
        return false;
    }
    return handler.address.path.size() == target.path.size() + 1
        || std::holds_alternative<ProjectTiledChildSelector>(
            handler.address.path.back());
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

ProjectGraph::ProjectGraph(std::size_t sample_rate)
    : sample_rate_(sample_rate)
    , definitions_snapshot_(std::make_shared<NodeDefinitionsSnapshot const>())
    , compile_worker_(
        [this](std::stop_token stop) { compile_worker(stop); })
{}

ProjectGraph::~ProjectGraph()
{
    stop_compilation();
}

void ProjectGraph::stop_compilation()
{
    {
        std::scoped_lock lock(mutex_);
        compile_worker_.request_stop();
    }
    control_compile_changed_.notify_all();
    if (compile_worker_.joinable()) compile_worker_.join();
    {
        std::scoped_lock lock(mutex_);
        pending_graph_compile_.reset();
        latest_execution_attempt_.reset();
        staged_execution_.reset();
        observation_candidate_.reset();
        predicted_observation_requirements_.reset();
        observation_prediction_pending_ = false;
        execution_recovery_requested_ = false;
        pending_control_compile_.reset();
        inflight_control_compile_.reset();
        compile_inflight_ = false;
        restore_live_targets_ = false;
    }
    control_compile_changed_.notify_all();
}

bool ProjectGraph::wait_for_compilation(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return control_compile_changed_.wait_for(lock, timeout, [&] {
        return !pending_graph_compile_ && !execution_recovery_requested_ && !compile_inflight_ && !restore_live_targets_
            && !pending_control_compile_ && !inflight_control_compile_ && !can_compile_observation_prediction_locked();
    });
}

void ProjectGraph::acknowledge_execution(std::uint64_t project_generation)
{
    {
        std::scoped_lock lock(mutex_);
        if (!staged_execution_ || staged_execution_->compiled->project_generation != project_generation) return;
        active_execution_ = std::exchange(staged_execution_, std::nullopt);
    }
    control_compile_changed_.notify_all();
    IV_EVENT_SOURCE(ProjectGraph, "execution checkpoint activated") {
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_execution_published_event,
            ProjectGraphPublication{.graph_changed = true});
    }
}

std::vector<ManualControlId> ProjectGraph::desired_dynamic_inputs_locked(
    bool include_predictive_hover, std::string_view exclude_retained_client) const
{
    std::vector<ManualControlId> result;
    if (!current_generation_ || !current_generation_->graph) return result;
    for (auto const& control : current_generation_->graph->manual_controls) {
        auto const required = std::ranges::any_of(
            manual_interactions_,
            [&](ActiveManualControlInteraction const& interaction) {
                return interaction.control == control.id;
            }) || std::ranges::any_of(hover_retained_inputs_, [&](auto const& entry) {
                return entry.first != exclude_retained_client && entry.second == control.id;
            });
        // A prepared override replaces its source when activated. Do not
        // activate another user's hover together with an ongoing gesture.
        auto const predicted = include_predictive_hover && manual_interactions_.empty() && hover_retained_inputs_.empty()
            && std::ranges::any_of(
                hovered_controls_,
                [&](auto const& entry) {
                    return entry.second == control.id;
                });
        if (required || predicted) {
            if (!std::ranges::contains(result, control.id)) result.push_back(control.id);
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

void ProjectGraph::prepare_released_control_locked()
{
    if (!settled_preparation_client_) return;
    if (!hover_retained_inputs_.contains(*settled_preparation_client_)) {
        settled_preparation_client_.reset();
        return;
    }
    if (!control_code_matches_locked(dynamic_inputs_)) return;
    auto predicted = desired_dynamic_inputs_locked(false, *settled_preparation_client_);
    if (predicted != dynamic_inputs_) schedule_control_compile_locked(std::move(predicted), false);
}

void ProjectGraph::schedule_control_compile_locked(
    std::vector<ManualControlId> dynamic_inputs,
    bool activate_on_ready)
{
    if (compile_worker_.get_stop_token().stop_requested() || configuration_failure_
        || control_code_matches_locked(dynamic_inputs)) return;
    if (!current_generation_ || !current_generation_->graph
        || !current_generation_->definitions) return;
    if (!current_generation_->compiled_graph || !current_generation_->graph_executor_accepted
        || !current_generation_->execution_generation) {
        // A gesture can arrive before the structural executable is ready.
        // Only real interactions affect the structural request; hover-only
        // prediction waits for that executable instead of delaying readiness.
        if (activate_on_ready) schedule_graph_compile_locked();
        return;
    }
    auto const matches_current_request = [&](ControlCompileRequest const& request) {
        return request.request_id == latest_control_compile_request_
            && compile_inputs_are_current_locked(
                request.inputs.project_generation, request.inputs.observations.revision)
            && std::ranges::equal(request.dynamic_inputs, dynamic_inputs)
            && request.inputs.graph
            && control_configuration_matches_locked(*request.inputs.graph, dynamic_inputs);
    };
    if (pending_control_compile_
        && matches_current_request(*pending_control_compile_)) {
        pending_control_compile_->activate_on_ready |= activate_on_ready;
        if (activate_on_ready) pending_control_compile_->trace->status.predictive = false;
        return;
    }
    if (inflight_control_compile_
        && matches_current_request(*inflight_control_compile_)) {
        inflight_control_compile_->activate_on_ready |= activate_on_ready;
        if (activate_on_ready) inflight_control_compile_->trace->status.predictive = false;
        return;
    }
    if (ready_control_code_matches_locked(dynamic_inputs)) {
        cancel_pending_control_compile_locked();
        latest_control_compile_request_ = ready_control_code_->trace->status.request_id;
        if (!activate_on_ready) return;
        auto const ready = *ready_control_code_;
        ready.trace->status.predictive = false;
        pending_control_compile_ = ControlCompileRequest{
            .request_id = latest_control_compile_request_,
            .inputs = {
                .project_generation = ready.graph->project_generation,
                .graph = ready.graph->configured_graph,
                .definitions = current_generation_->definitions,
                .observations = ready.graph->observations,
                .configured_generation = current_generation_->configured_generation,
                .configured_bundles = current_generation_->configured_bundles,
                .bundle_migration_identities = ready.graph->bundle_migration_identities,
            },
            .dynamic_inputs = std::move(dynamic_inputs),
            .activate_on_ready = true,
            .trace = ready.trace,
            .prepared_graph = ready.graph,
        };
        queue_control_compile_status_locked(ready.trace, "queued", "prepared code awaiting staging");
        return;
    }
    auto const requested_at = std::chrono::steady_clock::now();
    auto graph = graph_for_compile_locked(dynamic_inputs);
    auto const request_id = next_control_compile_request_++;
    latest_control_compile_request_ = request_id;
    cancel_pending_control_compile_locked();
    auto trace = std::make_shared<ControlCompileTrace>();
    trace->requested_at = requested_at;
    trace->status.request_id = request_id;
    trace->status.project_generation = *current_generation_->execution_generation;
    trace->status.dynamic = !dynamic_inputs.empty();
    trace->status.predictive = !activate_on_ready;
    auto const* named_graph = graph.get();
    if (dynamic_inputs.empty() && staged_control_graph_ && staged_control_graph_->configured_graph)
        named_graph = staged_control_graph_->configured_graph.get();
    for (auto const& control : named_graph->manual_controls) {
        if (control.dynamic && !std::ranges::contains(trace->status.inputs, control.id.port_name))
            trace->status.inputs.push_back(control.id.port_name);
    }
    pending_control_compile_ = ControlCompileRequest{
        .request_id = request_id,
        .inputs = capture_compile_inputs_locked(std::move(graph), *current_generation_->execution_generation),
        .dynamic_inputs = std::move(dynamic_inputs),
        .activate_on_ready = activate_on_ready,
        .trace = std::move(trace),
    };
    queue_control_compile_status_locked(pending_control_compile_->trace, "queued");
    control_compile_changed_.notify_all();
}

GraphJitCompileRequest ProjectGraph::capture_compile_inputs_locked(
    std::shared_ptr<ConfiguredGraph const> graph, std::uint64_t execution_generation) const
{
    auto const& generation = *current_generation_;
    return {
        .project_generation = execution_generation,
        .graph = std::move(graph),
        .definitions = generation.definitions,
        .observations = resolve_observation_requirements(
            *generation.graph, generation.placements, observation_requirements_),
        .configured_generation = generation.configured_generation,
        .configured_bundles = generation.configured_bundles,
        .bundle_migration_identities = accepted_execution_graph_
            && accepted_execution_graph_->configured_bundles == generation.configured_bundles
            ? accepted_execution_graph_->bundle_migration_identities : nullptr,
    };
}

bool ProjectGraph::compile_inputs_are_current_locked(
    std::uint64_t project_generation, std::uint64_t observation_revision) const
{
    return current_generation_
        && current_generation_->execution_generation == project_generation
        && observation_requirements_.revision == observation_revision;
}

bool ProjectGraph::control_code_matches_locked(std::span<ManualControlId const> inputs) const
{
    if (!current_generation_ || !accepted_execution_graph_ || !staged_control_graph_
        || current_generation_->execution_generation != accepted_execution_graph_->project_generation
        || !staged_control_graph_->configured_graph) return false;
    return control_configuration_matches_locked(*staged_control_graph_->configured_graph, inputs);
}

bool ProjectGraph::ready_control_code_matches_locked(std::span<ManualControlId const> inputs) const
{
    if (!ready_control_code_) return false;
    auto const& graph = *ready_control_code_->graph;
    return compile_inputs_are_current_locked(graph.project_generation, graph.observations.revision)
        && graph.configured_graph
        && control_configuration_matches_locked(*graph.configured_graph, inputs);
}

bool ProjectGraph::control_configuration_matches_locked(ConfiguredGraph const& candidate,
    std::span<ManualControlId const> inputs) const
{
    if (!current_generation_ || !current_generation_->graph) return false;
    auto const& current = current_generation_->graph->manual_controls;
    auto const& staged = candidate.manual_controls;
    if (current.size() != staged.size()) return false;
    for (std::size_t index = 0; index < current.size(); ++index) {
        auto const& control = current[index];
        auto const& code = staged[index];
        auto const dynamic = std::ranges::contains(inputs, control.id);
        if (code.id != control.id || code.input != control.input || code.output != control.output
            || code.dynamic != dynamic
            || code.override_connections != (dynamic && control.id.kind == ManualControlKind::input)) return false;
        if (dynamic && !control.requires_preparation) continue;
        auto const saved = std::ranges::find(manual_controls_, control.id, &ProjectManualControl::id);
        if (!control.output && !dynamic && code.authored != (saved != manual_controls_.end())) return false;
        auto value = saved != manual_controls_.end()
            ? saved->stored_value : control.settled_value;
        if (control.requires_preparation) {
            std::scoped_lock live(live_mutex_);
            for (auto const& entry : live_interactions_) if (entry.control == control.id) value = entry.value;
        }
        if (code.settled_value != value) return false;
    }
    return true;
}

bool ProjectGraph::update_graph_executor_controls_locked(
    std::vector<ManualControlTargetValue> targets,
    std::optional<std::span<ManualControlId const>> code_inputs,
    std::shared_ptr<ManualControlLiveTarget>* begun)
{
    if (!current_generation_ || !current_generation_->compiled_graph || !current_generation_->execution_generation
        || !current_generation_->graph_executor_accepted) return false;
    ProjectGraphGraphExecutorControlsRequest request;
    auto const reuse_code = code_inputs && control_code_matches_locked(*code_inputs);
    request.targets.reserve(targets.size());
    for (auto& target : targets) {
        request.targets.push_back(ProjectGraphManualControlTarget{
            .project_generation = *current_generation_->execution_generation,
            .id = std::move(target.id),
            .value = target.value,
            .gesture = target.gesture,
        });
    }
    bool const stage_code = !reuse_code && code_inputs
        && ready_control_code_matches_locked(*code_inputs)
        && std::ranges::equal(ready_control_code_->dynamic_inputs, *code_inputs);
    if (stage_code && GraphExecutor::can_replace_realtime_code(
            *current_generation_->compiled_graph, *ready_control_code_->graph)) {
        request.realtime_code = ready_control_code_->graph;
    }
    if (request.targets.empty() && !request.realtime_code) return reuse_code;
    auto const stage_begin = std::chrono::steady_clock::now();
    (void)IV_INVOKE_SINGLETON_EVENT(
        iv_runtime_project_graph_graph_executor_controls_requested_event,
        request);
    if (begun) {
        for (auto& target : request.targets) {
            if (target.gesture == ManualControlGesturePhase::begin) {
                *begun = std::move(target.live_target);
            }
        }
    }
    if (reuse_code) return true;
    if (!request.realtime_code
        || request.realtime_code_result != GraphExecutorStageResult::accepted) {
        if (request.realtime_code) {
            queue_control_compile_status_locked(ready_control_code_->trace, "failed",
                "GraphExecutor did not accept the prepared code");
        }
        return false;
    }
    if (ready_control_code_->trace) {
        ready_control_code_->trace->status.predictive = false;
        queue_control_compile_status_locked(ready_control_code_->trace, "staged", {},
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - stage_begin).count()));
    }
    staged_control_graph_ = ready_control_code_->graph;
    ready_control_code_.reset();
    return true;
}

void ProjectGraph::reconcile_manual_control_activity_locked(
    bool settled_dynamic_input,
    std::vector<ManualControlTargetValue> targets)
{
    auto const next_inputs = desired_dynamic_inputs_locked(false);
    if (next_inputs.empty() && settling_controls_ && !settled_dynamic_input && targets.empty()) return;
    if (next_inputs == dynamic_inputs_ && !next_inputs.empty() && control_code_matches_locked(next_inputs)) {
        if (!targets.empty()) (void)update_graph_executor_controls_locked(std::move(targets), std::nullopt);
        return;
    }
    dynamic_inputs_ = next_inputs;
    if (dynamic_inputs_.empty()) {
        if (!settled_dynamic_input && !targets.empty()) {
            (void)update_graph_executor_controls_locked(std::move(targets), std::nullopt);
        }
        // Hover leave may consume the settled candidate prepared at release,
        // or promote the same pending/in-flight job instead of restarting it.
        if (settled_dynamic_input) {
            settling_controls_ = true;
            if (update_graph_executor_controls_locked(std::move(targets), std::span<ManualControlId const>{})) {
                settling_controls_ = false;
                cancel_pending_control_compile_locked();
                ready_control_code_.reset();
                latest_control_compile_request_ = next_control_compile_request_++;
            } else schedule_control_compile_locked({}, true);
        } else {
            cancel_pending_control_compile_locked();
            ready_control_code_.reset();
            latest_control_compile_request_ = next_control_compile_request_++;
        }
        if (!settling_controls_) {
            auto const predictive_inputs = desired_dynamic_inputs_locked(true);
            if (!predictive_inputs.empty()) schedule_control_compile_locked(predictive_inputs, false);
        }
        control_compile_changed_.notify_all();
        return;
    }

    settling_controls_ = false;
    auto const requested_inputs = desired_dynamic_inputs_locked(true);
    if (!update_graph_executor_controls_locked(
            std::move(targets), std::span<ManualControlId const>(requested_inputs))) {
        schedule_control_compile_locked(requested_inputs, true);
    }
}

void ProjectGraph::queue_control_compile_status_locked(
    std::shared_ptr<ControlCompileTrace> const& trace,
    std::string phase, std::string message, std::uint64_t stage_us)
{
    if (!trace) return;
    auto& status = trace->status;
    status.phase = std::move(phase);
    status.message = std::move(message);
    status.elapsed_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - trace->requested_at).count());
    if (status.phase == "compiling") status.queue_us = status.elapsed_us;
    if (status.phase == "prepared") status.preparation_us = status.elapsed_us;
    status.stage_us = stage_us;
    control_compile_notifications_.push_back(status);
    control_compile_changed_.notify_all();
}

void ProjectGraph::cancel_pending_control_compile_locked()
{
    if (!pending_control_compile_) return;
    queue_control_compile_status_locked(pending_control_compile_->trace, "superseded");
    pending_control_compile_.reset();
}

void ProjectGraph::publish_control_compile_status()
{
    std::vector<ProjectControlCompileStatus> notifications;
    {
        std::scoped_lock lock(mutex_);
        notifications.swap(control_compile_notifications_);
    }
    if (notifications.empty()) return;
    IV_EVENT_SOURCE(ProjectGraph, "control compilation status") {
        IV_INVOKE_LINKER_EVENT(iv_runtime_control_compile_status_event,
            std::span<ProjectControlCompileStatus const>{notifications});
    }
}

std::shared_ptr<ConfiguredGraph> ProjectGraph::graph_for_compile_locked(
    std::span<ManualControlId const> dynamic_inputs)
{
    auto graph = std::make_shared<ConfiguredGraph>(*current_generation_->graph);
    std::scoped_lock live(live_mutex_);
    for (auto& control : graph->manual_controls) {
        control.dynamic = std::ranges::contains(dynamic_inputs, control.id);
        control.override_connections = control.dynamic && control.id.kind == ManualControlKind::input;
        auto const* saved = find_manual_control_locked(control.id);
        if (!control.output) control.authored = saved != nullptr;
        if (saved) {
            control.settled_value = saved->stored_value;
            control.participation = saved->participation;
        }
        if (control.requires_preparation) {
            for (auto const& entry : live_interactions_) if (entry.control == control.id) control.settled_value = entry.value;
        }
    }
    return graph;
}

bool ProjectGraph::can_compile_observation_prediction_locked() const
{
    return observation_prediction_pending_ && predicted_observation_requirements_
        && !staged_execution_
        && predicted_observation_requirements_->revision == observation_requirements_.revision + 1
        && current_generation_ && current_generation_->graph && current_generation_->graph_executor_accepted
        && !configuration_failure_ && accepted_execution_graph_;
}

void ProjectGraph::schedule_graph_compile_locked(std::shared_ptr<ControlCompileTrace> trace,
    bool observation_update, std::shared_ptr<ConfiguredGraph const> graph)
{
    if (!current_generation_ || !current_generation_->graph
        || configuration_failure_ || compile_worker_.get_stop_token().stop_requested()) return;
    auto candidate = std::exchange(observation_candidate_, {});
    if (candidate && (graph || candidate->requirements != observation_requirements_
        || candidate->predecessor != accepted_execution_graph_
        || candidate->inputs.configured_generation != current_generation_->configured_generation
        || candidate->inputs.definitions != current_generation_->definitions
        || !control_configuration_matches_locked(*candidate->inputs.graph, desired_dynamic_inputs_locked(false))))
        candidate.reset();
    auto inputs = candidate ? candidate->inputs : capture_compile_inputs_locked(
        graph ? std::move(graph) : graph_for_compile_locked(desired_dynamic_inputs_locked(false)));
    observation_prediction_pending_ = predicted_observation_requirements_.has_value();
    if (pending_graph_compile_ && pending_graph_compile_->trace)
        queue_control_compile_status_locked(pending_graph_compile_->trace, "superseded");
    auto const attempt = next_execution_attempt_++;
    latest_execution_attempt_ = attempt;
    pending_graph_compile_ = GraphCompileRequest{
        .attempt = attempt,
        .configured = current_generation_,
        .inputs = std::move(inputs),
        .trace = std::move(trace),
        .observation_candidate = std::move(candidate),
        .observation_update = observation_update,
    };
    control_compile_changed_.notify_all();
}

std::shared_ptr<CompiledGraph const> ProjectGraph::observation_predecessor_code_locked(
    GraphJitCompileRequest const& inputs, std::shared_ptr<CompiledGraph const> const& predecessor) const
{
    if (!predecessor || !active_execution_
        || active_execution_->compiled != predecessor || !current_generation_
        || current_generation_->configured_bundles != predecessor->configured_bundles)
        return {};
    // A knob edit may have joined the pending work. A probe-only publication
    // may use the same audio program but cannot consume that knob edit.
    if (!staged_control_graph_ || !staged_control_graph_->configured_graph || !inputs.graph
        || staged_control_graph_->configured_graph->manual_controls != inputs.graph->manual_controls
        || !control_configuration_matches_locked(*inputs.graph, desired_dynamic_inputs_locked(false)))
        return {};
    return staged_control_graph_;
}

bool ProjectGraph::stage_observation_update_locked(GraphCompileRequest const& request,
    std::shared_ptr<CompiledGraph const> const& predecessor, GraphJitCompileResult const* compiled)
{
    if (!request.observation_update || latest_execution_attempt_ != request.attempt
        || observation_requirements_.revision != request.inputs.observations.revision
        || !observation_predecessor_code_locked(request.inputs, predecessor))
        return false;
    if (compiled && !compiled->compiled_graph) return false;

    auto state = std::make_shared<ProjectGraphGeneration>(*current_generation_);
    state->execution_generation = predecessor->project_generation;
    state->compiled_graph = predecessor;
    state->graph_jit_attempted = true;
    state->graph_executor_attempted = true;
    state->graph_executor_accepted = true;
    state->execution_phase = "staged";
    state->execution_message.clear();
    state->graph_jit_diagnostics.clear();
    state->manual_controls = manual_controls_;
    ProjectGraphRealtimeObservationRequest update{
        .execution_generation = predecessor->project_generation,
    };
    if (compiled) {
        state->graph_jit_timings = compiled->timings;
        update.update = compiled->compiled_graph;
    } else {
        update.update = request.inputs.observations;
    }
    if (!IV_INVOKE_SINGLETON_EVENT(iv_runtime_project_graph_realtime_observation_requested_event, update))
        return false;

    if (compiled) staged_control_graph_ = compiled->compiled_graph;
    current_generation_ = std::move(state);
    latest_execution_attempt_.reset();
    prepare_released_control_locked();
    auto const predictive_inputs = desired_dynamic_inputs_locked(true);
    if (!predictive_inputs.empty()) schedule_control_compile_locked(predictive_inputs, false);
    return true;
}

void ProjectGraph::compile_graph(GraphCompileRequest request, std::stop_token stop)
{
    using Clock = std::chrono::steady_clock;
    auto const begin = Clock::now();
    bool completion_published = false;
    struct CompletionPublication {
        std::function<void()> publish;
        ~CompletionPublication() { try { publish(); } catch (...) {} }
    } completion{[&] {
        if (completion_published) return;
        ProjectGraphPublication progress;
        {
            std::scoped_lock lock(mutex_);
            // Discarding obsolete work must not strand catalog compilation.
            // If another target is queued, keep its preparation gate closed;
            // a configuration failure instead releases the obsolete gate.
            progress.execution_preparation = ExecutionPreparationProgress{
                .attempt = pending_graph_compile_ ? pending_graph_compile_->attempt : request.attempt,
                .pending = pending_graph_compile_.has_value(),
            };
            if (latest_execution_attempt_ == request.attempt) latest_execution_attempt_.reset();
        }
        IV_EVENT_SOURCE(ProjectGraph, "discarded graph compilation") {
            IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_execution_published_event, progress);
        }
    }};
    std::shared_ptr<CompiledGraph const> predecessor;
    auto desired = [&] {
        return current_generation_ && latest_execution_attempt_ == request.attempt;
    };
    auto eligible = [&] {
        return !stop.stop_requested() && !configuration_failure_
            && accepted_execution_graph_ == predecessor;
    };
    {
        std::scoped_lock lock(mutex_);
        predecessor = accepted_execution_graph_;
        if (!eligible()) return;
    }
    auto stage_observation_update = [&](GraphJitCompileResult const* compiled) {
        if (!request.observation_update) return false;
        IV_EVENT_SOURCE(ProjectGraph, "observation publication") {
            std::scoped_lock lock(mutex_);
            if (!eligible() || !stage_observation_update_locked(request, predecessor, compiled))
                return false;
        }
        IV_EVENT_SOURCE(ProjectGraph, "observation publication accepted") {
            IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_execution_published_event,
                ProjectGraphPublication{.graph_changed = true,
                    .execution_preparation = ExecutionPreparationProgress{.attempt = request.attempt}});
        }
        return true;
    };
    GraphJitCompileResult compiled;
    ProjectGraphPublication publication;
    auto compile_for_predecessor = [&](std::shared_ptr<CompiledGraph const> observation_code) {
        GraphJitCompileResult result;
        IV_EVENT_SOURCE(ProjectGraph, "graph compilation") {
            result = IV_INVOKE_SINGLETON_EVENT(iv_runtime_project_graph_graph_jit_requested_event,
                ProjectGraphGraphJitRequest{
                    .successor = request.inputs,
                    .predecessor = predecessor, .initial_generation = !predecessor,
                    .active_observation_predecessor_code = std::move(observation_code),
                });
        }
        return result;
    };
    try {
        if (stage_observation_update(nullptr)) {
            completion_published = true;
            return;
        }
        std::shared_ptr<CompiledGraph const> observation_code;
        {
            std::scoped_lock lock(mutex_);
            if (desired()) {
                if (request.observation_update)
                    observation_code = observation_predecessor_code_locked(request.inputs, predecessor);
                auto state = std::make_shared<ProjectGraphGeneration>(*current_generation_);
                state->execution_phase = "compiling";
                current_generation_ = std::move(state);
            }
        }
        IV_EVENT_SOURCE(ProjectGraph, "graph compilation started") {
            IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_execution_published_event,
                ProjectGraphPublication{.graph_changed = true,
                    .execution_preparation = ExecutionPreparationProgress{.attempt = request.attempt, .pending = true}});
        }
        auto const& candidate = request.observation_candidate;
        if (candidate && candidate->predecessor == predecessor && candidate->result.compiled_graph)
            compiled = std::move(candidate->result);
        else
            compiled = compile_for_predecessor(std::move(observation_code));
        if (stage_observation_update(&compiled)) {
            completion_published = true;
            return;
        }
        if (compiled.reuses_observation_layout) {
            {
                std::scoped_lock lock(mutex_);
                if (!eligible() || !desired()) return;
            }
            // A steady-only artifact cannot start a new generation. If its
            // active target changed, prepare the still-current request as a cutover.
            auto cutover = compile_for_predecessor({});
            compiled.timings.insert(compiled.timings.end(),
                std::make_move_iterator(cutover.timings.begin()),
                std::make_move_iterator(cutover.timings.end()));
            cutover.timings = std::move(compiled.timings);
            compiled = std::move(cutover);
        }
        auto const compiled_at = Clock::now();
        IV_EVENT_SOURCE(ProjectGraph, "prepare execution generation") {
            {
                std::scoped_lock lock(mutex_);
                if (!eligible()) {
                    queue_control_compile_status_locked(request.trace, "superseded");
                    return;
                }
                // The artifact only receives an execution identity when it
                // needs its own storage and an ordered executor cutover.
                if (compiled.compiled_graph)
                    compiled.compiled_graph->project_generation = next_execution_generation_++;
            }
            // Preparation allocates all storage/migration resources outside the
            // project lock. Its move-only handle publishes one chain link only
            // after the target is checked again below.
            ProjectGraphGraphExecutorResult execution;
            if (compiled.compiled_graph)
                execution = IV_INVOKE_SINGLETON_EVENT(iv_runtime_project_graph_graph_executor_requested_event,
                    ProjectGraphGraphExecutorRequest{.graph = compiled.compiled_graph,
                        .definitions = request.inputs.definitions});
            std::scoped_lock lock(mutex_);
            if (!eligible()) {
                queue_control_compile_status_locked(request.trace, "superseded");
                return;
            }
            bool const is_desired = desired();
            auto state = std::make_shared<ProjectGraphGeneration>(*request.configured);
            state->graph_jit_attempted = compiled.attempted;
            state->graph_jit_diagnostics = std::move(compiled.diagnostics);
            state->graph_jit_timings = std::move(compiled.timings);
            state->graph_executor_attempted = execution.attempted;
            if (compiled.compiled_graph) queue_control_compile_status_locked(request.trace, "prepared");
            bool const deferred = execution.prepared.deferred_activation();
            bool const accepted = execution.prepared
                && std::move(execution.prepared).publish() == GraphExecutorStageResult::accepted;
            if (accepted) {
                state->execution_generation = compiled.compiled_graph->project_generation;
                state->compiled_graph = std::move(compiled.compiled_graph);
                state->graph_executor_accepted = true;
                accepted_execution_graph_ = state->compiled_graph;
                auto& checkpoint = deferred ? staged_execution_ : active_execution_;
                checkpoint = ExecutionCheckpoint{.configured = state, .compiled = state->compiled_graph};
                staged_control_graph_ = state->compiled_graph;
                state->execution_phase = "staged";
                queue_control_compile_status_locked(request.trace, "staged");
                // The next worker cause attaches live targets to this
                // accepted checkpoint even when a newer edit is queued.
                std::scoped_lock live(live_mutex_);
                restore_live_targets_ = !live_interactions_.empty();
            } else {
                state->execution_phase = compiled.attempted ? "failed" : "unavailable";
                state->execution_message = state->graph_jit_diagnostics.empty()
                    ? "No executable graph was accepted" : state->graph_jit_diagnostics.front().message;
                queue_control_compile_status_locked(request.trace, "failed", state->execution_message);
            }
            auto const end = Clock::now();
            auto const us = [](auto from, auto to) { return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(to - from).count()); };
            state->rebuild_timings.graph_jit_us = us(begin, compiled_at);
            state->rebuild_timings.executor_stage_us = us(compiled_at, end);
            state->rebuild_timings.total_us += us(begin, end);
            if (is_desired) {
                current_generation_ = state;
                latest_execution_attempt_.reset();
            }
            publication.graph_changed = true;
            publication.execution_preparation = ExecutionPreparationProgress{.attempt = request.attempt};
            if (!accepted && compiled.attempted) {
                auto message = "Graph compilation failed for configuration "
                    + std::to_string(state->configured_generation) + ". ";
                message += accepted_execution_graph_ ? "The previous audio graph remains active."
                    : "No audio graph has been accepted yet.";
                if (state->graph_jit_diagnostics.empty()) message += "\n" + state->execution_message;
                for (auto const& diagnostic : state->graph_jit_diagnostics) {
                    message += "\n" + diagnostic.message;
                    if (!diagnostic.node_type_id.empty()) message += " [node " + diagnostic.node_type_id + "]";
                    if (!diagnostic.package_root.empty()) message += " [package " + diagnostic.package_root + "]";
                }
                publication.execution_failure = ProjectStatusNotification{
                    .level = "error", .code = "graphCompilationFailed", .message = std::move(message)};
            }
            if (diagnostic_flag_enabled("IV_EDIT_TIMING")) log_edit_timing(*state);
            if (accepted && is_desired) {
                // Storage-changing controls settle through structural staging
                // rather than code replacement. Release the same settlement
                // gate before preparing the latest hover-only variant.
                if (settling_controls_ && dynamic_inputs_.empty()
                    && state->compiled_graph->configured_graph
                    && control_configuration_matches_locked(
                        *state->compiled_graph->configured_graph, dynamic_inputs_)) {
                    settling_controls_ = false;
                }
                prepare_released_control_locked();
                if ((!dynamic_inputs_.empty() || settling_controls_) && !control_code_matches_locked(dynamic_inputs_))
                    schedule_control_compile_locked(dynamic_inputs_, true);
                else {
                    auto const predictive = desired_dynamic_inputs_locked(true);
                    if (!predictive.empty()) schedule_control_compile_locked(predictive, false);
                }
            }
        }
    } catch (std::exception const& error) {
        std::scoped_lock lock(mutex_);
        if (!eligible()) return;
        auto state = std::make_shared<ProjectGraphGeneration>(*request.configured);
        state->execution_phase = "failed";
        state->execution_message = error.what();
        state->graph_jit_attempted = true;
        state->graph_jit_diagnostics = {{.message = error.what()}};
        publication.execution_preparation = ExecutionPreparationProgress{.attempt = request.attempt};
        if (desired()) {
            current_generation_ = std::move(state);
            latest_execution_attempt_.reset();
        }
        queue_control_compile_status_locked(request.trace, "failed", error.what());
        publication.graph_changed = true;
        publication.execution_failure = ProjectStatusNotification{
            .level = "error", .code = "graphCompilationFailed", .message = error.what()};
    }
    IV_EVENT_SOURCE(ProjectGraph, "graph compilation completion") {
        // This is the compilation cause's one read-model publication. It is not
        // a durable project edit or a new undo entry.
        if (!publication.empty()) IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_graph_execution_published_event, publication);
        completion_published = true;
    }
}

void ProjectGraph::request_execution_recovery()
{
    {
        std::scoped_lock lock(mutex_);
        if (compile_worker_.get_stop_token().stop_requested() || execution_recovery_requested_) return;
        execution_recovery_requested_ = true;
    }
    control_compile_changed_.notify_all();
}

void ProjectGraph::recover_failed_execution(std::stop_token stop)
{
    std::optional<FailedExecutionGeneration> recovery;
    struct PublishRebuiltSuffix {
        std::optional<FailedExecutionGeneration>& recovery;
        ~PublishRebuiltSuffix() { if (recovery) recovery->publish(); }
    } publish{recovery};
    IV_EVENT_SOURCE(ProjectGraph, "reject pending execution generation") {
        recovery = IV_INVOKE_SINGLETON_EVENT(iv_runtime_project_graph_execution_recovery_event);
        if (!recovery) return;
        auto const failed = recovery->failed->project_generation;
        ProjectGraphPublication publication{
            .graph_changed = true,
            .execution_failure = ProjectStatusNotification{
                .level = "error", .code = "graphPreparationFailed",
                .message = "Execution generation " + std::to_string(failed)
                    + " could not prepare: " + recovery->message},
        };
        {
            std::scoped_lock lock(mutex_);
            accepted_execution_graph_ = recovery->predecessor;
            staged_execution_.reset();
            if (active_execution_ && active_execution_->compiled != recovery->predecessor)
                active_execution_.reset();
            staged_control_graph_ = recovery->predecessor;
            ready_control_code_.reset();
            cancel_pending_control_compile_locked();
            restore_live_targets_ = false;
            if (current_generation_ && current_generation_->execution_generation == failed) {
                auto state = std::make_shared<ProjectGraphGeneration>(*current_generation_);
                state->execution_generation.reset();
                state->graph_executor_accepted = false;
                state->compiled_graph.reset();
                state->execution_phase = "failed";
                state->execution_message = recovery->message;
                current_generation_ = std::move(state);
            } else if (current_generation_ && current_generation_->execution_generation
                && std::ranges::contains(recovery->successors,
                    *current_generation_->execution_generation, &GraphJitCompileRequest::project_generation)) {
                auto state = std::make_shared<ProjectGraphGeneration>(*current_generation_);
                state->graph_executor_accepted = false;
                state->compiled_graph.reset();
                state->execution_phase = "compiling";
                current_generation_ = std::move(state);
            }
        }
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_execution_published_event, publication);
    }

    auto predecessor = std::move(recovery->predecessor);
    for (auto const& request : recovery->successors) {
        if (stop.stop_requested()) return;
        // Rebase previously accepted successors using their original inputs.
        // Freshness against the latest desired project is for new work.
        IV_EVENT_SOURCE(ProjectGraph, "recompile pending execution generation") {
            GraphJitCompileResult compiled;
            std::string failure;
            bool deferred = false;
            try {
                compiled = IV_INVOKE_SINGLETON_EVENT(iv_runtime_project_graph_graph_jit_requested_event,
                    ProjectGraphGraphJitRequest{.successor = request, .predecessor = predecessor,
                        .initial_generation = !predecessor});
                if (!compiled.compiled_graph) {
                    failure = compiled.diagnostics.empty() ? "GraphJit did not produce an executable graph"
                        : compiled.diagnostics.front().message;
                } else {
                    auto execution = IV_INVOKE_SINGLETON_EVENT(iv_runtime_project_graph_graph_executor_requested_event,
                        ProjectGraphGraphExecutorRequest{.graph = compiled.compiled_graph,
                            .definitions = request.definitions});
                    if (stop.stop_requested()) return;
                    deferred = execution.prepared.deferred_activation();
                    if (!execution.prepared
                        || std::move(execution.prepared).publish() != GraphExecutorStageResult::accepted) {
                        failure = "GraphExecutor did not accept the rebased cutover";
                    }
                }
            } catch (std::exception const& error) {
                failure = error.what();
            }
            ProjectGraphPublication publication{.graph_changed = true};
            {
                std::scoped_lock lock(mutex_);
                if (failure.empty()) {
                    predecessor = compiled.compiled_graph;
                    accepted_execution_graph_ = predecessor;
                    staged_control_graph_ = predecessor;
                }
                if (current_generation_ && current_generation_->execution_generation == request.project_generation) {
                    auto state = std::make_shared<ProjectGraphGeneration>(*current_generation_);
                    if (!failure.empty()) state->execution_generation.reset();
                    state->compiled_graph = failure.empty() ? compiled.compiled_graph : nullptr;
                    state->graph_jit_attempted = compiled.attempted;
                    state->graph_jit_diagnostics = std::move(compiled.diagnostics);
                    state->graph_jit_timings = std::move(compiled.timings);
                    state->graph_executor_accepted = failure.empty();
                    state->execution_phase = failure.empty() ? "staged" : "failed";
                    state->execution_message = failure;
                    if (failure.empty()) {
                        auto& checkpoint = deferred ? staged_execution_ : active_execution_;
                        checkpoint = ExecutionCheckpoint{
                            .configured = current_generation_, .compiled = compiled.compiled_graph};
                    }
                    current_generation_ = std::move(state);
                    restore_live_targets_ = failure.empty();
                }
            }
            if (!failure.empty()) publication.execution_failure = ProjectStatusNotification{
                .level = "error", .code = "graphRebaseFailed",
                .message = "Execution generation " + std::to_string(request.project_generation)
                    + " could not be rebuilt: " + failure};
            IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_execution_published_event, publication);
        }
    }
}

void ProjectGraph::compile_worker(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        {
            IV_EVENT_SOURCE(ProjectGraph, "restore live controls after graph preparation") {
                std::scoped_lock lock(mutex_);
                if (std::exchange(restore_live_targets_, false) && accepted_execution_graph_) {
                    std::scoped_lock live(live_mutex_);
                    ProjectGraphGraphExecutorControlsRequest controls;
                    for (auto const& entry : live_interactions_) controls.targets.push_back({
                        .project_generation = accepted_execution_graph_->project_generation, .id = entry.control,
                        .value = entry.value, .gesture = ManualControlGesturePhase::begin});
                    if (!controls.targets.empty()) {
                        (void)IV_INVOKE_SINGLETON_EVENT(iv_runtime_project_graph_graph_executor_controls_requested_event, controls);
                        for (std::size_t index = 0; index < live_interactions_.size(); ++index) {
                            auto& entry = live_interactions_[index];
                            auto& target = controls.targets[index];
                            if (entry.target) {
                                auto open = entry.target->open_gestures.load(std::memory_order_relaxed);
                                while (open && !entry.target->open_gestures.compare_exchange_weak(
                                    open, open - 1, std::memory_order_release, std::memory_order_relaxed)) {}
                            }
                            entry.target = std::move(target.live_target);
                        }
                    }
                }
            }
        }
        control_compile_changed_.notify_all();
        std::optional<ControlCompileRequest> request;
        std::optional<GraphCompileRequest> graph_request;
        std::shared_ptr<ObservationCompileCandidate> observation_prediction;
        bool recover_execution = false;
        {
            std::unique_lock lock(mutex_);
            // A previous iteration has completed publication, optional storage
            // scheduling and live-target restoration before becoming idle.
            compile_inflight_ = false;
            control_compile_changed_.notify_all();
            control_compile_changed_.wait(lock, [&] {
                return stop.stop_requested() || execution_recovery_requested_ || (pending_graph_compile_ && !staged_execution_)
                    || pending_control_compile_.has_value() || !control_compile_notifications_.empty()
                    || can_compile_observation_prediction_locked();
            });
            if (stop.stop_requested()) return;
            if (execution_recovery_requested_) {
                recover_execution = std::exchange(execution_recovery_requested_, false);
                compile_inflight_ = true;
            } else if (pending_graph_compile_ && !staged_execution_) {
                graph_request = std::exchange(pending_graph_compile_, std::nullopt);
                compile_inflight_ = true;
            } else if (pending_control_compile_) {
                compile_inflight_ = true;
                request = std::move(pending_control_compile_);
                pending_control_compile_.reset();
                inflight_control_compile_ = *request;
                if (!request->prepared_graph) queue_control_compile_status_locked(request->trace, "compiling");
            } else if (can_compile_observation_prediction_locked()) {
                observation_prediction_pending_ = false;
                try {
                    auto graph = graph_for_compile_locked(desired_dynamic_inputs_locked(false));
                    GraphJitCompileRequest inputs{.graph = std::move(graph),
                        .definitions = current_generation_->definitions,
                        .observations = resolve_observation_requirements(*current_generation_->graph,
                            current_generation_->placements, *predicted_observation_requirements_),
                        .configured_generation = current_generation_->configured_generation,
                        .configured_bundles = current_generation_->configured_bundles};
                    bool const attachable = staged_control_graph_ && staged_control_graph_->configured_graph
                        && staged_control_graph_->configured_graph->manual_controls == inputs.graph->manual_controls
                        && GraphExecutor::can_attach_realtime_observations(*staged_control_graph_, inputs.observations);
                    if (!attachable) {
                        observation_prediction = observation_candidate_ = std::make_shared<ObservationCompileCandidate>(
                            ObservationCompileCandidate{*predicted_observation_requirements_, std::move(inputs), accepted_execution_graph_});
                        compile_inflight_ = true;
                    }
                } catch (...) { observation_candidate_.reset(); }
            }
        }
        // Requests queue notifications; this independent cause publishes them
        // after releasing the graph lock, without re-entering an RPC cause.
        publish_control_compile_status();
        if (recover_execution) {
            try { recover_failed_execution(stop); }
            catch (std::exception const& error) {
                try { diagnostic_stream() << "execution generation recovery: " << error.what() << '\n';
                    flush_diagnostic_stream(); } catch (...) {}
            }
            continue;
        }
        if (graph_request) {
            try { compile_graph(std::move(*graph_request), stop); }
            catch (std::exception const& error) {
                try { diagnostic_stream() << "graph compilation worker: " << error.what() << '\n';
                    flush_diagnostic_stream(); } catch (...) {}
            }
            continue;
        }
        if (observation_prediction) {
            std::shared_ptr<CompiledGraph const> observation_code;
            {
                std::scoped_lock lock(mutex_);
                auto const claimed = pending_graph_compile_
                    && pending_graph_compile_->observation_candidate == observation_prediction;
                if (!claimed && observation_candidate_ != observation_prediction) continue;
                if (!claimed && (pending_graph_compile_ || pending_control_compile_ || execution_recovery_requested_)) {
                    observation_candidate_.reset();
                    observation_prediction_pending_ = true;
                    continue;
                }
                observation_code = observation_predecessor_code_locked(
                    observation_prediction->inputs, observation_prediction->predecessor);
            }
            try {
                IV_EVENT_SOURCE(ProjectGraph, "predictive observation compilation") {
                    observation_prediction->result = IV_INVOKE_SINGLETON_EVENT(iv_runtime_project_graph_graph_jit_requested_event,
                        ProjectGraphGraphJitRequest{.successor = observation_prediction->inputs,
                            .predecessor = observation_prediction->predecessor,
                            .active_observation_predecessor_code = std::move(observation_code)});
                }
            } catch (...) {
                // The required compilation reports its own diagnostics if
                // this viewport eventually becomes active.
            }
            continue;
        }
        if (!request) continue;

        // A finished compile completes work scheduled by an earlier cause, so
        // it is a cause of its own: at most one GraphJit request, then at most one
        // GraphExecutor control update.
        bool storage_change = false;
        IV_EVENT_SOURCE(ProjectGraph, "control compile completion") {
            auto result = request->prepared_graph
                ? GraphJitCompileResult{}
                : IV_INVOKE_SINGLETON_EVENT(
                    iv_runtime_project_graph_graph_jit_requested_event,
                    ProjectGraphGraphJitRequest{.successor = request->inputs});
            auto code = request->prepared_graph
                ? request->prepared_graph : std::shared_ptr<CompiledGraph const>{std::move(result.compiled_graph)};

            std::scoped_lock lock(mutex_);
            auto activate_on_ready = request->activate_on_ready;
            if (inflight_control_compile_
                && inflight_control_compile_->request_id == request->request_id) {
                activate_on_ready |=
                    inflight_control_compile_->activate_on_ready;
                inflight_control_compile_.reset();
                control_compile_changed_.notify_all();
            }
            if (stop.stop_requested()) return;
            if (request->request_id != latest_control_compile_request_
                || !compile_inputs_are_current_locked(
                    request->inputs.project_generation, request->inputs.observations.revision)) {
                queue_control_compile_status_locked(request->trace, "superseded");
                continue;
            }
            if (activate_on_ready && !control_configuration_matches_locked(*request->inputs.graph, dynamic_inputs_)) {
                queue_control_compile_status_locked(request->trace, "superseded");
                continue;
            }
            if (!code) {
                queue_control_compile_status_locked(request->trace, "failed",
                    result.diagnostics.empty() ? "GraphJit returned no executable graph"
                        : result.diagnostics.front().message);
                continue;
            }
            ready_control_code_ = ReadyControlCode{
                .dynamic_inputs = request->dynamic_inputs,
                .graph = std::move(code),
                .trace = request->trace,
            };
            if (!request->prepared_graph) queue_control_compile_status_locked(request->trace, "prepared");
            auto staged = false;
            if (activate_on_ready) {
                storage_change = !GraphExecutor::can_replace_realtime_code(
                    *current_generation_->compiled_graph, *ready_control_code_->graph);
                if (storage_change) {
                    queue_control_compile_status_locked(request->trace, "compilingStorage",
                        "preparing a graph transition for changed storage");
                }
                if (!storage_change) {
                    staged = update_graph_executor_controls_locked(
                        {},
                        std::span<ManualControlId const>(request->dynamic_inputs));
                }
            }
            if (staged) prepare_released_control_locked();
            // A settled constant replacement consumes the one ready-code slot.
            // If the client still hovers a participating control, prepare that
            // exact dynamic set again without activating it.
            if (staged && request->dynamic_inputs.empty()
                && dynamic_inputs_.empty()) {
                settling_controls_ = false;
                auto const predictive_inputs = desired_dynamic_inputs_locked(true);
                if (!predictive_inputs.empty()) {
                    schedule_control_compile_locked(predictive_inputs, false);
                }
            }
        }
        if (storage_change) {
            publish_control_compile_status();
            compile_control_storage_change(request->request_id, request->trace);
        }
    }
}

void ProjectGraph::compile_control_storage_change(std::uint64_t request_id,
    std::shared_ptr<ControlCompileTrace> const& trace)
{
    IV_EVENT_SOURCE(ProjectGraph, "manual-control storage configuration") {
        {
            std::scoped_lock lock(mutex_);
            if (request_id != latest_control_compile_request_ || !accepted_execution_graph_
                || !ready_control_code_matches_locked(dynamic_inputs_)) {
                queue_control_compile_status_locked(trace, "superseded");
                return;
            }
            auto graph = ready_control_code_->graph->configured_graph;
            ready_control_code_.reset();
            schedule_graph_compile_locked(trace, false, std::move(graph));
        }
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_execution_published_event,
            ProjectGraphPublication{.graph_changed = true});
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
            << "\nedit timing: configuration " << generation.configured_generation
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
                    << " loads substituted, "
                    << compile.region_specialization_allocated_bytes
                    << " allocated code bytes, "
                    << compile.region_specialization_cache_bytes << " / "
                    << compile.region_specialization_cache_budget_bytes
                    << " cache bytes\n";
                if (compile.region_specializations_compiled != 0) {
                    out << "    cold specialization: prepare "
                        << ms(compile.region_specialization_prepare_us)
                        << " + emit/link "
                        << ms(compile.region_specialization_emit_link_us)
                        << " + optimize "
                        << ms(compile.region_specialization_optimization_us)
                        << " + codegen "
                        << ms(compile.region_specialization_materialization_us)
                        << " ms; "
                        << compile.region_specialization_retargeted_functions
                        << " functions retargeted\n";
                }
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

namespace {
void configure_manual_controls(
    ConfiguredGraph& finished_graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    std::span<ProjectManualControl const> controls,
    std::vector<ProjectGraphGeneration::ManualControlDiagnostic>& diagnostics)
{
    // Authored defaults are already constants. Give eligible public inputs the
    // same manual binding without inventing saved state or an undo action.
    std::optional<graph_jit::ResolvedPortRealization> gain_realization;
    bool resolved_gain_types = false;
    auto supports_gain = [&](NodeBundlePortId output) {
        auto const config = finished_graph.node_bundles.resolve_sample_output(output).config;
        auto type = continuous_value_type_domain(config).sole_type();
        if (!type) {
            if (!resolved_gain_types) {
                resolved_gain_types = true;
                auto resolved = graph_jit::resolve_port_realization(finished_graph);
                if (resolved) gain_realization.emplace(std::move(*resolved));
            }
            if (gain_realization) type = gain_realization->output(output.node_bundle_handle, output.port_index).value_type;
        }
        return type && continuous_value_type_info(*type).apply_output_gain != nullptr;
    };
    std::vector<ProjectManualControl> effective_controls(controls.begin(), controls.end());
    std::unordered_map<std::string, std::vector<ManualControlId>> declared_controls;
    for (auto const& control : controls)
        declared_controls[control.id.node.instance_id].push_back(control.id);
    for (auto const& [instance_id, placement] : placements) {
        auto add_control = [&](ManualControlId id, Sample value, bool enabled) {
            auto& declared = declared_controls[instance_id];
            if (std::ranges::contains(declared, id)) return;
            declared.push_back(id);
            effective_controls.push_back({.id = std::move(id), .stored_value = value, .enabled = enabled});
        };
        std::unordered_set<NodeBundleHandle> seen_members;
        auto register_scope = [&](auto const& self, NodeAddress address, ProjectNodePathMatch match,
                                  NodeBundleHandle representative) -> void {
            bool const is_root = address.path.empty();
            if (match.kind == ProjectNodePathMatch::Kind::bundle) {
                if (!seen_members.insert(representative).second) return;
                if (!is_root) {
                    auto path = project_member_path(finished_graph, placement.root, representative);
                    if (!path) return;
                    address.path = std::move(*path);
                }
            }
            auto const interface = match.kind == ProjectNodePathMatch::Kind::symbolic_node
                ? read_symbolic_node_interface(finished_graph.node_bundles, finished_graph.symbolic_nodes.record(match.symbolic_node), representative)
                : read_node_instance_interface(finished_graph.node_bundles, representative);
            for (auto const& port : interface.sample_inputs) {
                if (port.name.empty() || !port.manual_defaults) continue;
                add_control({.node = address, .port_name = port.name}, port.manual_defaults->value,
                    match.kind == ProjectNodePathMatch::Kind::bundle && port.manual_defaults->enabled);
            }
            for (auto const& port : interface.sample_outputs) {
                if (!port.name.empty() && port.index < finished_graph.node_bundles.bundle(representative).sample_output_count()
                        && supports_gain({representative, PortKind::sample, port.index}))
                    add_control({.node = address, .port_name = port.name, .kind = ManualControlKind::output_gain},
                        Sample{1}, true);
            }
            if (is_root || interface.has_children || match.kind == ProjectNodePathMatch::Kind::symbolic_node
                || finished_graph.node_bundles.bundle(representative).is_tiled()) {
                for (auto const& child : project_child_nodes(finished_graph, match)) {
                    auto child_address = address;
                    child_address.path.insert(child_address.path.end(), child.path.begin(), child.path.end());
                    auto const matches = resolve_node_address(finished_graph, placements, child_address);
                    if (matches.size() == 1) self(self, std::move(child_address), matches.front(), child.bundle);
                }
            }
        };
        register_scope(register_scope, NodeAddress{.instance_id = instance_id},
            {.kind = ProjectNodePathMatch::Kind::bundle, .bundle = placement.root}, placement.root);
    }
    struct ResolvedPort {
        std::size_t selection_index;
        NodeBundlePortId port;
        bool requires_preparation = false;
    };
    std::vector<ResolvedPort> resolved_ports;
    for (std::size_t control_index = 0; control_index < effective_controls.size(); ++control_index) {
        auto const& control = effective_controls[control_index];
        auto const matches = resolve_node_address(
            finished_graph, placements, control.id.node);
        bool const output_gain = control.id.kind == ManualControlKind::output_gain;
        std::vector<NodeBundleHandle> bundles;
        if (output_gain) {
            // A public tiled output is one multichannel output. Keep its
            // identity rather than applying several unrelated member gains.
            auto append = [&](NodeBundleHandle bundle) {
                if (!std::ranges::contains(bundles, bundle)) bundles.push_back(bundle);
            };
            for (auto const& match : matches) {
                if (match.kind == ProjectNodePathMatch::Kind::bundle) append(match.bundle);
                else {
                    auto const& members = finished_graph.symbolic_nodes.record(match.symbolic_node).node_bundle_handles;
                    for (auto member : match.symbolic_member_indices)
                        if (member < members.size()) append(members[member]);
                }
            }
        } else bundles = concrete_node_address_matches(finished_graph, matches);
        if (bundles.empty()) {
            if (!control.enabled) continue;
            diagnostics.push_back({
                .id = control.id,
                .message = "manual control address does not select a concrete node",
            });
            continue;
        }
        resolved_ports.clear();
        for (std::size_t selection_index = 0; selection_index < bundles.size(); ++selection_index) {
            auto const bundle = bundles[selection_index];
            auto const& selected = finished_graph.node_bundles.bundle(bundle);
            if (output_gain) {
                std::optional<std::size_t> port;
                for (std::size_t index = 0; index < selected.sample_output_count(); ++index)
                    if (finished_graph.node_bundles.resolve_sample_output({bundle, PortKind::sample, index}).config.name == control.id.port_name)
                        port = index;
                if (!port) {
                    diagnostics.push_back({control.id, "gain output name is unavailable"});
                    continue;
                }
                auto const source = NodeBundlePortId{bundle, PortKind::sample, *port};
                if (!supports_gain(source)) {
                    diagnostics.push_back({control.id, "output value type has no scalar gain operation"});
                    continue;
                }
                resolved_ports.push_back({selection_index, source});
                continue;
            }
            std::size_t port = 0;
            for (; port < selected.sample_input_count(); ++port) {
                NodeBundlePortId const input{bundle, PortKind::sample, port};
                auto const config = finished_graph.node_bundles.resolve_sample_input(input).config;
                if (config.name != control.id.port_name) continue;
                if (manual_input_defaults(config)) {
                    resolved_ports.push_back({selection_index, input, supports_coverage_manual_control(config)});
                } else if (control.enabled) {
                    diagnostics.push_back({control.id, "manual controls require a Sequential or windowed Coverage Sample input"});
                }
                break;
            }
            if (port == selected.sample_input_count() && control.enabled)
                diagnostics.push_back({control.id, "manual control input name does not select a continuous input"});
        }
        bool const symbolic_selection = std::ranges::any_of(matches, [](auto const& match) {
            return match.kind == ProjectNodePathMatch::Kind::symbolic_node;
        });
        if (symbolic_selection && resolved_ports.size() != bundles.size()) continue;
        for (auto const& resolved : resolved_ports) {
            if (output_gain) {
                finished_graph.manual_controls.push_back({.id = control.id,
                    .concrete_selection_index = resolved.selection_index,
                    .settled_value = control.stored_value, .output = resolved.port});
            } else {
                finished_graph.manual_controls.push_back({
                    .id = control.id, .input = resolved.port,
                    .concrete_selection_index = resolved.selection_index,
                    .settled_value = control.stored_value,
                    .participation = control.participation,
                    .requires_preparation = resolved.requires_preparation,
                    .enabled = control.enabled,
                    .authored = control_index < controls.size(),
                });
            }
        }
    }
    graph_jit::detail::classify_output_gain_consumers(finished_graph);
    std::vector<ManualControlId> prepared_groups;
    for (auto const& control : finished_graph.manual_controls)
        if (control.requires_preparation && !control.id.node.path.empty()
            && std::holds_alternative<ProjectSymbolicNodeSelector>(control.id.node.path.back())
            && !std::ranges::contains(prepared_groups, control.id))
            prepared_groups.push_back(control.id);
    for (auto& control : finished_graph.manual_controls)
        if (std::ranges::contains(prepared_groups, control.id)) control.requires_preparation = true;
}
} // namespace

ProjectGraph::RebuildResult ProjectGraph::rebuild_locked(
    NodeInstancesMutation instance_mutation,
    GraphConnectionsMutation connection_mutation,
    bool publish_execution,
    NodeDefinitionsSnapshotChanged const* definitions_change,
    InteractionTransactions* interaction_transactions,
    std::optional<std::vector<ProjectManualControl>> proposed_controls,
    bool record_history,
    ProjectEditEffect* accepted_effect)
{
    using Clock = std::chrono::steady_clock;
    auto const rebuild_begin = Clock::now();
    auto const& definitions = definitions_change ? definitions_change->snapshot : definitions_snapshot_;
    bool const capture_effect = publish_execution && (record_history || accepted_effect)
        && (!std::holds_alternative<std::monostate>(instance_mutation)
            || !std::holds_alternative<std::monostate>(connection_mutation) || proposed_controls);
    std::optional<ProjectEditEffect> effect;
    auto commit_controls_and_history = [&] {
        if (effect) {
            if (accepted_effect) *accepted_effect = std::move(*effect);
            else record_edit_locked(std::move(*effect));
        }
        if (proposed_controls) manual_controls_ = std::move(*proposed_controls);
    };
    GraphBuilder root_builder;
    NodeInstancesProjectGraphRequest request{
        .snapshot = definitions,
        .root_builder = &root_builder,
        .mutation = std::move(instance_mutation),
        .prepare_failure_interfaces = publish_execution,
        .capture_effect = capture_effect,
    };
    if (interaction_transactions) {
        auto& transaction = interaction_transactions->node_instances;
        if (!transaction) {
            NodeInstancesInteractionRequest open{};
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_project_graph_node_instances_interaction_requested_event,
                open);
            if (!open.handled) {
                throw std::runtime_error(
                    "ProjectGraph cannot run a node-message interaction without NodeInstances");
            }
            transaction = std::move(open.interaction);
        }
        transaction.run(request);
    } else {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_graph_node_instances_requested_event,
            request);
    }
    if (!request.handled) {
        throw std::runtime_error("ProjectGraph cannot rebuild without NodeInstances");
    }

    // The read-model changes of this build. Source introspection is updated
    // once, from both the definitions publication and the instance
    // declarations, before configuration can fail: neither depends on the
    // configured graph. Previews change no read model.
    ProjectGraphPublication publication;
    if (publish_execution) {
        publication.graph_changed = true;
        if (request.instances_list_changed) publication.instances = request.instances;
        if (definitions_change || request.instance_declarations_changed) {
            IvModuleSourceIntrospectionProjectGraphRequest introspection{};
            if (definitions_change) introspection.package_definitions = definitions_change->package_definitions;
            if (request.instance_declarations_changed) {
                introspection.instance_declarations = request.instances;
            }
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_project_graph_source_introspection_requested_event,
                introspection);
            publication.symbolic_nodes = std::move(introspection.result);
        }
    }

    // A failed configuration pass discards the root builder and publishes no
    // generation; the previous generation stays current and keeps running.
    auto fail = [&](ProjectGraphConfigurationFailure failure) {
        if (publish_execution && !failure.instance_interfaces) {
            // A later connection/finish failure can also leave the desired
            // interfaces newer than the executable generation. Read already
            // configured results, never ports from that older generation.
            auto interfaces = std::make_shared<NodeInstanceFailureInterfaces>();
            interfaces->definitions = request.snapshot;
            for (auto const& [id, placement] : request.result.placements) {
                interfaces->configured_instances.emplace(id, placement.configured);
                if (auto const& graph = placement.configured->graph) {
                    interfaces->interfaces.emplace(id, read_node_instance_interface(
                        graph->node_bundles, placement.configured->local_root));
                } else {
                    // A NodeRef-dependent result has no closed reusable graph.
                    // Its original session can supply metadata only while valid.
                    try {
                        interfaces->interfaces.emplace(id, read_node_instance_interface(
                            details::builder_session_state(root_builder._builder_session()).node_bundles(),
                            placement.root));
                    } catch (std::logic_error const&) {
                        // Unavailable is preferable to a stale desired interface.
                    }
                }
            }
            failure.instance_interfaces = std::move(interfaces);
        }
        auto shared = std::make_shared<ProjectGraphConfigurationFailure const>(
            std::move(failure));
        if (publish_execution) {
            commit_controls_and_history();
            prune_live_interactions_locked();
            log_configuration_failure(*shared);
            configuration_failure_ = shared;
            pending_graph_compile_.reset();
            cancel_pending_control_compile_locked();
            ready_control_code_.reset();
            latest_control_compile_request_ = next_control_compile_request_++;
            if (auto attempt = std::exchange(latest_execution_attempt_, std::nullopt))
                publication.execution_preparation = ExecutionPreparationProgress{.attempt = *attempt};
            control_compile_changed_.notify_all();
        }
        return RebuildResult{
            .created_instance_ids = std::move(request.created_instance_ids),
            .failure = std::move(shared),
            .publication = std::move(publication),
        };
    };
    auto const definitions_generation = definitions->generation;
    // A failed instance batch leaves the root builder unusable, so connections
    // are not realized; their mutation still joins the desired revision.
    bool const instances_failed = !request.result.diagnostics.empty();
    GraphConnectionsProjectGraphRequest connections_request{
        .root_builder = instances_failed ? nullptr : &root_builder,
        .placements = &request.result.placements,
        .mutation = std::move(connection_mutation),
        .capture_effect = capture_effect,
    };
    auto const instances_end = Clock::now();
    if (interaction_transactions) {
        auto& transaction = interaction_transactions->graph_connections;
        if (!transaction) {
            GraphConnectionsInteractionRequest open{};
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_project_graph_connections_interaction_requested_event,
                open);
            if (!open.handled) {
                throw std::runtime_error(
                    "ProjectGraph cannot run a node-message interaction without GraphConnections");
            }
            transaction = std::move(open.interaction);
        }
        transaction.run(connections_request);
    } else {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_project_graph_connections_requested_event,
            connections_request);
    }
    auto const connections_end = Clock::now();
    if (!connections_request.handled) {
        throw std::runtime_error("ProjectGraph cannot rebuild without GraphConnections");
    }
    if (capture_effect) {
        // NodeInstances owns replacement removal. Explicit project deletion
        // prunes connection matchers in apply_edit_locked before realization.
        std::unordered_set<std::string> remaining_instances;
        for (auto const& value : request.after.instances) remaining_instances.insert(value.instance_id);
        std::unordered_set<std::string> removed_instances;
        for (auto const& value : request.before.instances) {
            if (!remaining_instances.contains(value.instance_id)) removed_instances.insert(value.instance_id);
        }
        auto const control_was_removed = [&](auto const& control) { return removed_instances.contains(control.id.node.instance_id); };
        if (!proposed_controls && std::ranges::any_of(manual_controls_, control_was_removed))
            proposed_controls = manual_controls_;
        if (proposed_controls) std::erase_if(*proposed_controls, control_was_removed);
        effect = ProjectEditEffect{
            .instances = project_record_changes(request.before.instances, request.after.instances),
            .replacements = project_record_changes(request.before.replacements, request.after.replacements),
            .connections = project_record_changes(connections_request.before, connections_request.after),
            .manual_controls = proposed_controls ? project_record_changes(manual_controls_, *proposed_controls)
                                                 : std::vector<ProjectRecordChange<ProjectManualControl>>{},
        };
    }
    if (instances_failed) {
        return fail(ProjectGraphConfigurationFailure{
            .definitions_generation = definitions_generation,
            .instance_interfaces = std::move(request.failure_interfaces),
            .node_instance_diagnostics = std::move(request.result.diagnostics),
        });
    }
    if (connections_request.result.failure) {
        return fail(ProjectGraphConfigurationFailure{
            .definitions_generation = definitions_generation,
            .connection_failure = std::move(connections_request.result.failure),
        });
    }

    std::string finish_failure;
    auto configured_graph = [&]() -> std::optional<ConfiguredGraph> {
        try {
            root_builder.outputs();
            return std::move(root_builder).finish();
        } catch (std::exception const& error) {
            finish_failure = error.what();
            return std::nullopt;
        }
    }();
    if (!configured_graph) {
        return fail(ProjectGraphConfigurationFailure{
            .definitions_generation = definitions_generation,
            .connection_failure = GraphConnectionDiagnostic{
                .message = "project graph finalization failed: " + finish_failure,
            },
        });
    }
    auto finished_graph = std::move(*configured_graph);
    auto generation = std::make_shared<ProjectGraphGeneration>();
    generation->configured_generation = next_configured_generation_;
    generation->definitions_generation = definitions_generation;
    generation->definitions = definitions;
    generation->placements = std::move(request.result.placements);
    for (auto const& [instance_id, placement] : generation->placements) {
        if (finished_graph.node_bundles.bundle(placement.root).is_concrete())
            finished_graph.project_leaf_instances.emplace(placement.root, instance_id);
    }
    generation->source_sample_connection_count = connections_request.result.source_sample_count;
    generation->source_event_connection_count = connections_request.result.source_event_count;
    generation->applied_connection_ids =
        std::move(connections_request.result.applied_connection_ids);
    generation->connection_diagnostics =
        std::move(connections_request.result.diagnostics);
    generation->node_replacements =
        std::move(request.realized_node_replacements);
    auto const& controls = proposed_controls ? *proposed_controls : manual_controls_;
    generation->manual_controls = controls;

    configure_manual_controls(
        finished_graph, generation->placements, controls, generation->manual_control_diagnostics);

    auto next_manual_interactions = manual_interactions_;
    auto next_hovered_controls = hovered_controls_;
    auto next_retained_inputs = hover_retained_inputs_;
    auto next_dynamic_inputs = dynamic_inputs_;
    if (publish_execution) {
        auto const is_available = [&](ManualControlId const& id) {
            return std::ranges::any_of(
                finished_graph.manual_controls,
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
        std::erase_if(next_retained_inputs, [&](auto const& entry) {
            return !is_available(entry.second);
        });
        next_dynamic_inputs.clear();
        for (auto const& control : finished_graph.manual_controls) {
            auto const required = std::ranges::any_of(
                next_manual_interactions,
                [&](ActiveManualControlInteraction const& interaction) {
                    return interaction.control == control.id;
                }) || std::ranges::any_of(next_retained_inputs, [&](auto const& entry) {
                    return entry.second == control.id;
                });
            if (required && !std::ranges::contains(next_dynamic_inputs, control.id)) {
                next_dynamic_inputs.push_back(control.id);
            }
        }
        for (auto& configured : finished_graph.manual_controls) {
            configured.dynamic = std::ranges::contains(
                next_dynamic_inputs, configured.id);
            configured.override_connections = configured.dynamic && configured.id.kind == ManualControlKind::input;
        }
    }
    generation->graph = std::make_shared<ConfiguredGraph const>(
        std::move(finished_graph));
    static std::atomic<std::uint64_t> next_configured_bundle_snapshot{1};
    generation->configured_bundles = {
        .snapshot = next_configured_bundle_snapshot.fetch_add(1, std::memory_order_relaxed),
        .size = generation->graph->node_bundles.size(),
    };
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
        auto const finished_at = Clock::now();
        auto const microseconds = [](auto begin, auto end) {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<
                std::chrono::microseconds>(end - begin).count());
        };
        generation->rebuild_timings = {
            .node_instances_us = microseconds(rebuild_begin, instances_end),
            .graph_connections_us = microseconds(instances_end, connections_end),
            .finish_us = microseconds(connections_end, finished_at),
            .total_us = microseconds(rebuild_begin, finished_at),
        };
        commit_controls_and_history();
        manual_interactions_ = std::move(next_manual_interactions);
        prune_live_interactions_locked();
        hovered_controls_ = std::move(next_hovered_controls);
        hover_retained_inputs_ = std::move(next_retained_inputs);
        dynamic_inputs_ = std::move(next_dynamic_inputs);
        settling_controls_ = false;
        current_generation_ = generation;
        configuration_failure_.reset();
        ready_control_code_.reset();
        cancel_pending_control_compile_locked();
        latest_control_compile_request_ = next_control_compile_request_++;
        control_compile_changed_.notify_all();
        ++next_configured_generation_;
        schedule_graph_compile_locked();
        if (latest_execution_attempt_)
            publication.execution_preparation = ExecutionPreparationProgress{
                .attempt = *latest_execution_attempt_, .pending = true};
    }
    return RebuildResult{
        .generation = std::move(generation),
        .created_instance_ids = std::move(request.created_instance_ids),
        .publication = std::move(publication),
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
    InteractionTransactions transactions{};
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

    result.project_graph_generation = generation->configured_generation;
    result.definitions_generation = definitions_snapshot_ ? definitions_snapshot_->generation : generation->definitions_generation;
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

    if (definitions_snapshot_) {
        for (auto const& revision : definitions_snapshot_->package_revisions) {
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
                false,
                nullptr,
                &state.interaction->transactions);
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
    NodeMessageDispatchResult& result,
    std::optional<NodeMessageGestureId> const& gesture)
{
    if (!current_generation_) {
        throw std::runtime_error(
            "cannot dispatch a node message without a project graph");
    }
    if (gesture && std::ranges::none_of(node_message_gestures_, [&](auto const& open) {
            return open.id == *gesture && open.target == target;
        })) throw std::invalid_argument("node-message gesture ended or conflicted with another edit");
    NodeMessageInteraction interaction{
        .generation = current_generation_,
        .original_replacements = current_generation_->node_replacements,
        .replacements = current_generation_->node_replacements,
    };
    dispatch_node_message_locked(
        target, std::move(message), interaction, result);
    if (interaction.replacements == interaction.original_replacements) return;

    ProjectEditEffect effect;
    auto rebuilt = rebuild_locked(
        NodeReplacementsReplaceMutation{
            .replacements = std::move(interaction.replacements),
        },
        std::monostate{},
        true,
        nullptr,
        &interaction.transactions,
        std::nullopt,
        !gesture,
        gesture ? &effect : nullptr);
    if (gesture && !effect.empty()) {
        close_conflicting_node_gestures_locked(effect, gesture);
        auto open = std::ranges::find(node_message_gestures_, *gesture, &NodeMessageGesture::id);
        merge_project_effect(open->effect, std::move(effect));
    }
    result.project_state_changed = true;
    result.publication = std::move(rebuilt.publication);
}

NodeMessageDispatchResult ProjectGraph::dispatch_node_message(
    NodeAddress const& target,
    OwnedNodeMessage message,
    std::optional<NodeMessageGestureId> const& gesture)
{
    NodeMessageDispatchResult result;
    {
        std::scoped_lock lock(mutex_);
        dispatch_root_node_message_locked(
            target, std::move(message), result, gesture);
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
            target, std::move(owned), result, message.gesture);
    }
    if (result.project_state_changed) {
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
    }
    return result;
}

void ProjectGraph::handle_node_definitions_snapshot_changed(
    NodeDefinitionsSnapshotChanged& change)
{
    if (!change.snapshot) {
        throw std::invalid_argument("ProjectGraph received an empty definitions snapshot");
    }

    std::scoped_lock lock(mutex_);
    if (definitions_snapshot_
        && change.snapshot->generation <= definitions_snapshot_->generation) {
        return;
    }
    // Catalog publication alone does not change a configured graph. Preserve
    // its pinned provider closure, executable and pending compiler request.
    // A prior configuration failure must retry: it may name a newly available
    // definition not represented in the previous successful placements.
    if (current_generation_ && !configuration_failure_
        && std::ranges::all_of(current_generation_->placements, [&](auto const& entry) {
            return entry.second.configured && configured_definition_closure_matches(
                *entry.second.configured, *change.snapshot);
        })) {
        // Keep definition/source metadata current for later instance creation,
        // without rebuilding the existing configured graph or its executable.
        IvModuleSourceIntrospectionProjectGraphRequest introspection{};
        introspection.package_definitions = change.package_definitions;
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_source_introspection_requested_event, introspection);
        change.publication.symbolic_nodes = std::move(introspection.result);
    } else {
        auto rebuilt = rebuild_locked(
            std::monostate{}, std::monostate{}, true, &change);
        change.publication = std::move(rebuilt.publication);
    }
    // A reported configuration failure still accepts the desired catalog.
    // Exceptions leave this publication retryable.
    definitions_snapshot_ = change.snapshot;
}

void ProjectGraph::append_history_effect_locked(ProjectEditEffect effect)
{
    if (effect.empty()) return;
    edit_history_.resize(history_position_);
    edit_history_.push_back(std::move(effect));
    history_position_ = edit_history_.size();
    ++history_serial_;
}

void ProjectGraph::close_conflicting_node_gestures_locked(ProjectEditEffect const& effect,
    std::optional<NodeMessageGestureId> const& except)
{
    for (auto at = node_message_gestures_.begin(); at != node_message_gestures_.end();) {
        auto& open = *at;
        if (except && open.id == *except) { ++at; continue; }
        auto const changes_constructor = std::ranges::any_of(effect.instances, [&](auto const& change) {
            auto const& record = change.after ? *change.after : *change.before;
            return record.instance_id == open.target.instance_id && (!change.before || !change.after
                || change.before->definition_id != change.after->definition_id
                || change.before->configuration_source != change.after->configuration_source);
        });
        auto const replaces_target = std::ranges::any_of(effect.replacements, [&](auto const& change) {
            auto const& address = (change.after ? *change.after : *change.before).address;
            return address.instance_id == open.target.instance_id && address.path.size() <= open.target.path.size()
                && std::equal(address.path.begin(), address.path.end(), open.target.path.begin());
        });
        if (!changes_constructor && !replaces_target && !project_effects_overlap(open.effect, effect)) { ++at; continue; }
        append_history_effect_locked(std::move(open.effect));
        at = node_message_gestures_.erase(at);
    }
}

bool ProjectGraph::close_node_message_gestures_locked(std::optional<std::string> client)
{
    bool changed = false;
    for (auto at = node_message_gestures_.begin(); at != node_message_gestures_.end();) {
        auto& open = *at;
        if (client && open.id.client_id != *client) { ++at; continue; }
        changed |= !open.effect.empty();
        append_history_effect_locked(std::move(open.effect));
        at = node_message_gestures_.erase(at);
    }
    return changed;
}

void ProjectGraph::record_edit_locked(ProjectEditEffect effect)
{
    close_conflicting_node_gestures_locked(effect);
    append_history_effect_locked(std::move(effect));
}

void ProjectGraph::record_manual_edit_locked(std::optional<ProjectManualControl> before,
    std::optional<ProjectManualControl> after, ActiveManualControlInteraction& interaction)
{
    if (before == after) return;
    if (interaction.history_serial == history_serial_ && history_position_ != 0
        && history_position_ == edit_history_.size()) {
        auto& effect = edit_history_.back();
        if (effect.instances.empty() && effect.replacements.empty() && effect.connections.empty()
            && !effect.audio_devices && !effect.toolchain && effect.manual_controls.size() == 1 && effect.manual_controls[0].after == before) {
            effect.manual_controls[0].after = after;
            if (effect.manual_controls[0].before == effect.manual_controls[0].after) {
                edit_history_.pop_back();
                --history_position_;
                ++history_serial_;
                interaction.history_serial = 0;
            }
            return;
        }
    }
    record_edit_locked({.manual_controls = {{std::move(before), std::move(after)}}});
    interaction.history_serial = history_serial_;
}

void ProjectGraph::set_interaction_value_locked(ActiveManualControlInteraction& interaction, Sample value)
{
    auto* current = find_manual_control_locked(interaction.control);
    std::optional<ProjectManualControl> before = current ? std::optional(*current) : std::nullopt;
    ProjectManualControl next;
    if (current) {
        next = *current;
    } else {
        if (!current_generation_ || !current_generation_->graph)
            throw std::invalid_argument("manual control is unavailable");
        auto const& configured = current_generation_->graph->manual_controls;
        auto found = std::ranges::find(configured, interaction.control, &ConfiguredManualControl::id);
        if (found == configured.end()) throw std::invalid_argument("manual control is unavailable");
        next = {.id = interaction.control,
            .stored_value = found->output ? Sample{1} : found->settled_value,
            .participation = found->participation, .enabled = found->enabled};
        if (next.stored_value == value) return; // Untouched implicit default.
    }
    next.stored_value = value;
    // Returning to an implicit default within the same uninterrupted action
    // restores absence as well as the number. An intervening edit seals history.
    bool restore_absence = false;
    if (!interaction.initial_control && interaction.history_serial == history_serial_
        && history_position_ != 0 && history_position_ == edit_history_.size()
        && current_generation_ && current_generation_->graph) {
        auto const& configured = current_generation_->graph->manual_controls;
        auto found = std::ranges::find(configured, interaction.control, &ConfiguredManualControl::id);
        if (found != configured.end()) {
            if (found->output) restore_absence = value == Sample{1};
            else if (auto const defaults = manual_input_defaults(
                current_generation_->graph->node_bundles.resolve_sample_input(found->input).config))
                restore_absence = value == defaults->value && next.enabled == defaults->enabled;
        }
    }
    if (restore_absence) {
        std::erase_if(manual_controls_, [&](auto const& entry) { return entry.id == interaction.control; });
        record_manual_edit_locked(before, std::nullopt, interaction);
    } else {
        if (current) *current = next;
        else manual_controls_.push_back(next);
        record_manual_edit_locked(before, next, interaction);
    }
}

void ProjectGraph::open_transactions_locked(InteractionTransactions& transactions) const
{
    NodeInstancesInteractionRequest nodes{};
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_node_instances_interaction_requested_event, nodes);
    if (!nodes.handled) throw std::runtime_error("project editing requires NodeInstances");
    transactions.node_instances = std::move(nodes.interaction);
    GraphConnectionsInteractionRequest connections{};
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_connections_interaction_requested_event, connections);
    if (!connections.handled) throw std::runtime_error("project editing requires GraphConnections");
    transactions.graph_connections = std::move(connections.interaction);
}

namespace {
void remove_deleted_instance_endpoints(std::vector<ProjectConnection>& connections,
    std::unordered_set<std::string> const& removed_instances)
{
    auto const removed = [&](ProjectNodePortMatcher const& matcher) {
        return removed_instances.contains(matcher.instance_id);
    };
    auto const loses_side = [&](std::vector<ProjectNodePortMatcher> const& matchers) {
        return !matchers.empty() && std::ranges::all_of(matchers, removed);
    };
    std::erase_if(connections, [&](ProjectConnection const& connection) {
        return std::visit([&](auto const& value) {
            return loses_side(value.outputs) || loses_side(value.inputs);
        }, connection);
    });
    for (auto& connection : connections) {
        std::visit([&](auto& value) {
            std::erase_if(value.outputs, removed);
            std::erase_if(value.inputs, removed);
        }, connection);
    }
}

void append_project_instance_duplicates(ProjectEditBatch& edit,
    std::unordered_map<std::string, ProjectNodeInstanceDeclaration const*> const& existing_instances,
    ProjectInstanceRecords const& instances, std::span<ProjectConnection const> connections,
    std::span<ProjectManualControl const> controls, std::span<ProjectLaneView const> lane_views)
{
    if (edit.instance_duplicates.empty()) return;
    std::unordered_map<std::string, std::string> copies;
    for (auto const& [source_instance_id, instance_id] : edit.instance_duplicates) {
        auto const source = existing_instances.find(source_instance_id);
        if (source == existing_instances.end())
            throw std::invalid_argument("unknown node instance id: " + source_instance_id);
        if (instance_id.empty()) throw std::invalid_argument("duplicate instance id must not be empty");
        if (!copies.emplace(source_instance_id, instance_id).second)
            throw std::invalid_argument("project batch duplicates the same node instance twice");
        auto instance = *source->second;
        instance.instance_id = instance_id;
        edit.require_absent_instances.push_back(instance_id);
        edit.instances.push_back({instance_id, std::move(instance)});
    }
    for (auto const& original : instances.replacements) {
        auto const copy = copies.find(original.address.instance_id);
        if (copy == copies.end()) continue;
        auto replacement = original;
        replacement.address.instance_id = copy->second;
        edit.replacements.push_back({replacement.address, std::move(replacement)});
    }
    for (auto const& original : controls) {
        auto const copy = copies.find(original.id.node.instance_id);
        if (copy == copies.end()) continue;
        auto control = original;
        control.id.node.instance_id = copy->second;
        edit.manual_controls.push_back({control.id, std::move(control)});
    }
    for (auto const& original : lane_views) {
        auto const copy = copies.find(original.id.address.instance_id);
        if (copy == copies.end()) continue;
        auto view = original;
        view.id.address.instance_id = copy->second;
        edit.lane_views.push_back({view.id, std::move(view)});
    }
    auto duplicate_matchers = [&](std::span<ProjectNodePortMatcher const> matchers) {
        std::vector<ProjectNodePortMatcher> result;
        for (auto const& original : matchers) {
            auto const copy = copies.find(original.instance_id);
            if (copy == copies.end()) continue;
            auto& matcher = result.emplace_back(original);
            matcher.instance_id = copy->second;
        }
        return result;
    };
    for (auto const& connection : connections) {
        std::visit([&](auto const& original) {
            auto outputs = duplicate_matchers(original.outputs);
            if (outputs.empty()) return;
            auto inputs = duplicate_matchers(original.inputs);
            if (inputs.empty()) return;
            auto copy = original;
            copy.connection_id = generate_uuid_v4().str();
            copy.outputs = std::move(outputs);
            copy.inputs = std::move(inputs);
            auto id = copy.connection_id;
            edit.connections.push_back({std::move(id), std::move(copy)});
        }, connection);
    }
}
bool is_module_lane_parent(ProjectGraphGeneration const& generation, NodeAddress const& address)
{
    auto const matches = resolve_node_address(*generation.graph, generation.placements, address);
    return matches.size() == 1 && matches.front().kind == ProjectNodePathMatch::Kind::bundle
        && generation.graph->node_bundles.bundle(matches.front().bundle).is_subgraph();
}
} // namespace

ProjectGraph::RebuildResult ProjectGraph::apply_edit_locked(
    ProjectEditBatch const& batch, bool record_history, std::optional<bool> history_undo,
    std::function<void()> const& authorize_audio)
{
    if (history_undo && !manual_interactions_.empty())
        throw std::runtime_error("finish the active manual-control interaction before editing project history");
    if (history_undo && (*history_undo ? history_position_ == 0 : history_position_ == edit_history_.size()))
        throw std::runtime_error(*history_undo ? "nothing to undo" : "nothing to redo");
    auto const* historical = history_undo
        ? &edit_history_[*history_undo ? history_position_ - 1 : history_position_] : nullptr;
    ProjectEditBatch edit = batch;
    bool const needs_audio = historical ? historical->audio_devices.has_value() : edit.audio_devices.has_value();
    bool const needs_toolchain = historical ? historical->toolchain.has_value() : edit.toolchain.has_value();
    if (needs_audio && authorize_audio) authorize_audio();
    AudioDeviceSelectionTransactionRequest audio;
    ProjectToolchainTransactionRequest toolchain;
    std::optional<ProjectAudioDeviceSelection> before_audio;
    std::optional<ProjectToolchainSettings> before_toolchain;
    if (needs_audio) {
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_audio_selection_requested_event, audio);
        if (!audio.handled) throw std::runtime_error("audio selection service is unavailable");
        before_audio = audio.transaction.record();
        if (historical) edit.audio_devices = project_history_setting(*before_audio, *historical->audio_devices, *history_undo);
    }
    if (needs_toolchain) {
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_toolchain_requested_event, toolchain);
        if (!toolchain.handled) throw std::runtime_error("toolchain settings service is unavailable");
        before_toolchain = toolchain.transaction.record();
        if (historical) edit.toolchain = project_history_setting(*before_toolchain, *historical->toolchain, *history_undo);
    }
    bool const needs_graph = historical
        ? !historical->instances.empty() || !historical->replacements.empty()
            || !historical->connections.empty() || !historical->manual_controls.empty()
        : !edit.instances.empty() || !edit.instance_updates.empty() || !edit.replacements.empty()
            || !edit.connections.empty() || !edit.manual_controls.empty() || !edit.instance_duplicates.empty();
    RebuildResult rebuilt;
    ProjectEditEffect effect;
    if (historical) {
        if (historical->lane_order)
            edit.lane_order = project_history_setting(presentation_.lane_order, *historical->lane_order, *history_undo);
        if (historical->lane_parents)
            edit.lane_parents = project_history_setting(presentation_.lane_parents, *historical->lane_parents, *history_undo);
        if (historical->timeline_snap_memory_json)
            edit.timeline_snap_memory_json = project_history_setting(
                presentation_.timeline_snap_memory_json, *historical->timeline_snap_memory_json, *history_undo);
        for (auto const& change : historical->lane_views) {
            auto const& desired = *history_undo ? change.before : change.after;
            auto const& other = *history_undo ? change.after : change.before;
            // Lane navigation can change a view after a duplication. Undo
            // still removes that lane and its current view.
            edit.lane_views.push_back({desired ? desired->id : other->id, desired});
        }
    }
    std::optional<ProjectPresentation> next_presentation;
    auto prepare_presentation = [&](std::unordered_set<std::string> const& remaining_instances) {
        auto valid_placement_instances = [&](ProjectLaneParent const& placement) {
            return remaining_instances.contains(placement.instance_id)
                && remaining_instances.contains(placement.parent.instance_id);
        };
        if (edit.lane_views.empty() && !edit.lane_order && !edit.lane_parents && !edit.timeline_snap_memory_json
            && std::ranges::all_of(presentation_.lane_parents, valid_placement_instances)
            && std::ranges::all_of(presentation_.lane_order, [&](auto const& address) {
                return remaining_instances.contains(address.instance_id);
            }) && std::ranges::all_of(presentation_.lane_views, [&](auto const& view) {
                return remaining_instances.contains(view.id.address.instance_id);
            })) return;
        auto& next = next_presentation.emplace(presentation_);
        for (auto const& view : edit.lane_views) {
            if (view.value && !remaining_instances.contains(view.key.address.instance_id))
                throw std::invalid_argument("lane view addresses an unknown instance");
        }
        if (edit.lane_order) {
            for (auto const& address : *edit.lane_order) {
                if (!remaining_instances.contains(address.instance_id))
                    throw std::invalid_argument("lane order addresses an unknown instance");
            }
            next.lane_order = *edit.lane_order;
        }
        if (edit.lane_parents) {
            validate_project_lane_parents(*edit.lane_parents);
            if (!std::ranges::all_of(*edit.lane_parents, valid_placement_instances))
                throw std::invalid_argument("lane nesting addresses an unknown project instance");
            if (!historical) {
                std::unordered_map<std::string_view, NodeAddress const*> old_parents;
                for (auto const& placement : presentation_.lane_parents)
                    old_parents.emplace(placement.instance_id, &placement.parent);
                for (auto const& placement : *edit.lane_parents) {
                    auto const old = old_parents.find(placement.instance_id);
                    if (old != old_parents.end() && *old->second == placement.parent) continue;
                    if (!current_generation_ || !current_generation_->graph
                        || !is_module_lane_parent(*current_generation_, placement.parent))
                        throw std::invalid_argument("lane nesting requires a module instance as its parent");
                }
            }
            next.lane_parents = *edit.lane_parents;
        }
        std::erase_if(next.lane_parents, [&](auto const& placement) { return !valid_placement_instances(placement); });
        if (edit.timeline_snap_memory_json) next.timeline_snap_memory_json = *edit.timeline_snap_memory_json;
        apply_project_record_edits(next.lane_views, edit.lane_views);
        std::erase_if(next.lane_order, [&](auto const& address) {
            return !remaining_instances.contains(address.instance_id);
        });
        std::erase_if(next.lane_views, [&](auto const& view) {
            return !remaining_instances.contains(view.id.address.instance_id);
        });
    };
    if (needs_graph) {
        InteractionTransactions transactions;
        open_transactions_locked(transactions);
        auto instances = transactions.node_instances.records();
        auto connections = transactions.graph_connections.records();
        auto controls = manual_controls_;
        if (history_undo) {
            bool const undo = *history_undo;
            auto const& effect = edit_history_[undo ? history_position_ - 1 : history_position_];
            edit.instances = project_history_edits<ProjectNodeInstanceDeclaration, std::string>(instances.instances, effect.instances, undo);
            edit.replacements = project_history_edits<ProjectNodeReplacement, NodeAddress>(instances.replacements, effect.replacements, undo);
            edit.connections = project_history_edits<ProjectConnection, std::string>(connections, effect.connections, undo);
            edit.manual_controls = project_history_edits<ProjectManualControl, ManualControlId>(controls, effect.manual_controls, undo);
        }
        std::unordered_map<std::string, ProjectNodeInstanceDeclaration const*> existing_instances;
        for (auto const& value : instances.instances) existing_instances.emplace(value.instance_id, &value);
        append_project_instance_duplicates(edit, existing_instances, instances, connections, controls, presentation_.lane_views);
        for (auto const& id : edit.require_absent_instances) {
            if (existing_instances.contains(id))
                throw std::invalid_argument("duplicate node instance id: " + id);
        }
        for (auto const& update : edit.instance_updates) {
            auto found = existing_instances.find(update.instance_id);
            if (found == existing_instances.end()) throw std::invalid_argument("unknown node instance id: " + update.instance_id);
            auto value = *found->second;
            if (update.display_name) {
                value.display_name = update.display_name->empty() ? std::nullopt : update.display_name;
            }
            if (update.configuration_source) value.configuration_source = *update.configuration_source;
            edit.instances.push_back({update.instance_id, std::move(value)});
        }
        auto const before_instances = instances;
        auto const before_connections = connections;
        apply_project_record_edits(instances.instances, edit.instances);
        apply_project_record_edits(instances.replacements, edit.replacements);
        apply_project_record_edits(connections, edit.connections);
        apply_project_record_edits(controls, edit.manual_controls);
        std::unordered_set<std::string> remaining_instances;
        for (auto const& value : instances.instances) remaining_instances.insert(value.instance_id);
        prepare_presentation(remaining_instances);
        std::unordered_set<std::string> removed_instances;
        for (auto const& value : before_instances.instances) {
            if (!remaining_instances.contains(value.instance_id)) removed_instances.insert(value.instance_id);
        }
        std::erase_if(instances.replacements, [&](auto const& value) { return removed_instances.contains(value.address.instance_id); });
        std::erase_if(controls, [&](auto const& value) { return removed_instances.contains(value.id.node.instance_id); });
        // History already supplies the exact connection records for this edit.
        if (!historical && !removed_instances.empty()) remove_deleted_instance_endpoints(connections, removed_instances);
        for (auto const& control : controls) {
            if (control.id.port_name.empty() || control.id.node.instance_id.empty())
                throw std::invalid_argument("project manual control requires a node address and input name");
            if (!remaining_instances.contains(control.id.node.instance_id))
                throw std::invalid_argument("project manual control addresses an unknown instance");
        }
        NodeInstanceReplaceAllMutation nodes{instances.instances, instances.replacements};
        GraphConnectionReplaceAllMutation wires{connections};
        GraphBuilder validation_builder;
        NodeInstancesProjectGraphRequest node_validation{
            .snapshot = definitions_snapshot_, .root_builder = &validation_builder,
            .mutation = nodes, .validate_only = true,
        };
        transactions.node_instances.run(node_validation);
        GraphConnectionsProjectGraphRequest connection_validation{
            .mutation = wires, .validate_only = true,
        };
        transactions.graph_connections.run(connection_validation);
        bool const changed = !project_record_changes(before_instances.instances, instances.instances).empty()
            || !project_record_changes(before_instances.replacements, instances.replacements).empty()
            || !project_record_changes(before_connections, connections).empty()
            || !project_record_changes(manual_controls_, controls).empty();
        if (changed) {
            rebuilt = rebuild_locked(std::move(nodes), std::move(wires), true, nullptr,
                &transactions, std::move(controls), false, &effect);
        }
    } else if (!edit.lane_views.empty() || edit.lane_order || edit.lane_parents || edit.timeline_snap_memory_json) {
        NodeInstancesProjectGraphRead instances{};
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_node_instances_read_event, instances);
        if (!instances.handled) throw std::runtime_error("project editing requires NodeInstances");
        std::unordered_set<std::string> remaining_instances;
        for (auto const& instance : instances.instances)
            remaining_instances.insert(instance.instance_id);
        prepare_presentation(remaining_instances);
    }
    if (next_presentation) {
        effect.lane_views = project_record_changes(presentation_.lane_views, next_presentation->lane_views);
        if (presentation_.lane_order != next_presentation->lane_order)
            effect.lane_order = {presentation_.lane_order, next_presentation->lane_order};
        if (presentation_.lane_parents != next_presentation->lane_parents)
            effect.lane_parents = {presentation_.lane_parents, next_presentation->lane_parents};
        if (presentation_.timeline_snap_memory_json != next_presentation->timeline_snap_memory_json)
            effect.timeline_snap_memory_json = {presentation_.timeline_snap_memory_json, next_presentation->timeline_snap_memory_json};
        presentation_ = std::move(*next_presentation);
    }
    // All record applicability and batch validation has succeeded. Settings
    // commits report activation failures as status, not rejected durable edits.
    if (edit.toolchain) {
        if (*before_toolchain != *edit.toolchain) {
            toolchain.transaction.commit(*edit.toolchain);
            effect.toolchain = ProjectRecordChange<ProjectToolchainSettings>{*before_toolchain, *edit.toolchain};
        }
    }
    if (edit.audio_devices) {
        rebuilt.audio_devices = audio.transaction.commit(*edit.audio_devices);
        if (*before_audio != *edit.audio_devices)
            effect.audio_devices = ProjectRecordChange<ProjectAudioDeviceSelection>{*before_audio, *edit.audio_devices};
    }
    rebuilt.state_changed = !effect.empty() || !!historical;
    if (record_history) {
        // Continuous view settings are saved without creating an undo entry.
        // Graph edits still record the views they create or remove.
        if (!needs_graph) effect.lane_views.clear();
        record_edit_locked(std::move(effect));
    }
    if (history_undo) {
        if (*history_undo) --history_position_;
        else ++history_position_;
    }
    return rebuilt;
}

void ProjectGraph::handle_project_apply_edit(ProjectEditBatch const& batch, ProjectAckBuilder& builder)
{
    bool changed = false;
    {
        std::scoped_lock lock(mutex_);
        auto rebuilt = apply_edit_locked(batch, true);
        changed = rebuilt.state_changed;
        builder.set_publication(std::move(rebuilt.publication));
    }
    builder.succeed();
    if (changed) IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_undo(ProjectAckBuilder& builder, std::function<void()> const& authorize_audio)
{
    {
        std::scoped_lock lock(mutex_);
        close_node_message_gestures_locked();
        auto rebuilt = apply_edit_locked({}, false, true, authorize_audio);
        builder.set_publication(std::move(rebuilt.publication));
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_redo(ProjectAckBuilder& builder, std::function<void()> const& authorize_audio)
{
    {
        std::scoped_lock lock(mutex_);
        close_node_message_gestures_locked();
        auto rebuilt = apply_edit_locked({}, false, false, authorize_audio);
        builder.set_publication(std::move(rebuilt.publication));
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
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
        builder.set_publication(std::move(rebuilt.publication));
    }
    builder.succeed(std::move(created_instance_id));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_delete_node_instance(
    ProjectDeleteNodeInstanceRequest const& request,
    ProjectAckBuilder& builder)
{
    ProjectEditBatch batch;
    batch.instances.push_back({request.instance_id, std::nullopt});
    handle_project_apply_edit(batch, builder);
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
        auto rebuilt = rebuild_locked(std::move(mutation), std::monostate{});
        builder.set_publication(std::move(rebuilt.publication));
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
        auto rebuilt = rebuild_locked(
            std::monostate{},
            GraphConnectionUpsertMutation{.connection = request.connection});
        builder.set_publication(std::move(rebuilt.publication));
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
        auto rebuilt = rebuild_locked(
            std::monostate{},
            GraphConnectionDeleteMutation{.connection_id = request.connection_id});
        builder.set_publication(std::move(rebuilt.publication));
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
        if (control.id.port_name.empty()) {
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
        auto controls = request.manual_controls;
        for (auto& control : controls)
            if (control.id.kind == ManualControlKind::output_gain) control.enabled = true;
        auto presentation = request.presentation;
        validate_project_lane_parents(presentation.lane_parents);
        std::erase_if(presentation.lane_parents, [&](auto const& placement) {
            return !instance_ids.contains(placement.instance_id) || !instance_ids.contains(placement.parent.instance_id);
        });
        std::erase_if(presentation.lane_order, [&](auto const& address) {
            return !instance_ids.contains(address.instance_id);
        });
        std::erase_if(presentation.lane_views, [&](auto const& view) {
            return !instance_ids.contains(view.id.address.instance_id);
        });
        auto rebuilt = rebuild_locked(
            NodeInstanceReplaceAllMutation{
                .instances = request.instances,
                .node_replacements = request.node_replacements,
            },
            GraphConnectionReplaceAllMutation{
                .connections = request.connections,
            }, true, nullptr, nullptr, std::move(controls), false);
        presentation_ = std::move(presentation);
        edit_history_.clear();
        node_message_gestures_.clear();
        history_position_ = 0;
        ++history_serial_;
        builder.set_publication(std::move(rebuilt.publication));
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
    if (request.control.id.port_name.empty()) {
        throw std::invalid_argument(
            "project manual control input name must not be empty");
    }
    {
        std::scoped_lock lock(mutex_);
        auto controls = manual_controls_;
        auto found = std::ranges::find(
            controls, request.control.id, &ProjectManualControl::id);
        auto control = request.control;
        if (control.id.kind == ManualControlKind::output_gain) control.enabled = true;
        if (found == controls.end()) {
            controls.push_back(std::move(control));
        } else {
            *found = std::move(control);
        }
        auto rebuilt = rebuild_locked(std::monostate{}, std::monostate{}, true, nullptr, nullptr, std::move(controls));
        builder.set_publication(std::move(rebuilt.publication));
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
        auto controls = manual_controls_;
        std::erase_if(controls, [&](ProjectManualControl const& control) {
            return control.id == request.id;
        });
        auto rebuilt = rebuild_locked(std::monostate{}, std::monostate{}, true, nullptr, nullptr, std::move(controls));
        builder.set_publication(std::move(rebuilt.publication));
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
            if (!current_generation_ || !current_generation_->graph
                || !manual_control_is_available(
                    *current_generation_->graph, *request.id)) {
                throw std::invalid_argument(
                    "manual-control hover names an unavailable control");
            }
            hovered_controls_.insert_or_assign(request.client_id, *request.id);
        } else {
            hovered_controls_.erase(request.client_id);
        }
        auto retained = hover_retained_inputs_.find(request.client_id);
        if (retained != hover_retained_inputs_.end() && (!request.id || retained->second != *request.id)) {
            hover_retained_inputs_.erase(retained);
            reconcile_manual_control_activity_locked(true);
        } else if (!settling_controls_) {
            auto const requested_inputs = desired_dynamic_inputs_locked(true);
            if (!requested_inputs.empty()) {
                schedule_control_compile_locked(
                    requested_inputs, !dynamic_inputs_.empty());
            } else {
                // A hover-only artifact must never become current after its
                // hover ended. The active executable is unchanged.
                cancel_pending_control_compile_locked();
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
        if (!current_generation_ || !current_generation_->graph
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
        manual_interactions_.push_back({
            .interaction = request.interaction,
            .control = request.id,
            .initial_control = control ? std::optional(*control) : std::nullopt,
        });
        hover_retained_inputs_.erase(request.interaction.client_id);
        if (settled_preparation_client_ == request.interaction.client_id) settled_preparation_client_.reset();
        // The gesture changes no durable state until it ends. Its live value
        // starts here and follows the client's updates.
        if (settling_controls_) {
            cancel_pending_control_compile_locked();
            ready_control_code_.reset();
            latest_control_compile_request_ = next_control_compile_request_++;
        }
        settling_controls_ = false;
        dynamic_inputs_ = desired_dynamic_inputs_locked(false);
        auto const requested_inputs = desired_dynamic_inputs_locked(true);
        std::shared_ptr<ManualControlLiveTarget> live_target;
        auto const staged = update_graph_executor_controls_locked(
            {ManualControlTargetValue{
                .id = request.id,
                .value = request.value,
                .gesture = ManualControlGesturePhase::begin,
            }},
            requested_inputs.empty()
                ? std::nullopt
                : std::optional(std::span<ManualControlId const>(requested_inputs)),
            &live_target);
        auto const requires_preparation = std::ranges::any_of(
            current_generation_->graph->manual_controls, [&](auto const& configured) {
                return configured.id == request.id && configured.requires_preparation;
            });
        {
            std::scoped_lock live(live_mutex_);
            live_interactions_.push_back({
                .interaction = request.interaction,
                .control = request.id,
                .target = std::move(live_target),
                .value = request.value,
                .requires_preparation = requires_preparation,
            });
        }
        if (!requested_inputs.empty() && (!staged || requires_preparation)) {
            schedule_control_compile_locked(requested_inputs, true);
        }
    }
    builder.succeed();
}

void ProjectGraph::handle_live_manual_control_update(LiveManualControlUpdateRequest& request)
{
    bool prepare = false;
    {
        std::scoped_lock live(live_mutex_);
        auto interaction = std::ranges::find(
            live_interactions_, request.interaction,
            &LiveManualControlInteraction::interaction);
        // A late update for a gesture that already ended or was invalidated is
        // dropped: its end carried the authoritative value.
        if (interaction == live_interactions_.end()) return;
        interaction->value = request.value;
        prepare = interaction->requires_preparation;
        if (interaction->target && !prepare) {
            interaction->target->target_bits.store(
                std::bit_cast<std::uint32_t>(request.value.value),
                std::memory_order_relaxed);
        }
        request.applied = true;
        request.control = interaction->control;
    }
    if (prepare) {
        std::scoped_lock lock(mutex_);
        // The fast transport still rejects updates after end. A structural
        // edit or gesture completion between the two locks invalidates it.
        if (!std::ranges::any_of(manual_interactions_, [&](auto const& entry) { return entry.interaction == request.interaction; })) return;
        schedule_control_compile_locked(desired_dynamic_inputs_locked(true), true);
    }
}

std::optional<ProjectGraph::LiveManualControlInteraction>
ProjectGraph::close_live_interaction_locked(ManualControlInteractionId const& interaction)
{
    std::scoped_lock live(live_mutex_);
    auto found = std::ranges::find(
        live_interactions_, interaction, &LiveManualControlInteraction::interaction);
    if (found == live_interactions_.end()) return std::nullopt;
    auto closed = std::move(*found);
    live_interactions_.erase(found);
    return closed;
}

void ProjectGraph::prune_live_interactions_locked()
{
    std::scoped_lock live(live_mutex_);
    std::erase_if(live_interactions_, [&](LiveManualControlInteraction& entry) {
        if (std::ranges::contains(manual_interactions_, entry.interaction,
                &ActiveManualControlInteraction::interaction)) {
            return false;
        }
        if (entry.target) {
            auto open = entry.target->open_gestures.load(std::memory_order_relaxed);
            while (open != 0 && !entry.target->open_gestures.compare_exchange_weak(
                       open, open - 1, std::memory_order_release,
                       std::memory_order_relaxed)) {}
        }
        return true;
    });
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
        // The end value is the gesture's one durable change and history entry,
        // appended in the order the gesture completes.
        set_interaction_value_locked(*interaction, request.value);
        auto const live = close_live_interaction_locked(request.interaction);
        auto const settled_dynamic_input = std::ranges::contains(
            dynamic_inputs_, control_id);
        auto const hover = hovered_controls_.find(interaction->interaction.client_id);
        if (hover != hovered_controls_.end() && hover->second == control_id) {
            hover_retained_inputs_.insert_or_assign(interaction->interaction.client_id, control_id);
            settled_preparation_client_ = interaction->interaction.client_id;
        }
        manual_interactions_.erase(interaction);
        reconcile_manual_control_activity_locked(
            settled_dynamic_input,
            {ManualControlTargetValue{
                .id = control_id,
                .value = request.value,
                .gesture = live && live->target
                    ? ManualControlGesturePhase::end : ManualControlGesturePhase::none,
            }});
        prepare_released_control_locked();
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder& builder) const
{
    std::scoped_lock lock(mutex_);
    InteractionTransactions transactions;
    open_transactions_locked(transactions);
    auto instances = transactions.node_instances.records();
    builder.add_node_instances(std::move(instances.instances));
    builder.add_node_replacements(std::move(instances.replacements));
    builder.add_graph_connections(transactions.graph_connections.records());
    builder.add_manual_controls(manual_controls_);
    builder.add_project_presentation(presentation_);
}

void ProjectGraph::handle_socket_rpc_set_audio_devices(SetAudioDevicesRequest const& request,
    SocketRpcAudioDevicesResultBuilder& builder)
{
    try {
        std::scoped_lock lock(mutex_);
        ProjectEditBatch edit;
        edit.audio_devices = ProjectAudioDeviceSelection{request.output_device_id, request.input_device_id};
        auto result = apply_edit_locked(edit, true);
        builder.succeed(std::move(*result.audio_devices));
    } catch (std::exception const& error) { builder.fail(error.what()); return; }
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_set_audio_devices(ProjectSetAudioDevicesRequest const& request,
    ProjectAudioDevicesBuilder& builder)
{
    {
        std::scoped_lock lock(mutex_);
        ProjectEditBatch edit;
        edit.audio_devices = ProjectAudioDeviceSelection{request.output_device_id, request.input_device_id};
        auto result = apply_edit_locked(edit, true);
        builder.succeed(std::move(*result.audio_devices));
    }
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_socket_rpc_set_project_toolchain(SetProjectToolchainRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectEditBatch edit;
        edit.toolchain = request.settings;
        ProjectAckBuilder project_builder;
        handle_project_apply_edit(edit, project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) { builder.fail(error.what()); }
}

void ProjectGraph::handle_socket_rpc_apply_project_edit(
    ApplyProjectEditRequest const& request, SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_apply_edit(request.batch, project_builder);
        builder.set_publication(project_builder.take_publication());
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) { builder.fail(error.what()); }
}

void ProjectGraph::handle_socket_rpc_undo_project_edit(
    UndoProjectEditRequest const& request, SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_undo(project_builder, request.authorize_audio_control);
        builder.set_publication(project_builder.take_publication());
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) { builder.fail(error.what()); }
}

void ProjectGraph::handle_socket_rpc_redo_project_edit(
    RedoProjectEditRequest const& request, SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_redo(project_builder, request.authorize_audio_control);
        builder.set_publication(project_builder.take_publication());
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) { builder.fail(error.what()); }
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
        builder.set_publication(project_builder.take_publication());
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
        builder.set_publication(project_builder.take_publication());
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
        builder.set_publication(project_builder.take_publication());
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
        builder.set_publication(project_builder.take_publication());
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
        builder.set_publication(project_builder.take_publication());
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
        builder.set_publication(project_builder.take_publication());
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
        builder.set_publication(project_builder.take_publication());
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
        builder.set_publication(project_builder.take_publication());
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
        builder.set_publication(project_builder.take_publication());
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
        builder.set_publication(project_builder.take_publication());
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_client_disconnected(
    SocketRpcClientDisconnectedRequest& request)
{
    auto const& client_id = request.client_id;
    if (client_id.empty()) return;
    {
        std::scoped_lock lock(mutex_);
        auto const hover_count = hovered_controls_.size();
        auto const controls_before = manual_controls_;
        auto settled_dynamic_input = false;
        std::vector<ManualControlTargetValue> ended;
        // Each open gesture ends at the last value received from its client,
        // exactly as an explicit end would, so its history entry is ordinary.
        for (auto interaction = manual_interactions_.begin();
             interaction != manual_interactions_.end();) {
            if (interaction->interaction.client_id != client_id) {
                ++interaction;
                continue;
            }
            if (auto const live = close_live_interaction_locked(interaction->interaction)) {
                // A control disabled since the gesture began keeps its record;
                // the gesture still closes its live target at the received value.
                try {
                    set_interaction_value_locked(*interaction, live->value);
                } catch (std::invalid_argument const&) {}
                ended.push_back({
                    .id = interaction->control,
                    .value = live->value,
                    .gesture = live->target
                        ? ManualControlGesturePhase::end : ManualControlGesturePhase::none,
                });
            }
            settled_dynamic_input |= std::ranges::contains(
                dynamic_inputs_, interaction->control);
            interaction = manual_interactions_.erase(interaction);
        }
        hovered_controls_.erase(client_id);
        auto const retained_removed = hover_retained_inputs_.erase(client_id) != 0;
        settled_dynamic_input |= retained_removed;
        if (!ended.empty() || settled_dynamic_input
            || hovered_controls_.size() != hover_count) {
            reconcile_manual_control_activity_locked(
                settled_dynamic_input, std::move(ended));
        }
        request.project_state_changed = close_node_message_gestures_locked(client_id) || manual_controls_ != controls_before;
    }
    if (request.project_state_changed) {
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
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
                .gesture = request.gesture,
            });
        builder.set_publication(std::move(result.publication));
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

void ProjectGraph::handle_socket_rpc_node_message_gesture(NodeMessageGestureRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        if (request.id.client_id.empty() || request.id.interaction_id.empty())
            throw std::invalid_argument("node-message gesture requires a client and interaction id");
        bool changed = false;
        {
            std::scoped_lock lock(mutex_);
            auto open = std::ranges::find(node_message_gestures_, request.id, &NodeMessageGesture::id);
            if (request.begin) {
                if (open != node_message_gestures_.end()) throw std::invalid_argument("node-message gesture is already open");
                if (!current_generation_ || std::ranges::none_of(current_generation_->message_handlers,
                        [&](auto const& handler) { return handler.address == request.target; }))
                    throw std::invalid_argument("node-message gesture target is unavailable");
                node_message_gestures_.push_back({.id = request.id, .target = request.target});
            } else if (open != node_message_gestures_.end()) {
                if (open->target != request.target) throw std::invalid_argument("node-message gesture target changed");
                changed = !open->effect.empty();
                append_history_effect_locked(std::move(open->effect));
                node_message_gestures_.erase(open);
            }
        }
        builder.succeed();
        if (changed) IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
    } catch (std::exception const& error) { builder.fail(error.what()); }
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

namespace {
void classify_scope_node(
    ProjectGraphScopeNode& node,
    bool definition_available,
    IvPackageInfo const* package,
    bool publication_pending)
{
    using State = ProjectGraphScopeNodeState;
    if (package && package->build_state == PackageBuildState::failed) {
        node.load_state = State::failed;
        if (!node.error_message.empty()) node.error_message += '\n';
        node.error_message += package->build_message.empty() ? "Package build failed" : package->build_message;
    } else if (!definition_available) {
        if (package && package->build_state == PackageBuildState::built && !publication_pending) {
            node.load_state = State::failed;
            if (node.error_message.empty()) node.error_message = package->publication_message.empty()
                ? "The built package did not publish definition '" + node.instance.definition_id + "'"
                : package->publication_message;
        } else {
            // A missing definition before a package has completed is pending,
            // even though startup replay produced an availability diagnostic.
            node.load_state = State::waiting_for_definition;
        }
    } else if (!node.error_message.empty()) {
        node.load_state = State::failed;
    } else {
        node.load_state = node.instance.realized ? State::ready : State::waiting_for_graph;
    }
}

struct SourceScopeOutputAliases {
    std::map<std::tuple<NodeBundleHandle, std::size_t, std::size_t>, std::vector<ProjectNodePortMatcher>> sample;
    std::map<std::pair<NodeBundleHandle, std::size_t>, std::vector<ProjectNodePortMatcher>> event;
};

std::string unqualified_child_type(std::string_view type)
{
    constexpr std::string_view anonymous_namespace = "(anonymous namespace)::";
    std::string label;
    while (!type.empty()) {
        if (type.starts_with(anonymous_namespace)) type.remove_prefix(anonymous_namespace.size());
        auto const end = type.find_first_of("<>, &*()");
        auto const word = type.substr(0, end);
        auto const qualifier = word.find_last_of(":.");
        label += qualifier == std::string_view::npos ? word : word.substr(qualifier + 1);
        if (end == std::string_view::npos) break;
        label += type[end];
        type.remove_prefix(end + 1);
    }
    return label.empty() ? "Node" : label;
}

std::string child_lane_display_name(
    ProjectGraphGeneration const& generation, ProjectChildNode const& child,
    std::string_view definition_id,
    std::unordered_map<std::string_view, std::string_view> const& leaf_names_by_type)
{
    auto const& graph = *generation.graph;
    if (child.symbolic_node) {
        auto const& record = graph.symbolic_nodes.record(*child.symbolic_node);
        if (record.type_identity == "sample-port") return "Sample Port";
        if (record.type_identity == "event-port") return "Event Port";
    }

    auto const& bundle = graph.node_bundles.bundle(child.bundle);
    auto const type = bundle.is_subgraph() ? bundle.subgraph_kind() : bundle.type_identity();
    if (generation.definitions) {
        if (!definition_id.empty()) {
            auto const it = generation.definitions->by_id.find(std::string(definition_id));
            if (it != generation.definitions->by_id.end()) {
                auto const& name = std::visit([](auto const& definition) -> std::string const& {
                    return definition.display_name;
                }, it->second.definition);
                if (!name.empty()) return name;
            }
        } else if (auto const it = leaf_names_by_type.find(type);
                   it != leaf_names_by_type.end() && !it->second.empty()) {
            return std::string(it->second);
        }
    }
    return unqualified_child_type(type);
}

void append_source_scope_connections(ProjectGraphScope& scope,
    ProjectGraphGeneration const& generation,
    std::unordered_map<NodeBundleHandle, std::vector<NodeAddress>> const& children,
    std::unordered_map<NodeBundleHandle, std::vector<NodeAddress>> const& boundaries,
    SourceScopeOutputAliases const& aliases)
{
    auto const& connections = generation.graph->connections;
    std::unordered_set<std::string> ids;
    for (auto const& entry : scope.connections) ids.insert(project_connection_id(entry.connection));
    auto append = [&](auto connection, std::string id) {
        if (connection.inputs.empty() || connection.outputs.empty()) return;
        while (!ids.insert(id).second) id.push_back('_');
        connection.connection_id = std::move(id);
        scope.connections.push_back({
            .connection = std::move(connection), .applied = true, .source_owned = true,
        });
    };
    auto add_port = [](auto port, auto const& addresses, std::vector<ProjectNodePortMatcher>& endpoints,
                       bool module_boundary = false) {
        auto const found = addresses.find(port.bundle);
        if (found == addresses.end()) return;
        for (auto const& address : found->second) {
            ProjectNodePortMatcher matcher{
                .instance_id = address.instance_id, .path = address.path, .port = {.index = port.port},
                .module_boundary = module_boundary,
            };
            if (!std::ranges::contains(endpoints, matcher)) endpoints.push_back(std::move(matcher));
        }
    };
    auto add_aliases = [](auto const& index, auto key, std::vector<ProjectNodePortMatcher>& endpoints) {
        auto const found = index.find(key);
        if (found == index.end()) return;
        for (auto const& matcher : found->second)
            if (!std::ranges::contains(endpoints, matcher)) endpoints.push_back(matcher);
    };
    auto const samples = connections.configured_sample_connections();
    for (std::size_t index = 0; index < generation.source_sample_connection_count; ++index) {
        auto const& wiring = samples[index];
        ProjectSampleConnection connection{.source_type = wiring.source_type, .target_type = wiring.target_type};
        for (auto const& source : wiring.source_channels) {
            add_port(source.channel, children, connection.outputs);
            add_port(source.channel, boundaries, connection.outputs, true);
            add_aliases(aliases.sample, std::tuple{source.channel.bundle, source.channel.port, source.channel.channel},
                connection.outputs);
        }
        for (auto const target : wiring.target_channels) {
            add_port(target, children, connection.inputs);
            add_port(target, boundaries, connection.inputs, true);
        }
        append(std::move(connection), "source:sample:" + std::to_string(index));
    }
    auto const events = connections.configured_event_connections();
    for (std::size_t index = 0; index < generation.source_event_connection_count; ++index) {
        auto const& wiring = events[index];
        ProjectEventConnection connection{.source_type = wiring.source_type, .target_type = wiring.target_type};
        for (auto const& source : wiring.sources) {
            add_port(source.port, children, connection.outputs);
            add_port(source.port, boundaries, connection.outputs, true);
            add_aliases(aliases.event, std::pair{source.port.bundle, source.port.port}, connection.outputs);
        }
        for (auto const target : wiring.targets) {
            add_port(target, children, connection.inputs);
            add_port(target, boundaries, connection.inputs, true);
        }
        append(std::move(connection), "source:event:" + std::to_string(index));
    }
}
} // namespace

void ProjectGraph::handle_socket_rpc_get_graph_scope(
    GetGraphScopeRequest const& request,
    SocketRpcGraphScopeResultBuilder& builder)
{
    try {
        // Every project mutation also holds this lock while it changes
        // NodeInstances and GraphConnections, so the three reads below
        // describe one revision.
        std::scoped_lock lock(mutex_);
        NodeInstancesProjectGraphRead instances{};
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_node_instances_read_event, instances);
        GraphConnectionsProjectGraphRead connections{};
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_connections_read_event, connections);
        PackageDefinitionsProjectGraphRead packages{};
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_packages_read_event, packages);
        if (!instances.handled || !connections.handled || !packages.handled) {
            builder.fail("the graph scope needs NodeInstances, GraphConnections, and PackageDefinitions");
            return;
        }

        ProjectGraphScope scope;
        scope.presentation = presentation_;
        auto const generation = current_generation_;
        if (generation && generation->graph) {
            std::unordered_set<NodeAddress, ProjectRecordKeyHash> seen;
            for (auto const& placement : presentation_.lane_parents) {
                if (seen.insert(placement.parent).second && !is_module_lane_parent(*generation, placement.parent))
                    scope.unavailable_lane_parents.push_back(placement.parent);
            }
        }
        if (generation) scope.generation = generation->configured_generation;
        scope.sample_rate = generation && generation->compiled_graph
            ? generation->compiled_graph->specialization.sample_rate : sample_rate_;
        scope.execution_phase = configuration_failure_ ? "configurationFailed"
            : generation ? generation->execution_phase : "unconfigured";
        scope.execution_message = generation ? generation->execution_message : std::string{};
        if (accepted_execution_graph_) scope.prepared_generation = accepted_execution_graph_->configured_generation
            .value_or(accepted_execution_graph_->project_generation);
        GraphExecutorExecutionReadRequest execution;
        IV_INVOKE_LINKER_EVENT(iv_runtime_project_graph_execution_read_event, execution);
        scope.active_generation = execution.active_generation;
        std::unordered_map<std::string, IvPackageInfo const*> packages_by_root;
        std::unordered_map<std::string, IvPackageInfo const*> packages_by_definition;
        for (auto const& package : packages.packages) {
            if (!package.package_root.empty()) packages_by_root.emplace(package.package_root.lexically_normal().generic_string(), &package);
            for (auto const& id : package.module_ids) packages_by_definition.emplace(id, &package);
            for (auto const& id : package.node_type_ids) packages_by_definition.emplace(id, &package);
        }
        std::unordered_map<std::string, std::string> instance_errors;
        if (configuration_failure_) {
            for (auto const& diagnostic : configuration_failure_->node_instance_diagnostics) {
                auto& message = instance_errors[diagnostic.instance_id];
                if (!message.empty()) message += '\n';
                message += diagnostic.message;
            }
            if (auto const& prepared = configuration_failure_->instance_interfaces) {
                for (auto const& diagnostic : prepared->diagnostics) {
                    instance_errors.try_emplace(diagnostic.instance_id, diagnostic.message);
                }
            }
        }
        auto describe_manual_controls = [&](ProjectGraphScopeNode& node) {
            auto describe = [&](NodeInstancePortInfo const& port, ManualControlKind kind) {
                ManualControlId id{.node = node.address, .port_name = port.name, .kind = kind};
                ConfiguredManualControl const* configured = nullptr;
                if (generation && generation->graph) {
                    auto const& controls = generation->graph->manual_controls;
                    auto const found = std::ranges::find(controls, id, &ConfiguredManualControl::id);
                    if (found != controls.end()) configured = std::addressof(*found);
                }
                if (!configured && (!node.address.path.empty() || kind == ManualControlKind::output_gain
                        || !port.manual_defaults)) return;
                auto const* saved = find_manual_control_locked(id);
                std::optional<Sample> live_value;
                {
                    std::scoped_lock live(live_mutex_);
                    for (auto const& entry : live_interactions_ | std::views::reverse)
                        if (entry.control == id) { live_value = entry.value; break; }
                }
                bool const available = configured && node.load_state == ProjectGraphScopeNodeState::ready;
                bool const enabled = kind == ManualControlKind::output_gain
                    || (saved ? saved->enabled : configured ? configured->enabled : port.manual_defaults->enabled);
                auto const value = kind == ManualControlKind::output_gain ? Sample{1}
                    : port.manual_defaults ? port.manual_defaults->value : configured->settled_value;
                node.manual_controls.push_back({
                    .control = saved ? *saved : ProjectManualControl{.id = std::move(id), .stored_value = value, .enabled = enabled},
                    .available = available, .participates = available && enabled, .live_value = live_value,
                });
                node.manual_controls.back().control.enabled = enabled;
            };
            for (auto const& port : node.interface.sample_inputs) describe(port, ManualControlKind::input);
            for (auto const& port : node.interface.sample_outputs) describe(port, ManualControlKind::output_gain);
        };
        std::unordered_map<NodeBundleHandle, std::vector<NodeAddress>> expanded_children;
        std::unordered_map<NodeBundleHandle, std::vector<NodeAddress>> expanded_boundaries;
        SourceScopeOutputAliases expanded_aliases;
        std::unordered_map<std::string_view, std::string_view> leaf_names_by_type;
        if (!request.expanded.empty() && generation && generation->graph && generation->definitions) {
            for (auto const& [id, entry] : generation->definitions->by_id) {
                auto const* leaf = std::get_if<LeafNodeDefinition>(&entry.definition);
                if (!leaf || leaf->display_name.empty() || !leaf->compiler_record.type_name
                    || leaf->compiler_record.type_name_size == 0) continue;
                std::string_view const type(leaf->compiler_record.type_name, leaf->compiler_record.type_name_size);
                auto const [it, inserted] = leaf_names_by_type.try_emplace(type, leaf->display_name);
                // A type created directly by C++ has no specific registration.
                // Use the C++ name when registrations disagree on its display name.
                if (!inserted && it->second != leaf->display_name) it->second = {};
            }
        }
        for (auto const& address : request.expanded) {
            if (!generation || !generation->graph) break;
            auto const owner = std::ranges::find(instances.instances,
                address.instance_id, &IvModuleInstanceInfo::instance_id);
            if (owner == instances.instances.end()) continue;
            auto const& graph = *generation->graph;
            auto const matches = resolve_node_address(graph, generation->placements, address);
            if (matches.size() != 1) continue;
            auto& child_scope = scope.child_scopes.emplace_back(ProjectGraphChildScope{.subject = address});
            if (matches.front().kind == ProjectNodePathMatch::Kind::bundle) {
                if (auto const boundary = graph.node_bundles.bundle(matches.front().bundle).subgraph_boundary_handle()) {
                    child_scope.has_boundary = true;
                    expanded_boundaries[*boundary].push_back(address);
                }
            }
            for (auto const& child : project_child_nodes(graph, matches.front())) {
                auto child_address = address;
                child_address.path.insert(child_address.path.end(), child.path.begin(), child.path.end());
                std::optional<NodeAddress> membership_address;
                std::optional<std::size_t> symbolic_member_count;
                std::optional<ProjectGraphScopeSource> declaration_source;
                if (!child.symbolic_node) {
                    auto const placement = generation->placements.find(address.instance_id);
                    if (placement != generation->placements.end()) {
                        auto path = project_member_path(graph, placement->second.root, child.bundle);
                        if (path && *path != child_address.path) {
                            membership_address = child_address;
                            child_address.path = std::move(*path);
                        }
                    }
                    auto& addresses = expanded_children[child.bundle];
                    if (!std::ranges::contains(addresses, child_address))
                        addresses.insert(addresses.begin(), child_address);
                } else {
                    auto const& record = graph.symbolic_nodes.record(*child.symbolic_node);
                    auto const declaration = std::ranges::find_if(record.source_infos,
                        [](SourceInfo const& info) {
                            return info.declaration_line != 0 && !info.span.file_path.empty();
                        });
                    if (declaration != record.source_infos.end()) {
                        declaration_source = ProjectGraphScopeSource{
                            .file_path = declaration->span.file_path,
                            .line = declaration->declaration_line,
                            .byte_offset = declaration->span.begin,
                        };
                    }
                    auto const selected = resolve_node_address(graph, generation->placements, child_address);
                    if (selected.size() == 1) {
                        auto const& members = selected.front().symbolic_member_indices;
                        symbolic_member_count = members.size();
                        if (record.type_identity == "sample-port" || record.type_identity == "event-port") {
                            auto includes_bundle = [&](NodeBundleHandle bundle) {
                                return std::ranges::any_of(members, [&](auto index) { return record.node_bundle_handles[index] == bundle; });
                            };
                            auto matcher = [&](std::size_t port_index) {
                                return ProjectNodePortMatcher{.instance_id = child_address.instance_id,
                                    .path = child_address.path, .port = {.index = port_index}};
                            };
                            for (auto const& output : record.sample_outputs) for (auto const channel : output.channels) {
                                if (includes_bundle(channel.bundle))
                                    expanded_aliases.sample[{channel.bundle, channel.port, channel.channel}].push_back(matcher(output.index));
                            }
                            for (auto const& output : record.event_outputs) for (auto const port : output.node_bundle_ports) {
                                if (includes_bundle(port.node_bundle_handle))
                                    expanded_aliases.event[{port.node_bundle_handle, port.port_index}].push_back(matcher(output.index));
                            }
                        } else {
                            // NodeRef mappings preserve their member port indexes.
                            for (auto const index : members)
                                expanded_children[record.node_bundle_handles[index]].push_back(child_address);
                        }
                    }
                }
                auto const& bundle = graph.node_bundles.bundle(child.bundle);
                auto const* registered = graph.node_bundles.registered_node_type_identity(child.bundle);
                auto const definition_id = bundle.is_subgraph() ? bundle.subgraph_kind()
                    : registered ? std::string_view(registered->node_type_id) : std::string_view{};
                auto interface = child.symbolic_node
                    ? read_symbolic_node_interface(graph.node_bundles, graph.symbolic_nodes.record(*child.symbolic_node), child.bundle)
                    : read_node_instance_interface(graph.node_bundles, child.bundle);
                interface.has_children = child.symbolic_node || interface.has_children || bundle.is_tiled();
                auto& node = child_scope.nodes.emplace_back(ProjectGraphScopeNode{
                    .address = std::move(child_address),
                    .membership_address = std::move(membership_address),
                    .symbolic_node = child.symbolic_node.has_value(),
                    .symbolic_member_count = symbolic_member_count,
                    .instance = *owner,
                    .child_display_name = child_lane_display_name(*generation, child, definition_id, leaf_names_by_type),
                    .child_source_label = child.source_label,
                    .declaration_source = std::move(declaration_source),
                    .load_state = ProjectGraphScopeNodeState::ready,
                    .interface = std::move(interface),
                    .construction_definition_id = std::string(definition_id.empty()
                        ? bundle.type_identity() : definition_id),
                });
                describe_manual_controls(node);
            }
        }
        for (auto& instance : instances.instances) {
            ProjectGraphScopeNode node{
                .address = NodeAddress{.instance_id = instance.instance_id},
            };
            if (auto error = instance_errors.find(instance.instance_id); error != instance_errors.end()) {
                node.error_message = std::move(error->second);
            }
            if (configuration_failure_ && configuration_failure_->instance_interfaces) {
                auto const& prepared = *configuration_failure_->instance_interfaces;
                if (auto found = prepared.interfaces.find(instance.instance_id);
                    found != prepared.interfaces.end()) {
                    node.interface = found->second;
                }
            } else if (generation && generation->graph) {
                auto const placement = generation->placements.find(instance.instance_id);
                if (placement != generation->placements.end()) {
                    node.interface = read_node_instance_interface(
                        generation->graph->node_bundles, placement->second.root);
                    node.interface.has_children = node.interface.has_children
                        || generation->graph->node_bundles.bundle(placement->second.root).is_tiled();
                }
            }
            node.instance = std::move(instance);
            if (generation) {
                auto const placement = generation->placements.find(node.instance.instance_id);
                if (placement != generation->placements.end() && placement->second.configured) {
                    auto const& configured = *placement->second.configured;
                    node.construction_definition_id = configured.definition_id;
                    if (configured.read_construction_arguments) {
                        auto values = configured.read_construction_arguments();
                        node.construction_arguments_json = std::move(values.json);
                        node.construction_error = std::move(values.error);
                    } else node.construction_error = "accepted construction values are unavailable";
                }
            }
            IvPackageInfo const* package = nullptr;
            if (auto found = packages_by_definition.find(node.instance.definition_id); found != packages_by_definition.end()) {
                package = found->second;
            } else if (auto found = packages_by_root.find(node.instance.package_root.lexically_normal().generic_string());
                       found != packages_by_root.end()) {
                package = found->second;
            }
            if (!package) {
                for (auto const& candidate : packages.packages)
                    if (!candidate.package_namespace.empty()
                        && package_namespace_owns(candidate.package_namespace, node.instance.definition_id)
                        && (!package || candidate.package_namespace.size() > package->package_namespace.size()))
                        package = &candidate;
            }
            classify_scope_node(node, definitions_snapshot_ && definitions_snapshot_->by_id.contains(node.instance.definition_id),
                package, packages.publication_pending);
            describe_manual_controls(node);
            scope.nodes.push_back(std::move(node));
        }
        if (generation && !configuration_failure_) {
            auto const& graph = *generation->graph;
            for (NodeBundleHandle bundle = 0; bundle < graph.node_bundles.size(); ++bundle) {
                auto const& candidate = graph.node_bundles.bundle(bundle);
                if (!candidate.is_subgraph() || candidate.subgraph_kind() != "builtin.timeline") continue;
                for (auto const& [instance, placement] : generation->placements) {
                    auto path = project_member_path(graph, placement.root, bundle);
                    if (!path) continue;
                    scope.timeline_nodes.push_back({instance, std::move(*path)});
                    break;
                }
            }
        }
        std::unordered_set<std::string> applied;
        if (generation) {
            applied.insert(
                generation->applied_connection_ids.begin(),
                generation->applied_connection_ids.end());
        }
        for (auto& connection : connections.connections) {
            auto const is_applied = applied.contains(project_connection_id(connection));
            scope.connections.push_back(ProjectGraphScopeConnection{
                .connection = std::move(connection),
                .applied = is_applied,
            });
        }
        if (generation && generation->graph && (!expanded_children.empty() || !expanded_boundaries.empty()
                || !expanded_aliases.sample.empty() || !expanded_aliases.event.empty())) {
            append_source_scope_connections(scope, *generation, expanded_children, expanded_boundaries, expanded_aliases);
        }
        builder.succeed(std::move(scope));
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}
} // namespace iv

namespace iv {
void ProjectGraph::handle_observation_request(ObservationGraphRequest& request)
{
    request.handled = true;
    try {
        auto const scheduling_begin = std::chrono::steady_clock::now();
        std::scoped_lock lock(mutex_);
        if (request.requirements) {
            if (request.requirements->probes.size() > maximum_live_observation_probes)
                throw std::invalid_argument("too many executable observation probes");
            observation_requirements_ = *request.requirements;
            ready_control_code_.reset();
            cancel_pending_control_compile_locked();
            latest_control_compile_request_ = next_control_compile_request_++;
            schedule_graph_compile_locked({}, true);
        }
        if (request.prediction_changed) {
            observation_candidate_.reset();
            predicted_observation_requirements_ = std::move(request.prediction);
            observation_prediction_pending_ = predicted_observation_requirements_.has_value();
            control_compile_changed_.notify_all();
        }
        request.scheduling_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - scheduling_begin).count());
        using Json = nlohmann::ordered_json;
        auto describe_result = [](ObservationOperation const& operation) {
            Json fields = Json::array();
            for (auto const& field : operation.result_fields) {
                auto const type = field.kind == ObservationNumberKind::float32 ? "float32"
                    : field.kind == ObservationNumberKind::float64 ? "float64" : "uint64String";
                fields.push_back({{"name", field.name}, {"type", type}});
            }
            return Json{{"operation", operation.id}, {"fields", std::move(fields)}};
        };
        request.configured_generation = current_generation_ && !configuration_failure_
            ? std::optional(current_generation_->configured_generation) : std::nullopt;
        if (active_execution_) {
            auto const& active = *active_execution_;
            request.window_configured_generation = active.configured->configured_generation;
            request.window_execution_generation = active.compiled->project_generation;
            request.window_sample_rate = active.compiled->specialization.sample_rate;
        }
        auto const* observation_graph = current_generation_ ? current_generation_->compiled_graph.get() : nullptr;
        if (!observation_graph && request.configured_generation && accepted_execution_graph_
            && accepted_execution_graph_->configured_generation == request.configured_generation) {
            observation_graph = accepted_execution_graph_.get();
        }
        Json capabilities = Json::array();
        auto describe = [&](ConfiguredGraph const& graph,
                            std::unordered_map<std::string, NodeInstancePlacement> const& placements,
                            CompiledGraph const* compiled) {
            auto const* port_realization = compiled ? &compiled->port_realization : nullptr;
            // Reuse the current graph's solved types for all requested sources.
            // If code has not been compiled, constraint solving alone suffices.
            std::optional<graph_jit::ResolvedPortRealization> inferred_ports;
            if (!port_realization || port_realization->bundle_count() < graph.node_bundles.size()) {
                port_realization = nullptr;
                auto resolved = graph_jit::resolve_port_realization(graph);
                if (resolved) {
                    inferred_ports.emplace(std::move(*resolved));
                    port_realization = &*inferred_ports;
                }
            }
            for (std::size_t node_index = 0; node_index < request.describe_nodes.size(); ++node_index) {
                auto const& address = request.describe_nodes[node_index];
                auto matches = resolve_node_address(graph, placements, address);
                std::map<std::string, ChannelTypeId> outputs, inputs;
                for (auto const& match : matches) {
                    if (match.kind == ProjectNodePathMatch::Kind::symbolic_node) {
                        auto const& node = graph.symbolic_nodes.record(match.symbolic_node);
                        for (auto const& mapping : node.sample_outputs)
                            outputs.emplace(mapping.name, mapping.channel_layout.channel_type);
                        for (auto const& mapping : node.sample_inputs)
                            inputs.emplace(mapping.name, mapping.channel_layout.channel_type);
                        continue;
                    }
                    auto const& bundle = graph.node_bundles.bundle(match.bundle);
                    for (std::size_t port = 0; port < bundle.sample_output_count(); ++port) {
                        auto const config = graph.node_bundles.resolve_sample_output({match.bundle, PortKind::sample, port}).config;
                        outputs.emplace(config.name, config.channel_layout.channel_type);
                    }
                    for (std::size_t port = 0; port < bundle.sample_input_count(); ++port) {
                        auto const config = graph.node_bundles.resolve_sample_input({match.bundle, PortKind::sample, port}).config;
                        inputs.emplace(config.name, config.channel_layout.channel_type);
                    }
                }
                auto describe_channels = [&](auto const& sources, ObservationSourceKind kind,
                                             std::string const& name, ChannelTypeId channel_type) {
                    auto const input = kind == ObservationSourceKind::input;
                    auto const* kind_name = input ? "input" : "output";
                    auto const summed = observes_symbolic_output({.node = address, .kind = kind});
                    auto const& public_layout = channel_type_info(channel_type);
                    auto const channel_count = summed ? public_layout.channel_count : sources.size();
                    if (sources.empty()) return;
                    Json channels = Json::array(), window_channels = Json::array();
                    for (std::size_t channel = 0; channel < channel_count; ++channel) {
                        ObservationRequirements requirements{.probes = {{.source = {
                            .node = address, .kind = kind, .member = name,
                            .channel = channel}, .operation = "level"}}};
                        auto plan = resolve_observation_requirements(graph, placements, requirements, port_realization);
                        ObservationWindowQuery window_query;
                        bool const window_available = resolve_observation_window_source(graph, placements,
                            requirements.probes.front().source, "waveform", window_query, port_realization, compiled);
                        if (!plan.probes.front().available && !window_available) continue;
                        auto const source = sources[std::min(channel, sources.size() - 1)];
                        NodeBundlePortId const port{source.bundle, PortKind::sample, source.port};
                        auto const physical_type = input
                            ? graph.node_bundles.resolve_sample_input(port).config.channel_layout.channel_type
                            : graph.node_bundles.resolve_sample_output(port).config.channel_layout.channel_type;
                        auto const& physical_layout = channel_type_info(physical_type);
                        auto const label = summed || sources.size() == public_layout.channel_count
                            ? public_layout.channel_names[channel] : physical_layout.channel_names[source.channel];
                        if (plan.probes.front().available)
                            channels.push_back({{"index", channel}, {"name", label}});
                        if (window_available)
                            window_channels.push_back({{"index", channel}, {"name", label}});
                    }
                    auto describe_delivery = [&](Json channels, bool window) {
                        if (channels.empty()) return;
                        Json operations = Json::array(), schemas = Json::array();
                        for (auto const& operation : continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations) {
                            if (window ? !operation.capture_point : !operation.capture_block) continue;
                            operations.push_back(operation.id);
                            schemas.push_back(describe_result(operation));
                        }
                        Json capability{{"nodeIndex", node_index}, {"kind", kind_name},
                            {"member", name}, {"channels", std::move(channels)}, {"operations", std::move(operations)},
                            {"resultSchemas", std::move(schemas)}};
                        if (window) capability["delivery"] = "window";
                        else if (summed) capability["gainApplied"] = true;
                        capabilities.push_back(std::move(capability));
                    };
                    describe_delivery(std::move(window_channels), true);
                    describe_delivery(std::move(channels), false);
                };
                for (auto const& [name, channel_type] : outputs)
                    describe_channels(resolve_project_sample_output(graph, placements, address, name),
                        ObservationSourceKind::output, name, channel_type);
                for (auto const& [name, channel_type] : inputs)
                    describe_channels(resolve_project_sample_input(graph, placements, address, name),
                        ObservationSourceKind::input, name, channel_type);
                auto bundles = concrete_node_address_matches(graph, matches);
                if (bundles.size() != 1) continue;
                if (compiled) {
                    auto const handle = bundles.front();
                    auto const& node = graph.node_bundles.bundle(handle);
                    for (std::size_t index = 0; index < node.event_output_count(); ++index) {
                        NodeBundlePortId const port{handle, PortKind::event, index};
                        if (!recorded_event_window_storage(compiled->background_evaluation_plan, port)) continue;
                        auto const config = graph.node_bundles.resolve_event_output(port).config;
                        auto operations = Json::array({recorded_event_timestamps_operation});
                        if (config.type == EventTypeId::trigger) operations.push_back(recorded_trigger_bins_operation);
                        capabilities.push_back({{"nodeIndex", node_index}, {"kind", "output"}, {"portKind", "event"},
                            {"member", config.name}, {"delivery", "window"}, {"operations", std::move(operations)}});
                    }
                }
                std::size_t index = 0;
                graph.node_bundles.for_each_configured_bundle([&](ConfiguredNodeBundleView const& bundle) {
                    if (index++ != bundles.front() || !bundle.operations
                        || !bundle.operations->runtime.tick_block || !bundle.operations->runtime.state_structures
                        || !bundle.operations->runtime.state_structures->state) return;
                    for (auto const& field : bundle.operations->runtime.state_structures->state->fields) {
                        if (field.bit_width || field.bit_offset % 8) continue;
                        for (auto const& codec : state_observation_codecs) {
                            if (field.type_name == codec.field_type_id && field.size_bits == codec.size_bits)
                                capabilities.push_back({{"nodeIndex", node_index}, {"kind", "stateField"},
                                    {"member", field.name}, {"operations", {codec.operation->id}},
                                    {"resultSchemas", {describe_result(*codec.operation)}}});
                        }
                    }
                });
            }
        };
        if (!request.describe_nodes.empty()) {
            if (configuration_failure_ && configuration_failure_->instance_interfaces) {
                for (auto const& [id, configured] : configuration_failure_->instance_interfaces->configured_instances) {
                    if (!configured || !configured->graph) continue;
                    if (std::ranges::none_of(request.describe_nodes,
                        [&](auto const& address) { return address.instance_id == id; })) continue;
                    std::unordered_map<std::string, NodeInstancePlacement> placements{
                        {id, {.configured = configured, .root = configured->local_root}}};
                    describe(*configured->graph, placements, nullptr);
                }
            } else if (!configuration_failure_ && current_generation_) {
                describe(*current_generation_->graph, current_generation_->placements,
                    observation_graph);
            }
        }
        for (auto& window : request.windows) {
            if (!window.select_execution(request.window_configured_generation.value_or(0),
                    request.window_execution_generation)) continue;
            auto& query = *window.query;
            if (active_execution_) {
                auto const& active = *active_execution_;
                if (resolve_observation_window_source(*active.configured->graph, active.configured->placements,
                        window.source, window.operation, query, &active.compiled->port_realization, active.compiled.get()))
                    continue;
            }
            bool const pending_first_execution = !active_execution_ && current_generation_ && !configuration_failure_
                && (current_generation_->execution_phase == "queued" || current_generation_->execution_phase == "compiling"
                    || staged_execution_.has_value());
            query.result.status = pending_first_execution ? ObservationWindowResult::Status::pending_execution
                : ObservationWindowResult::Status::unavailable;
            query.complete.store(true, std::memory_order_release);
        }
        request.capabilities = std::make_shared<nlohmann::ordered_json const>(std::move(capabilities));
    } catch (std::exception const& error) {
        request.error = error.what();
    }
}
} // namespace iv
