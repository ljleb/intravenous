#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>

#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {
namespace {

IndexedCoverage coverage_difference(
    IndexedCoverage const& left,
    IndexedCoverage const& right)
{
    return (left - right) | (right - left);
}

class IndexedPropagationWorkspace {
    struct InputChangeAccumulator {
        IndexedCoverage coverage{};
        IndexedCoverage changed{};
    };

    struct OutputChangeAccumulator {
        IndexedPropagationWorkspace* owner = nullptr;
        graph_jit::IndexedEndpointOrdinal endpoint = 0;
        IndexedCoverage previous_coverage{};
        IndexedCoverage changed{};
        bool touched = false;
    };

    struct InputRequirementAccumulator {
        IndexedPropagationWorkspace* owner = nullptr;
        graph_jit::IndexedEndpointOrdinal endpoint = 0;
        IndexedCoverage coverage{};
    };

    struct OutputRequirementAccumulator {
        IndexedCoverage required{};
    };

    struct NodeBindingStorage {
        std::vector<IndexedInputChange> sample_input_changes{};
        std::vector<IndexedInputChange> event_input_changes{};
        std::vector<IndexedOutputChange> sample_output_changes{};
        std::vector<IndexedOutputChange> event_output_changes{};
        std::vector<IndexedInputRequirement> sample_input_requirements{};
        std::vector<IndexedInputRequirement> event_input_requirements{};
        std::vector<IndexedOutputRequirement> sample_output_requirements{};
        std::vector<IndexedOutputRequirement> event_output_requirements{};
    };

    graph_jit::IndexedPlan const& plan_;
    std::size_t sample_rate_ = 0;
    std::vector<IndexedCoverage> propagated_output_coverages_{};
    std::vector<IndexedCoverage> candidate_output_coverages_{};
    std::vector<InputChangeAccumulator> input_changes_{};
    std::vector<OutputChangeAccumulator> output_changes_{};
    std::vector<InputRequirementAccumulator> input_requirements_{};
    std::vector<OutputRequirementAccumulator> output_requirements_{};
    std::vector<IndexedCoverage> input_requirements_by_endpoint_{};
    std::vector<IndexedInputChange> input_change_bindings_{};
    std::vector<IndexedOutputChange> output_change_bindings_{};
    std::vector<IndexedInputRequirement> input_requirement_bindings_{};
    std::vector<IndexedOutputRequirement> output_requirement_bindings_{};
    std::vector<NodeBindingStorage> node_bindings_{};
    std::vector<graph_jit::IndexedNodeBatchFrame> node_frames_{};
    graph_jit::IndexedBatchFrame batch_{};

    [[noreturn]] static void invalid_plan(char const* message)
    {
        throw std::invalid_argument(message);
    }

    [[nodiscard]] graph_jit::IndexedEndpointPlan const& endpoint(
        graph_jit::IndexedEndpointOrdinal ordinal) const
    {
        if (ordinal >= plan_.endpoints.size()) {
            throw std::out_of_range(
                "GraphExecutor indexed endpoint ordinal is out of range");
        }
        return plan_.endpoints[ordinal];
    }

    [[nodiscard]] std::size_t output_slot(
        graph_jit::IndexedEndpointOrdinal ordinal) const
    {
        auto const& planned = endpoint(ordinal);
        if (planned.direction != graph_jit::IndexedEndpointDirection::output
            || !planned.accumulators.output_change
            || !planned.accumulators.output_requirement) {
            throw std::invalid_argument(
                "GraphExecutor indexed root does not name an output endpoint");
        }
        return *planned.accumulators.output_change;
    }

    [[nodiscard]] std::size_t input_slot(
        graph_jit::IndexedEndpointOrdinal ordinal) const
    {
        auto const& planned = endpoint(ordinal);
        if (planned.direction != graph_jit::IndexedEndpointDirection::input
            || !planned.accumulators.input_change
            || !planned.accumulators.input_requirement) {
            throw std::invalid_argument(
                "GraphExecutor indexed propagation reached an input without accumulators");
        }
        return *planned.accumulators.input_change;
    }

