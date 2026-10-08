#include <intravenous/runtime/background_coverage_propagation.h>

#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {
namespace {

Coverage coverage_difference(Coverage const& left, Coverage const& right)
{
    return (left - right) | (right - left);
}

} // namespace

[[nodiscard]] graph_jit::BackgroundPortPlan const&
BackgroundPropagationWorkspace::port(graph_jit::BackgroundPortIndex index) const
{
    return plan_->ports[index];
}

[[nodiscard]] std::size_t BackgroundPropagationWorkspace::output_index(
    graph_jit::BackgroundPortIndex index) const
{
    auto const& planned = port(index);
    return *planned.accumulators.output_change;
}

[[nodiscard]] std::size_t BackgroundPropagationWorkspace::input_index(
    graph_jit::BackgroundPortIndex index) const
{
    auto const& planned = port(index);
    return *planned.accumulators.input_change;
}

[[nodiscard]] Coverage BackgroundPropagationWorkspace::input_coverage(
    graph_jit::BackgroundPortIndex input) const
{
    Coverage result;
    auto const& planned_input = port(input);
    for (auto const connection_index : planned_input.incoming_connections) {
        auto const& connection = plan_->connections[connection_index];
        for (auto const source : connection.source_coverage_ports) {
            auto const index = output_index(source);
            result.include(candidate_output_coverages_[index]);
        }
    }
    return result;
}

void BackgroundPropagationWorkspace::activate_node(
    graph_jit::BackgroundNodeIndex node,
    graph_jit::BackgroundNodeActivity activity)
{
    node_calls_[node].activity = node_calls_[node].activity | activity;
}

void BackgroundPropagationWorkspace::add_input_change(
    graph_jit::BackgroundPortIndex input, Coverage const& changed,
    bool recompute_coverage)
{
    auto const& planned = port(input);
    if (!planned.accumulators.input_change)
        return;
    auto const index = input_index(input);
    auto& accumulator = input_changes_[index];
    if (recompute_coverage) {
        auto const next = input_coverage(input);
        accumulator.changed.include(
            coverage_difference(accumulator.coverage, next));
        accumulator.coverage = next;
        auto const requirement_index = *planned.accumulators.input_requirement;
        input_requirements_[requirement_index].coverage = next;
    }
    accumulator.changed.include(changed);
    if (!changed.empty() || !accumulator.changed.empty()) {
        activate_node(planned.node, graph_jit::BackgroundNodeActivity::forward);
    }
}

void BackgroundPropagationWorkspace::route_output_change(
    graph_jit::BackgroundPortIndex output, Coverage const& changed,
    bool coverage_changed)
{
    auto const& planned_output = port(output);
    for (auto const connection_index : planned_output.outgoing_connections) {
        auto const& connection = plan_->connections[connection_index];
        for (auto const target : connection.target_coverage_ports) {
            add_input_change(target, changed, coverage_changed);
        }
    }
}

void BackgroundPropagationWorkspace::set_input_change(
    graph_jit::BackgroundPortIndex input, Coverage const& coverage,
    Coverage const& changed)
{
    auto const& planned = port(input);
    auto const index = input_index(input);
    auto& accumulator = input_changes_[index];
    accumulator.changed.include(
        coverage_difference(accumulator.coverage, coverage));
    accumulator.changed.include(changed);
    accumulator.coverage = coverage;
    auto const requirement_index = *planned.accumulators.input_requirement;
    input_requirements_[requirement_index].coverage = coverage;
    if (!accumulator.changed.empty()) {
        activate_node(planned.node, graph_jit::BackgroundNodeActivity::forward);
    }
}

void BackgroundPropagationWorkspace::publish_output_coverage(
    graph_jit::BackgroundPortIndex output, Coverage const& coverage)
{
    auto const index = output_index(output);
    auto& accumulator = output_changes_[index];
    auto const changed =
        coverage_difference(candidate_output_coverages_[index], coverage);
    candidate_output_coverages_[index] = coverage;
    accumulator.touched = accumulator.touched || !changed.empty();
    accumulator.changed.include(changed);
    route_output_change(output, changed, true);
}

