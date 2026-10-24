#include <intravenous/runtime/background_coverage_propagation.h>

#include <algorithm>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {
namespace {

[[nodiscard]] IndexCoverage shifted_forward(
    IndexCoverage const& coverage, std::size_t latency)
{
    if (latency == 0) return coverage;
    IndexCoverage result;
    for (auto const range : coverage) {
        result.include({
            .begin = range.begin.advanced_by(latency),
            .extent = range.extent,
        });
    }
    return result;
}

[[nodiscard]] IndexCoverage shifted_back(
    IndexCoverage const& coverage, std::size_t latency)
{
    if (latency == 0) return coverage;
    IndexCoverage result;
    for (auto const range : coverage) {
        result.include({
            .begin = range.begin.retreated_by(latency),
            .extent = range.extent,
        });
    }
    return result;
}

IndexCoverage coverage_difference(IndexCoverage const& left, IndexCoverage const& right)
{
    return (left - right) | (right - left);
}

bool shares_configuration_identity(
    std::shared_ptr<void const> const& left,
    std::shared_ptr<void const> const& right) noexcept
{
    return left && right && left.get() == right.get()
        && !left.owner_before(right) && !right.owner_before(left);
}

bool local_port_semantics_equal(
    graph_jit::BackgroundPortPlan const& current,
    graph_jit::BackgroundPortPlan const& previous)
{
    if (current.direction != previous.direction
        || current.kind != previous.kind
        || current.random_access_input != previous.random_access_input
        || current.tick_sequential_input != previous.tick_sequential_input
        || current.replay_sequential_input != previous.replay_sequential_input
        || current.authored_tock_output != previous.authored_tock_output
        || current.persisted_tick_output != previous.persisted_tick_output
        || current.replayed_tick_output != previous.replayed_tick_output
        || current.sample_neutral_value != previous.sample_neutral_value
        || current.sequential_history != previous.sequential_history
        || current.output_history != previous.output_history
        || current.output_latency != previous.output_latency
        || current.retention != previous.retention
        || current.sample_layout != previous.sample_layout
        || current.event_type != previous.event_type
        || current.max_events_per_index != previous.max_events_per_index
        || current.accumulators.input_change.has_value()
            != previous.accumulators.input_change.has_value()
        || current.accumulators.output_change.has_value()
            != previous.accumulators.output_change.has_value()
        || current.accumulators.output_requirement.has_value()
            != previous.accumulators.output_requirement.has_value()
        || current.accumulators.input_requirement.has_value()
            != previous.accumulators.input_requirement.has_value()) {
        return false;
    }
    return current.direction == graph_jit::PortDirection::input
        ? current.stable_input_identity == previous.stable_input_identity
            && current.stable_input_identity.has_value()
        : current.stable_identity == previous.stable_identity
            && current.stable_identity.has_value();
}

bool node_ports_equal(
    graph_jit::BackgroundEvaluationPlan const& current_plan,
    graph_jit::BackgroundNodePlan const& current,
    graph_jit::BackgroundEvaluationPlan const& previous_plan,
    graph_jit::BackgroundNodePlan const& previous)
{
    auto equal_set = [&](std::span<graph_jit::BackgroundPortIndex const> current_ports,
                         std::span<graph_jit::BackgroundPortIndex const> previous_ports) {
        if (current_ports.size() != previous_ports.size()) return false;
        std::vector<bool> matched(previous_ports.size(), false);
        for (auto const current_index : current_ports) {
            if (current_index >= current_plan.ports.size()) return false;
            auto found = previous_ports.size();
            for (std::size_t index = 0; index < previous_ports.size(); ++index) {
                auto const previous_index = previous_ports[index];
                if (matched[index]
                    || previous_index >= previous_plan.ports.size()
                    || !local_port_semantics_equal(
                        current_plan.ports[current_index],
                        previous_plan.ports[previous_index])) {
                    continue;
                }
                found = index;
                break;
            }
            if (found == previous_ports.size()) return false;
            matched[found] = true;
        }
        return true;
    };
    return equal_set(current.inputs, previous.inputs)
        && equal_set(current.outputs, previous.outputs);
}

bool node_local_semantics_equal(
    graph_jit::BackgroundEvaluationPlan const& current_plan,
    graph_jit::BackgroundNodePlan const& current,
    graph_jit::BackgroundEvaluationPlan const& previous_plan,
    graph_jit::BackgroundNodePlan const& previous,
    bool local_semantic_environment_changed)
{
    return !local_semantic_environment_changed
        && current.implementation_identity == previous.implementation_identity
        && current.provider_version != 0
        && current.provider_version == previous.provider_version
        && shares_configuration_identity(
            current.configuration_identity,
            previous.configuration_identity)
        && current.authored_tock_execution == previous.authored_tock_execution
        && current.replays_tick == previous.replays_tick
        && current.uses_replay_forward_coverage
            == previous.uses_replay_forward_coverage
        && current.uses_replay_reverse_coverage
            == previous.uses_replay_reverse_coverage
        && current.uses_imported_tick_block_for_replay
            == previous.uses_imported_tick_block_for_replay
        && node_ports_equal(
            current_plan, current, previous_plan, previous);
}

IndexCoverage input_coverage(
    graph_jit::BackgroundEvaluationPlan const& plan,
    std::span<IndexCoverage const> output_coverages,
    graph_jit::BackgroundPortIndex input)
{
    IndexCoverage result;
    for (auto const connection_index : plan.ports[input].incoming_connections) {
        auto const& connection = plan.connections[connection_index];
        for (auto const source : connection.source_coverage_ports) {
            auto const accumulator = plan.ports[source].accumulators.output_change;
            if (accumulator && *accumulator < output_coverages.size()) {
                result.include(output_coverages[*accumulator]);
            }
        }
    }
    return result;
}

struct StableSampleConnectionSource {
    graph_jit::StableOutputPortId port{};
    std::size_t channel = 0;
    graph_jit::PlannedDeliveryMechanism delivery =
        graph_jit::PlannedDeliveryMechanism::tick_to_sequential;