    [[nodiscard]] IndexedCoverage aggregate_input_coverage(
        graph_jit::IndexedEndpointOrdinal input) const
    {
        IndexedCoverage result;
        auto const& planned_input = endpoint(input);
        for (auto const connection_ordinal : planned_input.incoming_connections) {
            if (connection_ordinal >= plan_.connections.size()) {
                invalid_plan(
                    "GraphExecutor indexed input references a missing connection");
            }
            auto const& connection = plan_.connections[connection_ordinal];
            for (auto const source : connection.source_endpoints) {
                auto const slot = output_slot(source);
                if (slot >= candidate_output_coverages_.size()) {
                    invalid_plan(
                        "GraphExecutor indexed output coverage slot is out of range");
                }
                result.include(candidate_output_coverages_[slot]);
            }
        }
        return result;
    }

    void mark_node_activity(
        graph_jit::IndexedNodeOrdinal node,
        graph_jit::IndexedNodeBatchActivity activity)
    {
        if (node >= node_frames_.size()) {
            invalid_plan(
                "GraphExecutor indexed endpoint references a missing node");
        }
        node_frames_[node].activity = node_frames_[node].activity | activity;
    }

    void add_input_change(
        graph_jit::IndexedEndpointOrdinal input,
        IndexedCoverage const& changed,
        bool recompute_coverage)
    {
        auto const& planned = endpoint(input);
        if (!planned.accumulators.input_change) return;
        auto const slot = input_slot(input);
        auto& accumulator = input_changes_[slot];
        if (recompute_coverage) {
            auto const next = aggregate_input_coverage(input);
            accumulator.changed.include(
                coverage_difference(accumulator.coverage, next));
            accumulator.coverage = next;
            auto const requirement_slot =
                *planned.accumulators.input_requirement;
            input_requirements_[requirement_slot].coverage = next;
        }
        accumulator.changed.include(changed);
        if (!changed.empty() || !accumulator.changed.empty()) {
            mark_node_activity(
                planned.node,
                graph_jit::IndexedNodeBatchActivity::forward);
        }
    }

    void route_output_change(
        graph_jit::IndexedEndpointOrdinal output,
        IndexedCoverage const& changed,
        bool coverage_changed)
    {
        auto const& planned_output = endpoint(output);
        for (auto const connection_ordinal : planned_output.outgoing_connections) {
            if (connection_ordinal >= plan_.connections.size()) {
                invalid_plan(
                    "GraphExecutor indexed output references a missing connection");
            }
            auto const& connection = plan_.connections[connection_ordinal];
            for (auto const target : connection.target_endpoints) {
                add_input_change(target, changed, coverage_changed);
            }
        }
    }

    void seed_input_change(
        graph_jit::IndexedEndpointOrdinal input,
        IndexedCoverage const& coverage,
        IndexedCoverage const& changed)
    {
        auto const& planned = endpoint(input);
        auto const slot = input_slot(input);
        auto& accumulator = input_changes_[slot];
        accumulator.changed.include(
            coverage_difference(accumulator.coverage, coverage));
        accumulator.changed.include(changed);
        accumulator.coverage = coverage;
        auto const requirement_slot =
            *planned.accumulators.input_requirement;
        input_requirements_[requirement_slot].coverage = coverage;
        if (!accumulator.changed.empty()) {
            mark_node_activity(
                planned.node,
                graph_jit::IndexedNodeBatchActivity::forward);
        }
    }

    void publish_output_coverage(
        graph_jit::IndexedEndpointOrdinal output,
        IndexedCoverage const& coverage)
    {
        auto const slot = output_slot(output);
        if (slot >= candidate_output_coverages_.size()) {
            invalid_plan(
                "GraphExecutor indexed output coverage slot is out of range");
        }
        auto& accumulator = output_changes_[slot];
        auto const changed = coverage_difference(
            candidate_output_coverages_[slot], coverage);
        candidate_output_coverages_[slot] = coverage;
        accumulator.touched = accumulator.touched || !changed.empty();
        accumulator.changed.include(changed);
        route_output_change(output, changed, true);
    }

    void publish_output_change(
        graph_jit::IndexedEndpointOrdinal output,
        IndexedCoverage const& changed)
    {
        if (changed.empty()) return;
        auto const slot = output_slot(output);
        auto& accumulator = output_changes_[slot];
        accumulator.touched = true;
        accumulator.changed.include(changed);
        route_output_change(output, changed, false);
    }