void BackgroundPropagationWorkspace::publish_output_change(
    graph_jit::BackgroundPortIndex output, Coverage const& changed)
{
    if (changed.empty())
        return;
    auto const index = output_index(output);
    auto& accumulator = output_changes_[index];
    accumulator.touched = true;
    accumulator.changed.include(changed);
    route_output_change(output, changed, false);
}

void BackgroundPropagationWorkspace::publish_output_coverage_callback(
    void* opaque, Coverage const& coverage)
{
    auto& accumulator = *static_cast<OutputChangeAccumulator*>(opaque);
    accumulator.owner->publish_output_coverage(accumulator.port, coverage);
}

void BackgroundPropagationWorkspace::publish_output_change_callback(
    void* opaque, Coverage const& changed)
{
    auto& accumulator = *static_cast<OutputChangeAccumulator*>(opaque);
    accumulator.owner->publish_output_change(accumulator.port, changed);
}

void BackgroundPropagationWorkspace::require_output(
    graph_jit::BackgroundPortIndex output, Coverage const& requested,
    bool activate_producer)
{
    auto const& planned = port(output);
    auto const change_index = output_index(output);
    auto const requirement_index = *planned.accumulators.output_requirement;
    auto const required = requested & candidate_output_coverages_[change_index];
    if (required.empty())
        return;
    output_requirements_[requirement_index].required.include(required);
    if (activate_producer) {
        activate_node(planned.node, graph_jit::BackgroundNodeActivity::reverse);
    }
}

void BackgroundPropagationWorkspace::require_input(
    graph_jit::BackgroundPortIndex input, Coverage const& requested)
{
    auto const& planned_input = port(input);
    Coverage coverage;
    InputRequirementAccumulator* accumulator = nullptr;
    if (planned_input.accumulators.input_requirement) {
        auto const index = *planned_input.accumulators.input_requirement;
        accumulator = &input_requirements_[index];
        coverage = accumulator->coverage;
    } else {
        coverage = input_coverage(input);
    }
    auto const required = requested & coverage;
    if (required.empty())
        return;
    input_requirements_by_port_[input].include(required);

    for (auto const connection_index : planned_input.incoming_connections) {
        auto const& connection = plan_->connections[connection_index];
        for (auto const source : connection.source_coverage_ports) {
            auto const& planned_source = port(source);
            auto const stored_boundary =
                planned_source.retention &&
                *planned_source.retention == OutputRetention::persisted;
            require_output(source, required, !stored_boundary);
        }
    }
}

void BackgroundPropagationWorkspace::publish_input_requirement_callback(
    void* opaque, Coverage const& required)
{
    auto& accumulator = *static_cast<InputRequirementAccumulator*>(opaque);
    accumulator.owner->require_input(accumulator.port, required);
}

void BackgroundPropagationWorkspace::replay_forward_coverage(
    void*, ReflectedNodeForwardCoverageContext const& context)
{
    Coverage coverage;
    Coverage changed;
    for (auto const& input :
         static_cast<std::span<InputCoverageChange const>>(context.inputs)) {
        coverage.include(input.coverage());
        changed.include(input.changed());
    }
    for (auto const& input : static_cast<std::span<InputCoverageChange const>>(
             context.event_inputs)) {
        coverage.include(input.coverage());
        changed.include(input.changed());
    }
    for (auto const& output :
         static_cast<std::span<OutputCoverageChange>>(context.outputs)) {
        output.publish_coverage(coverage);
        output.change(changed);
    }
    for (auto const& output :
         static_cast<std::span<OutputCoverageChange>>(context.event_outputs)) {
        output.publish_coverage(coverage);
        output.change(changed);
    }
}

void BackgroundPropagationWorkspace::replay_reverse_coverage(
    void*, ReflectedNodeReverseCoverageContext const& context)
{
    Coverage required;
    for (auto const& output :
         static_cast<std::span<OutputCoverageRequirement const>>(
             context.outputs)) {
        required.include(output.required());
    }
    for (auto const& output :
         static_cast<std::span<OutputCoverageRequirement const>>(
             context.event_outputs)) {
        required.include(output.required());
    }
    for (auto const& input :
         static_cast<std::span<InputCoverageRequirement>>(context.inputs)) {
        input.require(required & input.coverage());
    }
    for (auto const& input : static_cast<std::span<InputCoverageRequirement>>(
             context.event_inputs)) {
        input.require(required & input.coverage());
    }
}