    bool operator==(StableSampleConnectionSource const&) const = default;
};

struct StableEventConnectionSource {
    graph_jit::StableOutputPortId port{};
    EventTypeId source_type = EventTypeId::empty;
    EventConversionPlan conversion{};
    graph_jit::PlannedDeliveryMechanism delivery =
        graph_jit::PlannedDeliveryMechanism::tick_to_sequential;

    bool operator==(StableEventConnectionSource const&) const = default;
};

struct StableInputConnectionDescription {
    PortKind kind = PortKind::sample;
    ChannelTypeId sample_source_type = ChannelTypeId::mono;
    ChannelTypeId sample_target_type = ChannelTypeId::mono;
    std::vector<StableSampleConnectionSource> sample_sources{};
    std::vector<std::size_t> sample_target_channels{};
    std::vector<graph_jit::SampleProjectionPlan> sample_projections{};
    EventTypeId event_source_type = EventTypeId::empty;
    EventTypeId event_target_type = EventTypeId::empty;
    EventConversionPlan event_conversion{};
    std::vector<StableEventConnectionSource> event_sources{};
    bool requires_conversion = false;

    bool operator==(StableInputConnectionDescription const&) const = default;
};

std::optional<StableInputConnectionDescription> describe_connection(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::BackgroundConnectionIndex connection_index,
    graph_jit::BackgroundPortIndex input)
{
    if (connection_index >= plan.connections.size()) return std::nullopt;
    auto const& connection = plan.connections[connection_index];
    StableInputConnectionDescription result{
        .kind = connection.kind,
        .sample_source_type = connection.sample_source_type,
        .sample_target_type = connection.sample_target_type,
        .sample_projections = connection.sample_projections,
        .event_source_type = connection.event_source_type,
        .event_target_type = connection.event_target_type,
        .event_conversion = connection.event_conversion,
        .requires_conversion = connection.requires_conversion,
    };
    if (connection.kind == PortKind::sample) {
        if (!connection.sample_target_port
            || *connection.sample_target_port != input
            || connection.sample_source_channels.size()
                != connection.sample_source_port_by_channel.size()
            || connection.sample_source_channels.size()
                != connection.sample_deliveries.size()) {
            return std::nullopt;
        }
        result.sample_sources.reserve(connection.sample_source_channels.size());
        for (std::size_t index = 0;
             index < connection.sample_source_channels.size(); ++index) {
            auto const source = connection.sample_source_port_by_channel[index];
            if (!source || *source >= plan.ports.size()
                || !plan.ports[*source].stable_identity) {
                return std::nullopt;
            }
            auto const& configured_source = plan.ports[*source].configured_port;
            if (configured_source.node_bundle_handle
                    != connection.sample_source_channels[index].bundle
                || configured_source.port_index
                    != connection.sample_source_channels[index].port) {
                return std::nullopt;
            }
            result.sample_sources.push_back({
                .port = *plan.ports[*source].stable_identity,
                .channel = connection.sample_source_channels[index].channel,
                .delivery = connection.sample_deliveries[index],
            });
        }
        result.sample_target_channels.reserve(
            connection.sample_target_channels.size());
        for (auto const& target : connection.sample_target_channels) {
            auto const& configured_input = plan.ports[input].configured_port;
            if (target.bundle != configured_input.node_bundle_handle
                || target.port != configured_input.port_index) {
                return std::nullopt;
            }
            result.sample_target_channels.push_back(target.channel);
        }
        return result;
    }

    for (auto const& delivery : connection.event_deliveries) {
        if (!delivery.target_port || *delivery.target_port != input) continue;
        if (!delivery.source_port || *delivery.source_port >= plan.ports.size()
            || !plan.ports[*delivery.source_port].stable_identity) {
            return std::nullopt;
        }
        auto const& configured_source =
            plan.ports[*delivery.source_port].configured_port;
        if (configured_source.node_bundle_handle != delivery.source.bundle
            || configured_source.port_index != delivery.source.port) {
            return std::nullopt;
        }
        result.event_sources.push_back({
            .port = *plan.ports[*delivery.source_port].stable_identity,
            .source_type = delivery.source_type,
            .conversion = delivery.conversion,
            .delivery = delivery.mechanism,
        });
    }
    return result;
}

bool input_connections_equal(
    graph_jit::BackgroundEvaluationPlan const& current_plan,
    graph_jit::BackgroundPortIndex current_input,
    graph_jit::BackgroundEvaluationPlan const& previous_plan,
    graph_jit::BackgroundPortIndex previous_input)
{
    auto const& current = current_plan.ports[current_input];
    auto const& previous = previous_plan.ports[previous_input];
    if (current.kind != previous.kind
        || current.sample_layout != previous.sample_layout
        || current.event_type != previous.event_type
        || current.incoming_connections.size()
            != previous.incoming_connections.size()) {
        return false;
    }

    for (std::size_t index = 0;
         index < current.incoming_connections.size(); ++index) {
        auto current_description = describe_connection(
            current_plan, current.incoming_connections[index], current_input);
        auto previous_description = describe_connection(
            previous_plan,
            previous.incoming_connections[index],
            previous_input);
        if (!current_description || !previous_description
            || *current_description != *previous_description) {
            return false;
        }
    }
    return true;
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

[[nodiscard]] IndexCoverage BackgroundPropagationWorkspace::input_coverage(
    graph_jit::BackgroundPortIndex input) const
{
    IndexCoverage result;
    auto const& planned_input = port(input);
    for (auto const connection_index : planned_input.incoming_connections) {
        auto const& connection = plan_->connections[connection_index];
        bool used_exact_routes = false;
        if (connection.kind == PortKind::sample
            && connection.sample_target_port == input
            && connection.sample_source_port_by_channel.size()
                == connection.sample_read_latencies.size()) {
            IndexCoverage complete;
            bool first_source = true;
            for (std::size_t channel = 0;
                 channel < connection.sample_source_port_by_channel.size();
                 ++channel) {
                auto const source =
                    connection.sample_source_port_by_channel[channel];
                if (!source) continue;
                auto available = shifted_forward(
                    candidate_output_coverages_[output_index(*source)],
                    connection.sample_read_latencies[channel]);
                complete = first_source
                    ? std::move(available)
                    : complete & available;
                first_source = false;
                used_exact_routes = true;
            }
            if (!first_source) result.include(complete);
        } else if (connection.kind == PortKind::event) {
            IndexCoverage complete;
            bool first_source = true;
            for (auto const& delivery : connection.event_deliveries) {
                if (delivery.target_port != input || !delivery.source_port)
                    continue;
                auto const& available = candidate_output_coverages_[
                    output_index(*delivery.source_port)];
                complete = first_source
                    ? available
                    : complete & available;
                first_source = false;
                used_exact_routes = true;
            }
            if (!first_source) result.include(complete);
        }
        // Component tests can construct port-granular plans without the
        // data-specific route records retained by connection analysis.
        if (!used_exact_routes) {
            for (auto const source : connection.source_coverage_ports) {
                result.include(candidate_output_coverages_[
                    output_index(source)]);
            }
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
    graph_jit::BackgroundPortIndex input, IndexCoverage const& changed,
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
    graph_jit::BackgroundPortIndex output, IndexCoverage const& changed,
    bool coverage_changed)
{
    auto const& planned_output = port(output);
    for (auto const connection_index : planned_output.outgoing_connections) {
        auto const& connection = plan_->connections[connection_index];
        bool used_exact_routes = false;
        if (connection.kind == PortKind::sample
            && connection.sample_target_port
            && connection.sample_source_port_by_channel.size()
                == connection.sample_read_latencies.size()) {
            for (std::size_t channel = 0;
                 channel < connection.sample_source_port_by_channel.size();
                 ++channel) {
                if (connection.sample_source_port_by_channel[channel]
                    != output) {
                    continue;
                }
                add_input_change(
                    *connection.sample_target_port,
                    shifted_forward(
                        changed, connection.sample_read_latencies[channel]),
                    coverage_changed);
                used_exact_routes = true;
            }
        } else if (connection.kind == PortKind::event) {
            for (auto const& delivery : connection.event_deliveries) {
                if (delivery.source_port != output || !delivery.target_port)
                    continue;
                add_input_change(
                    *delivery.target_port, changed, coverage_changed);
                used_exact_routes = true;
            }
        }
        if (!used_exact_routes) {
            for (auto const target : connection.target_coverage_ports) {
                add_input_change(target, changed, coverage_changed);
            }
        }
    }
}

void BackgroundPropagationWorkspace::set_input_change(
    graph_jit::BackgroundPortIndex input, IndexCoverage const& coverage,
    IndexCoverage const& changed)
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
    graph_jit::BackgroundPortIndex output, IndexCoverage const& coverage)
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
    graph_jit::BackgroundPortIndex output, IndexCoverage const& changed)
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
    void* opaque, IndexCoverage const& coverage)
{
    auto& accumulator = *static_cast<OutputChangeAccumulator*>(opaque);
    accumulator.owner->publish_output_coverage(accumulator.port, coverage);
}

void BackgroundPropagationWorkspace::publish_output_change_callback(
    void* opaque, IndexCoverage const& changed)
{
    auto& accumulator = *static_cast<OutputChangeAccumulator*>(opaque);
    accumulator.owner->publish_output_change(accumulator.port, changed);
}

void BackgroundPropagationWorkspace::require_output(
    graph_jit::BackgroundPortIndex output, IndexCoverage const& requested,
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
    graph_jit::BackgroundPortIndex input, IndexCoverage const& requested)
{
    auto const& planned_input = port(input);
    IndexCoverage coverage;
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
        auto require_source = [&](graph_jit::BackgroundPortIndex source,
                                  IndexCoverage source_required) {
            auto const& planned_source = port(source);
            auto const stored_boundary = planned_source.retention
                && *planned_source.retention == OutputRetention::persisted;
            require_output(
                source, source_required, !stored_boundary);
        };
        bool used_exact_routes = false;
        if (connection.kind == PortKind::sample
            && connection.sample_target_port == input
            && connection.sample_source_port_by_channel.size()
                == connection.sample_read_latencies.size()) {
            for (std::size_t channel = 0;
                 channel < connection.sample_source_port_by_channel.size();
                 ++channel) {
                auto const source =
                    connection.sample_source_port_by_channel[channel];
                if (!source) continue;
                require_source(
                    *source,
                    shifted_back(
                        required, connection.sample_read_latencies[channel]));
                used_exact_routes = true;
            }
        } else if (connection.kind == PortKind::event) {
            for (auto const& delivery : connection.event_deliveries) {
                if (delivery.target_port != input || !delivery.source_port)
                    continue;
                require_source(*delivery.source_port, required);
                used_exact_routes = true;
            }
        }
        if (!used_exact_routes) {
            for (auto const source : connection.source_coverage_ports) {
                require_source(source, required);
            }
        }
    }
}