    static void publish_output_coverage_thunk(
        void* opaque,
        IndexedCoverage const& coverage)
    {
        auto& accumulator = *static_cast<OutputChangeAccumulator*>(opaque);
        accumulator.owner->publish_output_coverage(
            accumulator.endpoint, coverage);
    }

    static void publish_output_change_thunk(
        void* opaque,
        IndexedCoverage const& changed)
    {
        auto& accumulator = *static_cast<OutputChangeAccumulator*>(opaque);
        accumulator.owner->publish_output_change(
            accumulator.endpoint, changed);
    }

    void require_output(
        graph_jit::IndexedEndpointOrdinal output,
        IndexedCoverage const& requested,
        bool activate_producer)
    {
        auto const& planned = endpoint(output);
        auto const change_slot = output_slot(output);
        auto const requirement_slot = *planned.accumulators.output_requirement;
        if (requirement_slot >= output_requirements_.size()) {
            invalid_plan(
                "GraphExecutor indexed output requirement slot is out of range");
        }
        auto const required = requested & candidate_output_coverages_[change_slot];
        if (required.empty()) return;
        output_requirements_[requirement_slot].required.include(required);
        if (activate_producer) {
            mark_node_activity(
                planned.node,
                graph_jit::IndexedNodeBatchActivity::reverse);
        }
    }

    void require_input(
        graph_jit::IndexedEndpointOrdinal input,
        IndexedCoverage const& requested)
    {
        auto const& planned_input = endpoint(input);
        if (planned_input.direction
            != graph_jit::IndexedEndpointDirection::input) {
            throw std::invalid_argument(
                "GraphExecutor indexed input demand does not name an input endpoint");
        }
        IndexedCoverage coverage;
        InputRequirementAccumulator* accumulator = nullptr;
        if (planned_input.accumulators.input_requirement) {
            auto const slot = *planned_input.accumulators.input_requirement;
            if (slot >= input_requirements_.size()) {
                invalid_plan(
                    "GraphExecutor indexed input requirement slot is out of range");
            }
            accumulator = &input_requirements_[slot];
            coverage = accumulator->coverage;
        } else {
            coverage = aggregate_input_coverage(input);
        }
        auto const required = requested & coverage;
        if (required.empty()) return;
        input_requirements_by_endpoint_[input].include(required);

        for (auto const connection_ordinal : planned_input.incoming_connections) {
            if (connection_ordinal >= plan_.connections.size()) {
                invalid_plan(
                    "GraphExecutor indexed input references a missing connection");
            }
            auto const& connection = plan_.connections[connection_ordinal];
            for (auto const source : connection.source_endpoints) {
                auto const& planned_source = endpoint(source);
                auto const stored_boundary = planned_source.retention
                    && *planned_source.retention == OutputRetention::persisted;
                require_output(source, required, !stored_boundary);
            }
        }
    }

    static void publish_input_requirement_thunk(
        void* opaque,
        IndexedCoverage const& required)
    {
        auto& accumulator = *static_cast<InputRequirementAccumulator*>(opaque);
        accumulator.owner->require_input(accumulator.endpoint, required);
    }

    static void synthesized_forward(
        void*,
        ReflectedNodeForwardCoverageContext const& context)
    {
        IndexedCoverage coverage;
        IndexedCoverage changed;
        for (auto const& input :
             static_cast<std::span<IndexedInputChange const>>(context.inputs)) {
            coverage.include(input.coverage());
            changed.include(input.changed());
        }
        for (auto const& input :
             static_cast<std::span<IndexedInputChange const>>(
                 context.event_inputs)) {
            coverage.include(input.coverage());
            changed.include(input.changed());
        }
        for (auto const& output :
             static_cast<std::span<IndexedOutputChange>>(context.outputs)) {
            output.publish_coverage(coverage);
            output.change(changed);
        }
        for (auto const& output :
             static_cast<std::span<IndexedOutputChange>>(
                 context.event_outputs)) {
            output.publish_coverage(coverage);
            output.change(changed);
        }
    }