void BackgroundPropagationWorkspace::initialize_calls()
{
    for (graph_jit::BackgroundPortIndex index = 0; index < plan_->ports.size();
         ++index) {
        auto const& planned = plan_->ports[index];
        if (planned.direction == graph_jit::PortDirection::input &&
            planned.accumulators.input_change) {
            auto const change_index = *planned.accumulators.input_change;
            auto const requirement_index =
                *planned.accumulators.input_requirement;
            auto& requirement = input_requirements_[requirement_index];
            requirement.owner = this;
            requirement.port = index;
            callback_input_changes_[change_index] = InputCoverageChange{
                .coverage_value = &input_changes_[change_index].coverage,
                .changed_value = &input_changes_[change_index].changed,
            };
            callback_input_requirements_[requirement_index] =
                InputCoverageRequirement{
                    .data = &requirement,
                    .coverage_value = &requirement.coverage,
                    .publish_required_value =
                        &publish_input_requirement_callback,
            };
        } else if (planned.direction == graph_jit::PortDirection::output) {
            auto const change_index = *planned.accumulators.output_change;
            auto const requirement_index =
                *planned.accumulators.output_requirement;
            auto& change = output_changes_[change_index];
            change.owner = this;
            change.port = index;
            callback_output_changes_[change_index] = OutputCoverageChange{
                .data = &change,
                .previous_coverage_value = &change.previous_coverage,
                .publish_coverage_value = &publish_output_coverage_callback,
                .publish_changed_value = &publish_output_change_callback,
            };
            callback_output_requirements_[requirement_index] =
                OutputCoverageRequirement{
                    .required_value =
                        &output_requirements_[requirement_index].required,
            };
        }
    }
    for (graph_jit::BackgroundNodeIndex index = 0; index < plan_->nodes.size();
         ++index) {
        auto const& node = plan_->nodes[index];

        auto& calls = node_call_data_[index];
        for (auto const port_index : node.inputs) {
            auto const& planned = port(port_index);
            auto const callback_input =
                node.authored_tock_execution ? planned.random_access_input
                : node.replays_tick          ? planned.replay_sequential_input
                                             : false;
            if (!callback_input)
                continue;
            auto const change_index = *planned.accumulators.input_change;
            auto const requirement_index =
                *planned.accumulators.input_requirement;
            if (planned.kind == PortKind::sample) {
                calls.sample_input_changes.push_back(
                    callback_input_changes_[change_index]);
                calls.sample_input_requirements.push_back(
                    callback_input_requirements_[requirement_index]);
            } else {
                calls.event_input_changes.push_back(
                    callback_input_changes_[change_index]);
                calls.event_input_requirements.push_back(
                    callback_input_requirements_[requirement_index]);
            }
        }

        for (auto const port_index : node.outputs) {
            auto const& planned = port(port_index);
            auto const callback_output =
                node.authored_tock_execution ? planned.authored_tock_output
                : node.replays_tick          ? planned.replayed_tick_output
                                             : false;
            if (!callback_output)
                continue;
            auto const change_index = *planned.accumulators.output_change;
            auto const requirement_index =
                *planned.accumulators.output_requirement;
            if (planned.kind == PortKind::sample) {
                calls.sample_output_changes.push_back(
                    callback_output_changes_[change_index]);
                calls.sample_output_requirements.push_back(
                    callback_output_requirements_[requirement_index]);
            } else {
                calls.event_output_changes.push_back(
                    callback_output_changes_[change_index]);
                calls.event_output_requirements.push_back(
                    callback_output_requirements_[requirement_index]);
            }
        }

        auto& frame = node_calls_[index];
        frame.forward = ReflectedNodeForwardCoverageContext{
            .inputs = calls.sample_input_changes,
            .outputs = calls.sample_output_changes,
            .event_inputs = calls.event_input_changes,
            .event_outputs = calls.event_output_changes,
            .sample_rate = sample_rate_,
        };
        frame.reverse = ReflectedNodeReverseCoverageContext{
            .inputs = calls.sample_input_requirements,
            .outputs = calls.sample_output_requirements,
            .event_inputs = calls.event_input_requirements,
            .event_outputs = calls.event_output_requirements,
            .sample_rate = sample_rate_,
        };
        if (node.uses_replay_forward_coverage) {
            frame.replay_forward = &replay_forward_coverage;
        }
        if (node.uses_replay_reverse_coverage) {
            frame.replay_reverse = &replay_reverse_coverage;
        }
    }
    call_.nodes = node_calls_;
}

