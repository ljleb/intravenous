#include <intravenous/graph/reflected_node_description.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace iv::details {
class NodeDescriptionBuilder {
    ReflectedNodeDescription& _description;

public:
    explicit NodeDescriptionBuilder(ReflectedNodeDescription& description)
        : _description(description)
    {}

    NodeDescriptionSink sink() const
    {
        return NodeDescriptionSink(std::addressof(_description));
    }
};

ReflectedNodeDescription& NodeDescriptionSink::description() const
{
    if (!_description) {
        throw std::logic_error("node description sink is not initialized");
    }
    return *static_cast<ReflectedNodeDescription*>(_description);
}

void NodeDescriptionSink::add_input(InputConfig const& input) const
{
    description().ports.input_configs.push_back(input);
}

void NodeDescriptionSink::add_output(OutputConfig const& output) const
{
    description().ports.output_configs.push_back(output);
}

void NodeDescriptionSink::set_internal_latency(std::size_t value) const
{
    description().internal_latency_samples = value;
}

void NodeDescriptionSink::set_maximum_block_size(std::size_t value) const
{
    description().maximum_block_size = value;
}

void NodeDescriptionSink::set_default_ttl(std::optional<std::size_t> value) const
{
    description().default_ttl_samples = value;
}

void NodeDescriptionSink::set_block_skippable(bool value) const
{
    description().block_skippable = value;
}

void NodeDescriptionSink::set_replay_mode(TickReplayMode value) const
{
    description().replay_mode = value;
}

void NodeDescriptionSink::set_tick_output_coverage(PortKind kind, std::size_t output,
    std::span<TickCoverageTerm const> terms) const
{
    auto& node = description();
    if (!node.ports.supports_tick_coverage(kind, output, terms))
        throw std::invalid_argument("Tick coverage rules require Sequential inputs and a computed Sequential output");
    if (std::ranges::any_of(node.tick_output_coverage, [&](auto const& rule) {
            return rule.kind == kind && rule.output == output;
        })) throw std::invalid_argument("Tick output coverage declared twice");
    node.tick_output_coverage.push_back({kind, output, {terms.begin(), terms.end()}});
}

void NodeDescriptionSink::set_static_sample_value(std::optional<Sample> value) const
{
    description().static_sample_value = value;
}