    static void synthesized_reverse(
        void*,
        ReflectedNodeReverseCoverageContext const& context)
    {
        IndexedCoverage required;
        for (auto const& output :
             static_cast<std::span<IndexedOutputRequirement const>>(
                 context.outputs)) {
            required.include(output.required());
        }
        for (auto const& output :
             static_cast<std::span<IndexedOutputRequirement const>>(
                 context.event_outputs)) {
            required.include(output.required());
        }
        for (auto const& input :
             static_cast<std::span<IndexedInputRequirement>>(context.inputs)) {
            input.require(required & input.coverage());
        }
        for (auto const& input :
             static_cast<std::span<IndexedInputRequirement>>(
                 context.event_inputs)) {
            input.require(required & input.coverage());
        }
    }

    void initialize_bindings()
    {
        std::vector<bool> input_change_slots_seen(input_changes_.size(), false);
        std::vector<bool> output_change_slots_seen(output_changes_.size(), false);
        std::vector<bool> input_requirement_slots_seen(
            input_requirements_.size(), false);
        std::vector<bool> output_requirement_slots_seen(
            output_requirements_.size(), false);
        for (graph_jit::IndexedEndpointOrdinal ordinal = 0;
             ordinal < plan_.endpoints.size(); ++ordinal) {
            auto const& planned = plan_.endpoints[ordinal];
            if (planned.direction == graph_jit::IndexedEndpointDirection::input
                && planned.accumulators.input_change) {
                if (!planned.accumulators.input_requirement) invalid_plan(
                    "GraphExecutor indexed input has an incomplete accumulator plan");
                auto const change_slot = *planned.accumulators.input_change;
                auto const requirement_slot =
                    *planned.accumulators.input_requirement;
                if (change_slot >= input_changes_.size()
                    || requirement_slot >= input_requirements_.size()) {
                    invalid_plan(
                        "GraphExecutor indexed input accumulator is out of range");
                }
                if (input_change_slots_seen[change_slot]
                    || input_requirement_slots_seen[requirement_slot]) {
                    invalid_plan(
                        "GraphExecutor indexed input accumulator slot is duplicated");
                }
                input_change_slots_seen[change_slot] = true;
                input_requirement_slots_seen[requirement_slot] = true;
                auto& requirement = input_requirements_[requirement_slot];
                requirement.owner = this;
                requirement.endpoint = ordinal;
                input_change_bindings_[change_slot] = IndexedInputChange{
                    .coverage_value = &input_changes_[change_slot].coverage,
                    .changed_value = &input_changes_[change_slot].changed,
                };
                input_requirement_bindings_[requirement_slot] =
                    IndexedInputRequirement{
                        .data = &requirement,
                        .coverage_value = &requirement.coverage,
                        .publish_required_value =
                            &publish_input_requirement_thunk,
                    };
            } else if (
                planned.direction == graph_jit::IndexedEndpointDirection::output) {
                if (!planned.accumulators.output_change
                    || !planned.accumulators.output_requirement) {
                    invalid_plan(
                        "GraphExecutor indexed output has an incomplete accumulator plan");
                }
                auto const change_slot = *planned.accumulators.output_change;
                auto const requirement_slot =
                    *planned.accumulators.output_requirement;
                if (change_slot >= output_changes_.size()
                    || requirement_slot >= output_requirements_.size()) {
                    invalid_plan(
                        "GraphExecutor indexed output accumulator is out of range");
                }
                if (output_change_slots_seen[change_slot]
                    || output_requirement_slots_seen[requirement_slot]) {
                    invalid_plan(
                        "GraphExecutor indexed output accumulator slot is duplicated");
                }
                output_change_slots_seen[change_slot] = true;
                output_requirement_slots_seen[requirement_slot] = true;
                auto& change = output_changes_[change_slot];
                change.owner = this;
                change.endpoint = ordinal;
                output_change_bindings_[change_slot] = IndexedOutputChange{
                    .data = &change,
                    .previous_coverage_value = &change.previous_coverage,
                    .publish_coverage_value = &publish_output_coverage_thunk,
                    .publish_changed_value = &publish_output_change_thunk,
                };
                output_requirement_bindings_[requirement_slot] =
                    IndexedOutputRequirement{
                        .required_value =
                            &output_requirements_[requirement_slot].required,
                    };
            }
        }
        auto const all_seen = [](std::vector<bool> const& values) {
            for (bool const value : values) {
                if (!value) return false;
            }
            return true;
        };
        if (!all_seen(input_change_slots_seen)
            || !all_seen(output_change_slots_seen)
            || !all_seen(input_requirement_slots_seen)
            || !all_seen(output_requirement_slots_seen)) {
            invalid_plan(
                "GraphExecutor indexed accumulator plan contains an unbound slot");
        }

        for (graph_jit::IndexedNodeOrdinal ordinal = 0;
             ordinal < plan_.nodes.size(); ++ordinal) {
            auto const& node = plan_.nodes[ordinal];
            if (node.authored_tock_execution
                && node.synthesized_tick_replay) {
                invalid_plan(
                    "GraphExecutor indexed node cannot use authored Tock and synthesized replay together");
            }

            auto& bindings = node_bindings_[ordinal];
            std::size_t planned_input_count = 0;
            for (auto const endpoint_ordinal : node.inputs) {
                auto const& planned = endpoint(endpoint_ordinal);
                if (planned.accumulators.input_change) ++planned_input_count;
                auto const callback_input = node.authored_tock_execution
                    ? planned.random_access_input
                    : node.synthesized_tick_replay
                        ? planned.replay_sequential_input
                        : false;
                if (!callback_input) continue;
                if (!planned.accumulators.input_change
                    || !planned.accumulators.input_requirement) {
                    invalid_plan(
                        "GraphExecutor indexed callback input has no accumulator slots");
                }
                auto const change_slot = *planned.accumulators.input_change;
                auto const requirement_slot =
                    *planned.accumulators.input_requirement;
                if (planned.kind == PortKind::sample) {
                    bindings.sample_input_changes.push_back(
                        input_change_bindings_[change_slot]);
                    bindings.sample_input_requirements.push_back(
                        input_requirement_bindings_[requirement_slot]);
                } else {
                    bindings.event_input_changes.push_back(
                        input_change_bindings_[change_slot]);
                    bindings.event_input_requirements.push_back(
                        input_requirement_bindings_[requirement_slot]);
                }
            }

            std::size_t planned_output_count = 0;
            for (auto const endpoint_ordinal : node.outputs) {
                auto const& planned = endpoint(endpoint_ordinal);
                if (planned.accumulators.output_change) ++planned_output_count;
                auto const callback_output = node.authored_tock_execution
                    ? planned.authored_tock_output
                    : node.synthesized_tick_replay
                        ? planned.replayed_tick_output
                        : false;
                if (!callback_output) continue;
                if (!planned.accumulators.output_change
                    || !planned.accumulators.output_requirement) {
                    invalid_plan(
                        "GraphExecutor indexed callback output has no accumulator slots");
                }
                auto const change_slot = *planned.accumulators.output_change;
                auto const requirement_slot =
                    *planned.accumulators.output_requirement;
                if (planned.kind == PortKind::sample) {
                    bindings.sample_output_changes.push_back(
                        output_change_bindings_[change_slot]);
                    bindings.sample_output_requirements.push_back(
                        output_requirement_bindings_[requirement_slot]);
                } else {
                    bindings.event_output_changes.push_back(
                        output_change_bindings_[change_slot]);
                    bindings.event_output_requirements.push_back(
                        output_requirement_bindings_[requirement_slot]);
                }
            }

            if (planned_input_count != node.accumulators.input_change_count
                || planned_input_count
                    != node.accumulators.input_requirement_count
                || planned_output_count != node.accumulators.output_change_count
                || planned_output_count
                    != node.accumulators.output_requirement_count) {
                invalid_plan(
                    "GraphExecutor indexed node accumulator counts do not match its endpoints");
            }

            auto& frame = node_frames_[ordinal];
            frame.forward = ReflectedNodeForwardCoverageContext{
                .inputs = bindings.sample_input_changes,
                .outputs = bindings.sample_output_changes,
                .event_inputs = bindings.event_input_changes,
                .event_outputs = bindings.event_output_changes,
                .sample_rate = sample_rate_,
            };
            frame.reverse = ReflectedNodeReverseCoverageContext{
                .inputs = bindings.sample_input_requirements,
                .outputs = bindings.sample_output_requirements,
                .event_inputs = bindings.event_input_requirements,
                .event_outputs = bindings.event_output_requirements,
                .sample_rate = sample_rate_,
            };
            if (node.synthesized_forward_coverage) {
                frame.synthesized_forward = &synthesized_forward;
            }
            if (node.synthesized_reverse_coverage) {
                frame.synthesized_reverse = &synthesized_reverse;
            }
        }
        batch_.nodes = node_frames_;
    }