BackgroundCoverageState::BackgroundCoverageState(std::size_t output_count)
    : output_coverages_(output_count)
{
}

void BackgroundPropagationWorkspace::reset(
    BackgroundCoverageState const& coverage)
{
    candidate_output_coverages_ = coverage.output_coverages_;
    for (std::size_t index = 0; index < candidate_output_coverages_.size();
         ++index) {
        output_changes_[index].previous_coverage =
            coverage.output_coverages_[index];
    }
    for (auto& value : input_changes_)
        value = {};
    for (auto& value : output_changes_) {
        value.changed = {};
        value.touched = false;
    }
    for (auto& value : input_requirements_) {
        value.coverage = {};
    }
    for (auto& value : output_requirements_)
        value = {};
    for (auto& value : input_requirements_by_port_)
        value = {};
    for (auto& frame : node_calls_) {
        frame.activity = graph_jit::BackgroundNodeActivity::none;
        frame.forward.local_state_changed = false;
    }

    for (graph_jit::BackgroundPortIndex index = 0; index < plan_->ports.size();
         ++index) {
        auto const& planned = plan_->ports[index];
        if (planned.direction != graph_jit::PortDirection::input ||
            !planned.accumulators.input_change) {
            continue;
        }
        auto const change_index = input_index(index);
        auto const requirement_index = *planned.accumulators.input_requirement;
        auto coverage = input_coverage(index);
        input_changes_[change_index].coverage = coverage;
        input_requirements_[requirement_index].coverage = std::move(coverage);
    }
}

BackgroundPropagationWorkspace::BackgroundPropagationWorkspace(
    graph_jit::BackgroundEvaluationPlan const& plan, std::size_t sample_rate)
    : plan_(&plan), sample_rate_(sample_rate),
      candidate_output_coverages_(plan.accumulators.output_change_count),
      input_changes_(plan.accumulators.input_change_count),
      output_changes_(plan.accumulators.output_change_count),
      input_requirements_(plan.accumulators.input_requirement_count),
      output_requirements_(plan.accumulators.output_requirement_count),
      input_requirements_by_port_(plan.ports.size()),
      callback_input_changes_(plan.accumulators.input_change_count),
      callback_output_changes_(plan.accumulators.output_change_count),
      callback_input_requirements_(plan.accumulators.input_requirement_count),
      callback_output_requirements_(plan.accumulators.output_requirement_count),
      node_call_data_(plan.nodes.size()), node_calls_(plan.nodes.size())
{
    initialize_calls();
}