ReflectedPortConstraintSink NodeDescriptionSink::port_constraint_sink() const
{
    auto* constraints = std::addressof(description().port_constraints);
    return {
        .state = constraints,
        .equal_value_types = +[](
            void* state,
            PortConstraintRef first,
            PortConstraintRef second) {
            static_cast<NodePortConstraints*>(state)
                ->value_type_equalities.push_back({first, second});
        },
        .anchor_value_types = +[](
            void* state,
            PortConstraintRef port,
            ContinuousValueTypeId value_type) {
            static_cast<NodePortConstraints*>(state)
                ->value_type_anchors.push_back({port, value_type});
        },
        .equal_sizes = +[](
            void* state, PortConstraintRef first, PortConstraintRef second) {
            static_cast<NodePortConstraints*>(state)
                ->size_equalities.push_back({first, second});
        },
        .anchor_sizes = +[](
            void* state, PortConstraintRef port, std::size_t size) {
            static_cast<NodePortConstraints*>(state)
                ->size_anchors.push_back({port, size});
        },
        .equal_paces = +[](
            void* state, PortConstraintRef first, PortConstraintRef second) {
            static_cast<NodePortConstraints*>(state)
                ->pace_equalities.push_back({first, second});
        },
        .anchor_paces = +[](
            void* state, PortConstraintRef port, PortPace pace) {
            static_cast<NodePortConstraints*>(state)
                ->pace_anchors.push_back({port, pace});
        },
        .equal_histories = +[](
            void* state, PortConstraintRef first, PortConstraintRef second) {
            static_cast<NodePortConstraints*>(state)
                ->history_equalities.push_back({first, second});
        },
        .anchor_histories = +[](
            void* state, PortConstraintRef port, std::size_t history) {
            static_cast<NodePortConstraints*>(state)
                ->history_anchors.push_back({port, history});
        },
        .equal_latencies = +[](
            void* state, PortConstraintRef first, PortConstraintRef second) {
            static_cast<NodePortConstraints*>(state)
                ->latency_equalities.push_back({first, second});
        },
        .anchor_latencies = +[](
            void* state, PortConstraintRef port, std::size_t latency) {
            static_cast<NodePortConstraints*>(state)
                ->latency_anchors.push_back({port, latency});
        },
        .equal_orderings = +[](
            void* state, PortConstraintRef first, PortConstraintRef second) {
            static_cast<NodePortConstraints*>(state)
                ->ordering_equalities.push_back({first, second});
        },
        .anchor_orderings = +[](
            void* state, PortConstraintRef port, CoverageOrdering ordering) {
            static_cast<NodePortConstraints*>(state)
                ->ordering_anchors.push_back({port, ordering});
        },
        .anchor_absolute_pace = +[](
            void* state, PortConstraintRef port, PortPace pace) {
            static_cast<NodePortConstraints*>(state)
                ->absolute_pace_anchors.push_back({port, pace});
        },
        .set_independent_pace = +[](void* state, PortConstraintRef port) {
            static_cast<NodePortConstraints*>(state)->independent_paces.push_back(port);
        },
        .request_preparation_lead = +[](void* state, PortConstraintRef port) {
            auto& requests = static_cast<NodePortConstraints*>(state)->preparation_leads;
            if (!std::ranges::contains(requests, port)) requests.push_back(port);
        },
        .equal_preparation_lead = +[](void* state, PortPreparationLeadEquality equality) {
            static_cast<NodePortConstraints*>(state)->preparation_lead_equalities.push_back(equality);
        },
        .event_count_context = std::addressof(description()),
        .set_event_count_bound = +[](void* opaque, PortConstraintRef port, std::size_t count) {
            auto& description = *static_cast<ReflectedNodeDescription*>(opaque);
            std::size_t event_index = 0;
            for (auto& config : description.ports.output_configs) {
                auto* event = std::get_if<EventOutputProperties>(&config.kind);
                if (!event) continue;
                if (event_index++ == port.port_index) {
                    event->max_event_count = event->max_event_count
                        ? std::min(*event->max_event_count, count) : count;
                    return;
                }
            }
            throw std::logic_error("event count bound references an absent output");
        },
    };
}

ReflectedNodeDescription materialize_node_description(
    NodeBuildRequest const& request,
    std::shared_ptr<void const> storage,
    NodeConfigRelocations relocations)
{
    if (!request.compiler_record || !request.config || !request.describe
        || request.config_size == 0 || request.config_alignment == 0 || !storage) {
        throw std::invalid_argument("invalid node build request");
    }

    auto const& record = *request.compiler_record;
    if (!record.operations.valid()
        || (!record.type_name && record.type_name_size != 0)) {
        throw std::invalid_argument("node compiler record has an invalid type name");
    }

    ReflectedNodeDescription result {
        .node_storage = std::move(storage),
        .config_relocations = std::move(relocations),
        .code_key = record.code_key,
        .node_size = request.config_size,
        .node_alignment = request.config_alignment,
        .type_name = record.type_name
            ? std::string_view{record.type_name, record.type_name_size}
            : std::string_view{},
    };
    result.operations.runtime = make_runtime_operations(
        record, result.node_storage.get());

    NodeDescriptionBuilder builder(result);
    auto sink = builder.sink();
    request.describe(result.node_storage.get(), sink);
    if (result.replay_mode != record.replay_mode) {
        throw std::invalid_argument(
            "node replay mode disagrees with its compiler record");
    }
    if (result.replay_mode != TickReplayMode::disabled
        && result.internal_latency_samples != 0) {
        throw std::invalid_argument(
            "intrinsically replayable node requires zero configured internal latency");
    }
    return result;
}

} // namespace iv::details