    void reset()
    {
        for (std::size_t slot = 0;
             slot < candidate_output_coverages_.size(); ++slot) {
            candidate_output_coverages_[slot] =
                propagated_output_coverages_[slot];
            output_changes_[slot].previous_coverage =
                propagated_output_coverages_[slot];
        }
        for (auto& value : input_changes_) value = {};
        for (auto& value : output_changes_) {
            value.changed = {};
            value.touched = false;
        }
        for (auto& value : input_requirements_) {
            value.coverage = {};
        }
        for (auto& value : output_requirements_) value = {};
        for (auto& value : input_requirements_by_endpoint_) value = {};
        for (auto& frame : node_frames_) {
            frame.activity = graph_jit::IndexedNodeBatchActivity::none;
            frame.forward.local_state_changed = false;
        }

        for (graph_jit::IndexedEndpointOrdinal ordinal = 0;
             ordinal < plan_.endpoints.size(); ++ordinal) {
            auto const& planned = plan_.endpoints[ordinal];
            if (planned.direction != graph_jit::IndexedEndpointDirection::input
                || !planned.accumulators.input_change) {
                continue;
            }
            auto const change_slot = input_slot(ordinal);
            auto const requirement_slot = *planned.accumulators.input_requirement;
            auto coverage = aggregate_input_coverage(ordinal);
            input_changes_[change_slot].coverage = coverage;
            input_requirements_[requirement_slot].coverage = std::move(coverage);
        }
    }

public:
    IndexedPropagationWorkspace(
        graph_jit::IndexedPlan const& plan,
        std::size_t sample_rate)
        : plan_(plan)
        , sample_rate_(sample_rate)
        , propagated_output_coverages_(plan.accumulators.output_change_count)
        , candidate_output_coverages_(plan.accumulators.output_change_count)
        , input_changes_(plan.accumulators.input_change_count)
        , output_changes_(plan.accumulators.output_change_count)
        , input_requirements_(plan.accumulators.input_requirement_count)
        , output_requirements_(plan.accumulators.output_requirement_count)
        , input_requirements_by_endpoint_(plan.endpoints.size())
        , input_change_bindings_(plan.accumulators.input_change_count)
        , output_change_bindings_(plan.accumulators.output_change_count)
        , input_requirement_bindings_(plan.accumulators.input_requirement_count)
        , output_requirement_bindings_(plan.accumulators.output_requirement_count)
        , node_bindings_(plan.nodes.size())
        , node_frames_(plan.nodes.size())
    {
        if (!plan.empty() && sample_rate == 0) {
            invalid_plan(
                "GraphExecutor indexed plan requires a non-zero sample rate");
        }
        initialize_bindings();
        reset();
    }