[[nodiscard]] PreparedCoveragePropagation
BackgroundPropagationWorkspace::prepare(
    CompiledGraphBackgroundOperations const& operations, std::byte* storage,
    BackgroundCoverageState const& coverage,
    CoveragePropagationRequest const& request,
    BackgroundCoverageDemandExpansion expansion)
{
    auto const check_port = [this](graph_jit::BackgroundPortIndex index,
                                   graph_jit::PortDirection direction) {
        if (index >= plan_->ports.size()) {
            throw std::out_of_range(
                "Background coverage request port index is out of range");
        }
        if (port(index).direction != direction) {
            throw std::invalid_argument(
                "Background coverage request has the wrong port direction");
        }
    };
    for (auto const node : request.locally_changed_nodes) {
        if (node >= node_calls_.size()) {
            throw std::out_of_range(
                "Background coverage request node index is out of range");
        }
    }
    for (auto const& change : request.input_changes) {
        check_port(change.port, graph_jit::PortDirection::input);
    }
    for (auto const& change : request.output_changes) {
        check_port(change.port, graph_jit::PortDirection::output);
    }
    for (auto const& demand : request.input_demands) {
        check_port(demand.port, graph_jit::PortDirection::input);
    }
    for (auto const& demand : request.output_demands) {
        check_port(demand.port, graph_jit::PortDirection::output);
    }
    reset(coverage);
    for (auto const node : request.locally_changed_nodes) {
        node_calls_[node].forward.local_state_changed = true;
        activate_node(node, graph_jit::BackgroundNodeActivity::forward);
    }
    for (auto const& root : request.output_changes) {
        publish_output_coverage(root.port, root.coverage);
        publish_output_change(root.port, root.changed);
    }
    for (auto const& root : request.input_changes) {
        set_input_change(root.port, root.coverage, root.changed);
    }

    operations.propagate_forward(storage, &call_);

    for (auto const& root : request.input_demands) {
        require_input(root.port, root.required);
    }
    for (auto const& root : request.output_demands) {
        auto const& planned = port(root.port);
        auto const stored_boundary = planned.retention
            && *planned.retention == OutputRetention::persisted;
        require_output(root.port, root.required, !stored_boundary);
    }
    if (expansion.expand_output) {
        for (graph_jit::BackgroundPortIndex index = 0;
             index < plan_->ports.size(); ++index) {
            auto const& planned = port(index);
            if (planned.direction != graph_jit::PortDirection::output)
                continue;
            auto const change_index = output_index(index);
            Coverage additional;
            expansion.expand_output(
                expansion.data, index,
                candidate_output_coverages_[change_index],
                output_changes_[change_index].changed,
                output_requirements_[
                    *planned.accumulators.output_requirement].required,
                additional);
            require_output(index, additional, true);
        }
    }
    operations.propagate_reverse(storage, &call_);

    CoveragePropagationResult result;
    result.output_changes.reserve(output_changes_.size());
    for (std::size_t index = 0; index < output_changes_.size(); ++index) {
        auto const& change = output_changes_[index];
        if (!change.touched)
            continue;
        result.output_changes.push_back(PropagatedOutputChange{
            .port = change.port,
            .coverage = candidate_output_coverages_[index],
            .changed = change.changed,
        });
    }
    result.input_requirements.reserve(plan_->ports.size());
    for (graph_jit::BackgroundPortIndex index = 0; index < plan_->ports.size();
         ++index) {
        auto const& required = input_requirements_by_port_[index];
        if (required.empty())
            continue;
        result.input_requirements.push_back(RequiredInputCoverage{
            .port = index,
            .required = required,
        });
    }
    result.output_requirements.reserve(output_requirements_.size());
    for (graph_jit::BackgroundPortIndex index = 0; index < plan_->ports.size();
         ++index) {
        auto const& planned = plan_->ports[index];
        if (planned.direction != graph_jit::PortDirection::output ||
            !planned.accumulators.output_requirement) {
            continue;
        }
        auto const requirement_index = *planned.accumulators.output_requirement;
        auto const& required = output_requirements_[requirement_index].required;
        if (required.empty())
            continue;
        result.output_requirements.push_back(RequiredOutputCoverage{
            .port = index,
            .required = required,
        });
    }

    PreparedCoveragePropagation prepared;
    prepared.result_ = std::move(result);
    prepared.output_coverages_ = std::move(candidate_output_coverages_);
    prepared.node_activity_.reserve(node_calls_.size());
    for (auto const& frame : node_calls_) {
        auto activity = frame.activity;
        if (graph_jit::has_activity(
                activity, graph_jit::BackgroundNodeActivity::reverse)) {
            activity = activity | graph_jit::BackgroundNodeActivity::evaluate;
        }
        prepared.node_activity_.push_back(activity);
    }
    return prepared;
}

CoveragePropagationResult BackgroundPropagationWorkspace::commit(
    BackgroundCoverageState& coverage,
    PreparedCoveragePropagation&& prepared) noexcept
{
    coverage.output_coverages_.swap(prepared.output_coverages_);
    candidate_output_coverages_.swap(prepared.output_coverages_);
    return std::move(prepared.result_);
}

void BackgroundPropagationWorkspace::discard(
    PreparedCoveragePropagation&& prepared) noexcept
{
    candidate_output_coverages_.swap(prepared.output_coverages_);
}

} // namespace iv