void BackgroundPropagationWorkspace::publish_input_requirement_callback(
    void* opaque, IndexCoverage const& required)
{
    auto& accumulator = *static_cast<InputRequirementAccumulator*>(opaque);
    accumulator.owner->require_input(accumulator.port, required);
}

void BackgroundPropagationWorkspace::replay_forward_coverage(
    void*, ReflectedNodeForwardCoverageContext const& context)
{
    IndexCoverage coverage;
    IndexCoverage changed;
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
    if (context.local_state_changed) {
        changed.include(coverage);
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
    IndexCoverage required;
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

BackgroundCoverageState::Migration
BackgroundCoverageState::prepare_migration_from(
    BackgroundCoverageState& previous,
    graph_jit::BackgroundEvaluationPlan const& current_plan,
    graph_jit::BackgroundEvaluationPlan const& previous_plan,
    bool local_semantic_environment_changed)
{
    if (output_coverages_.size()
            != current_plan.accumulators.output_change_count
        || previous.output_coverages_.size()
            != previous_plan.accumulators.output_change_count) {
        throw std::invalid_argument(
            "background coverage state does not match its compiled plan");
    }

    auto is_output = [](graph_jit::BackgroundPortPlan const& port) {
        return port.direction == graph_jit::PortDirection::output
            && port.stable_identity
            && port.accumulators.output_change;
    };
    auto compatible = [](graph_jit::BackgroundPortPlan const& current,
                         graph_jit::BackgroundPortPlan const& prior) {
        if (current.kind != prior.kind) return false;
        return current.kind == PortKind::sample
            ? current.sample_layout == prior.sample_layout
            : current.event_type == prior.event_type;
    };

    Migration migration;
    migration.current = this;
    migration.previous = &previous;
    migration.current_plan = &current_plan;
    migration.previous_plan = &previous_plan;

    std::map<graph_jit::StableOutputPortId, graph_jit::BackgroundPortIndex>
        current_outputs;
    std::map<graph_jit::StableOutputPortId, graph_jit::BackgroundPortIndex>
        previous_outputs;
    for (graph_jit::BackgroundPortIndex index = 0;
         index < current_plan.ports.size(); ++index) {
        auto const& port = current_plan.ports[index];
        if (is_output(port)
            && !current_outputs.emplace(*port.stable_identity, index).second) {
            throw std::invalid_argument(
                "current background plan has duplicate stable output identity");
        }
    }
    for (graph_jit::BackgroundPortIndex index = 0;
         index < previous_plan.ports.size(); ++index) {
        auto const& port = previous_plan.ports[index];
        if (is_output(port)
            && !previous_outputs.emplace(*port.stable_identity, index).second) {
            throw std::invalid_argument(
                "previous background plan has duplicate stable output identity");
        }
    }
    for (std::size_t current_index = 0;
         current_index < current_plan.ports.size(); ++current_index) {
        auto const& current = current_plan.ports[current_index];
        if (!is_output(current)) continue;
        auto const found = previous_outputs.find(*current.stable_identity);
        if (found == previous_outputs.end()) continue;
        auto const& matched = previous_plan.ports[found->second];
        if (!compatible(current, matched)) continue;
        if (*current.accumulators.output_change >= output_coverages_.size()
            || *matched.accumulators.output_change
                >= previous.output_coverages_.size()) {
            throw std::invalid_argument(
                "background stable output has an invalid coverage accumulator");
        }
        migration.outputs.push_back({
            .current = *current.accumulators.output_change,
            .previous = *matched.accumulators.output_change,
        });
    }

    std::map<graph_jit::StableConcreteNodeId, graph_jit::BackgroundNodeIndex>
        current_nodes;
    std::map<graph_jit::StableConcreteNodeId, graph_jit::BackgroundNodeIndex>
        previous_nodes;
    for (graph_jit::BackgroundNodeIndex index = 0;
         index < current_plan.nodes.size(); ++index) {
        auto const& node = current_plan.nodes[index];
        if (node.stable_identity
            && !current_nodes.emplace(*node.stable_identity, index).second) {
            throw std::invalid_argument(
                "current background plan has duplicate stable node identity");
        }
    }
    for (graph_jit::BackgroundNodeIndex index = 0;
         index < previous_plan.nodes.size(); ++index) {
        auto const& node = previous_plan.nodes[index];
        if (node.stable_identity
            && !previous_nodes.emplace(*node.stable_identity, index).second) {
            throw std::invalid_argument(
                "previous background plan has duplicate stable node identity");
        }
    }
    for (std::size_t current_index = 0;
         current_index < current_plan.nodes.size(); ++current_index) {
        auto const& current = current_plan.nodes[current_index];
        graph_jit::BackgroundNodePlan const* matched = nullptr;
        if (current.stable_identity) {
            auto const found = previous_nodes.find(*current.stable_identity);
            if (found != previous_nodes.end()) {
                matched = &previous_plan.nodes[found->second];
            }
        }
        auto const unchanged = matched
            && node_local_semantics_equal(
                current_plan,
                current,
                previous_plan,
                *matched,
                local_semantic_environment_changed);
        if (!unchanged) {
            migration.reconciliation_roots.locally_changed_nodes.push_back(
                current_index);
        }
    }

    std::map<graph_jit::StableInputPortId, graph_jit::BackgroundPortIndex>
        current_inputs;
    std::map<graph_jit::StableInputPortId, graph_jit::BackgroundPortIndex>
        previous_inputs;
    auto index_inputs = [](auto const& plan, auto& indexed, char const* error) {
        for (graph_jit::BackgroundPortIndex index = 0;
             index < plan.ports.size(); ++index) {
            auto const& port = plan.ports[index];
            if (port.direction == graph_jit::PortDirection::input
                && port.stable_input_identity
                && !indexed.emplace(
                    *port.stable_input_identity, index).second) {
                throw std::invalid_argument(error);
            }
        }
    };
    index_inputs(
        current_plan,
        current_inputs,
        "current background plan has duplicate stable input identity");
    index_inputs(
        previous_plan,
        previous_inputs,
        "previous background plan has duplicate stable input identity");
    for (graph_jit::BackgroundPortIndex current_index = 0;
         current_index < current_plan.ports.size(); ++current_index) {
        auto const& current = current_plan.ports[current_index];
        if (current.direction != graph_jit::PortDirection::input
            || !current.stable_input_identity
            || !current.accumulators.input_change) {
            continue;
        }
        auto const found = previous_inputs.find(*current.stable_input_identity);
        if (found == previous_inputs.end()
            || !previous_plan.ports[found->second].accumulators.input_change) {
            continue;
        }
        if (!input_connections_equal(
                current_plan, current_index, previous_plan, found->second)) {
            migration.changed_inputs.push_back({
                .current = current_index,
                .previous = found->second,
            });
        }
    }
    return migration;
}

void BackgroundCoverageState::Migration::commit()
{
    if (committed || !current || !previous) {
        throw std::logic_error(
            "invalid or already committed background coverage migration");
    }
    for (auto const& output : outputs) {
        current->output_coverages_[output.current] =
            previous->output_coverages_[output.previous];
    }
    for (auto const& input : changed_inputs) {
        auto current_coverage = input_coverage(
            *current_plan, current->output_coverages_, input.current);
        auto previous_coverage = input_coverage(
            *previous_plan, previous->output_coverages_, input.previous);
        auto changed = current_coverage | previous_coverage;
        reconciliation_roots.input_changes.push_back({
            .port = input.current,
            .coverage = std::move(current_coverage),
            .changed = std::move(changed),
        });
    }
    committed = true;
}

CoveragePropagationRequest
BackgroundCoverageState::Migration::take_reconciliation_roots()
{
    if (!committed) {
        throw std::logic_error(
            "background coverage reconciliation roots are not committed");
    }
    return std::move(reconciliation_roots);
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
    CompiledGraphBackgroundOperations const& operations,
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

    operations.propagate_forward(&call_);

    BackgroundForwardOutputChanges const forward{
        .data = this,
        .changed_value = +[](void const* opaque,
                             graph_jit::BackgroundPortIndex output)
            -> IndexCoverage const& {
            auto const& workspace =
                *static_cast<BackgroundPropagationWorkspace const*>(opaque);
            return workspace.output_changes_[
                workspace.output_index(output)].changed;
        },
    };
    for (auto const& root : request.input_demands) {
        if (!expansion.restrict_input) {
            require_input(root.port, root.required);
            continue;
        }
        auto required = root.required;
        expansion.restrict_input(expansion.data, root.port, forward, required);
        require_input(root.port, required);
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
            IndexCoverage additional;
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
    operations.propagate_reverse(&call_);

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