    IndexedPropagationWorkspace(IndexedPropagationWorkspace const&) = delete;
    IndexedPropagationWorkspace& operator=(IndexedPropagationWorkspace const&) = delete;

    [[nodiscard]] GraphExecutorIndexedPropagationResult run(
        CompiledGraphIndexedOperations const& operations,
        std::byte* storage,
        GraphExecutorIndexedPropagationRequest const& request)
    {
        reset();
        for (auto const node : request.locally_changed_nodes) {
            if (node >= node_frames_.size()) {
                throw std::out_of_range(
                    "GraphExecutor indexed node ordinal is out of range");
            }
            node_frames_[node].forward.local_state_changed = true;
            mark_node_activity(
                node, graph_jit::IndexedNodeBatchActivity::forward);
        }
        for (auto const& root : request.output_changes) {
            publish_output_coverage(root.endpoint, root.coverage);
            publish_output_change(root.endpoint, root.changed);
        }
        for (auto const& root : request.input_changes) {
            seed_input_change(root.endpoint, root.coverage, root.changed);
        }

        operations.propagate_forward(storage, &batch_);

        for (auto const& root : request.input_demands) {
            require_input(root.endpoint, root.required);
        }
        for (auto const& root : request.output_demands) {
            require_output(root.endpoint, root.required, true);
        }
        operations.propagate_reverse(storage, &batch_);

        GraphExecutorIndexedPropagationResult result;
        result.output_changes.reserve(output_changes_.size());
        for (std::size_t slot = 0; slot < output_changes_.size(); ++slot) {
            auto const& change = output_changes_[slot];
            if (!change.touched) continue;
            result.output_changes.push_back(GraphExecutorIndexedOutputChange{
                .endpoint = change.endpoint,
                .coverage = candidate_output_coverages_[slot],
                .changed = change.changed,
            });
        }
        result.input_requirements.reserve(plan_.endpoints.size());
        for (graph_jit::IndexedEndpointOrdinal ordinal = 0;
             ordinal < plan_.endpoints.size(); ++ordinal) {
            auto const& required = input_requirements_by_endpoint_[ordinal];
            if (required.empty()) continue;
            result.input_requirements.push_back(
                GraphExecutorIndexedInputRequirement{
                    .endpoint = ordinal,
                    .required = required,
                });
        }
        result.output_requirements.reserve(output_requirements_.size());
        for (graph_jit::IndexedEndpointOrdinal ordinal = 0;
             ordinal < plan_.endpoints.size(); ++ordinal) {
            auto const& planned = plan_.endpoints[ordinal];
            if (planned.direction != graph_jit::IndexedEndpointDirection::output
                || !planned.accumulators.output_requirement) {
                continue;
            }
            auto const slot = *planned.accumulators.output_requirement;
            auto const& required = output_requirements_[slot].required;
            if (required.empty()) continue;
            result.output_requirements.push_back(
                GraphExecutorIndexedOutputRequirement{
                    .endpoint = ordinal,
                    .required = required,
                });
        }

        propagated_output_coverages_.swap(candidate_output_coverages_);
        return result;
    }
};

void validate_compiled_graph(CompiledGraph const& graph)
{
    if (!graph.root_operations.valid()) {
        throw std::invalid_argument(
            "GraphExecutor requires a valid compiled root operation");
    }
    if (graph.specialization.block_size == 0) {
        throw std::invalid_argument(
            "GraphExecutor requires a non-zero compiled block size");
    }
    if (graph.node_layout.max_block_size == 0
        || graph.node_layout.max_block_size > graph.specialization.block_size) {
        throw std::invalid_argument(
            "GraphExecutor compiled NodeLayout block size is invalid");
    }
    if (!graph.indexed_plan.empty() && !graph.indexed_operations.valid()) {
        throw std::invalid_argument(
            "GraphExecutor indexed plan requires valid compiled indexed operations");
    }
}

} // namespace

class GraphExecutor::Impl {
    struct Realization {
        std::shared_ptr<CompiledGraph const> graph{};
        NodeStorage storage{};
        IndexedPropagationWorkspace indexed_workspace;

        Realization(
            std::shared_ptr<CompiledGraph const> graph_,
            ResourceContext const& resources)
            : graph(std::move(graph_))
            , storage(graph->node_layout.create_storage(resources))
            , indexed_workspace(
                graph->indexed_plan, graph->specialization.sample_rate)
        {}
    };

    struct PendingRealization {
        std::unique_ptr<Realization> realization{};
        bool initialized = false;
    };

public:
    // NodeStorage retains a pointer to this value, so it lives in the stable
    // heap-allocated Impl rather than directly inside movable GraphExecutor.
    ResourceContext resources{};
    std::unique_ptr<Realization> active{};
    std::optional<PendingRealization> pending{};

    explicit Impl(ResourceContext resources_)
        : resources(std::move(resources_))
    {}

    GraphExecutorStageResult stage(
        std::shared_ptr<CompiledGraph const> compiled_graph)
    {
        if (!compiled_graph) {
            throw std::invalid_argument(
                "GraphExecutor cannot stage an empty compiled graph");
        }
        validate_compiled_graph(*compiled_graph);

        auto const newest_generation = pending
            ? pending->realization->graph->project_generation
            : active
                ? active->graph->project_generation
                : std::uint64_t{0};
        if ((active || pending)
            && compiled_graph->project_generation <= newest_generation) {
            return GraphExecutorStageResult::ignored_stale;
        }

        PendingRealization candidate{
            .realization = std::make_unique<Realization>(
                std::move(compiled_graph), resources),
        };
        if (!active) {
            candidate.realization->storage.initialize();
            candidate.initialized = true;
        }
        pending = std::move(candidate);
        return GraphExecutorStageResult::staged;
    }

    bool activate_pending()
    {
        if (!pending) return false;
        if (!pending->initialized) {
            if (!active) {
                pending->realization->storage.initialize();
            } else {
                auto migration =
                    pending->realization->storage.prepare_migration_from(
                        active->storage);
                migration.commit();
            }
            pending->initialized = true;
        }

        auto previous = std::move(active);
        active = std::move(pending->realization);
        pending.reset();
        // Keep the previous code/layout alive through migration commit and
        // release. Its realization is destroyed only after publication.
        previous.reset();
        return true;
    }

    void tick_block(std::size_t sample_index, std::size_t block_size)
    {
        if (!active) {
            throw std::logic_error(
                "GraphExecutor cannot execute without an active generation");
        }
        if (block_size == 0
            || block_size > active->graph->specialization.block_size) {
            throw std::invalid_argument(
                "GraphExecutor tick block size is outside the compiled specialization");
        }
        active->graph->root_operations.tick_block(
            active->storage.buffer().data(), sample_index, block_size);
    }

    [[nodiscard]] GraphExecutorIndexedPropagationResult propagate_indexed(
        GraphExecutorIndexedPropagationRequest const& request)
    {
        if (!active) {
            throw std::logic_error(
                "GraphExecutor cannot propagate indexed work without an active generation");
        }
        if (active->graph->indexed_plan.empty()) {
            if (!request.locally_changed_nodes.empty()
                || !request.input_changes.empty()
                || !request.output_changes.empty()
                || !request.input_demands.empty()
                || !request.output_demands.empty()) {
                throw std::invalid_argument(
                    "GraphExecutor indexed roots require an active indexed plan");
            }
            return {};
        }
        return active->indexed_workspace.run(
            active->graph->indexed_operations,
            active->storage.buffer().data(),
            request);
    }
};

GraphExecutor::GraphExecutor(ResourceContext resources)
    : impl_(std::make_unique<Impl>(std::move(resources)))
{}

GraphExecutor::~GraphExecutor() = default;
GraphExecutor::GraphExecutor(GraphExecutor&&) noexcept = default;
GraphExecutor& GraphExecutor::operator=(GraphExecutor&&) noexcept = default;

GraphExecutorStageResult GraphExecutor::stage(
    std::shared_ptr<CompiledGraph const> compiled_graph)
{
    return impl_->stage(std::move(compiled_graph));
}

bool GraphExecutor::activate_pending()
{
    return impl_->activate_pending();
}

std::optional<std::uint64_t> GraphExecutor::active_generation() const noexcept
{
    if (!impl_->active) return std::nullopt;
    return impl_->active->graph->project_generation;
}

std::optional<std::uint64_t> GraphExecutor::pending_generation() const noexcept
{
    if (!impl_->pending) return std::nullopt;
    return impl_->pending->realization->graph->project_generation;
}

std::shared_ptr<CompiledGraph const> GraphExecutor::active_graph() const noexcept
{
    return impl_->active ? impl_->active->graph : nullptr;
}

GraphExecutorIndexedPropagationResult GraphExecutor::propagate_indexed(
    GraphExecutorIndexedPropagationRequest const& request)
{
    return impl_->propagate_indexed(request);
}

void GraphExecutor::tick_block(
    std::size_t sample_index,
    std::size_t block_size)
{
    impl_->tick_block(sample_index, block_size);
}

} // namespace iv
